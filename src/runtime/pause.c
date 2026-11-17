#include "pause.h"
#include <SDL3/SDL.h>

static SDL_AtomicInt reasons, parked;

void rt_pause_set(int reason, int on) {
    int before, after;
    do {
        before = SDL_GetAtomicInt(&reasons);
        after = on ? before | reason : before & ~reason;
    } while (!SDL_CompareAndSwapAtomicInt(&reasons, before, after));
}
int rt_pause_requested(void) { return SDL_GetAtomicInt(&reasons) != 0; }
int rt_pause_active(void) { return SDL_GetAtomicInt(&parked); }
uint64_t rt_pause_wait(void) {
    if (!rt_pause_requested()) return 0;
    uint64_t start = SDL_GetTicksNS();
    /* Guest threads pass a single baton. Parking its owner also stops CD/SPU/timer callbacks and every
       other guest thread. Publish only once it is safe for the UI to inspect the frozen game state. */
    SDL_SetAtomicInt(&parked, 1);
    while (rt_pause_requested()) SDL_Delay(1);
    SDL_SetAtomicInt(&parked, 0);
    return SDL_GetTicksNS() - start;
}
