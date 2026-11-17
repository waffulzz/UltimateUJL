#include "pause_menu.h"
#include "touch_pad.h"
#include <algorithm>

namespace {
constexpr uint16_t start = 1u << B_START, up = 1u << B_UP, down = 1u << B_DOWN;
uint32_t color(int r, int g, int b, int a = 255) { return r | g << 8 | b << 16 | (uint32_t)a << 24; }
int rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
void rect(std::vector<uint32_t> &p, int x, int y, int w, int h, uint32_t c) {
    for (int j = std::max(0, y); j < std::min(PauseMenu::height, y + h); j++)
        for (int i = std::max(0, x); i < std::min(PauseMenu::width, x + w); i++) p[j * PauseMenu::width + i] = c;
}
}

PauseAction PauseMenu::toggle(uint16_t held) {
    previous = held;
    if (stage_picker) return PauseAction::Back;
    if (!visible) { visible = true; selection = 0; return PauseAction::Open; }
    return choose(0, held);
}
PauseAction PauseMenu::choose(int row, uint16_t held) {
    if (!visible || row < 0 || row > 2) return PauseAction::None;
    selection = row;
    if (stage_picker) return row == 0 ? PauseAction::Resume : row == 1 ? PauseAction::Again : PauseAction::Settings;
    if (row == 2) return PauseAction::Settings;
    visible = false;
    blocked |= held; // the answer/resume button must be released before it can become a note
    previous = held;
    return row == 0 ? PauseAction::Resume : PauseAction::Again;
}
PauseAction PauseMenu::input(uint16_t held, bool available, bool confirm_cross, bool settings_open) {
    uint16_t pressed = held & ~previous;
    previous = held;
    if (settings_open) return PauseAction::None;
    if (!visible) return available && (pressed & start) ? toggle(held) : PauseAction::None;
    if (pressed & start) return choose(stage_picker ? selection : 0, held);
    if (pressed & up) selection = (selection + 2) % 3;
    if (pressed & down) selection = (selection + 1) % 3;
    uint16_t yes = 1u << (confirm_cross ? B_CROSS : B_CIRCLE);
    uint16_t no = 1u << (confirm_cross ? B_CIRCLE : B_CROSS);
    if (pressed & no) return stage_picker ? PauseAction::Back : choose(0, held);
    if (pressed & yes) return choose(selection, held);
    return PauseAction::None;
}
uint16_t PauseMenu::game_buttons(uint16_t held) {
    blocked &= held;
    return visible ? 0 : held & ~blocked;
}
int PauseMenu::hit(float x, float y) {
    if (x < 28 || x >= width - 28 || y < 56 || y >= 140) return -1;
    return (int)(y - 56) / 28;
}

void PauseMenu::capture_font(const uint8_t *ram, const uint16_t *vram) {
    // JP executable metrics, ten bytes per character. Same resident font used by the game's English captions.
    for (int i = 0; i < 128; i++) {
        const uint8_t *g = ram + 0x63c98 + i * 10;
        glyphs[i] = {rd16(g), rd16(g + 2), g[4], g[5], (int16_t)rd16(g + 6)};
    }
    font.assign(256 * 128, 0);
    for (int y = 0; y < 128; y++) for (int x = 0; x < 256; x++) {
        int index = (vram[((385 + y) & 511) * 1024 + 960 + x / 4] >> ((x & 3) * 4)) & 15;
        uint16_t c = vram[475 * 1024 + 1008 + index];
        if (c) font[y * 256 + x] = color(((c & 31) * 255 + 15) / 31,
            (((c >> 5) & 31) * 255 + 15) / 31, (((c >> 10) & 31) * 255 + 15) / 31);
    }
}
int PauseMenu::text_width(const char *s) const {
    int w = 0, previous = -1;
    for (; *s; s++) {
        int ch = (unsigned char)*s & 127;
        if (previous >= 0) w += glyphs[previous].spacing;
        w += glyphs[ch].u == 65535 || !glyphs[ch].w ? 8 : glyphs[ch].w;
        previous = ch;
    }
    return w;
}
void PauseMenu::text(std::vector<uint32_t> &pixels, const char *s, int y, uint32_t tint) const {
    int x = (width - text_width(s)) / 2, previous = -1;
    for (; *s; s++) {
        int ch = (unsigned char)*s & 127;
        const Glyph &g = glyphs[ch];
        if (previous >= 0) x += glyphs[previous].spacing;
        previous = ch;
        if (g.u == 65535 || !g.w) { x += 8; continue; }
        for (int j = 0; j < g.h && j < 16; j++) for (int i = 0; i < g.w && i < 32; i++) {
            if (g.u + i >= 256 || g.v + j >= 128 || x + i < 0 || x + i >= width || y + j >= height) continue;
            uint32_t c = font[(g.v + j) * 256 + g.u + i];
            if (c >> 24) pixels[(y + j) * width + x + i] = color((c & 255) * (tint & 255) / 255,
                ((c >> 8) & 255) * ((tint >> 8) & 255) / 255, ((c >> 16) & 255) * ((tint >> 16) & 255) / 255);
        }
        x += g.w;
    }
}
void PauseMenu::rasterize(std::vector<uint32_t> &pixels) const {
    pixels.assign(width * height, 0);
    if (font.empty()) return;
    // Native-sized pixels, white frame and dark backing like the in-game prompts. No host font or window chrome.
    rect(pixels, 6, 6, width - 8, height - 8, color(0, 0, 0, 220));
    rect(pixels, 2, 2, width - 8, height - 8, color(236, 236, 236));
    rect(pixels, 4, 4, width - 12, height - 12, color(16, 16, 28, 245));
    const char *titles[] = {"SOLO", "CO-OP", "VERSUS"};
    text(pixels, stage_picker ? (picker_mode < 0 ? "STAGE 1" : titles[picker_mode]) : "PAUSE", 20, color(255, 232, 112));
    rect(pixels, 28, 45, width - 56, 1, color(112, 112, 128));
    const char *items[] = {"RESUME", replay ? "EXIT REPLAY" : "AGAIN?", "SETTINGS"};
    if (stage_picker) {
        items[0] = picker_mode < 0 ? "SOLO" : picker_mode == 0 ? "LAMMY" : "LAMMY & LAMMY";
        items[1] = picker_mode < 0 ? "CO-OP" : picker_mode == 0 ? "PARAPPA" : "LAMMY & PARAPPA";
        items[2] = picker_mode < 0 ? "VERSUS" : "BACK";
    }
    for (int i = 0; i < 3; i++) {
        if (i == selection) {
            rect(pixels, 24, 56 + i * 28, width - 48, 24, color(48, 56, 100));
            for (int k = 0; k < 5; k++) rect(pixels, 31 + k, 64 + i * 28 + k, 1, 9 - 2 * k, color(255, 232, 112));
        }
        text(pixels, items[i], 61 + i * 28, i == selection ? color(255, 232, 112) : color(255, 255, 255));
    }
}
