// Disc identification and the setup wizard (first run, and Settings > Discs > Manage discs).
#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct SDL_Window;

// main.cpp: ujl.ini access and the exe folder
std::string ini_get(const char *key, const std::string &def = "");
void ini_set(const char *key, const std::string &val);
extern std::string g_base_dir;

enum DiscKind {
    DISC_UNREADABLE,  // not a raw 2352-byte PlayStation image (or no file)
    DISC_JP,          // the disc this build was recompiled from (retail Japan, SCPS-18011)
    DISC_USA,         // USA retail (SCUS-94448): asset source
    DISC_USA_OTHER,   // boots SCUS_944.48 but the executable isn't the known release
    DISC_EUROPE,      // Europe retail (SCES-01753): recognized, not used yet
    DISC_PROTO,       // January 1999 prototype (boots SCPS_180.11, different program)
    DISC_ARCADE_CD,   // UJL NOW! arcade CD
    DISC_PS1_OTHER,   // some other PlayStation disc
    DISC_UNKNOWN,     // an ISO 9660 image that isn't any of the above
};

struct DiscInfo {
    DiscKind kind = DISC_UNREADABLE;
    std::string path;   // the image data file (.cue/.ccd resolved)
    std::string boot;   // boot executable name from SYSTEM.CNF
};

DiscInfo disc_identify(const std::string &path);
// Every recognizable image (*.bin/*.img/*.iso) in these folders.
std::vector<DiscInfo> disc_search(const std::vector<std::string> &dirs);
// The exe folder and up to four parents.
std::vector<std::string> disc_search_dirs();

// UJL NOW! ROM set: a MAME zip, a folder, or any file inside that folder. `path` is set to the zip or folder.
struct RomCheck { bool ok = false; std::string path, message; };
RomCheck romset_check(const std::string &path);

// What's imported (ujl.ini disc / usa_disc / arcade_cd / arcade_roms).
struct DiscSetup { std::string jp, usa, arcade_cd, arcade_roms; };
DiscSetup discs_from_ini();
// How a path is stored in ujl.ini (iOS: relative to the app's Documents folder, which moves on reinstall)
std::string disc_path_to_ini(const std::string &p);
std::string disc_path_from_ini(const std::string &p);
void discs_to_ini(const DiscSetup &s);

// Wizard. Draw it inside an ImGui frame every frame while active.
enum WizardResult { WIZ_RUNNING, WIZ_DONE, WIZ_QUIT, WIZ_CLOSED };
void wizard_open(bool first_run, const DiscSetup &start);
bool wizard_active();
WizardResult wizard_draw(SDL_Window *win);   // WIZ_DONE: saved to ujl.ini; WIZ_QUIT: first run abandoned
void wizard_drop_file(const char *path);     // a file dragged onto the window
const DiscSetup &wizard_result();
bool wizard_finish();                        // save to ujl.ini and close (needs the Japanese disc)
void wizard_set_page(int page);              // dev (UJL_SETUP_SHOTS): 0 welcome, 1 Japanese disc, 2 optional, 3 summary
// Settings: which features each disc enables, with a lock reason when missing.
void discs_draw_features(const DiscSetup &s, unsigned only_needs = ~0u);
