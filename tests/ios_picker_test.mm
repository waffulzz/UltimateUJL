// Native UIKit picker lifecycle checks in an isolated simulator app. Files and replay bytes are synthetic.
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "platform.h"
#include <functional>
#include <cstdio>
#include <cstdlib>

static std::string result_path;
static void progress(const char *message) {
    FILE *file = fopen((platform_data_dir() + "picker-test-progress.txt").c_str(), "a");
    if (file) { fprintf(file, "%s\n", message); fclose(file); }
}
static void check(bool ok, const char *message) {
    if (ok) return;
    std::string error = std::string("FAIL: ") + message + "\n";
    SDL_SaveFile(result_path.c_str(), error.data(), error.size());
    fprintf(stderr, "%s", error.c_str());
    exit(1);
}
static void wait_for(const std::function<bool()> &done) {
    Uint64 until = SDL_GetTicks() + 15000;
    while (!done() && SDL_GetTicks() < until) {
        SDL_PumpEvents();
        [[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    check(done(), "picker callback/presentation timed out");
}
struct Pick { int count = 0; bool error = false; std::string path; };
static void SDLCALL picked(void *ud, const char *const *files, int) {
    auto *p = static_cast<Pick *>(ud);
    p->count++; p->error = files == nullptr; p->path = files && files[0] ? files[0] : "";
}
static UIDocumentPickerViewController *current_picker(SDL_Window *window) {
    UIWindow *win = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController *vc = win.rootViewController;
    while (vc) {
        if ([vc isKindOfClass:UIDocumentPickerViewController.class])
            return vc.isBeingPresented || !vc.view.window ? nil : (UIDocumentPickerViewController *)vc;
        vc = vc.presentedViewController;
    }
    return nil;
}
static NSURL *staged_file(NSString *name) {
    NSDirectoryEnumerator *files = [[NSFileManager defaultManager] enumeratorAtURL:
        [NSURL fileURLWithPath:NSTemporaryDirectory()] includingPropertiesForKeys:nil options:0 errorHandler:nil];
    for (NSURL *file in files) if ([file.lastPathComponent isEqualToString:name]) return file;
    return nil;
}

int main(int, char **) {
    result_path = platform_data_dir() + "picker-test-result.txt";
    SDL_RemovePath(result_path.c_str());
    progress("Entered SDL main");
    check(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization");
    SDL_Window *window = SDL_CreateWindow("Picker checks", 320, 240, 0);
    check(window != nullptr, "SDL window creation");
    SDL_PumpEvents();
    progress("Created SDL window");
    const char bytes[] = "synthetic replay data";
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *docs = [NSString stringWithUTF8String:platform_data_dir().c_str()];

    Pick exported;
    platform_export_file_dialog(picked, &exported, window, "Test.ujlreplay", bytes, sizeof bytes);
    wait_for([&] { return current_picker(window) != nil; });
    progress("Presented export picker");
    UIDocumentPickerViewController *picker = current_picker(window);
    check(picker.documentPickerMode == UIDocumentPickerModeExportToService, "native export picker mode");
    NSURL *staged = staged_file(@"Test.ujlreplay");
    check(staged != nil, "replay is staged before export");
    check([[NSData dataWithContentsOfURL:staged] isEqualToData:[NSData dataWithBytes:bytes length:sizeof bytes]], "staged data");
    check(exported.count == 0, "export must await completion");
    Pick busy;
    platform_open_file_dialog(picked, &busy, window, nullptr, 0, nullptr, false, false);
    check(busy.count == 1 && busy.error && exported.count == 0, "busy picker cannot steal pending export");
    // Simulate the provider's completed copy, then deliver UIKit's documented completion callback.
    NSURL *destination = [NSURL fileURLWithPath:[docs stringByAppendingPathComponent:@"Exported.ujlreplay"]];
    check([[NSData dataWithContentsOfURL:staged] writeToURL:destination atomically:YES], "provider copy fixture");
    id<UIDocumentPickerDelegate> delegate = picker.delegate;
    [delegate documentPicker:picker didPickDocumentsAtURLs:@[destination]];
    wait_for([&] { return exported.count == 1; });
    progress("Completed export callback");
    check(!exported.error && exported.path == destination.absoluteString.UTF8String, "export destination callback");
    check(![fm fileExistsAtPath:staged.path] && [fm fileExistsAtPath:destination.path], "export cleans staging, keeps destination");
    [delegate documentPickerWasCancelled:picker];
    check(exported.count == 1, "duplicate callback ignored");

    for (bool swipe : {false, true}) {
        Pick cancelled;
        platform_export_file_dialog(picked, &cancelled, window, "Cancel.ujlreplay", bytes, sizeof bytes);
        wait_for([&] { return current_picker(window) != nil; });
        picker = current_picker(window); staged = staged_file(@"Cancel.ujlreplay");
        check(staged != nil, "cancel staging exists");
        delegate = picker.delegate;
        if (swipe) [(id<UIAdaptivePresentationControllerDelegate>)delegate presentationControllerDidDismiss:picker.presentationController];
        else [delegate documentPickerWasCancelled:picker];
        wait_for([&] { return cancelled.count == 1; });
        progress(swipe ? "Completed swipe cancellation" : "Completed button cancellation");
        check(!cancelled.error && cancelled.path.empty() && ![fm fileExistsAtPath:staged.path], "cancel/swipe completes and cleans staging");
    }

    // Opening a same-named invalid replay must not replace the app's existing settings file.
    NSString *settings = [docs stringByAppendingPathComponent:@"ujl.ini"];
    [@"original settings" writeToFile:settings atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSURL *imported = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:@"ujl.ini"]];
    [@"invalid replay fixture" writeToURL:imported atomically:YES encoding:NSUTF8StringEncoding error:nil];
    Pick opened;
    platform_open_file_dialog(picked, &opened, window, nullptr, 0, nullptr, false, false);
    wait_for([&] { return current_picker(window) != nil; });
    picker = current_picker(window);
    check(picker.documentPickerMode == UIDocumentPickerModeImport, "native replay import is a sandbox copy");
    [picker.delegate documentPicker:picker didPickDocumentsAtURLs:@[imported]];
    wait_for([&] { return opened.count == 1; });
    progress("Completed import callback");
    check(!opened.error && opened.path == imported.fileSystemRepresentation, "temporary import path survives callback");
    check([[NSString stringWithContentsOfFile:settings encoding:NSUTF8StringEncoding error:nil] isEqualToString:@"original settings"], "import preserves settings");
    check([fm fileExistsAtPath:imported.path], "import readable for next UI tick");

    check(SDL_SaveFile(result_path.c_str(), "PASS\n", 5), "write success marker");
    SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
