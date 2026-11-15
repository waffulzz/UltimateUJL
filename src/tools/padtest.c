/* Lists joysticks/gamepads SDL can see, then prints button changes for a few seconds.
   usage: ujl_padtest [seconds] */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) { printf("SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    int nj = 0, ng = 0;
    SDL_JoystickID *js = SDL_GetJoysticks(&nj);
    SDL_JoystickID *gs = SDL_GetGamepads(&ng);
    printf("joysticks: %d   gamepads: %d\n", nj, ng);
    for (int i = 0; i < nj; i++) {
        SDL_GUID guid = SDL_GetJoystickGUIDForID(js[i]);
        char g[64];
        SDL_GUIDToString(guid, g, sizeof g);
        printf("  [%u] %s  vid=%04x pid=%04x gamepad=%d guid=%s\n", (unsigned)js[i], SDL_GetJoystickNameForID(js[i]),
               SDL_GetJoystickVendorForID(js[i]), SDL_GetJoystickProductForID(js[i]), SDL_IsGamepad(js[i]), g);
    }
    SDL_Gamepad *pads[8] = {0};
    for (int i = 0; i < ng && i < 8; i++) {
        pads[i] = SDL_OpenGamepad(gs[i]);
        printf("  open gamepad %u: %s (%s)\n", (unsigned)gs[i], pads[i] ? SDL_GetGamepadName(pads[i]) : "FAILED", pads[i] ? "ok" : SDL_GetError());
    }
    uint64_t end = SDL_GetTicks() + (uint64_t)secs * 1000;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) printf("  button down: %s\n", SDL_GetGamepadStringForButton((SDL_GamepadButton)e.gbutton.button));
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) printf("  added %u\n", (unsigned)e.gdevice.which);
        }
        SDL_Delay(5);
    }
    SDL_free(js);
    SDL_free(gs);
    SDL_Quit();
    return 0;
}
