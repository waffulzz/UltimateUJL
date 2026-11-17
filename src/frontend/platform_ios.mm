// iOS/iPadOS host functions (see platform.h): the Documents folder and UIKit's document picker.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <vector>
#include <cstdio>
#include "platform.h"

std::string platform_data_dir() {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (!docs) return "";
    [[NSFileManager defaultManager] createDirectoryAtPath:docs withIntermediateDirectories:YES attributes:nil error:nil];
    return std::string(docs.fileSystemRepresentation) + "/";
}

bool platform_can_pick_folders() { return false; }

@interface UJLDocumentPicker : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>
@property(nonatomic) SDL_DialogFileCallback cb;
@property(nonatomic) void *ud;
@property(nonatomic) BOOL keepInDocuments;
@property(nonatomic) BOOL exporting;
@property(nonatomic) BOOL finished;
@property(nonatomic, strong) NSURL *exportDirectory;
@property(nonatomic, weak) UIDocumentPickerViewController *controller;
@end

static UJLDocumentPicker *g_active_picker;   // the delegate is weak on the controller: keep it alive here

@implementation UJLDocumentPicker
- (void)finish:(const std::vector<std::string> &)paths {
    if (self.finished) return; // Cancel and sheet dismissal can both arrive.
    self.finished = YES;
    std::vector<std::string> copied = paths;
    void (^complete)(void) = ^{
        if (self.exportDirectory) [[NSFileManager defaultManager] removeItemAtURL:self.exportDirectory error:nil];
        std::vector<const char *> list;
        for (const std::string &p : copied) list.push_back(p.c_str());
        list.push_back(nullptr);
        if (self.cb) self.cb(self.ud, list.data(), -1);
        if (g_active_picker == self) g_active_picker = nil;
    };
    // Publish after dismissal, so an invalid replay can immediately offer another picker safely.
    UIDocumentPickerViewController *picker = self.controller;
    if (picker.isBeingDismissed && picker.transitionCoordinator) {
        BOOL waiting = [picker.transitionCoordinator animateAlongsideTransition:nil completion:^(id<UIViewControllerTransitionCoordinatorContext> context) {
            (void)context; complete();
        }];
        if (!waiting) dispatch_async(dispatch_get_main_queue(), complete);
    } else if (picker.presentingViewController) {
        [picker dismissViewControllerAnimated:YES completion:complete];
    } else {
        complete();
    }
}

// asCopy:YES hands over private copies in the app's tmp folder; move them into Documents so they stay (and show
// up in the Files app next to everything else). A file of the same name there is replaced.
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (self.exporting || !self.keepInDocuments) {
        // Exports are already copied to the provider by UIKit. Imports made with asCopy:YES remain readable
        // in the app sandbox until termination; replay validation must not overwrite Documents by filename.
        std::vector<std::string> paths;
        for (NSURL *url in urls) {
            const char *path = self.exporting ? url.absoluteString.UTF8String : url.fileSystemRepresentation;
            if (path) paths.push_back(path);
        }
        [self finish:paths];
        return;
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *docs = [NSString stringWithUTF8String:platform_data_dir().c_str()];
    std::vector<std::string> paths;
    for (NSURL *url in urls) {
        NSString *dst = [docs stringByAppendingPathComponent:url.lastPathComponent];
        NSError *err = nil;
        if (![url.path isEqualToString:dst]) {
            [fm removeItemAtPath:dst error:nil];
            if (![fm moveItemAtURL:url toURL:[NSURL fileURLWithPath:dst] error:&err] &&
                ![fm copyItemAtURL:url toURL:[NSURL fileURLWithPath:dst] error:&err]) {
                fprintf(stderr, "picker: couldn't keep %s: %s\n", url.path.UTF8String, err.localizedDescription.UTF8String);
                continue;
            }
        }
        fprintf(stderr, "picker: %s\n", dst.UTF8String);
        paths.push_back(dst.fileSystemRepresentation);
    }
    [self finish:paths];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    [self finish:std::vector<std::string>()];
}

- (void)presentationControllerDidDismiss:(UIPresentationController *)presentationController {
    [self finish:std::vector<std::string>()];
}
@end

static UIViewController *picker_root(SDL_DialogFileCallback cb, void *ud, SDL_Window *win) {
    UIWindow *uiwin = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                                                    SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController *root = uiwin.rootViewController;
    if (!root || g_active_picker) {
        SDL_SetError("%s", root ? "Another file picker is already open" : "No window for the file picker");
        cb(ud, nullptr, -1);
        return nil;
    }
    while (root.presentedViewController) root = root.presentedViewController;
    return root;
}

static void present_picker(UIViewController *root, UIDocumentPickerViewController *picker,
                           UJLDocumentPicker *delegate) {
    g_active_picker = delegate;
    delegate.controller = picker;
    picker.delegate = delegate;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    [root presentViewController:picker animated:YES completion:nil];
    picker.presentationController.delegate = delegate;
}

void platform_open_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win, const SDL_DialogFileFilter *, int,
                               const char *, bool many, bool keep_in_documents) {
    UIViewController *root = picker_root(cb, ud, win);
    if (!root) return;
    // Disc images (.bin/.img/.cue), card dumps and zips have no common type: offer every file.
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
    UJLDocumentPicker *delegate = [UJLDocumentPicker new];
    delegate.cb = cb;
    delegate.ud = ud;
    delegate.keepInDocuments = keep_in_documents;
    picker.allowsMultipleSelection = many;
    present_picker(root, picker, delegate);
}

void platform_export_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win,
                                 const char *name, const void *bytes, size_t size) {
    UIViewController *root = picker_root(cb, ud, win);
    if (!root) return;
    if (!bytes || !size) {
        SDL_SetError("No replay data to export");
        cb(ud, nullptr, -1);
        return;
    }
    NSURL *directory = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
        [@"UJLReplayExport-" stringByAppendingString:NSUUID.UUID.UUIDString]] isDirectory:YES];
    NSString *filename = name ? [[NSString stringWithUTF8String:name] lastPathComponent] : nil;
    if (!filename.length || [filename isEqualToString:@"."] || [filename isEqualToString:@".."]) filename = @"Replay.ujlreplay";
    NSURL *file = [directory URLByAppendingPathComponent:filename];
    NSError *error = nil;
    NSFileManager *fm = [NSFileManager defaultManager];
    NSData *data = [NSData dataWithBytes:bytes length:size];
    if (![fm createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:&error] ||
        ![data writeToURL:file options:NSDataWritingAtomic error:&error]) {
        [fm removeItemAtURL:directory error:nil];
        SDL_SetError("Couldn't prepare replay export: %s", error.localizedDescription.UTF8String);
        cb(ud, nullptr, -1);
        return;
    }
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[file] asCopy:YES];
    UJLDocumentPicker *delegate = [UJLDocumentPicker new];
    delegate.cb = cb;
    delegate.ud = ud;
    delegate.exporting = YES;
    delegate.exportDirectory = directory;
    present_picker(root, picker, delegate);
}
