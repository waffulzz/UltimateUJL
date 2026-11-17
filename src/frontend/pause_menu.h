// Native pause presentation: LETTER_E glyphs copied from the user's live game, no bundled font or widgets.
#pragma once
#include <cstdint>
#include <vector>

enum class PauseAction { None, Open, Resume, Again, Settings };
struct PauseMenu {
    bool visible = false;
    bool replay = false;
    int selection = 0;
    uint16_t previous = 0, blocked = 0, blocked2 = 0;   // blocked2: controller port 2
    PauseAction input(uint16_t held, bool available, bool confirm_cross, bool settings_open);
    PauseAction toggle(uint16_t held);
    PauseAction choose(int row, uint16_t held);
    uint16_t game_buttons(uint16_t held, int port = 0);
    static constexpr int width = 224, height = 160;
    static int hit(float x, float y);
    void capture_font(const uint8_t *ram, const uint16_t *vram); // only after rt_pause_active()
    void rasterize(std::vector<uint32_t> &pixels) const;
private:
    struct Glyph { int u, v, w, h, spacing; } glyphs[128]{};
    std::vector<uint32_t> font;
    int text_width(const char *s) const;
    void text(std::vector<uint32_t> &pixels, const char *s, int y, uint32_t tint) const;
};
