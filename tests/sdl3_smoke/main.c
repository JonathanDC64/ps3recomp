/*
 * ps3recomp - tests/sdl3_smoke/main.c
 *
 * The pinned SDL3 links, is the version cmake/sdl3_dep.cmake names, and starts its gamepad subsystem with
 * no display and no controller (a CI runner has neither). It lists what it finds, so a run on a PC with
 * pads shows them. Exit 0 on success.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    const int v = SDL_GetVersion();
    char have[32];
    snprintf(have, sizeof have, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    printf("SDL %s (%s), built against %d.%d.%d\n", have, SDL_GetRevision(),
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    if (strcmp(have, PS3RECOMP_SDL3_VERSION) != 0) {
        printf("FAIL: linked SDL %s, cmake/sdl3_dep.cmake pins %s\n", have, PS3RECOMP_SDL3_VERSION);
        return 1;
    }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("FAIL: SDL_Init(SDL_INIT_GAMEPAD): %s\n", SDL_GetError());
        return 1;
    }
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    printf("gamepads: %d\n", n);
    for (int i = 0; i < n; i++)
        printf("  %u: %s (type %s)\n", (unsigned)ids[i], SDL_GetGamepadNameForID(ids[i]),
               SDL_GetGamepadStringForType(SDL_GetGamepadTypeForID(ids[i])));
    SDL_free(ids);
    SDL_Quit();
    printf("ok\n");
    return 0;
}
