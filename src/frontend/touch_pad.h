// On-screen touch controller: a PS1 pad (d-pad, the four face buttons, L1/L2/R1/R2, Select/Start) drawn over the
// frontend with ImGui, plus a menu button that opens the settings. Multi-touch: every finger presses what it is
// on, and sliding a finger moves the press. Its buttons are OR-ed into the same pad word the physical controllers
// and the keyboard feed (main.cpp, g_input_buttons -> g_pad_buttons[0] at each vblank).
//
// The buttons sit in two columns at the screen's sides. While the pad shows, main.cpp fits the game picture into
// the space between them (touchpad_fit), so in 4:3 and widescreen alike the buttons stay in the pillarbox; a
// picture that would get too small (iPad, wide aspects) is allowed to run under the translucent buttons instead.
#pragma once
#include <cstdint>

union SDL_Event;
struct ImDrawList;

// PS1 digital pad bits (active low on the wire; the touch pad reports them active high)
enum { B_SELECT = 0, B_L3, B_R3, B_START, B_UP, B_RIGHT, B_DOWN, B_LEFT, B_L2, B_R2, B_L1, B_R1,
       B_TRIANGLE, B_CIRCLE, B_CROSS, B_SQUARE };

// Setting "touch_controls": 0 auto (shown while no controller is connected), 1 always, 2 never
enum { TOUCH_AUTO = 0, TOUCH_ALWAYS, TOUCH_NEVER };

struct TouchRect { float x0, y0, x1, y1; };

// Once per frame, before input is read: screen size and safe area (ImGui display units, i.e. window points),
// whether the pad shows, and whether a menu button is offered at all (touch screens). With the pad hidden the
// menu button still appears for a few seconds after the screen is touched.
void touchpad_update(float w, float h, TouchRect safe, bool pad_on, bool menu_on);
bool touchpad_visible();
// Feed every SDL event. True = a finger event the pad used.
bool touchpad_event(const SDL_Event &e);
uint16_t touchpad_held();           // buttons held (1 << B_x)
bool touchpad_take_menu();          // the menu button was tapped since the last call
void touchpad_draw(ImDrawList *dl);
// Inside an ImGui window (between Begin and End): a vertical touch drag scrolls it, like a phone list, even when it
// starts on a button (which then isn't pressed). ImGui itself only scrolls with a wheel or the scrollbar.
void touchpad_drag_scroll();
// Free space between the button columns (the whole screen while the pad is hidden).
TouchRect touchpad_free_rect();
// Place a picture of this aspect: inside the free space, or bigger (under the buttons) where that would be tiny.
void touchpad_fit(float aspect, float *x, float *y, float *w, float *h);
