// Exercise real guest suspension across a host thread and controller transition/release semantics.
#include "pause.h"
#include "pause_menu.h"
#include "touch_pad.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>

static SDL_AtomicInt finished;
static uint64_t held_ns;
static int SDLCALL guest(void *) { held_ns = rt_pause_wait(); SDL_SetAtomicInt(&finished, 1); return 0; }
int main() {
    assert(SDL_Init(0));
    rt_pause_set(RT_PAUSE_USER, 1);
    SDL_Thread *thread = SDL_CreateThread(guest, "pause-check", nullptr); assert(thread);
    Uint64 deadline = SDL_GetTicks() + 5000;
    while (!rt_pause_active() && SDL_GetTicks() < deadline) SDL_Delay(1);
    assert(rt_pause_active());
    SDL_Delay(40); assert(!SDL_GetAtomicInt(&finished));
    rt_pause_set(RT_PAUSE_BACKGROUND, 1);
    rt_pause_set(RT_PAUSE_USER, 0);
    SDL_Delay(40); assert(!SDL_GetAtomicInt(&finished) && rt_pause_active());
    rt_pause_set(RT_PAUSE_BACKGROUND, 0);
    SDL_WaitThread(thread, nullptr);
    assert(SDL_GetAtomicInt(&finished) && !rt_pause_active() && held_ns >= 80000000);
    assert(rt_pause_wait() == 0);

    constexpr uint16_t start = 1 << B_START, down = 1 << B_DOWN, cross = 1 << B_CROSS, select = 1 << B_SELECT;
    PauseMenu p;
    assert(p.input(start, false, true, false) == PauseAction::None && p.game_buttons(start) == start);
    p.input(0, true, true, false);
    assert(p.input(start, true, true, false) == PauseAction::Open && p.visible);
    assert(p.input(start, true, true, false) == PauseAction::None && p.visible);
    assert(!p.game_buttons(start | cross | select));
    p.input(0, true, true, false);
    assert(p.input(start | cross, true, true, false) == PauseAction::Resume && !p.visible);
    assert(!p.game_buttons(start | cross));
    assert(p.game_buttons(select) == select); // effect cycling is unchanged
    assert(p.game_buttons(cross) == cross);   // released answer can become a fresh note
    p.input(0, true, true, false); p.input(start, true, true, false); p.input(0, true, true, false);
    p.input(down, true, true, false); p.input(0, true, true, false);
    assert(p.input(cross, true, true, false) == PauseAction::Again && !p.visible);
    p.input(0, true, false, false); p.input(start, true, false, false); p.input(0, true, false, false);
    assert(p.input(cross, true, false, false) == PauseAction::Resume); // JP answer layout: Cross cancels
    p.toggle(0);
    assert(p.choose(2, 0) == PauseAction::Settings && p.visible);
    assert(p.input(start, true, true, true) == PauseAction::None && p.visible);
    assert(PauseMenu::hit(100, 65) == 0 && PauseMenu::hit(100, 93) == 1 && PauseMenu::hit(100, 121) == 2);
    assert(PauseMenu::hit(0, 0) == -1);
    puts("pause: guest suspension, background/manual nesting, menu actions and held-button quarantine passed");
    SDL_Quit();
}
