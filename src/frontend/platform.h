// Host platform differences for the frontend.
//
// iOS/iPadOS (UJL_IOS): the app bundle is read-only and has no folder next to it, so every writable file (ujl.ini,
// ujl.log, the memory card, shots) and the disc images live in the app's Documents folder, which Info.plist exposes
// in the Files app and Finder. SDL has no file dialog there, so platform_ios.mm presents UIKit's document picker,
// which copies the picked files into Documents. Desktop builds use SDL's dialogs and the exe folder.
#pragma once
#include <SDL3/SDL.h>
#include <string>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS
#define UJL_IOS 1
#else
#define UJL_IOS 0
#endif

// Folder for ujl.ini, the log, memory cards and shots, with a trailing separator.
std::string platform_data_dir();

// Open-file dialog. The callback gets a null-terminated list (empty = cancelled), maybe on another thread.
// iOS: filters and start_dir are ignored; picked files are copied into platform_data_dir() first and the
// callback gets those copies.
void platform_open_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win,
                               const SDL_DialogFileFilter *filters, int nfilters, const char *start_dir, bool many);

// Can folders be picked? (Not on iOS: a picked folder outside the app is only readable through a security scope.)
bool platform_can_pick_folders();

#if !UJL_IOS
inline std::string platform_data_dir() {
    const char *bp = SDL_GetBasePath();
    return bp ? bp : "";
}
inline void platform_open_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win,
                                      const SDL_DialogFileFilter *filters, int nfilters, const char *start_dir, bool many) {
    SDL_ShowOpenFileDialog(cb, ud, win, filters, nfilters, start_dir, many);
}
inline bool platform_can_pick_folders() { return true; }
#endif
