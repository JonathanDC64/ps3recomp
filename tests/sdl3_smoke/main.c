/*
 * ps3recomp - tests/sdl3_smoke/main.c
 *
 * The pinned SDL3 links, is the version cmake/sdl3_dep.cmake names, and starts its gamepad subsystem with
 * no display and no controller (a CI runner has neither). It lists what it finds, so a run on a PC with
 * pads shows them. On Windows it also loads libusb-1.0.dll, as SDL does for the Switch 2 controllers
 * (cmake/libusb_dep.cmake), checks its version and starts libusb. Exit 0 on success.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#ifdef PS3RECOMP_LIBUSB_VERSION
#include <windows.h>
#include <stdint.h>
/* libusb.h's struct libusb_version and the two calls, as the DLL exports them. */
struct usb_version { uint16_t major, minor, micro, nano; const char* rc; const char* describe; };
typedef const struct usb_version* (__stdcall* usb_get_version_fn)(void);
typedef int (__stdcall* usb_init_fn)(void** ctx);
typedef void (__stdcall* usb_exit_fn)(void* ctx);

static int check_libusb(void)
{
    HMODULE m = LoadLibraryA("libusb-1.0.dll");
    if (!m) { printf("FAIL: libusb-1.0.dll did not load (%lu)\n", (unsigned long)GetLastError()); return 0; }
    usb_get_version_fn ver = (usb_get_version_fn)(void*)GetProcAddress(m, "libusb_get_version");
    usb_init_fn init = (usb_init_fn)(void*)GetProcAddress(m, "libusb_init");
    usb_exit_fn fini = (usb_exit_fn)(void*)GetProcAddress(m, "libusb_exit");
    if (!ver || !init || !fini) { printf("FAIL: libusb-1.0.dll lacks libusb_get_version/init/exit\n"); return 0; }
    const struct usb_version* v = ver();
    char have[32];
    snprintf(have, sizeof have, "%u.%u.%u", v->major, v->minor, v->micro);
    printf("libusb %s (nano %u)\n", have, v->nano);
    if (strcmp(have, PS3RECOMP_LIBUSB_VERSION) != 0) {
        printf("FAIL: libusb-1.0.dll is %s, cmake/libusb_dep.cmake pins %s\n", have, PS3RECOMP_LIBUSB_VERSION);
        return 0;
    }
    void* ctx = NULL;
    const int rc = init(&ctx);
    if (rc != 0) { printf("FAIL: libusb_init: %d\n", rc); return 0; }
    fini(ctx);
    return 1;
}
#endif

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
#ifdef PS3RECOMP_LIBUSB_VERSION
    if (!check_libusb()) return 1;
#endif
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
