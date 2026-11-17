// Host platform differences for the frontend.
//
// iOS/iPadOS (UJL_IOS): the app bundle is read-only and has no folder next to it, so every writable file (ujl.ini,
// ujl.log, the memory card, shots) and the disc images live in the app's Documents folder, which Info.plist exposes
// in the Files app and Finder. SDL has no file dialog there, so platform_ios.mm presents UIKit's document picker,
// which keeps disc/card imports in Documents and uses temporary copies for replay import/export.
// Desktop builds use SDL's dialogs and the exe folder.
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
// callback gets those copies. With keep_in_documents=false, iOS returns its temporary import copy;
// it remains readable until app termination and does not replace any permanent user file.
void platform_open_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win,
                               const SDL_DialogFileFilter *filters, int nfilters, const char *start_dir, bool many,
                               bool keep_in_documents = true);

#if UJL_IOS
// Export a copy of bytes through Files. The callback indicates success (one destination URL), cancellation
// (empty list), or error (null list + SDL_GetError). The caller must not write the returned provider URL.
void platform_export_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win,
                                 const char *name, const void *bytes, size_t size);
#endif

// Can folders be picked? (Not on iOS: a picked folder outside the app is only readable through a security scope.)
bool platform_can_pick_folders();

#if !UJL_IOS
inline std::string platform_data_dir() {
    const char *bp = SDL_GetBasePath();
    return bp ? bp : "";
}
inline void platform_open_file_dialog(SDL_DialogFileCallback cb, void *ud, SDL_Window *win,
                                      const SDL_DialogFileFilter *filters, int nfilters, const char *start_dir, bool many,
                                      bool) {
    SDL_ShowOpenFileDialog(cb, ud, win, filters, nfilters, start_dir, many);
}
inline bool platform_can_pick_folders() { return true; }
#endif
