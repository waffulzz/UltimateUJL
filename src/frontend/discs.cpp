// Disc identification and the setup wizard.
//
// The port needs the Japanese disc (the one this build was recompiled from) and can use other discs as asset
// sources. Each image is identified from its own contents, never its file name:
//   Japan (SCPS-18011)   the boot executable's entry point matches this build (works for any build, CI included)
//   USA (SCUS-94448)     boot name + CRC-32 of the executable
//   Europe (SCES-01753)  boot name (recognized so the wizard can say what it is; nothing uses it yet)
//   Jan 1999 prototype   boots SCPS_180.11 like retail but with a different program
//   UJL NOW! arcade CD   no SYSTEM.CNF; identified by its arcade movie and Joe Chin XA files
//   UJL NOW! ROM set     CRC-32 of each board ROM (MAME set "ujlnow"), read from a zip's directory or a folder
// What was imported is stored in ujl.ini (disc, usa_disc, arcade_cd, arcade_roms; setup_done after the first run).
#include "discs.h"

#include <SDL3/SDL.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "imgui.h"
#include "platform.h"
#include "touch_pad.h"

extern "C" {
bool disc_image_pvd(const char *path, uint8_t pvd[2048]);
bool disc_image_read_file(const char *path, const char *file, uint8_t **data, uint32_t *size);
bool disc_image_has_file(const char *path, const char *file);
uint32_t rt_recomp_entry(void);
}

// ---------------------------------------------------------------- helpers
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static bool path_is(const std::string &p, SDL_PathType t) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == t;
}
static bool is_file(const std::string &p) { return path_is(p, SDL_PATHTYPE_FILE); }
static bool is_dir(const std::string &p) { return path_is(p, SDL_PATHTYPE_DIRECTORY); }

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static std::string ext_of(const std::string &p) {
    size_t dot = p.find_last_of('.'), sep = p.find_last_of("/\\");
    if (dot == std::string::npos || (sep != std::string::npos && dot < sep)) return "";
    return lower(p.substr(dot));
}
static std::string dir_of(const std::string &p) { return p.substr(0, p.find_last_of("/\\") + 1); }
static std::string base_of(const std::string &p) { return p.substr(p.find_last_of("/\\") + 1); }
static std::string strip_ext(const std::string &p) {
    std::string e = ext_of(p);
    return p.substr(0, p.size() - e.size());
}

// A .cue or .ccd names (or sits next to) the image data; everything else is the data itself.
static std::string resolve_image(const std::string &path) {
    std::string e = ext_of(path);
    if (e != ".cue" && e != ".ccd") return path;
    if (e == ".cue") {
        if (FILE *f = fopen(path.c_str(), "rb")) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                const char *q = strstr(line, "FILE \"");
                if (!q) continue;
                q += 6;
                const char *end = strchr(q, '"');
                if (!end) continue;
                std::string cand = dir_of(path) + std::string(q, end);
                if (is_file(cand)) { fclose(f); return cand; }
                break;   // only the first FILE is the data track; a stale name falls through to the sibling search
            }
            fclose(f);
        }
    }
    for (const char *x : {".bin", ".img", ".BIN", ".IMG"})
        if (is_file(strip_ext(path) + x)) return strip_ext(path) + x;
    return path;
}

// ---------------------------------------------------------------- identification
DiscInfo disc_identify(const std::string &path) {
    DiscInfo r;
    r.path = resolve_image(path);
    uint8_t pvd[2048];
    if (!disc_image_pvd(r.path.c_str(), pvd)) return r;
    uint8_t *cnf = nullptr;
    uint32_t sz = 0;
    if (disc_image_read_file(r.path.c_str(), "SYSTEM.CNF", &cnf, &sz)) {
        std::string s((const char *)cnf, std::min<uint32_t>(sz, 2048));
        free(cnf);
        size_t b = s.find("BOOT");
        size_t colon = b == std::string::npos ? b : s.find(':', b);
        if (colon != std::string::npos) {
            size_t i = colon + 1;
            while (i < s.size() && (s[i] == '\\' || s[i] == '/')) i++;
            while (i < s.size() && s[i] != ';' && !isspace((unsigned char)s[i])) r.boot += s[i++];
        }
        uint8_t *exe = nullptr;
        uint32_t exe_size = 0;
        uint32_t pc = 0, crc = 0;
        if (!r.boot.empty() && disc_image_read_file(r.path.c_str(), r.boot.c_str(), &exe, &exe_size)) {
            if (exe_size >= 0x20 && !memcmp(exe, "PS-X EXE", 8)) memcpy(&pc, exe + 0x10, 4);
            crc = crc32_update(0, exe, exe_size);
            free(exe);
        }
        std::string boot = lower(r.boot);
        if (pc && pc == rt_recomp_entry()) r.kind = DISC_JP;
        else if (boot == "scus_944.48") r.kind = crc == 0x30FFBCE1u ? DISC_USA : DISC_USA_OTHER;
        else if (boot == "sces_017.53") r.kind = DISC_EUROPE;
        else if (boot == "scps_180.11") r.kind = DISC_PROTO;   // retail would have matched the entry point above
        else r.kind = DISC_PS1_OTHER;
        return r;
    }
    if (disc_image_has_file(r.path.c_str(), "MOVIE/AM01.IKI") && disc_image_has_file(r.path.c_str(), "MOVIE/NAMCO.IKI") &&
        disc_image_has_file(r.path.c_str(), "XA/JOE1.XA"))
        r.kind = DISC_ARCADE_CD;
    else
        r.kind = !memcmp(pvd + 8, "PLAYSTATION", 11) ? DISC_PS1_OTHER : DISC_UNKNOWN;
    return r;
}

std::vector<std::string> disc_search_dirs() {
    std::vector<std::string> dirs = {g_base_dir};
#if UJL_IOS
    // the app's Documents folder (Files app / Finder file sharing), and Inbox where "Open in" puts files
    dirs.push_back(g_base_dir + "Inbox/");
    return dirs;
#endif
    for (int up = 0; up < 4; up++) {
        const std::string &d = dirs.back();
        if (d.size() < 2) break;
        size_t cut = d.find_last_of("/\\", d.size() - 2);
        if (cut == std::string::npos) break;
        dirs.push_back(d.substr(0, cut + 1));
    }
    return dirs;
}

std::vector<DiscInfo> disc_search(const std::vector<std::string> &dirs) {
    std::vector<DiscInfo> out;
    for (const std::string &d : dirs)
        for (const char *pat : {"*.bin", "*.img", "*.iso"}) {
            int n = 0;
            char **names = SDL_GlobDirectory(d.c_str(), pat, SDL_GLOB_CASEINSENSITIVE, &n);
            for (int i = 0; names && i < n; i++) {
                DiscInfo di = disc_identify(d + names[i]);
                if (di.kind == DISC_JP || di.kind == DISC_USA || di.kind == DISC_ARCADE_CD) out.push_back(di);
            }
            SDL_free(names);
        }
    return out;
}

// ---------------------------------------------------------------- UJL NOW! ROM set
struct RomFile { const char *name; uint32_t size, crc; };
static const RomFile k_roms[] = {
    {"ul1rom0l.ic12", 8388608, 0x7A77CFFD}, {"ul1rom0u.ic11", 8388608, 0x1FDB34B3},
    {"ul1rom1l.ic10", 8388608, 0x2D300D8C}, {"ul1rom1u.ic9", 8388608, 0xA4762002},
    {"ul1spro.10c", 524288, 0x1BD0E763},    {"ul1vera.11s", 524288, 0x2F466DF3},
    {"ul1vera.2l", 2097152, 0x40251BC3},    {"ul1vera.2p", 2097152, 0x98181955},
    {"ul1wave0.ic2", 8388608, 0x4EFBFF5D},  {"ul1wave1.ic1", 8388608, 0xAD512F4C},
};
static const int k_nroms = (int)(sizeof k_roms / sizeof k_roms[0]);

static std::string rom_report(const std::vector<std::string> &missing, const std::vector<std::string> &bad) {
    if (missing.size() == (size_t)k_nroms) return "That isn't the UJL NOW! ROM set (no ul1*.* ROM files in it).";
    std::string m;
    auto list = [](const std::vector<std::string> &v) {
        std::string s;
        for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + v[i];
        return s;
    };
    if (!missing.empty()) m += "Missing: " + list(missing) + ". ";
    if (!bad.empty()) m += "Different from the known dump: " + list(bad) + ".";
    return m;
}

static RomCheck romset_check_zip(const std::string &zip) {
    RomCheck rc;
    rc.path = zip;
    SDL_IOStream *f = SDL_IOFromFile(zip.c_str(), "rb");   // 64-bit sizes (archives of the whole arcade dump are > 4 GB)
    if (!f) { rc.message = "Couldn't open that file."; return rc; }
    Sint64 size = SDL_GetIOSize(f);
    Sint64 tail = std::min<Sint64>(size, 65557);
    std::vector<uint8_t> t((size_t)std::max<Sint64>(tail, 0));
    if (tail <= 0 || SDL_SeekIO(f, size - tail, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(f, t.data(), t.size()) != t.size()) tail = 0;
    Sint64 eocd = -1;
    for (Sint64 i = tail - 22; i >= 0; i--)
        if (t[i] == 0x50 && t[i + 1] == 0x4B && t[i + 2] == 5 && t[i + 3] == 6) { eocd = i; break; }
    if (eocd < 0) { SDL_CloseIO(f); rc.message = "That isn't a zip file."; return rc; }
    uint32_t cd_size, cd_off;
    memcpy(&cd_size, &t[eocd + 12], 4);
    memcpy(&cd_off, &t[eocd + 16], 4);
    // A ROM set zip is small; a zip64 directory (offset 0xFFFFFFFF) or a huge one isn't the ROM set.
    if (cd_off == 0xFFFFFFFFu || cd_size > (16u << 20) || (Sint64)cd_off + cd_size > size) {
        SDL_CloseIO(f);
        rc.message = "That zip isn't the UJL NOW! ROM set (unzip it first if the ROM set zip is inside it).";
        return rc;
    }
    std::vector<uint8_t> cd(cd_size);
    bool ok = SDL_SeekIO(f, cd_off, SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(f, cd.data(), cd.size()) == cd.size();
    SDL_CloseIO(f);
    if (!ok) { rc.message = "That zip file is damaged."; return rc; }
    std::vector<int> state(k_nroms, 0);   // 0 missing, 1 ok, 2 different
    for (size_t p = 0; p + 46 <= cd.size();) {
        uint32_t sig, crc, usize;
        uint16_t nlen, xlen, clen;
        memcpy(&sig, &cd[p], 4);
        if (sig != 0x02014B50) break;
        memcpy(&crc, &cd[p + 16], 4);
        memcpy(&usize, &cd[p + 24], 4);
        memcpy(&nlen, &cd[p + 28], 2);
        memcpy(&xlen, &cd[p + 30], 2);
        memcpy(&clen, &cd[p + 32], 2);
        if (p + 46 + nlen > cd.size()) break;
        std::string name = lower(base_of(std::string((const char *)&cd[p + 46], nlen)));
        for (int i = 0; i < k_nroms; i++)
            if (name == k_roms[i].name && state[i] != 1) state[i] = (crc == k_roms[i].crc && usize == k_roms[i].size) ? 1 : 2;
        p += 46 + nlen + xlen + clen;
    }
    std::vector<std::string> missing, bad;
    for (int i = 0; i < k_nroms; i++)
        if (state[i] == 0) missing.push_back(k_roms[i].name);
        else if (state[i] == 2) bad.push_back(k_roms[i].name);
    rc.ok = missing.empty() && bad.empty();
    rc.message = rc.ok ? "All " + std::to_string(k_nroms) + " ROM files verified." : rom_report(missing, bad);
    return rc;
}

static RomCheck romset_check_dir(const std::string &dir_in) {
    RomCheck rc;
    std::string dir = dir_in;
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    rc.path = dir;
    int n = 0;
    char **names = SDL_GlobDirectory(dir.c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &n);
    std::vector<std::string> missing, bad;
    std::vector<uint8_t> buf(1 << 20);
    for (int i = 0; i < k_nroms; i++) {
        std::string found;
        for (int k = 0; names && k < n; k++)
            if (lower(names[k]) == k_roms[i].name) found = dir + names[k];
        if (found.empty()) { missing.push_back(k_roms[i].name); continue; }
        FILE *f = fopen(found.c_str(), "rb");
        uint32_t crc = 0, total = 0;
        size_t got;
        while (f && (got = fread(buf.data(), 1, buf.size(), f)) > 0) { crc = crc32_update(crc, buf.data(), got); total += (uint32_t)got; }
        if (f) fclose(f);
        if (crc != k_roms[i].crc || total != k_roms[i].size) bad.push_back(k_roms[i].name);
    }
    SDL_free(names);
    rc.ok = missing.empty() && bad.empty();
    rc.message = rc.ok ? "All " + std::to_string(k_nroms) + " ROM files verified." : rom_report(missing, bad);
    return rc;
}

RomCheck romset_check(const std::string &path) {
    if (is_dir(path)) return romset_check_dir(path);
    if (ext_of(path) == ".zip") return romset_check_zip(path);
    if (is_file(path)) return romset_check_dir(dir_of(path));   // a file inside the ROM folder
    RomCheck rc;
    rc.message = "That file or folder doesn't exist.";
    return rc;
}

// ---------------------------------------------------------------- ujl.ini
// iOS moves the app's data container (a new UUID in its path) when the app is reinstalled or updated, so a disc
// image inside Documents is stored relative to it there and resolved again at load.
std::string disc_path_to_ini(const std::string &p) {
#if UJL_IOS
    if (!g_base_dir.empty() && p.compare(0, g_base_dir.size(), g_base_dir) == 0) return p.substr(g_base_dir.size());
#endif
    return p;
}
std::string disc_path_from_ini(const std::string &p) {
#if UJL_IOS
    if (!p.empty() && p[0] != '/') return g_base_dir + p;
    size_t d = p.find("/Documents/");   // an absolute path into an older container: the same file in this one
    if (d != std::string::npos && !is_file(p) && !is_dir(p)) return g_base_dir + p.substr(d + strlen("/Documents/"));
#endif
    return p;
}

DiscSetup discs_from_ini() {
    DiscSetup s;
    s.jp = disc_path_from_ini(ini_get("disc"));
    s.usa = disc_path_from_ini(ini_get("usa_disc"));
    s.arcade_cd = disc_path_from_ini(ini_get("arcade_cd"));
    s.arcade_roms = disc_path_from_ini(ini_get("arcade_roms"));
    return s;
}

void discs_to_ini(const DiscSetup &s) {
    ini_set("disc", disc_path_to_ini(s.jp));
    ini_set("usa_disc", disc_path_to_ini(s.usa));
    ini_set("arcade_cd", disc_path_to_ini(s.arcade_cd));
    ini_set("arcade_roms", disc_path_to_ini(s.arcade_roms));
}

// ---------------------------------------------------------------- features
enum { NEED_USA = 1, NEED_ARCADE_CD = 2, NEED_ARCADE_ROMS = 4 };
struct Feature { const char *name; unsigned needs; bool soon; };
static const Feature k_features[] = {
    {"English title logo", NEED_USA, false},
    {"English menus and tooltips", NEED_USA, false},
    {"English memory card messages", NEED_USA, false},
    {"English credits song", NEED_USA, false},
    {"Option: censored USA versions of Stages 1, 5 and 6", NEED_USA, false},
    {"Arcade attract movies", NEED_ARCADE_CD, true},
    {"Joe Chin cutscenes", NEED_ARCADE_CD, true},
    {"Stage 1 versus / PaRappa content", NEED_ARCADE_CD, true},
    {"Arcade guitar sounds and voice lines", NEED_ARCADE_ROMS, true},
};

static unsigned have_mask(const DiscSetup &s) {
    return (s.usa.empty() ? 0 : NEED_USA) | (s.arcade_cd.empty() ? 0 : NEED_ARCADE_CD) |
           (s.arcade_roms.empty() ? 0 : NEED_ARCADE_ROMS);
}

static const ImVec4 k_green(0.45f, 0.85f, 0.45f, 1), k_grey(0.6f, 0.6f, 0.6f, 1), k_red(1.0f, 0.45f, 0.4f, 1),
    k_yellow(0.95f, 0.8f, 0.35f, 1);

void discs_draw_features(const DiscSetup &s, unsigned only_needs) {
    unsigned have = have_mask(s);
    if (!ImGui::BeginTable("features", 2, ImGuiTableFlags_SizingStretchProp)) return;
    for (const Feature &f : k_features) {
        if (!(f.needs & only_needs)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool on = (f.needs & have) == f.needs;
        if (on) ImGui::TextUnformatted(f.name);
        else ImGui::TextColored(k_grey, "%s", f.name);
        ImGui::TableNextColumn();
        if (on && !f.soon) ImGui::TextColored(k_green, "On");
        else if (on) ImGui::TextColored(k_yellow, "Coming soon");
        else {
            const char *need = f.needs & NEED_USA ? "the USA disc" : f.needs & NEED_ARCADE_CD ? "the UJL NOW! CD" : "the UJL NOW! ROM set";
            ImGui::TextColored(k_grey, "Needs %s%s", need, f.soon ? " (coming soon)" : "");
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- wizard
enum { SLOT_JP, SLOT_USA, SLOT_ARCADE_CD, SLOT_ARCADE_ROMS, SLOT_GENERAL, SLOT_COUNT };
enum { PAGE_WELCOME, PAGE_JP, PAGE_OPTIONAL, PAGE_SUMMARY };

static struct Wizard {
    bool active = false, first_run = false;
    int page = 0;
    DiscSetup s, start;
    std::string msg[SLOT_COUNT];
    bool msg_err[SLOT_COUNT] = {};
} W;

// File dialogs may answer on another thread: picks are queued under a lock and handled in wizard_draw.
// (iOS lets several files be picked at once, e.g. both discs.)
struct Pick { int slot; std::string path; };
static std::vector<Pick> g_picks;
static SDL_Mutex *g_pick_mtx;
static void SDLCALL on_wizard_pick(void *ud, const char *const *files, int) {
    if (!files || !files[0]) return;
    SDL_LockMutex(g_pick_mtx);
    std::vector<Pick> cues;   // a .cue after the images it names, so its data file was already copied
    for (int i = 0; files[i]; i++) {
        Pick pk{(int)(intptr_t)ud, files[i]};
        std::string e = ext_of(pk.path);
        if (e == ".cue" || e == ".ccd") cues.push_back(pk);
        else g_picks.push_back(pk);
    }
    g_picks.insert(g_picks.end(), cues.begin(), cues.end());
    SDL_UnlockMutex(g_pick_mtx);
}

static void set_msg(int slot, const std::string &m, bool err) {
    W.msg[slot] = m;
    W.msg_err[slot] = err;
}

static const char *kind_text(DiscKind k) {
    switch (k) {
    case DISC_JP: return "the Japanese disc (SCPS-18011)";
    case DISC_USA: return "the USA disc (SCUS-94448)";
    case DISC_ARCADE_CD: return "the UJL NOW! arcade CD";
    default: return "";
    }
}

// Put a picked or dropped file where it belongs, whatever slot it was picked for. slot = where errors are shown.
static void wizard_assign(int slot, const std::string &path) {
    std::string e = ext_of(path);
    if (slot == SLOT_ARCADE_ROMS || is_dir(path) || e == ".zip") {
        RomCheck rc = romset_check(path);
        fprintf(stderr, "setup: %s -> ROM set %s: %s\n", path.c_str(), rc.ok ? "ok" : "rejected", rc.message.c_str());
        if (rc.ok) {
            W.s.arcade_roms = rc.path;
            set_msg(SLOT_ARCADE_ROMS, rc.message, false);
            if (slot != SLOT_ARCADE_ROMS) set_msg(slot, "That's the UJL NOW! ROM set, so it was added there.", false);
        } else
            set_msg(slot, rc.message, true);
        return;
    }
    DiscInfo d = disc_identify(path);
    if (d.kind == DISC_UNREADABLE && is_file(path)) {   // maybe one ROM file of the ROM set's folder
        RomCheck rc = romset_check_dir(dir_of(path));
        if (rc.message.rfind("That isn't", 0) != 0) {
            fprintf(stderr, "setup: %s -> ROM set folder %s: %s\n", path.c_str(), rc.ok ? "ok" : "rejected", rc.message.c_str());
            if (rc.ok) {
                W.s.arcade_roms = rc.path;
                set_msg(SLOT_ARCADE_ROMS, rc.message, false);
                if (slot != SLOT_ARCADE_ROMS) set_msg(slot, "That's from the UJL NOW! ROM set, so the set was added.", false);
            } else
                set_msg(slot, rc.message, true);
            return;
        }
    }
    static const char *kind_names[] = {"unreadable", "japan", "usa", "usa (other)", "europe", "prototype", "ujl now cd",
                                       "other ps1", "unknown"};
    fprintf(stderr, "setup: %s -> %s (data %s, boot %s)\n", path.c_str(), kind_names[d.kind], d.path.c_str(),
            d.boot.empty() ? "-" : d.boot.c_str());
    int target = -1;
    switch (d.kind) {
    case DISC_JP: W.s.jp = d.path; target = SLOT_JP; break;
    case DISC_USA: W.s.usa = d.path; target = SLOT_USA; break;
    case DISC_ARCADE_CD: W.s.arcade_cd = d.path; target = SLOT_ARCADE_CD; break;
    case DISC_USA_OTHER:
        set_msg(slot, "That's a USA Um Jammer Lammy disc, but its program doesn't match the known release "
                      "(SCUS-94448). The English assets are only taken from that release.", true);
        return;
    case DISC_EUROPE:
        set_msg(slot, "That's the Europe release (SCES-01753). It's recognized, but nothing uses it yet.", true);
        return;
    case DISC_PROTO:
        set_msg(slot, "That's an early Japanese build (the January 1999 prototype boots the same way as retail but "
                      "runs a different program). The port needs the retail Japanese disc (SCPS-18011).", true);
        return;
    case DISC_PS1_OTHER:
        set_msg(slot, d.boot.empty() ? "That's a PlayStation disc, but not one the port uses."
                                     : "That's a PlayStation disc, but not Um Jammer Lammy (it boots " + d.boot + ").", true);
        return;
    case DISC_UNKNOWN:
        set_msg(slot, "That's a CD image, but not one the port uses.", true);
        return;
    default:
        set_msg(slot, "That file isn't a disc image the port can read. Use a raw .bin or .img image "
                      "(2352-byte sectors), or the .cue/.ccd next to it.", true);
        return;
    }
    set_msg(target, "Found " + std::string(kind_text(d.kind)) + ".", false);
    if (slot != target && slot != SLOT_GENERAL)
        set_msg(slot, "That's " + std::string(kind_text(d.kind)) + ", so it was added there.", false);
    else if (slot == SLOT_GENERAL)
        set_msg(SLOT_GENERAL, "Added " + std::string(kind_text(d.kind)) + ".", false);
}

void wizard_open(bool first_run, const DiscSetup &start) {
    W = Wizard();
    W.active = true;
    W.first_run = first_run;
    W.page = first_run ? PAGE_WELCOME : PAGE_JP;
    W.s = W.start = start;
}

bool wizard_active() { return W.active; }
bool wizard_finish() {
    if (!W.active || W.s.jp.empty()) return false;
    discs_to_ini(W.s);
    ini_set("setup_done", "1");
    W.active = false;
    return true;
}
void wizard_set_page(int page) { W.page = std::max(0, std::min(page, (int)PAGE_SUMMARY)); }
const DiscSetup &wizard_result() { return W.s; }

void wizard_drop_file(const char *path) {
    if (!W.active || !path) return;
    wizard_assign(W.page == PAGE_JP ? SLOT_JP : SLOT_GENERAL, path);
}

static void open_dialog(SDL_Window *win, int slot) {
    static const SDL_DialogFileFilter discs[] = {{"PlayStation disc image", "bin;img;iso;cue;ccd"}, {"All files", "*"}};
    static const SDL_DialogFileFilter roms[] = {
        {"UJL NOW! ROM set (zip, or any ROM file in the folder)", "zip;ic1;ic2;ic9;ic10;ic11;ic12;10c;11s;2l;2p;chd"},
        {"All files", "*"}};
    std::string start_dir = g_base_dir;
    const std::string &cur = slot == SLOT_JP ? W.s.jp : slot == SLOT_USA ? W.s.usa : slot == SLOT_ARCADE_CD ? W.s.arcade_cd : W.s.arcade_roms;
    if (!cur.empty()) start_dir = dir_of(cur);
    else if (!W.s.jp.empty()) start_dir = dir_of(W.s.jp);
    if (!g_pick_mtx) g_pick_mtx = SDL_CreateMutex();
    if (slot == SLOT_ARCADE_ROMS) platform_open_file_dialog(on_wizard_pick, (void *)(intptr_t)slot, win, roms, 2, start_dir.c_str(), false);
    else platform_open_file_dialog(on_wizard_pick, (void *)(intptr_t)slot, win, discs, 2, start_dir.c_str(), UJL_IOS != 0);
}

static void draw_msg(int slot) {
    if (W.msg[slot].empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, W.msg_err[slot] ? k_red : k_green);
    ImGui::TextWrapped("%s", W.msg[slot].c_str());
    ImGui::PopStyleColor();
}

static void draw_path_status(const std::string &path) {
    if (path.empty()) { ImGui::TextColored(k_grey, "Not added"); return; }
    ImGui::TextColored(k_green, "Added");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", base_of(path.back() == '/' || path.back() == '\\' ? path.substr(0, path.size() - 1) : path).c_str());
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(path.c_str());
        ImGui::EndTooltip();
    }
}

static void draw_slot(SDL_Window *win, int slot, const char *title, const char *about, unsigned needs, std::string &path) {
    ImGui::PushID(slot);
    ImGui::SeparatorText(title);
    ImGui::TextWrapped("%s", about);
    draw_path_status(path);
    if (ImGui::Button(path.empty() ? "Choose..." : "Change...")) open_dialog(win, slot);
    if (slot == SLOT_ARCADE_ROMS && platform_can_pick_folders()) {
        ImGui::SameLine();
        if (ImGui::Button("Choose folder...")) {
            std::string start_dir = path.empty() ? g_base_dir : path;
            SDL_ShowOpenFolderDialog(on_wizard_pick, (void *)(intptr_t)slot, win, start_dir.c_str(), false);
        }
    }
    if (!path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) { path.clear(); set_msg(slot, "", false); }
    }
    draw_msg(slot);
    ImGui::Spacing();
    discs_draw_features(W.s, needs);
    ImGui::Spacing();
    ImGui::PopID();
}

static bool big_button(const char *label, bool enabled = true) {
    if (!enabled) ImGui::BeginDisabled();
    bool r = ImGui::Button(label, ImVec2(ImGui::GetFontSize() * 9, 0));
    if (!enabled) ImGui::EndDisabled();
    return r;
}

WizardResult wizard_draw(SDL_Window *win) {
    if (!W.active) return WIZ_CLOSED;
    if (g_pick_mtx) {
        SDL_LockMutex(g_pick_mtx);
        std::vector<Pick> picks;
        picks.swap(g_picks);
        SDL_UnlockMutex(g_pick_mtx);
        for (const Pick &pk : picks) wizard_assign(pk.slot, pk.path);
    }

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float fs = ImGui::GetFontSize();
    // within the safe area (a phone's camera cutout and rounded corners); the whole window elsewhere
    SDL_Rect sa{0, 0, (int)ds.x, (int)ds.y};
    if (!SDL_GetWindowSafeArea(win, &sa) || sa.w <= 0 || sa.h <= 0) sa = SDL_Rect{0, 0, (int)ds.x, (int)ds.y};
    float margin = UJL_IOS ? 12.0f : 48.0f;
    ImVec2 size(std::min(fs * 62, (float)sa.w - margin), std::min(fs * 58, (float)sa.h - margin));
    ImGuiCond cond = W.first_run ? ImGuiCond_Always : ImGuiCond_Appearing;
    ImGui::SetNextWindowPos(ImVec2(sa.x + sa.w * 0.5f, sa.y + sa.h * 0.5f), cond, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, cond);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
    if (W.first_run) flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    bool open = true;
    ImGui::Begin(W.first_run ? "Ultimate Um Jammer Lammy setup###setup" : "Manage discs###setup", W.first_run ? nullptr : &open, flags);
    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
    ImGui::BeginChild("page", ImVec2(0, -footer));
    const int first_page = W.first_run ? PAGE_WELCOME : PAGE_JP;
    char step[48];
    SDL_snprintf(step, sizeof step, "Step %d of 3", W.page);
    switch (W.page) {
    case PAGE_WELCOME:
        ImGui::SeparatorText("Welcome to Ultimate Um Jammer Lammy");
        ImGui::TextWrapped("This port runs the original game from your own disc images. No game data comes with it, "
                           "so the next steps ask where your discs are. They're read every time the game starts, so "
                           "keep the image files where they are.");
        ImGui::Spacing();
        ImGui::TextColored(k_yellow, "Required");
        ImGui::BulletText("Um Jammer Lammy, Japan (SCPS-18011). The game itself runs from this disc.");
        ImGui::TextColored(k_yellow, "Optional");
        ImGui::BulletText("Um Jammer Lammy, USA (SCUS-94448): English logo, menus, messages and credits song.");
        ImGui::BulletText("UJL NOW! arcade CD and ROM set: arcade extras (in progress).");
        ImGui::Spacing();
#if UJL_IOS
        ImGui::TextWrapped("Disc images are raw .bin or .img files. Copy them into this app's folder (Files app: "
                           "On My iPhone/iPad > Ultimate UJL, or Finder's file sharing with the device connected), "
                           "or choose them on the next pages, which copies them there. All of this can be changed "
                           "later in the menu (top left) > Discs.");
#else
        ImGui::TextWrapped("Disc images are raw .bin or .img files; picking the .cue or .ccd next to one works too. "
                           "You can also drag files onto this window. All of this can be changed later in "
                           "Settings (F1) > Discs.");
#endif
        break;
    case PAGE_JP:
        ImGui::TextDisabled("%s", step);
        ImGui::SeparatorText("Japanese disc (required)");
        ImGui::TextWrapped("Ultimate Um Jammer Lammy is built from the Japanese release, Um Jammer Lammy (SCPS-18011). "
                           "Choose your image of that disc.");
        ImGui::Spacing();
        ImGui::PushID(SLOT_JP);
        draw_path_status(W.s.jp);
        if (ImGui::Button(W.s.jp.empty() ? "Choose disc image..." : "Change...")) open_dialog(win, SLOT_JP);
        draw_msg(SLOT_JP);
        ImGui::PopID();
#if UJL_IOS
        if (ImGui::Button("Look again")) {   // after copying images in with the Files app while this is open
            int found = 0;
            for (const DiscInfo &d : disc_search(disc_search_dirs())) {
                found++;
                if (d.kind == DISC_JP) W.s.jp = d.path;
                else if (d.kind == DISC_USA && W.s.usa.empty()) W.s.usa = d.path;
                else if (d.kind == DISC_ARCADE_CD && W.s.arcade_cd.empty()) W.s.arcade_cd = d.path;
            }
            set_msg(SLOT_JP, W.s.jp.empty() ? "The Japanese disc isn't in the app's folder." :
                             "Found " + std::to_string(found) + " disc image(s) in the app's folder.", W.s.jp.empty());
        }
        if (W.s.jp.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Not in the app's folder yet. Copy it there with the Files app, or choose it.");
        }
#else
        if (W.s.jp.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Not found next to the game. Drag the image onto this window, or choose it.");
        }
#endif
        break;
    case PAGE_OPTIONAL:
        ImGui::TextDisabled("%s", step);
        ImGui::TextWrapped("These discs are optional. Each one adds the features listed under it; skip any you "
                           "don't have.");
        draw_slot(win, SLOT_USA, "Um Jammer Lammy, USA (SCUS-94448)",
                  "The English title logo, menu text and tooltips, memory card messages and credits song are taken "
                  "from this disc. The game still runs from the Japanese disc.", NEED_USA, W.s.usa);
        draw_slot(win, SLOT_ARCADE_CD, "UJL NOW! arcade CD",
                  "The CD from the arcade version (the .img, .ccd or .cue of a CloneCD image).", NEED_ARCADE_CD,
                  W.s.arcade_cd);
        draw_slot(win, SLOT_ARCADE_ROMS, "UJL NOW! ROM set",
                  "The arcade board's ROMs (MAME set \"ujlnow\"): its zip, or the folder with the ul1*.* files.",
                  NEED_ARCADE_ROMS, W.s.arcade_roms);
        draw_msg(SLOT_GENERAL);
        break;
    case PAGE_SUMMARY:
        ImGui::TextDisabled("%s", step);
        ImGui::SeparatorText(W.first_run ? "All set" : "Summary");
        ImGui::TextColored(k_green, "Japanese disc");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", base_of(W.s.jp).c_str());
        ImGui::Spacing();
        discs_draw_features(W.s);
        ImGui::Spacing();
        if (!W.first_run && (W.s.jp != W.start.jp || W.s.usa != W.start.usa || W.s.arcade_cd != W.start.arcade_cd ||
                             W.s.arcade_roms != W.start.arcade_roms))
            ImGui::TextColored(k_yellow, "Restart the game to use the new discs.");
        break;
    }
    touchpad_drag_scroll();
    ImGui::EndChild();
    ImGui::Separator();
    WizardResult res = WIZ_RUNNING;
    if (W.page > first_page) {
        if (big_button("Back")) W.page--;
    } else if (W.first_run) {
        if (big_button("Quit")) res = WIZ_QUIT;
    } else if (big_button("Cancel")) open = false;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - fs * 9);
    if (W.page < PAGE_SUMMARY) {
        if (big_button("Next", W.page != PAGE_JP || !W.s.jp.empty())) W.page++;
    } else if (big_button(W.first_run ? "Start game" : "Save", !W.s.jp.empty())) {
        wizard_finish();
        res = WIZ_DONE;
    }
    ImGui::End();
    if (!open) { W.active = false; return WIZ_CLOSED; }
    if (res == WIZ_QUIT) W.active = false;
    return res;
}
