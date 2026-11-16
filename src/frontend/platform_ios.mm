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

@interface UJLDocumentPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) SDL_DialogFileCallback cb;
@property(nonatomic) void *ud;
@end

static UJLDocumentPicker *g_active_picker;   // the delegate is weak on the controller: keep it alive here

@implementation UJLDocumentPicker
- (void)finish:(const std::vector<std::string> &)paths {
    std::vector<const char *> list;
    for (const std::string &p : paths) list.push_back(p.c_str());
    list.push_back(nullptr);
    if (self.cb) self.cb(self.ud, list.data(), -1);
    g_active_picker = nil;
}

// asCopy:YES hands over private copies in the app's tmp folder; move them into Documents so they stay (and show
// up in the Files app next to everything else). A file of the same name there is replaced.
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
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
@end

void platform_open_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win, const SDL_DialogFileFilter *, int,
                               const char *, bool many) {
    UIWindow *uiwin = (__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(win),
                                                                    SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    UIViewController *root = uiwin.rootViewController;
    if (!root || g_active_picker) {
        const char *none[] = {nullptr};
        cb(ud, none, -1);
        return;
    }
    while (root.presentedViewController) root = root.presentedViewController;
    // Disc images (.bin/.img/.cue), card dumps and zips have no common type: offer every file.
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
    g_active_picker = [UJLDocumentPicker new];
    g_active_picker.cb = cb;
    g_active_picker.ud = ud;
    picker.delegate = g_active_picker;
    picker.allowsMultipleSelection = many;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    [root presentViewController:picker animated:YES completion:nil];
}
