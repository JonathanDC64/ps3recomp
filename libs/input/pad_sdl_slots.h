/* SDL joystick device indices include non-gamepads and change on removal.
 * This helper owns the open controllers in the PS3 player slots.
 * SDL must be initialized before calling it. */
#ifndef PS3RECOMP_PAD_SDL_SLOTS_H
#define PS3RECOMP_PAD_SDL_SLOTS_H
static void pad_sdl_sync_slots(SDL_GameController** slots, int count)
{
    SDL_LockJoysticks();
    /* Release lost devices before looking for replacements in this sweep. */
    for (int slot = 0; slot < count; ++slot) {
        if (slots[slot] && !SDL_GameControllerGetAttached(slots[slot])) {
            SDL_GameControllerClose(slots[slot]);
            slots[slot] = NULL;
        }
    }
    const int num = SDL_NumJoysticks();
    for (int device = 0; device < num; ++device) {
        if (!SDL_IsGameController(device)) continue;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
        int vacant = -1, present = 0;
        for (int slot = 0; slot < count; ++slot) {
            if (!slots[slot]) {
                if (vacant < 0) vacant = slot;
            } else if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(slots[slot])) == id) {
                present = 1;
                break;
            }
        }
        if (!present && vacant >= 0) slots[vacant] = SDL_GameControllerOpen(device);
    }
    SDL_UnlockJoysticks();
}
#endif
