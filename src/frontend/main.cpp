// Ultimate Um Jammer Lammy (UltimateUJL) frontend: SDL3 window, input, ImGui overlay. The recompiled game runs on its own thread.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cfloat>
#include <string>
#include <algorithm>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "gl_funcs.h"
#include "app_icon.h"
#include "hw_renderer.h"
#include "discs.h"

extern "C" {
#include "psx.h"
#include "gpu.h"
bool rt_boot(const char *disc_path);
bool rt_probe_disc(const char *path);
bool aux_disc_open(const char *path);
bool disc_image_boots(const char *path, const char *exe_name);
void rt_run(void);
void gpu_soft_install(void);
void memcard_init(int slot, const char *path);
void memcard_flush(void);
void memcard_request_import(const char *path, int mode);
const char *memcard_import_result(void);
int memcard_list(int slot, char names[][21], int *blocks, int max);
int memcard_free_blocks(int slot);
const char *memcard_path(int slot);
extern int g_unlock_all;
extern int g_uncensored;                  // censor.c: 1 = JP (uncensored) content, 0 = the USA disc's Stages 1/5/6
int censor_available(void);               // censor.c: the USA disc is there to switch to
extern int g_autoplay;                    // autoplay.c: 0 off, 1 Autoplay, 2 Auto-COOL, 3 Auto-FEVER
int autoplay_active(void);                // autoplay.c: playing the current song (watermark)
extern "C" SDL_Thread *rt_create_thread(SDL_ThreadFunction fn, const char *name, void *data); // bios.c: big-stack thread
extern "C" int g_cd_fast_load;            // cdrom.c: fast data loading + loading turbo (setting "fast_load")
extern "C" int rt_load_turbo_active(void); // cpu.c: the emulation is fast-forwarding through a load
const char *rt_current_overlay_name(void);
#include "gpu_rec.h"
}

// ---------------------------------------------------------------- shared state (game <-> UI)
static SDL_Mutex *g_frame_mtx;
static std::vector<uint32_t> g_frame_rgba;       // display area, RGBA8
static int g_frame_w = 320, g_frame_h = 240;
static std::vector<uint32_t> g_vram_rgba(1024 * 512);
static std::atomic<bool> g_frame_new{false};
static std::atomic<uint16_t> g_input_buttons{0xFFFF};
static std::atomic<int> g_vblanks{0};
static bool g_show_vram;

// ---------------------------------------------------------------- dev automation (UJL_SCRIPT)
// UJL_SCRIPT=file with lines "<seconds> <button>|shot|overlay|quit" drives the game for renderer tests: buttons are
// held for 6 vblanks, "shot" saves the presented window to shots/hw_vNNNNN.bmp (vblank number) next to the exe. While a script
// runs, UJL_SCRIPT_CARD picks the memory card file (default script_card.mcd, never the player's card),
// UJL_SCRIPT_ASPECT the aspect index (not saved) and UJL_SCRIPT_UNLOCK=1 unlocks all stages. "overlay" toggles the
// settings window (F1).
struct ScriptEv { int vblank; int kind; uint16_t bit; }; // kind 0 button, 1 shot, 2 quit, 3 settings overlay
static std::vector<ScriptEv> g_script;
static std::atomic<int> g_shot_req{-1};
static std::atomic<bool> g_script_quit{false};
static std::atomic<int> g_script_overlay{0};   // pending settings-overlay toggles
static uint16_t script_btn(const char *n) {
    static const char *names[] = {"select","l3","r3","start","up","right","down","left","l2","r2","l1","r1","triangle","circle","cross","square"};
    for (int i = 0; i < 16; i++) if (!strcmp(n, names[i])) return (uint16_t)(1u << i);
    return 0;
}
static bool script_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[128], name[32];
    float t;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%f %31s", &t, name) == 2) {
            int vb = (int)(t * 60);
            int n = 0;
            if (!strcmp(name, "shot")) g_script.push_back({vb, 1, 0});
            else if (!strcmp(name, "burst") && sscanf(line, "%*f %*s %d", &n) == 1) /* a shot every vblank for n vblanks */
                for (int i = 0; i < n; i++) g_script.push_back({vb + i, 1, 0});
            else if (!strcmp(name, "quit")) g_script.push_back({vb, 2, 0});
            else if (!strcmp(name, "overlay")) g_script.push_back({vb, 3, 0});
            else if (uint16_t b = script_btn(name)) g_script.push_back({vb, 0, b});
        }
    fclose(f);
    fprintf(stderr, "script: %zu events from %s\n", g_script.size(), path);
    return true;
}
static uint16_t script_vblank(int vb) {
    uint16_t held = 0;
    for (const ScriptEv &e : g_script) {
        if (e.kind == 0 && vb >= e.vblank && vb < e.vblank + 6) held |= e.bit;
        else if (e.kind == 1 && vb == e.vblank) g_shot_req = vb;
        else if (e.kind == 2 && vb == e.vblank) g_script_quit = true;
        else if (e.kind == 3 && vb == e.vblank) g_script_overlay++;
    }
    return held;
}

struct Settings {
    bool show_overlay = false;
    bool show_fps = true;
    int  scale_filter = 0; // 0 nearest, 1 linear (software renderer output)
    int  renderer = 1;     // 0 software (original), 1 hardware (high resolution)
    int  res_scale = 0;    // 0 auto (match window), else 1..8
    int  textures = 1;     // 0 original (sharp pixels), 1 smooth (vector-like)
    int  aa = 2;           // MSAA: 0 off, 1 2x, 2 4x, 3 8x
    bool ui_sharp = true;  // unscaled 2D UI keeps nearest-neighbour pixels
    bool subpixel = true;  // 3D vertices at sub-pixel precision (no jitter)
    bool thin_fix = true;  // widen sub-pixel-thin 3D triangles to a native pixel
    int  aspect = 0;       // 0 4:3, 1 16:9, 2 16:10, 3 21:9, 4 32:9, 5 auto (window)
    bool stretch_ui = false;
};
static const float k_aspects[] = {4.0f / 3.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 9.0f, 0.0f};
static Settings g_set;

static inline uint32_t c15_to_rgba(uint16_t c) {
    uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    r |= r >> 5; g |= g >> 5; b |= b >> 5;
    return r | g << 8 | b << 16 | 0xFF000000u;
}

// Called on the game thread at every emulated vblank.
extern "C" void host_vblank(void) {
    int vb = ++g_vblanks;
    gpu_rec_vblank();
    g_pad_buttons[0] = g_input_buttons.load();
    if (!g_script.empty()) g_pad_buttons[0] &= (uint16_t)~script_vblank(vb);
    const GpuDisplay &d = g_gpu_display;
    int w = d.hres, h = d.vres;
    if (w <= 0 || h <= 0) return;
    SDL_LockMutex(g_frame_mtx);
    g_frame_w = w;
    g_frame_h = h;
    g_frame_rgba.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int vy = (d.disp_y + y) & 511;
        uint32_t *dst = &g_frame_rgba[(size_t)y * w];
        if (d.depth24) {
            const uint8_t *row = (const uint8_t *)&g_vram[vy * 1024];
            for (int x = 0; x < w; x++) {
                int bx = d.disp_x * 2 + x * 3;
                uint32_t r = row[(bx) & 2047], g = row[(bx + 1) & 2047], b = row[(bx + 2) & 2047];
                dst[x] = r | g << 8 | b << 16 | 0xFF000000u;
            }
        } else {
            for (int x = 0; x < w; x++) dst[x] = c15_to_rgba(g_vram[vy * 1024 + ((d.disp_x + x) & 1023)]);
        }
    }
    if (g_show_vram)
        for (int i = 0; i < 1024 * 512; i++) g_vram_rgba[i] = c15_to_rgba(g_vram[i]);
    if (!d.enabled) std::fill(g_frame_rgba.begin(), g_frame_rgba.end(), 0xFF000000u);
    SDL_UnlockMutex(g_frame_mtx);
    g_frame_new = true;
}

// ---------------------------------------------------------------- audio
static SDL_AudioStream *g_audio;
static std::atomic<int> g_volume{100};

/* Output pacing. The SPU makes samples on the emulated clock, which follows the host clock, but arrives in bursts
   when the game thread was held up (it waits for a slow renderer in gpu_rec_vblank, then catches up). Aim for
   ~60 ms queued:
   - small, slow drift (host clock vs the sound card's) is absorbed by nudging the playback speed, at most +-0.5%
     (about 9 cents, inaudible). This used to allow +-1.5%, and after every stall the queue sat above target with
     the speed pinned at +1.5%: the music played a quarter semitone sharp, most of all in slow-rendering menus.
   - a burst is trimmed instead: above 120 ms queued, one 64-sample chunk in eight is left out, crossfaded over
     32 samples so it doesn't click; above 300 ms every chunk is (something stalled for good).
   The last XF frames of each chunk are held back so a dropped chunk can be crossfaded into the next one. */
extern "C" void host_audio_push(const int16_t *lr, int frames) {
    enum { XF = 32, MAXF = 256 };
    static int16_t hold[XF * 2];
    static bool have_hold, xfade;
    static int since_drop;
    if (!g_audio) return;
    if (rt_load_turbo_active()) return; /* fast-forwarding a load: nothing to hear */
    if (frames > MAXF) frames = MAXF;
    int queued = SDL_GetAudioStreamQueued(g_audio) / 4;
    const int target = 44100 * 60 / 1000;
    float ratio = 1.0f + (float)(queued - target) / (float)target * 0.005f;
    ratio = std::max(0.995f, std::min(1.005f, ratio));
    bool drop = have_hold && frames > XF &&
                (queued > 44100 * 300 / 1000 || (queued > 44100 * 120 / 1000 && since_drop >= 8));
    {   /* UJL_AUDIO_STATS: samples made vs host time, queue depth, playback ratio and trimmed samples, once a second */
        static int st = -1; static uint64_t t0; static long made, dropped; static int qmin = 1 << 30, qmax;
        static double rsum; static long rn;
        if (st < 0) st = getenv("UJL_AUDIO_STATS") != NULL;
        if (st) {
            uint64_t now = SDL_GetTicksNS();
            if (!t0) t0 = now;
            made += frames; if (drop) dropped += frames;
            qmin = std::min(qmin, queued); qmax = std::max(qmax, queued); rsum += ratio; rn++;
            if (now - t0 >= 1000000000ull) {
                fprintf(stderr, "audio: %.0f samples/s made, dropped %ld, queued %d..%d, ratio avg %.4f\n",
                        made * 1e9 / (double)(now - t0), dropped, qmin, qmax, rsum / (double)rn);
                t0 = now; made = dropped = 0; qmin = 1 << 30; qmax = 0; rsum = 0; rn = 0;
            }
        }
    }
    if (drop) { xfade = true; since_drop = 0; return; }   // keep the held tail; the next chunk fades in over it
    since_drop++;
    SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
    int16_t buf[MAXF * 2];
    int vol = g_volume.load();
    for (int i = 0; i < frames * 2; i++) buf[i] = vol == 100 ? lr[i] : (int16_t)(lr[i] * vol / 100);
    if (frames <= XF) {   // too short to hold anything back (CHUNK is 64): pass through
        if (have_hold) { SDL_PutAudioStreamData(g_audio, hold, XF * 4); have_hold = false; }
        SDL_PutAudioStreamData(g_audio, buf, frames * 4);
        return;
    }
    if (have_hold) {
        if (xfade) {   // held tail of the last kept chunk fades out while this chunk fades in
            for (int i = 0; i < XF; i++) {
                float a = (i + 0.5f) / XF;
                for (int c = 0; c < 2; c++) buf[i * 2 + c] = (int16_t)std::lround(hold[i * 2 + c] * (1.0f - a) + buf[i * 2 + c] * a);
            }
            xfade = false;
        } else {
            SDL_PutAudioStreamData(g_audio, hold, XF * 4);
        }
    }
    SDL_PutAudioStreamData(g_audio, buf, (frames - XF) * 4);
    memcpy(hold, buf + (frames - XF) * 2, XF * 4);
    have_hold = true;
}

// ---------------------------------------------------------------- memory card import (file dialog)
static std::atomic<int> g_card_pick{0};       // 1 = a file was picked
static std::string g_card_pick_path;
static int g_card_import_mode = 0;            // 0 copy this game's saves, 1 replace the whole card
static void SDLCALL card_pick_cb(void *, const char *const *files, int) {
    if (files && files[0]) { g_card_pick_path = files[0]; g_card_pick = 1; }
}

// ---------------------------------------------------------------- discs (settings "Discs" section)
static std::string g_jp_disc_path, g_usa_disc_path;   // what this session booted / opened ("" = none)
static DiscSetup g_discs;                              // what ujl.ini lists (the wizard may change it mid-session)

// ---------------------------------------------------------------- input
// PS1 digital pad bits (active low)
enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1,
       B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };

static std::vector<SDL_Gamepad *> g_pads;

static void open_pad(SDL_JoystickID id) {
    for (SDL_Gamepad *g : g_pads)
        if (SDL_GetGamepadID(g) == id) return;
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { fprintf(stderr, "gamepad %u: open failed: %s\n", (unsigned)id, SDL_GetError()); return; }
    g_pads.push_back(g);
    fprintf(stderr, "gamepad connected: %s\n", SDL_GetGamepadName(g));
}

static void close_pad(SDL_JoystickID id) {
    for (size_t i = 0; i < g_pads.size(); i++)
        if (SDL_GetGamepadID(g_pads[i]) == id) {
            fprintf(stderr, "gamepad disconnected: %s\n", SDL_GetGamepadName(g_pads[i]));
            SDL_CloseGamepad(g_pads[i]);
            g_pads.erase(g_pads.begin() + (long)i);
            return;
        }
}

static uint16_t pad_held(SDL_Gamepad *pad);

static uint16_t poll_buttons(bool keyboard) {
    uint16_t held = 0;
    for (SDL_Gamepad *g : g_pads) held |= pad_held(g);
    if (!keyboard) return (uint16_t)~held;
    const bool *k = SDL_GetKeyboardState(nullptr);
    auto set = [&](int bit, bool on) { if (on) held |= (uint16_t)(1u << bit); };
    set(B_UP, k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]);
    set(B_DOWN, k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]);
    set(B_LEFT, k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]);
    set(B_RIGHT, k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]);
    set(B_CROSS, k[SDL_SCANCODE_K] || k[SDL_SCANCODE_X]);
    set(B_CIRCLE, k[SDL_SCANCODE_L] || k[SDL_SCANCODE_C]);
    set(B_SQUARE, k[SDL_SCANCODE_J] || k[SDL_SCANCODE_Z]);
    set(B_TRIANGLE, k[SDL_SCANCODE_I] || k[SDL_SCANCODE_V]);
    set(B_L1, k[SDL_SCANCODE_Q]);
    set(B_R1, k[SDL_SCANCODE_E]);
    set(B_L2, k[SDL_SCANCODE_1]);
    set(B_R2, k[SDL_SCANCODE_3]);
    set(B_START, k[SDL_SCANCODE_RETURN]);
    set(B_SELECT, k[SDL_SCANCODE_BACKSPACE]);
    return (uint16_t)~held;
}

static uint16_t pad_held(SDL_Gamepad *pad) {
    uint16_t held = 0;
    auto set = [&](int bit, bool on) { if (on) held |= (uint16_t)(1u << bit); };
    set(B_CROSS, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH));
    set(B_CIRCLE, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST));
    set(B_SQUARE, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST));
    set(B_TRIANGLE, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH));
    set(B_L1, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    set(B_R1, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));
    set(B_L2, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000);
    set(B_R2, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000);
    set(B_START, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START));
    set(B_SELECT, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK));
    set(B_UP, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP));
    set(B_DOWN, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN));
    set(B_LEFT, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT));
    set(B_RIGHT, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));

    // left stick also drives the d-pad
    const int dz = 16000;
    set(B_LEFT, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) < -dz);
    set(B_RIGHT, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) > dz);
    set(B_UP, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) < -dz);
    set(B_DOWN, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) > dz);
    return held;
}

// ---------------------------------------------------------------- finding the disc
std::string g_base_dir;   // folder containing the exe

static std::string ini_path() { return g_base_dir + "ujl.ini"; }

// ujl.ini: simple key=value lines (disc path + settings)
static std::vector<std::pair<std::string, std::string>> ini_load() {
    std::vector<std::pair<std::string, std::string>> kv;
    FILE *f = fopen(ini_path().c_str(), "rb");
    if (!f) return kv;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t eq = l.find('=');
        if (eq != std::string::npos) kv.emplace_back(l.substr(0, eq), l.substr(eq + 1));
    }
    fclose(f);
    return kv;
}

std::string ini_get(const char *key, const std::string &def) {
    for (auto &p : ini_load()) if (p.first == key) return p.second;
    return def;
}

void ini_set(const char *key, const std::string &val) {
    auto kv = ini_load();
    bool found = false;
    for (auto &p : kv) if (p.first == key) { p.second = val; found = true; }
    if (!found) kv.emplace_back(key, val);
    FILE *f = fopen(ini_path().c_str(), "wb");
    if (!f) return;
    for (auto &p : kv) fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    fclose(f);
}


static bool file_exists(const std::string &p) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
}

// Look in the exe folder and up to 4 parents for a disc image that matches this build.
static std::string search_disc() {
    std::string dir = g_base_dir;
    for (int up = 0; up <= 4; up++) {
        std::string preferred = dir + "Um Jammer Lammy (JP).bin";
        if (file_exists(preferred) && rt_probe_disc(preferred.c_str())) return preferred;
        for (const char *pat : {"*.bin", "*.img", "*.iso"}) {
            int n = 0;
            char **names = SDL_GlobDirectory(dir.c_str(), pat, SDL_GLOB_CASEINSENSITIVE, &n);
            for (int i = 0; names && i < n; i++) {
                std::string cand = dir + names[i];
                if (rt_probe_disc(cand.c_str())) { SDL_free(names); return cand; }
            }
            SDL_free(names);
        }
        // go up one level ("C:/a/b/" -> "C:/a/")
        if (dir.size() < 2) break;
        size_t cut = dir.find_last_of("/\\", dir.size() - 2);
        if (cut == std::string::npos) break;
        dir = dir.substr(0, cut + 1);
    }
    return "";
}

// USA disc (asset source for the English title logo), found the same way as the JP disc
static std::string search_usa_disc(const std::string &jp_disc) {
    std::string remembered = ini_get("usa_disc");   // picked in settings
    if (!remembered.empty() && file_exists(remembered) && disc_image_boots(remembered.c_str(), "SCUS_944.48"))
        return remembered;
    std::string dir = jp_disc.substr(0, jp_disc.find_last_of("/\\") + 1);
    std::vector<std::string> dirs = {dir, g_base_dir};
    for (int up = 0; up < 4; up++) {
        std::string &d = dirs.back();
        if (d.size() < 2) break;
        size_t cut = d.find_last_of("/\\", d.size() - 2);
        if (cut == std::string::npos) break;
        dirs.push_back(d.substr(0, cut + 1));
    }
    for (const std::string &d : dirs) {
        std::string pref = d + "Um Jammer Lammy (USA).bin";
        if (file_exists(pref) && disc_image_boots(pref.c_str(), "SCUS_944.48")) return pref;
        for (const char *pat : {"*.bin", "*.img", "*.iso"}) {
            int n = 0;
            char **names = SDL_GlobDirectory(d.c_str(), pat, SDL_GLOB_CASEINSENSITIVE, &n);
            for (int i = 0; names && i < n; i++) {
                std::string cand = d + names[i];
                if (disc_image_boots(cand.c_str(), "SCUS_944.48")) { SDL_free(names); return cand; }
            }
            SDL_free(names);
        }
    }
    return "";
}

static std::string absolute_path(const std::string &p) {
    if (p.empty() || (p.size() > 1 && (p[1] == ':' || p[0] == '/' || p[0] == '\\'))) return p;
    char *cwd = SDL_GetCurrentDirectory();
    std::string r = cwd ? std::string(cwd) + p : p;
    SDL_free(cwd);
    return r;
}

// Save the current GL back buffer to shots/<name> next to the exe.
static void save_shot(int fbw, int fbh, const char *name) {
    std::vector<uint8_t> px((size_t)fbw * fbh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    SDL_Surface *sf = SDL_CreateSurface(fbw, fbh, SDL_PIXELFORMAT_ABGR8888);
    for (int y = 0; y < fbh; y++) memcpy((uint8_t *)sf->pixels + y * sf->pitch, &px[(size_t)(fbh - 1 - y) * fbw * 4], (size_t)fbw * 4);
    SDL_CreateDirectory((g_base_dir + "shots").c_str());
    SDL_SaveBMP(sf, (g_base_dir + "shots/" + name).c_str());
    SDL_DestroySurface(sf);
}

// First-run setup (before the game boots): the wizard in its own frame loop. False = the player quit.
// Dev: UJL_SETUP_DROP="a;b;..." drops those files on the wizard at start; UJL_SETUP_SHOTS=1 saves each page to
// shots/setup_pN.bmp and quits without saving anything (UJL_SETUP_SHOTS=save: saves and starts the game instead).
static bool run_setup_wizard(SDL_Window *win, const DiscSetup &start) {
    ImGuiIO &io = ImGui::GetIO();
    ImGuiConfigFlags saved = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    wizard_open(true, start);
    if (const char *drops = getenv("UJL_SETUP_DROP")) {
        std::string all = drops;
        for (size_t a = 0; a < all.size();) {
            size_t b = all.find(';', a);
            if (b == std::string::npos) b = all.size();
            if (b > a) wizard_drop_file(all.substr(a, b - a).c_str());
            a = b + 1;
        }
    }
    const bool shots = getenv("UJL_SETUP_SHOTS") != nullptr;
    int frame = 0;
    WizardResult r = WIZ_RUNNING;
    while (r == WIZ_RUNNING) {
        if (shots) wizard_set_page(frame / 20);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) r = WIZ_QUIT;
            if (e.type == SDL_EVENT_DROP_FILE) wizard_drop_file(e.drop.data);
            if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11) {
                bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(win, !fs);
            }
        }
        if (r != WIZ_RUNNING) break;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        r = wizard_draw(win);
        ImGui::Render();
        int fbw, fbh;
        SDL_GetWindowSizeInPixels(win, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.06f, 0.06f, 0.08f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (shots && frame % 20 == 19) {
            char fn[32];
            SDL_snprintf(fn, sizeof fn, "setup_p%d.bmp", frame / 20);
            save_shot(fbw, fbh, fn);
            if (frame / 20 >= 3) r = (!strcmp(getenv("UJL_SETUP_SHOTS"), "save") && wizard_finish()) ? WIZ_DONE : WIZ_QUIT;
        }
        frame++;
        SDL_GL_SwapWindow(win);
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) SDL_Delay(16);
    }
    io.ConfigFlags = saved;
    return r == WIZ_DONE;
}

// ---------------------------------------------------------------- textures
static GLuint make_tex(int w, int h) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return t;
}

static int SDLCALL game_thread(void *) {
    rt_run();
    return 0;
}

int main(int argc, char **argv) {
    std::string disc_arg;
    bool start_fullscreen = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_log_level = 2;
        else if (!strcmp(argv[i], "--fullscreen")) start_fullscreen = true;
        else disc_arg = argv[i];
    }
    {
        const char *bp = SDL_GetBasePath();
        g_base_dir = bp ? bp : "";
        /* log to a file next to the exe so problems can be diagnosed when double-clicked */
        if (!getenv("UJL_LOG_STDERR")) freopen((g_base_dir + "ujl.log").c_str(), "w", stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
        g_confirm_cross = ini_get("confirm_cross", "0") == "1";
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // GL 4.1 core when available (sample masks: anti-aliased sprite edges), otherwise 3.3 core
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    // UJL_SCRIPT_HIDDEN=WxH (dev, with UJL_SCRIPT): hidden window, drawing into an offscreen buffer of that size, so
    // renderer tests don't put a window on the desktop. Shots read that buffer.
    int hid_w = 0, hid_h = 0;
    if (const char *hs = getenv("UJL_SCRIPT_HIDDEN"))
        if (sscanf(hs, "%dx%d", &hid_w, &hid_h) != 2 || hid_w < 64 || hid_h < 48) hid_w = hid_h = 0;
    SDL_Window *win = SDL_CreateWindow("Ultimate Um Jammer Lammy", hid_w ? hid_w : 1280, hid_h ? hid_h : 960,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (hid_w ? SDL_WINDOW_HIDDEN : 0));
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    if (SDL_Surface *ic = SDL_CreateSurfaceFrom(k_app_icon_w, k_app_icon_h, SDL_PIXELFORMAT_ABGR8888, (void *)k_app_icon_rgba, k_app_icon_w * 4)) {
        SDL_SetWindowIcon(win, ic); // window/taskbar icon on every platform (Windows also gets it from the exe resource)
        SDL_DestroySurface(ic);
    }
    if (start_fullscreen) SDL_SetWindowFullscreen(win, true);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        gl = SDL_GL_CreateContext(win);
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);
    // Launched from a shortcut/launcher without focus, or under another window: make sure it is shown and on top.
    if (!hid_w) {
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);
    }
    bool win_hidden = hid_w != 0;   // occluded or minimized: don't block on vsync for a window nobody can see

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = "ujl_imgui.ini";
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    g_frame_mtx = SDL_CreateMutex();
    {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 44100};
        g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
        else fprintf(stderr, "audio: %s\n", SDL_GetError());
    }
    // Discs. The setup wizard runs on the first start (no setup_done in ujl.ini), when the Japanese disc can't be
    // found, or with UJL_SETUP=1. Scripted runs (UJL_SCRIPT) never show it. Until setup has been done once, discs
    // are also searched for next to the game (older ujl.ini files, scripts); after it, only what was imported counts.
    const bool scripted = getenv("UJL_SCRIPT") != nullptr;
    bool setup_done = ini_get("setup_done") == "1";
    g_discs = discs_from_ini();
    std::string disc;
    disc_arg = absolute_path(disc_arg);
    if (!disc_arg.empty() && rt_probe_disc(disc_arg.c_str())) disc = disc_arg;
    if (disc.empty() && !g_discs.jp.empty() && rt_probe_disc(g_discs.jp.c_str())) disc = g_discs.jp;
    if (disc.empty() && (!setup_done || scripted)) disc = search_disc();
    if (!scripted && (getenv("UJL_SETUP") || !setup_done || disc.empty())) {
        DiscSetup start = g_discs;
        start.jp = disc;
        if (!setup_done)   // offer what's next to the game
            for (const DiscInfo &d : disc_search(disc_search_dirs())) {
                if (d.kind == DISC_USA && start.usa.empty()) start.usa = d.path;
                if (d.kind == DISC_ARCADE_CD && start.arcade_cd.empty()) start.arcade_cd = d.path;
            }
        if (!run_setup_wizard(win, start)) { SDL_GL_DestroyContext(gl); SDL_DestroyWindow(win); return 0; }
        g_discs = wizard_result();
        disc = g_discs.jp;
        setup_done = true;
    }
    if (disc.empty()) {
        fprintf(stderr, "disc: the Japanese Um Jammer Lammy disc (SCPS-18011) wasn't found\n");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ultimate Um Jammer Lammy",
            "The Japanese Um Jammer Lammy disc image (SCPS-18011) wasn't found.", win);
        return 1;
    }
    disc = absolute_path(disc);
    if (g_discs.jp != disc) { g_discs.jp = disc; ini_set("disc", disc); }
    g_jp_disc_path = disc;
    fprintf(stderr, "disc: %s\n", disc.c_str());
    {   /* the USA disc (if imported) supplies the English title logo, menu text and credits song */
        std::string usa = setup_done ? g_discs.usa : search_usa_disc(disc);
        if (!usa.empty() && file_exists(usa) && disc_image_boots(usa.c_str(), "SCUS_944.48") && aux_disc_open(usa.c_str())) {
            g_usa_disc_path = usa;
            fprintf(stderr, "usa disc: %s\n", usa.c_str());
        } else
            fprintf(stderr, "usa disc: %s (Japanese title logo and menus)\n", usa.empty() ? "not imported" : "missing");
    }
    if (!g_discs.arcade_cd.empty()) fprintf(stderr, "arcade cd: %s (not used yet)\n", g_discs.arcade_cd.c_str());
    if (!g_discs.arcade_roms.empty()) fprintf(stderr, "arcade roms: %s (not used yet)\n", g_discs.arcade_roms.c_str());
    {   /* memory card slot 1: a standard raw PS1 card image next to the exe (created blank if missing) */
        const char *sc = getenv("UJL_SCRIPT");
        if (sc && script_load(sc)) {
            const char *cp = getenv("UJL_SCRIPT_CARD");
            memcard_init(0, cp ? cp : (g_base_dir + "script_card.mcd").c_str());
        } else
            memcard_init(0, (g_base_dir + "ujl_card1.mcd").c_str());
        memcard_init(1, nullptr);
    }
    g_uncensored = ini_get("uncensored", "1") != "0";   // before boot: censor_init logs it (UJL_FORCE_CENSORED overrides)
    if (!rt_boot(disc.c_str())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ultimate Um Jammer Lammy", "Could not boot the disc image. See console output.", win);
        return 1;
    }
    g_set.renderer = ini_get("renderer", "1") == "0" ? 0 : 1;
    g_set.res_scale = std::max(0, std::min(8, atoi(ini_get("res_scale", "0").c_str())));
    bool hw_ok = ujl_gl_load();
    GLint max_tex = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    int max_scale = std::max(1, std::min(8, (int)max_tex / 1024));
    // Auto aspect and auto resolution follow the window size once it has held for half a second. Every change
    // reallocates the render targets and wide twins (hundreds of MB at high scales): a fullscreen transition, the
    // menu bar sliding over a fullscreen window on a notched Mac (2560x1664 <-> 2560x1600) or a live resize used to
    // rebuild them over and over, the picture flipping between 4:3 and wide with a hitch each time.
    int settled_w = 0, settled_h = 0;
    SDL_GetWindowSizeInPixels(win, &settled_w, &settled_h);
    int pend_w = settled_w, pend_h = settled_h;
    uint64_t pend_t = 0;
#ifdef __APPLE__
    // Auto on a Mac stops at 3x. A Retina window asked for 7x (a 7168x3584 VRAM target): Stage 1 ran at 12-20 fps
    // on an M2 (Apple's GL sits on Metal and its tile GPU loads/stores whole targets on every switch); 4x still
    // dipped to ~16 fps, 3x holds 56-60 like 2x. Higher scales stay available as explicit choices.
    const int auto_cap = 3;
#else
    const int auto_cap = 8;
#endif
    auto want_scale = [&]() {
        if (g_set.res_scale) return std::min(g_set.res_scale, max_scale);
        return std::max(1, std::min({max_scale, auto_cap, (settled_h + 239) / 240}));
    };
    g_set.textures = ini_get("textures", "1") == "0" ? 0 : 1;
    g_set.aa = std::max(0, std::min(3, atoi(ini_get("aa", "2").c_str())));
    if (hw_ok) hw_ok = hw_init(want_scale(), 1 << g_set.aa);
    g_set.ui_sharp = ini_get("ui_sharp", "1") != "0";
    g_set.subpixel = ini_get("subpixel", "1") != "0";
    g_set.thin_fix = ini_get("thin_fix", "1") != "0";
    if (hw_ok) { hw_set_smooth(g_set.textures); hw_set_ui_sharp(g_set.ui_sharp); hw_set_subpixel(g_set.subpixel); hw_set_thin_fix(g_set.thin_fix); }
    g_set.aspect = std::max(0, std::min(5, atoi(ini_get("aspect", "5").c_str())));
    g_set.stretch_ui = ini_get("stretch_ui", "0") == "1";
    g_unlock_all = ini_get("unlock_all", "0") == "1";
    g_cd_fast_load = ini_get("fast_load", "0") == "1";
    if (!g_script.empty()) {
        if (const char *a = getenv("UJL_SCRIPT_ASPECT")) g_set.aspect = std::max(0, std::min(5, atoi(a)));
        if (getenv("UJL_SCRIPT_UNLOCK")) g_unlock_all = 1;
    }
    if (!hw_ok) { g_set.renderer = 0; fprintf(stderr, "hw: unavailable, using the software renderer\n"); }
    gpu_rec_install();
    gpu_rec_set_enabled(g_set.renderer == 1);
    GpuDisplay hw_disp = g_gpu_display;
    bool hw_have_frame = false;
    SDL_Thread *gt = rt_create_thread(game_thread, "game", nullptr);
    (void)gt;

    GLuint tex_frame = make_tex(640, 480), tex_vram = make_tex(1024, 512);
    int tex_w = 640, tex_h = 480;
    {
        int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        fprintf(stderr, "gamepads at startup: %d\n", n);
        for (int i = 0; i < n; i++) open_pad(ids[i]);
        SDL_free(ids);
    }
    uint64_t fps_t0 = SDL_GetTicks();
    int fps_vb0 = 0;
    float emu_fps = 0, ui_fps = 0;
    int ui_frames = 0;

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
            if (e.type == SDL_EVENT_WINDOW_OCCLUDED || e.type == SDL_EVENT_WINDOW_MINIMIZED) win_hidden = true;
            if (e.type == SDL_EVENT_WINDOW_EXPOSED || e.type == SDL_EVENT_WINDOW_RESTORED ||
                e.type == SDL_EVENT_WINDOW_SHOWN || e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) win_hidden = hid_w != 0;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) open_pad(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED) close_pad(e.gdevice.which);
            if (e.type == SDL_EVENT_DROP_FILE) {   // a disc image or ROM set dropped on the window: open Manage discs with it
                if (!wizard_active()) { wizard_open(false, g_discs); g_set.show_overlay = true; }
                wizard_drop_file(e.drop.data);
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE) g_set.show_overlay = !g_set.show_overlay;
                if (e.key.key == SDLK_F2) g_show_vram = !g_show_vram;
                if (e.key.key == SDLK_F11) {
                    bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN;
                    SDL_SetWindowFullscreen(win, !fs);
                }
            }
        }
        g_input_buttons = poll_buttons(!ImGui::GetIO().WantCaptureKeyboard);

        {
            int fw, fh;
            SDL_GetWindowSizeInPixels(win, &fw, &fh);
            uint64_t tnow = SDL_GetTicks();
            if (fw != pend_w || fh != pend_h) { pend_w = fw; pend_h = fh; pend_t = tnow; }
            if ((pend_w != settled_w || pend_h != settled_h) && tnow - pend_t >= 500) { settled_w = pend_w; settled_h = pend_h; }
        }
        {
            float asp = k_aspects[g_set.aspect];
            if (asp == 0.0f) asp = settled_h > 0 ? (float)settled_w / settled_h : 4.0f / 3.0f;
            if (asp < 4.0f / 3.0f + 0.01f || g_set.renderer != 1) asp = 0.0f;
            // The front-end menus (title, candy bars, options, stage select: overlay COMOD0.MEN) are 2D art the game
            // rotates through the GTE around the screen center; widening would stretch them. Pillarbox them.
            if (const char *ov = rt_current_overlay_name()) if (!strcmp(ov, "COMOD0.MEN") && !g_set.stretch_ui) asp = 0.0f;
            {   static float last_asp = -1.0f;
                if (std::fabs(asp - last_asp) > 1e-4f) {
                    const char *ov = rt_current_overlay_name();
                    fprintf(stderr, "aspect: %.3f -> %.3f (window %dx%d, overlay %s, vblank %d)\n", last_asp, asp, settled_w, settled_h,
                            ov ? ov : "-", g_vblanks.load());
                    last_asp = asp;
                }
            }
            if (hw_ok) hw_set_widescreen(asp, g_set.stretch_ui);
            g_gte_xscale = asp > 0.0f ? (int32_t)std::lround(4096.0 * (4.0 / 3.0) / asp) : 4096;
        }
        if (g_set.renderer == 1) {
            int sc = want_scale();
            if (sc != hw_scale()) { hw_set_scale(sc); gpu_rec_request_resync(); hw_have_frame = false; }
            const uint8_t *cmds;
            size_t n = gpu_rec_take(&cmds);
            if (n && hw_replay(cmds, n, &hw_disp)) hw_have_frame = true;
        }

        if (g_frame_new.exchange(false)) {
            SDL_LockMutex(g_frame_mtx);
            glBindTexture(GL_TEXTURE_2D, tex_frame);
            if (g_frame_w != tex_w || g_frame_h != tex_h) {
                tex_w = g_frame_w; tex_h = g_frame_h;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, tex_h, GL_RGBA, GL_UNSIGNED_BYTE, g_frame_rgba.data());
            GLint f = g_set.scale_filter ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
            if (g_show_vram) {
                glBindTexture(GL_TEXTURE_2D, tex_vram);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512, GL_RGBA, GL_UNSIGNED_BYTE, g_vram_rgba.data());
            }
            SDL_UnlockMutex(g_frame_mtx);
        }

        if (g_cd_fast_load && rt_load_turbo_active() && !g_script_quit) {
            // fast-forwarding through a load: keep showing the last picture (no loading screen) and stay responsive
            SDL_Delay(1);
            continue;
        }
        uint64_t now = SDL_GetTicks();
        ui_frames++;
        if (now - fps_t0 >= 1000) {
            int vb = g_vblanks.load();
            emu_fps = (float)(vb - fps_vb0) * 1000.0f / (float)(now - fps_t0);
            ui_fps = (float)ui_frames * 1000.0f / (float)(now - fps_t0);
            static int fps_log = getenv("UJL_FPS_STATS") != nullptr;
            if (fps_log) fprintf(stderr, "fps: game %.1f  ui %.1f  scale %d  overlay %s\n", emu_fps, ui_fps, hw_scale(),
                                 rt_current_overlay_name() ? rt_current_overlay_name() : "-");
            ui_frames = 0;
            fps_vb0 = vb;
            fps_t0 = now;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // game image, letterboxed/pillarboxed to its aspect: the wide twin in widescreen, else 4:3.
        // FMV (24bpp) always comes from the native frame at 4:3.
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        bool use_hw = g_set.renderer == 1 && hw_have_frame && !hw_disp.depth24;
        HwView view{};
        if (use_hw) {
            bool wide_view = hw_view(hw_disp, &view);
            static int last_wide = -1, flips;
            static const bool fps_log = getenv("UJL_FPS_STATS") != nullptr;
            if (fps_log && (int)wide_view != last_wide) {
                if (flips++ < 400)
                    fprintf(stderr, "view: %s (disp %d,%d %dx%d, vblank %d, t %llu)\n", wide_view ? "wide" : "4:3",
                            hw_disp.disp_x, hw_disp.disp_y, hw_disp.hres, hw_disp.vres, g_vblanks.load(),
                            (unsigned long long)SDL_GetTicks());
                last_wide = wide_view;
            }
        }
        float aspect = use_hw ? view.aspect : 4.0f / 3.0f;
        float w = ds.x, h = ds.x / aspect;
        if (h > ds.y) { h = ds.y; w = h * aspect; }
        ImVec2 p0((ds.x - w) * 0.5f, (ds.y - h) * 0.5f);
        if (use_hw && hw_disp.enabled) {
            ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)view.tex, p0, ImVec2(p0.x + w, p0.y + h),
                                                     ImVec2(view.u0, view.v0), ImVec2(view.u1, view.v1));
        } else if (!use_hw) {
            ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)tex_frame, p0, ImVec2(p0.x + w, p0.y + h));
        }

        if (g_set.show_overlay) {
            ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
            ImGui::Begin("UltimateUJL Settings  (F1 / Esc)", &g_set.show_overlay);
            ImGui::SeparatorText("Display");
            if (!hw_ok) ImGui::BeginDisabled();
            if (ImGui::Combo("Renderer", &g_set.renderer, "Software (original 320x240)\0Hardware (high resolution)\0")) {
                gpu_rec_set_enabled(g_set.renderer == 1);
                hw_have_frame = false;
                ini_set("renderer", std::to_string(g_set.renderer));
            }
            if (!hw_ok) ImGui::EndDisabled();
            if (g_set.renderer == 1) {
                const char *items[] = {"Auto (match window)", "1x (320x240)", "2x (640x480)", "3x (960x720)", "4x (1280x960)",
                                       "5x (1600x1200)", "6x (1920x1440)", "7x (2240x1680)", "8x (2560x1920)"};
                int cnt = 1 + max_scale;
                char auto_label[48];
                if (auto_cap < max_scale) {   // say where Auto stops, so a higher scale is a deliberate choice
                    SDL_snprintf(auto_label, sizeof auto_label, "Auto (match window, up to %dx)", auto_cap);
                    items[0] = auto_label;
                }
                if (ImGui::Combo("Internal resolution", &g_set.res_scale, items, cnt))
                    ini_set("res_scale", std::to_string(g_set.res_scale));
                if (ImGui::Combo("Textures", &g_set.textures, "Original (sharp pixels)\0Smooth (vector-like HQ)\0")) {
                    hw_set_smooth(g_set.textures);
                    ini_set("textures", std::to_string(g_set.textures));
                }
                if (ImGui::Checkbox("Sub-pixel precision (no polygon jitter)", &g_set.subpixel)) {
                    hw_set_subpixel(g_set.subpixel);
                    ini_set("subpixel", g_set.subpixel ? "1" : "0");
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("The PS1 snaps projected 3D vertices to whole pixels, so models wobble.\n"
                                           "This draws them at the precise position the GTE computed.");
                    ImGui::EndTooltip();
                }
                if (ImGui::Checkbox("Solid thin geometry", &g_set.thin_fix)) {
                    hw_set_thin_fix(g_set.thin_fix);
                    ini_set("thin_fix", g_set.thin_fix ? "1" : "0");
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("3D parts thinner than an original pixel (Stage 1's drum and cymbal stands)\n"
                                           "are drawn one original pixel wide, like on the PS1, so they don't flicker\n"
                                           "at high resolution.");
                    ImGui::EndTooltip();
                }
                if (g_set.textures == 1 && ImGui::Checkbox("Keep UI pixel-sharp", &g_set.ui_sharp)) {
                    hw_set_ui_sharp(g_set.ui_sharp);
                    ini_set("ui_sharp", g_set.ui_sharp ? "1" : "0");
                }
                if (g_set.textures == 1 && ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("2D art drawn at its original size (HUD, menus, prompts, captions)\n"
                                           "is upscaled with nearest neighbour; only scaled art is smoothed.");
                    ImGui::EndTooltip();
                }
                if (ImGui::Combo("Anti-aliasing", &g_set.aa, "Off\0" "2x MSAA\0" "4x MSAA\0" "8x MSAA\0")) {
                    hw_set_msaa(1 << g_set.aa);
                    gpu_rec_request_resync();
                    hw_have_frame = false;
                    ini_set("aa", std::to_string(g_set.aa));
                }
                if (g_set.aa && hw_msaa() < (1 << g_set.aa))
                    ImGui::TextDisabled("Using %dx (limited by GPU memory at this resolution).", hw_msaa());
                if (g_set.aa && !hw_sprite_edge_aa())
                    ImGui::TextDisabled("Sprite edges need OpenGL 4: only polygon edges are smoothed.");
                if (ImGui::Combo("Aspect ratio", &g_set.aspect,
                                 "4:3 (original)\0" "16:9\0" "16:10\0" "21:9 (ultrawide)\0" "32:9 (super ultrawide)\0" "Auto (match window)\0")) {
                    ini_set("aspect", std::to_string(g_set.aspect));
                    gpu_rec_request_resync();
                }
                if (g_set.aspect != 0 && ImGui::Checkbox("Stretch UI to fill (for sadists)", &g_set.stretch_ui))
                    ini_set("stretch_ui", g_set.stretch_ui ? "1" : "0");
                ImGui::TextDisabled("Rendering at %dx. Movies always play at their original resolution.", hw_scale());
            } else {
                ImGui::Combo("Scaling filter", &g_set.scale_filter, "Nearest (sharp)\0Linear (smooth)\0");
            }
            ImGui::Checkbox("Show FPS", &g_set.show_fps);
            ImGui::SeparatorText("Audio");
            { int vol = g_volume.load(); if (ImGui::SliderInt("Volume", &vol, 0, 100)) g_volume = vol; }
            ImGui::SeparatorText("Game");
            {
                bool ua = g_unlock_all != 0;
                if (ImGui::Checkbox("Unlock all songs and variants", &ua)) {
                    g_unlock_all = ua;
                    ini_set("unlock_all", ua ? "1" : "0");
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("Every stage and mode is open in Stage Select.\n"
                                           "Saving system data while this is on writes the unlocks to your card.");
                    ImGui::EndTooltip();
                }
                bool fl = g_cd_fast_load != 0;
                if (ImGui::Checkbox("Instant loading (skip loading screens)", &fl)) {
                    g_cd_fast_load = fl;
                    ini_set("fast_load", fl ? "1" : "0");
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("Game data is read from the disc as fast as the game takes it, and the frames a\n"
                                           "load spends behind a loading screen are fast-forwarded with the picture held.\n"
                                           "Movies, stage title cards and songs still play at normal speed.");
                    ImGui::EndTooltip();
                }
                {   // the JP release is the uncensored one; off = the USA disc's versions of Stages 1, 5 and 6
                    bool can = censor_available() != 0;
                    bool unc = g_uncensored != 0 || !can;
                    if (!can) ImGui::BeginDisabled();
                    if (ImGui::Checkbox("Uncensored content", &unc)) {
                        g_uncensored = unc;
                        ini_set("uncensored", unc ? "1" : "0");
                    }
                    if (!can) ImGui::EndDisabled();
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::TextUnformatted("The Japanese release this port runs is the uncensored version.\n"
                                               "Off: Stage 1's song and lyrics, Stage 5's vocals and lyrics, and Stage 6's\n"
                                               "movie, vocals, lyrics and Lammy's outfit come from the USA release instead.\n"
                                               "Applies from the next stage loaded.");
                        if (!can) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Turning it off needs the USA disc (Settings > Discs).");
                        ImGui::EndTooltip();
                    }
                }
                // Autoplay: plays player 1's part through the controller (runtime autoplay.c); not saved, off at start
                ImGui::Combo("Autoplay", &g_autoplay, "Off\0Autoplay (follows the chart, GOOD)\0Auto-COOL (freestyles to COOL)\0Auto-FEVER (COOL, then fever codes)\0");
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("Autoplay plays each line exactly as the teacher does, which keeps the rating at GOOD.\n"
                                           "Auto-COOL adds its own rhythm to every line to reach COOL, then freestyles\n"
                                           "through COOL mode. Auto-FEVER plays like Auto-COOL, then enters the stage's\n"
                                           "fever codes in COOL mode (all six in turn, so the fevers stack).\n"
                                           "A watermark shows while either plays. A song played with either on, even for\n"
                                           "part of it, doesn't enter the high-score table. Off every time the game starts.");
                    ImGui::EndTooltip();
                }
            }
            ImGui::SeparatorText("Discs");
            {
                auto base_name = [](const std::string &p) {
                    std::string q = (!p.empty() && (p.back() == '/' || p.back() == '\\')) ? p.substr(0, p.size() - 1) : p;
                    return q.substr(q.find_last_of("/\\") + 1);
                };
                const ImVec4 ok(0.45f, 0.85f, 0.45f, 1), missing(0.6f, 0.6f, 0.6f, 1);
                auto row = [&](const char *name, const std::string &path, const char *about) {
                    ImGui::TextColored(path.empty() ? missing : ok, "%s", name);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", path.empty() ? "not added" : base_name(path).c_str());
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::TextUnformatted(about);
                        if (!path.empty()) ImGui::TextUnformatted(path.c_str());
                        ImGui::EndTooltip();
                    }
                };
                row("Japan (SCPS-18011)", g_discs.jp, "Required. The game itself runs from this disc.");
                row("USA (SCUS-94448)", g_discs.usa, "Optional. Adds the English title logo, English menu text and tooltips,\n"
                                                     "English memory card messages, and the English credits song.");
                row("UJL NOW! arcade CD", g_discs.arcade_cd, "Optional. Arcade attract movies, Joe Chin cutscenes and Stage 1\n"
                                                             "versus content (coming soon).");
                row("UJL NOW! ROM set", g_discs.arcade_roms, "Optional. Arcade guitar sounds and voice lines (coming soon).");
                if (ImGui::Button("Manage discs...") && !wizard_active()) wizard_open(false, g_discs);
                if (g_discs.jp != g_jp_disc_path || g_discs.usa != g_usa_disc_path)
                    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.35f, 1), "Restart the game to use the new discs.");
            }
            ImGui::SeparatorText("Memory card");
            {
                char names[15][21];
                int blocks[15];
                int n = memcard_list(0, names, blocks, 15);
                ImGui::TextDisabled("Slot 1: %s", memcard_path(0));
                ImGui::Text("%d save(s), %d free block(s)", n, memcard_free_blocks(0));
                for (int i = 0; i < n; i++) ImGui::BulletText("%s  (%d block%s)", names[i], blocks[i], blocks[i] == 1 ? "" : "s");
                ImGui::RadioButton("Copy Um Jammer Lammy saves", &g_card_import_mode, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Replace whole card", &g_card_import_mode, 1);
                if (ImGui::Button("Import card dump or save...")) {
                    static const SDL_DialogFileFilter filters[] = {
                        {"PS1 memory cards and saves", "mcr;mcd;gme;mem;vgs;vmp;srm;ps;mc;ddf;mcs;psx;mcb;bin"},
                        {"All files", "*"}};
                    SDL_ShowOpenFileDialog(card_pick_cb, nullptr, win, filters, 2, nullptr, false);
                }
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("Whole cards: .mcr/.mcd/.srm (raw), DexDrive .gme, .mem/.vgs, PSP/Vita .vmp.\n"
                                           "Single saves: .mcs/.psx/.mcb, or bare save data.\n"
                                           "US (SCUS-94448) saves work too; they're copied under the Japanese save name.");
                    ImGui::EndTooltip();
                }
                if (g_card_pick.exchange(0)) memcard_request_import(g_card_pick_path.c_str(), g_card_import_mode);
                if (const char *r = memcard_import_result()) ImGui::TextWrapped("%s", r);
            }
            ImGui::SeparatorText("Controllers");
            {
                bool cc = g_confirm_cross != 0;
                if (ImGui::Checkbox("Confirm with Cross in menus", &cc)) {
                    g_confirm_cross = cc;
                    ini_set("confirm_cross", cc ? "1" : "0");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::BeginItemTooltip()) {
                    ImGui::TextUnformatted("Japanese release: Circle confirms, Cross cancels.\n"
                                           "On: Cross confirms like the US/EU releases.\n"
                                           "Songs are unaffected (Circle and Cross are note buttons).");
                    ImGui::EndTooltip();
                }
            }
            if (g_pads.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "No controller detected (keyboard only)");
            for (SDL_Gamepad *g : g_pads) ImGui::BulletText("%s", SDL_GetGamepadName(g));
            {
                static const char *names[16] = {"Select", "L3", "R3", "Start", "Up", "Right", "Down", "Left",
                                                "L2", "R2", "L1", "R1", "Tri", "Circle", "Cross", "Square"};
                uint16_t held = (uint16_t)~g_input_buttons.load();
                std::string s2;
                for (int b = 0; b < 16; b++) if (held & (1u << b)) { s2 += names[b]; s2 += ' '; }
                ImGui::Text("Held: %s", s2.empty() ? "-" : s2.c_str());
            }
            ImGui::SeparatorText("Debug");
            ImGui::Checkbox("VRAM viewer (F2)", &g_show_vram);
            ImGui::Text("Display %dx%d @ %d,%d  %s", g_gpu_display.hres, g_gpu_display.vres,
                        g_gpu_display.disp_x, g_gpu_display.disp_y, g_gpu_display.depth24 ? "24bpp" : "15bpp");
            ImGui::End();
        }
        if (wizard_active() && wizard_draw(win) == WIZ_DONE) g_discs = wizard_result();
        if (g_show_vram) {
            ImGui::SetNextWindowSize(ImVec2(1040, 560), ImGuiCond_FirstUseEver);
            ImGui::Begin("VRAM (F2)", &g_show_vram);
            ImGui::Image((ImTextureID)(intptr_t)tex_vram, ImVec2(1024, 512));
            ImGui::End();
        }
        if (autoplay_active()) {   // watermark over the game picture's bottom-right corner while Autoplay plays
            const char *wm = g_autoplay == 3 ? "AUTO-FEVER" : g_autoplay == 2 ? "AUTO-COOL" : "AUTOPLAY";
            float fs = std::max(16.0f, h / 22.0f);
            ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, wm);
            ImVec2 at(p0.x + w - sz.x - fs * 0.6f, p0.y + h - sz.y - fs * 0.5f);
            ImDrawList *dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(ImVec2(at.x - fs * 0.3f, at.y - fs * 0.15f), ImVec2(at.x + sz.x + fs * 0.3f, at.y + sz.y + fs * 0.15f),
                              IM_COL32(0, 0, 0, 110), fs * 0.25f);
            dl->AddText(ImGui::GetFont(), fs, at, IM_COL32(255, 255, 255, 190), wm);
        }
        if (g_set.show_fps)
            ImGui::GetForegroundDrawList()->AddText(ImVec2(8, 8), 0xFF00FF00, (std::to_string((int)(ui_fps + 0.5f)) + " fps  (" + std::to_string((int)(emu_fps + 0.5f)) + " vblank/s)").c_str());

        ImGui::Render();
        int fbw, fbh;
        SDL_GetWindowSizeInPixels(win, &fbw, &fbh);
        if (hid_w) {
            static GLuint hid_fbo, hid_rb;
            fbw = hid_w; fbh = hid_h;
            if (!hid_fbo) {
                ujl_glGenRenderbuffers(1, &hid_rb);
                ujl_glBindRenderbuffer(0x8D41 /* GL_RENDERBUFFER */, hid_rb);
                ujl_glRenderbufferStorageMultisample(0x8D41, 0, GL_RGBA8, fbw, fbh);
                ujl_glGenFramebuffers(1, &hid_fbo);
                ujl_glBindFramebuffer(GL_FRAMEBUFFER, hid_fbo);
                ujl_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 0x8D41, hid_rb);
            }
            ujl_glBindFramebuffer(GL_FRAMEBUFFER, hid_fbo);
        }
        glViewport(0, 0, fbw, fbh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (int st = g_shot_req.exchange(-1); st >= 0) {
            char fn[64];
            SDL_snprintf(fn, sizeof fn, "hw_v%05d.bmp", st);
            save_shot(fbw, fbh, fn);
        }
        if (g_script_quit) running = false;
        for (int n = g_script_overlay.exchange(0); n > 0; n--) g_set.show_overlay = !g_set.show_overlay;
        if (win_hidden) {
            // Nothing visible to present. Swapping with vsync on can block indefinitely for an occluded window,
            // which left the game black until it was clicked; keep replaying frames and pace with a sleep.
            SDL_Delay(8);
        } else {
            uint64_t ts = SDL_GetTicks();
            SDL_GL_SwapWindow(win);
            uint64_t dt = SDL_GetTicks() - ts;
            if (dt > 250)
                fprintf(stderr, "present: swap stalled %llu ms (focused=%d)\n", (unsigned long long)dt,
                        (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0);
        }
    }
    memcard_flush(); // don't lose a save made in the last second
    // The game thread can't be unwound cleanly and keeps calling into SDL (audio stream, timers, semaphores), so
    // SDL_Quit() here would free those under it: a segfault on exit on Linux. Close only what the UI thread owns
    // (the game thread never touches the window or GL) and leave the process; exit releases the rest.
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(win);
    _Exit(0);
}
