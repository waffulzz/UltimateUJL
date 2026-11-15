// High-resolution OpenGL renderer: replays the recorded GPU command stream into an upscaled VRAM.
#pragma once
#include <cstdint>
#include <cstddef>
extern "C" {
#include "gpu.h"
}

bool hw_init(int scale, int samples);    // needs a current GL 3.3+ context and ujl_gl_load()
void hw_set_scale(int scale);            // reallocates; caller should request a VRAM resync
int  hw_scale();
int  hw_msaa();                          // effective MSAA samples
void hw_set_msaa(int samples);           // 1 = off; caller should request a VRAM resync
bool hw_sprite_edge_aa();                // GL 4: sprite silhouettes are anti-aliased too
void hw_set_subpixel(int on);         // 1: 3D vertices at the GTE's sub-pixel position (no PS1 vertex jitter)
void hw_set_thin_fix(int on);
int *hw_debug_flag(int i);
void hw_set_ui_sharp(int on);         // 1: unscaled 2D art (HUD, menus, text) stays nearest-neighbour
void hw_set_smooth(int on);           // 1: smooth "vector" texture filtering
// Replays recorded commands. Returns true if a frame marker was seen; *disp gets the last one.
bool hw_replay(const uint8_t *data, size_t len, GpuDisplay *disp);
unsigned hw_vram_texture();
// Widescreen: aspect 0 (or <= 4:3) = off. 3D is widened, 2D stays centered unless stretch_ui.
void hw_set_widescreen(float aspect, bool stretch_ui);
struct HwView { unsigned tex; float u0, v0, u1, v1; float aspect; };
bool hw_view(const GpuDisplay &d, HwView *v);  // what to show for this display state (true = wide twin)              // GL texture name of the upscaled VRAM (row 0 = VRAM y 0)
