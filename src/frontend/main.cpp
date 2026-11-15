// UJL-PC frontend: SDL3 window, input, ImGui overlay. The recompiled game runs on its own thread.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_opengl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "gl_funcs.h"
#include "app_icon.h"
#include "hw_renderer.h"

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
// UJL_SCRIPT=file with lines "<seconds> <button>|shot|quit" drives the game for renderer tests: buttons are
// held for 6 vblanks, "shot" saves the presented window to shots/hw_vNNNNN.bmp (vblank number) next to the exe. While a script
// runs, UJL_SCRIPT_CARD picks the memory card file (default script_card.mcd, never the player's card),
// UJL_SCRIPT_ASPECT the aspect index (not saved) and UJL_SCRIPT_UNLOCK=1 unlocks all stages.
struct ScriptEv { int vblank; int kind; uint16_t bit; }; // kind 0 button, 1 shot, 2 quit
static std::vector<ScriptEv> g_script;
static std::atomic<int> g_shot_req{-1};
static std::atomic<bool> g_script_quit{false};
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

extern "C" void host_audio_push(const int16_t *lr, int frames) {
    if (!g_audio) return;
    if (rt_load_turbo_active()) return; /* fast-forwarding a load: nothing to hear */
    /* Pace output gently: nudge playback speed (+-1.5%) to keep ~60 ms queued instead of
       dropping chunks, which clicks. Only drop if something stalled and >300 ms piled up. */
    int queued = SDL_GetAudioStreamQueued(g_audio) / 4;
    const int target = 44100 * 60 / 1000;
    if (queued > 44100 * 300 / 1000) return;
    float ratio = 1.0f + (float)(queued - target) / (float)target * 0.015f;
    if (ratio < 0.985f) ratio = 0.985f;
    if (ratio > 1.015f) ratio = 1.015f;
    SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
    int vol = g_volume.load();
    if (vol == 100) { SDL_PutAudioStreamData(g_audio, lr, frames * 4); return; }
    int16_t tmp[512];
    int n = frames * 2 > 512 ? 512 : frames * 2;
    for (int i = 0; i < n; i++) tmp[i] = (int16_t)(lr[i] * vol / 100);
    SDL_PutAudioStreamData(g_audio, tmp, n * 2);
}

// ---------------------------------------------------------------- memory card import (file dialog)
static std::atomic<int> g_card_pick{0};       // 1 = a file was picked
static std::string g_card_pick_path;
static int g_card_import_mode = 0;            // 0 copy this game's saves, 1 replace the whole card
static void SDLCALL card_pick_cb(void *, const char *const *files, int) {
    if (files && files[0]) { g_card_pick_path = files[0]; g_card_pick = 1; }
}

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
static std::string g_base_dir;   // folder containing the exe

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

static std::string ini_get(const char *key, const std::string &def = "") {
    for (auto &p : ini_load()) if (p.first == key) return p.second;
    return def;
}

static void ini_set(const char *key, const std::string &val) {
    auto kv = ini_load();
    bool found = false;
    for (auto &p : kv) if (p.first == key) { p.second = val; found = true; }
    if (!found) kv.emplace_back(key, val);
    FILE *f = fopen(ini_path().c_str(), "wb");
    if (!f) return;
    for (auto &p : kv) fprintf(f, "%s=%s\n", p.first.c_str(), p.second.c_str());
    fclose(f);
}

static std::string ini_get_disc() { return ini_get("disc"); }
static void ini_set_disc(const std::string &path) { ini_set("disc", path); }

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

struct PickState { std::string path; bool done = false; };
static void SDLCALL on_pick(void *ud, const char *const *files, int) {
    PickState *ps = (PickState *)ud;
    if (files && files[0]) ps->path = files[0];
    ps->done = true;
}

static std::string pick_disc(SDL_Window *win) {
    static const SDL_DialogFileFilter filters[] = {{"PlayStation disc image", "bin;img;iso"}, {"All files", "*"}};
    PickState ps;
    SDL_ShowOpenFileDialog(on_pick, &ps, win, filters, 2, g_base_dir.c_str(), false);
    while (!ps.done) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_QUIT) return "";
        SDL_Delay(10);
    }
    return ps.path;
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
    SDL_Window *win = SDL_CreateWindow("Um Jammer Lammy PC", hid_w ? hid_w : 1280, hid_h ? hid_h : 960,
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
    std::string disc;
    if (!disc_arg.empty() && rt_probe_disc(disc_arg.c_str())) disc = disc_arg;
    if (disc.empty()) { std::string d = ini_get_disc(); if (!d.empty() && rt_probe_disc(d.c_str())) disc = d; }
    if (disc.empty()) disc = search_disc();
    while (disc.empty()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Um Jammer Lammy PC",
            "Please select your Um Jammer Lammy (Japan) disc image (.bin / .img).", win);
        std::string pick = pick_disc(win);
        if (pick.empty()) return 0;
        if (rt_probe_disc(pick.c_str())) disc = pick;
        else SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Um Jammer Lammy PC",
            "That image isn't the Japanese Um Jammer Lammy disc this build supports (SCPS-18011).", win);
    }
    if (!(disc.size() > 1 && (disc[1] == ':' || disc[0] == '/' || disc[0] == '\\'))) {
        char *cwd = SDL_GetCurrentDirectory();
        if (cwd) { disc = std::string(cwd) + disc; SDL_free(cwd); }
    }
    ini_set_disc(disc);
    fprintf(stderr, "disc: %s\n", disc.c_str());
    {   /* the USA disc (if present) supplies the English title logo */
        std::string usa = search_usa_disc(disc);
        if (!usa.empty() && aux_disc_open(usa.c_str())) fprintf(stderr, "usa disc: %s\n", usa.c_str());
        else fprintf(stderr, "usa disc: not found (title keeps the shared emblem)\n");
    }
    {   /* memory card slot 1: a standard raw PS1 card image next to the exe (created blank if missing) */
        const char *sc = getenv("UJL_SCRIPT");
        if (sc && script_load(sc)) {
            const char *cp = getenv("UJL_SCRIPT_CARD");
            memcard_init(0, cp ? cp : (g_base_dir + "script_card.mcd").c_str());
        } else
            memcard_init(0, (g_base_dir + "ujl_card1.mcd").c_str());
        memcard_init(1, nullptr);
    }
    if (!rt_boot(disc.c_str())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UJL-PC", "Could not boot the disc image. See console output.", win);
        return 1;
    }
    g_set.renderer = ini_get("renderer", "1") == "0" ? 0 : 1;
    g_set.res_scale = std::max(0, std::min(8, atoi(ini_get("res_scale", "0").c_str())));
    bool hw_ok = ujl_gl_load();
    GLint max_tex = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    int max_scale = std::max(1, std::min(8, (int)max_tex / 1024));
    auto want_scale = [&]() {
        if (g_set.res_scale) return std::min(g_set.res_scale, max_scale);
        int fw, fh;
        SDL_GetWindowSizeInPixels(win, &fw, &fh);
        return std::max(1, std::min(max_scale, (fh + 239) / 240));
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
    SDL_Thread *gt = SDL_CreateThread(game_thread, "game", nullptr);
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
            float asp = k_aspects[g_set.aspect];
            if (asp == 0.0f) { int fw, fh; SDL_GetWindowSizeInPixels(win, &fw, &fh); asp = fh > 0 ? (float)fw / fh : 4.0f / 3.0f; }
            if (asp < 4.0f / 3.0f + 0.01f || g_set.renderer != 1) asp = 0.0f;
            // The front-end menus (title, candy bars, options, stage select: overlay COMOD0.MEN) are 2D art the game
            // rotates through the GTE around the screen center; widening would stretch them. Pillarbox them.
            if (const char *ov = rt_current_overlay_name()) if (!strcmp(ov, "COMOD0.MEN") && !g_set.stretch_ui) asp = 0.0f;
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
        if (use_hw) hw_view(hw_disp, &view);
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
            ImGui::Begin("UJL-PC Settings  (F1 / Esc)", &g_set.show_overlay);
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
            ImGui::BeginDisabled();
            bool off = false;
            ImGui::Checkbox("Autoplay (soon, watermarks gameplay)", &off);
            ImGui::Checkbox("Auto-COOL (soon, watermarks gameplay)", &off);
            ImGui::Checkbox("Restore uncensored content (soon)", &off);
            ImGui::EndDisabled();
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
        if (g_show_vram) {
            ImGui::SetNextWindowSize(ImVec2(1040, 560), ImGuiCond_FirstUseEver);
            ImGui::Begin("VRAM (F2)", &g_show_vram);
            ImGui::Image((ImTextureID)(intptr_t)tex_vram, ImVec2(1024, 512));
            ImGui::End();
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
            std::vector<uint8_t> px((size_t)fbw * fbh * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            SDL_Surface *sf = SDL_CreateSurface(fbw, fbh, SDL_PIXELFORMAT_ABGR8888);
            for (int y = 0; y < fbh; y++) memcpy((uint8_t *)sf->pixels + y * sf->pitch, &px[(size_t)(fbh - 1 - y) * fbw * 4], (size_t)fbw * 4);
            char fn[64];
            SDL_snprintf(fn, sizeof fn, "shots/hw_v%05d.bmp", st);
            SDL_CreateDirectory((g_base_dir + "shots").c_str());
            SDL_SaveBMP(sf, (g_base_dir + fn).c_str());
            SDL_DestroySurface(sf);
        }
        if (g_script_quit) running = false;
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
    // the game thread can't be unwound cleanly; leave the process
    SDL_Quit();
    _Exit(0);
}
