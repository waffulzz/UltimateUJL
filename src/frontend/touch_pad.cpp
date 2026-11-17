// On-screen touch controller (see touch_pad.h).
#include "touch_pad.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include "imgui.h"
#include "imgui_internal.h"   // ClearActiveID: a drag that scrolls stops being a press

namespace {

struct Pill { int bit; float cx, cy, hw, hh; const char *label; };
enum { P_L2, P_L1, P_R2, P_R1, P_SELECT, P_START, P_COUNT };

struct Layout {
    float W = 0, H = 0;
    TouchRect safe{}, free_r{};
    float dx = 0, dy = 0, dr = 0;           // d-pad center and radius
    float fx = 0, fy = 0, fo = 0, fr = 0;   // face buttons: diamond center, offset of each button, button radius
    Pill pill[P_COUNT];
    float mx = 0, my = 0, mhw = 0, mhh = 0; // menu button
} L;

struct Face { int bit; float ox, oy; };
const Face k_face[4] = {{B_TRIANGLE, 0, -1}, {B_CIRCLE, 1, 0}, {B_CROSS, 0, 1}, {B_SQUARE, -1, 0}};

bool g_pad_on, g_menu_on, g_menu_tap;
const char *g_start_label = "START";
uint64_t g_menu_until;                   // pad hidden: the menu button shows until then (ms) after a touch
struct Finger { float x, y; bool menu; };
std::map<SDL_FingerID, Finger> g_fingers;
uint16_t g_held;
uint16_t g_latched;   // pressed since touchpad_held() was last read: a tap shorter than a UI frame still counts

void layout(float w, float h, TouchRect s) {
    L.W = w;
    L.H = h;
    L.safe = s;
    float sw = s.x1 - s.x0, sh = s.y1 - s.y0;
    float col = std::min(std::max(sh * 0.36f, 96.0f), 190.0f);
    col = std::min(col, sw * 0.28f);
    float lx = s.x0 + col * 0.5f, rx = s.x1 - col * 0.5f;
    float R = std::min(col * 0.46f, sh * 0.2f);
    L.dx = lx; L.dy = s.y0 + sh * 0.6f; L.dr = R;
    L.fx = rx; L.fy = L.dy; L.fo = R * 0.64f; L.fr = R * 0.35f;
    float shw = col * 0.34f, shh = sh * 0.042f;
    L.pill[P_L2] = {B_L2, lx, s.y0 + sh * 0.165f, shw, shh, "L2"};
    L.pill[P_L1] = {B_L1, lx, s.y0 + sh * 0.275f, shw, shh, "L1"};
    L.pill[P_R2] = {B_R2, rx, s.y0 + sh * 0.165f, shw, shh, "R2"};
    L.pill[P_R1] = {B_R1, rx, s.y0 + sh * 0.275f, shw, shh, "R1"};
    L.pill[P_SELECT] = {B_SELECT, lx, s.y0 + sh * 0.91f, col * 0.3f, shh * 0.85f, "SELECT"};
    L.pill[P_START] = {B_START, rx, s.y0 + sh * 0.91f, col * 0.3f, shh * 0.85f, g_start_label};
    L.mx = lx; L.my = s.y0 + sh * 0.055f; L.mhw = col * 0.2f; L.mhh = sh * 0.036f;
    L.free_r = {s.x0 + col, 0, s.x1 - col, h};
}

bool menu_visible() { return g_menu_on && (g_pad_on || SDL_GetTicks() < g_menu_until); }

bool in_menu(float x, float y) {
    return std::fabs(x - L.mx) < L.mhw * 1.5f + 8 && std::fabs(y - L.my) < L.mhh * 1.5f + 8;
}

uint16_t hit(float x, float y) {
    uint16_t b = 0;
    float ddx = x - L.dx, ddy = y - L.dy, d = std::sqrt(ddx * ddx + ddy * ddy);
    if (d < L.dr * 1.35f) {   // 8-way: a component past cos(67.5) counts, so diagonals press two directions
        if (d < L.dr * 0.15f) return 0;
        float nx = ddx / d, ny = ddy / d;
        if (nx > 0.38f) b |= 1u << B_RIGHT;
        if (nx < -0.38f) b |= 1u << B_LEFT;
        if (ny > 0.38f) b |= 1u << B_DOWN;
        if (ny < -0.38f) b |= 1u << B_UP;
        return b;
    }
    float best = FLT_MAX;
    int bb = -1;
    for (const Face &f : k_face) {
        float cx = L.fx + f.ox * L.fo, cy = L.fy + f.oy * L.fo;
        float fd = std::hypot(x - cx, y - cy);
        if (fd < best) { best = fd; bb = f.bit; }
    }
    if (bb >= 0 && best < L.fo * 1.1f) return (uint16_t)(1u << bb);
    for (const Pill &p : L.pill)
        if (std::fabs(x - p.cx) < p.hw * 1.25f && std::fabs(y - p.cy) < p.hh * 1.6f) return (uint16_t)(1u << p.bit);
    return 0;
}

void recompute() {
    g_held = 0;
    if (!g_pad_on) { g_latched = 0; return; }
    for (auto &kv : g_fingers)
        if (!kv.second.menu) g_held |= hit(kv.second.x, kv.second.y);
    g_latched |= g_held;
}

} // namespace

void touchpad_update(float w, float h, TouchRect safe, bool pad_on, bool menu_on) {
    layout(w, h, safe);
    g_menu_on = menu_on;
    if (pad_on != g_pad_on) {
        g_pad_on = pad_on;
        g_fingers.clear();
        g_menu_until = 0;
    }
    recompute();
}

bool touchpad_visible() { return g_pad_on; }
void touchpad_start_label(const char *label) { g_start_label = label; }

bool touchpad_event(const SDL_Event &e) {
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION && e.type != SDL_EVENT_FINGER_UP &&
        e.type != SDL_EVENT_FINGER_CANCELED)
        return false;
    float x = e.tfinger.x * L.W, y = e.tfinger.y * L.H;
    SDL_FingerID id = e.tfinger.fingerID;
    static int log = -1;   // UJL_TOUCH_LOG=1: every finger event (dev)
    if (log < 0) log = getenv("UJL_TOUCH_LOG") != nullptr;
    if (log && e.type != SDL_EVENT_FINGER_MOTION)
        fprintf(stderr, "touch: %s touch %llu finger %llu at %.0f,%.0f (pad %d, menu %d)\n",
                e.type == SDL_EVENT_FINGER_DOWN ? "down" : e.type == SDL_EVENT_FINGER_UP ? "up" : "cancel",
                (unsigned long long)e.tfinger.touchID, (unsigned long long)id, x, y, g_pad_on, menu_visible());
    bool used = g_pad_on;
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        if (menu_visible() && in_menu(x, y)) {
            g_menu_tap = true;
            g_fingers[id] = {x, y, true};
            return true;
        }
        if (!g_pad_on) {   // no pad: a touch only brings up the menu button for a while
            g_menu_until = SDL_GetTicks() + 5000;
            return false;
        }
        g_fingers[id] = {x, y, false};
    } else if (e.type == SDL_EVENT_FINGER_MOTION) {
        auto it = g_fingers.find(id);
        if (it == g_fingers.end()) return used;
        it->second.x = x;
        it->second.y = y;
    } else {
        g_fingers.erase(id);
    }
    recompute();
    return used;
}

uint16_t touchpad_held() {
    uint16_t b = g_held | g_latched;
    g_latched = 0;
    return b;
}

void touchpad_drag_scroll() {
    ImGuiIO &io = ImGui::GetIO();
    if (io.MouseSource != ImGuiMouseSource_TouchScreen || !ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
    if (std::fabs(d.y) < 10.0f || std::fabs(d.y) < std::fabs(d.x)) return;   // a tap, or a horizontal slider drag
    if (ImGui::IsAnyItemActive()) ImGui::ClearActiveID();
    ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
}

bool touchpad_take_menu() {
    bool t = g_menu_tap;
    g_menu_tap = false;
    return t;
}

TouchRect touchpad_free_rect() { return g_pad_on ? L.free_r : TouchRect{0, 0, L.W, L.H}; }

void touchpad_fit(float aspect, float *x, float *y, float *w, float *h) {
    TouchRect f = touchpad_free_rect();
    float fw = f.x1 - f.x0, fh = f.y1 - f.y0;
    float pw = fw, ph = fw / aspect;
    if (ph > fh) { ph = fh; pw = ph * aspect; }
    if (g_pad_on) {   // keep the picture at least 80% of the screen height, even if it then runs under the buttons
        float minh = std::min(L.H * 0.8f, L.W / aspect);
        if (ph < minh) { ph = minh; pw = ph * aspect; }
    }
    *w = pw;
    *h = ph;
    *x = (f.x0 + f.x1) * 0.5f - pw * 0.5f;
    *y = (f.y0 + f.y1) * 0.5f - ph * 0.5f;
}

void touchpad_draw(ImDrawList *dl) {
    bool mv = menu_visible();
    if (!g_pad_on && !mv) return;
    const ImU32 fill = IM_COL32(255, 255, 255, 34), fill_on = IM_COL32(255, 255, 255, 120);
    const ImU32 edge = IM_COL32(255, 255, 255, 110), ink = IM_COL32(255, 255, 255, 200);
    float th = std::max(1.5f, L.dr * 0.035f);
    ImFont *font = ImGui::GetFont();
    auto label = [&](float cx, float cy, float size, const char *s, ImU32 col) {
        ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s);
        dl->AddText(font, size, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), col, s);
    };
    if (mv) {   // menu: three bars
        ImVec2 a(L.mx - L.mhw, L.my - L.mhh), b(L.mx + L.mhw, L.my + L.mhh);
        dl->AddRectFilled(a, b, fill, L.mhh);
        dl->AddRect(a, b, edge, L.mhh, 0, th);
        float bw = L.mhw * 0.45f, gap = L.mhh * 0.42f;
        for (int i = -1; i <= 1; i++)
            dl->AddLine(ImVec2(L.mx - bw, L.my + gap * (float)i), ImVec2(L.mx + bw, L.my + gap * (float)i), ink, th);
    }
    if (!g_pad_on) return;

    // d-pad: a plus of four arms, the pressed ones lit
    float a = L.dr * 0.34f, r = L.dr;
    auto on = [&](int bit) { return (g_held >> bit) & 1; };
    dl->AddRectFilled(ImVec2(L.dx - r, L.dy - a), ImVec2(L.dx + r, L.dy + a), fill, a * 0.4f);
    dl->AddRectFilled(ImVec2(L.dx - a, L.dy - r), ImVec2(L.dx + a, L.dy - a), fill, a * 0.4f, ImDrawFlags_RoundCornersTop);
    dl->AddRectFilled(ImVec2(L.dx - a, L.dy + a), ImVec2(L.dx + a, L.dy + r), fill, a * 0.4f, ImDrawFlags_RoundCornersBottom);
    struct Arm { int bit; float ox, oy; } arms[4] = {{B_UP, 0, -1}, {B_RIGHT, 1, 0}, {B_DOWN, 0, 1}, {B_LEFT, -1, 0}};
    for (const Arm &m : arms) {
        float cx = L.dx + m.ox * (r + a) * 0.5f, cy = L.dy + m.oy * (r + a) * 0.5f;
        float hw = m.ox != 0 ? (r - a) * 0.5f : a, hh = m.oy != 0 ? (r - a) * 0.5f : a;
        if (on(m.bit)) dl->AddRectFilled(ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), fill_on, a * 0.3f);
        // arrow pointing outwards
        float t = a * 0.5f, px = -m.oy, py = m.ox;   // perpendicular
        ImVec2 tip(cx + m.ox * t, cy + m.oy * t);
        ImVec2 b1(cx - m.ox * t * 0.4f + px * t, cy - m.oy * t * 0.4f + py * t);
        ImVec2 b2(cx - m.ox * t * 0.4f - px * t, cy - m.oy * t * 0.4f - py * t);
        dl->AddTriangleFilled(tip, b1, b2, ink);
    }
    // outline of the plus
    const ImVec2 plus[12] = {{L.dx - a, L.dy - r}, {L.dx + a, L.dy - r}, {L.dx + a, L.dy - a}, {L.dx + r, L.dy - a},
                             {L.dx + r, L.dy + a}, {L.dx + a, L.dy + a}, {L.dx + a, L.dy + r}, {L.dx - a, L.dy + r},
                             {L.dx - a, L.dy + a}, {L.dx - r, L.dy + a}, {L.dx - r, L.dy - a}, {L.dx - a, L.dy - a}};
    dl->AddPolyline(plus, 12, edge, ImDrawFlags_Closed, th);

    // face buttons with their symbols in the PlayStation colors
    for (const Face &f : k_face) {
        float cx = L.fx + f.ox * L.fo, cy = L.fy + f.oy * L.fo, br = L.fr, s = br * 0.48f;
        dl->AddCircleFilled(ImVec2(cx, cy), br, on(f.bit) ? fill_on : fill, 32);
        dl->AddCircle(ImVec2(cx, cy), br, edge, 32, th);
        float sth = std::max(2.0f, br * 0.11f);
        switch (f.bit) {
        case B_TRIANGLE:
            dl->AddTriangle(ImVec2(cx, cy - s), ImVec2(cx + s * 0.95f, cy + s * 0.65f), ImVec2(cx - s * 0.95f, cy + s * 0.65f),
                            IM_COL32(80, 225, 170, 240), sth);
            break;
        case B_CIRCLE:
            dl->AddCircle(ImVec2(cx, cy), s * 0.85f, IM_COL32(255, 100, 100, 240), 24, sth);
            break;
        case B_CROSS:
            dl->AddLine(ImVec2(cx - s * 0.75f, cy - s * 0.75f), ImVec2(cx + s * 0.75f, cy + s * 0.75f), IM_COL32(120, 165, 255, 240), sth);
            dl->AddLine(ImVec2(cx + s * 0.75f, cy - s * 0.75f), ImVec2(cx - s * 0.75f, cy + s * 0.75f), IM_COL32(120, 165, 255, 240), sth);
            break;
        case B_SQUARE:
            dl->AddRect(ImVec2(cx - s * 0.7f, cy - s * 0.7f), ImVec2(cx + s * 0.7f, cy + s * 0.7f), IM_COL32(255, 150, 215, 240), 0, 0, sth);
            break;
        }
    }

    // shoulder buttons, Select, Start
    for (const Pill &p : L.pill) {
        ImVec2 a0(p.cx - p.hw, p.cy - p.hh), a1(p.cx + p.hw, p.cy + p.hh);
        dl->AddRectFilled(a0, a1, on(p.bit) ? fill_on : fill, p.hh);
        dl->AddRect(a0, a1, edge, p.hh, 0, th);
        label(p.cx, p.cy, p.hh * (p.bit == B_SELECT || p.bit == B_START ? 0.95f : 1.25f), p.label, ink);
    }
}
