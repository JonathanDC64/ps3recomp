/* SDL3's gamepads in the PS3 player slots (cellPad.c's SDL3 backend). SDL3
 * lists gamepads by instance ID, and only gamepads: a slot keeps its pad until
 * that pad goes away, and a new pad takes the first empty slot, so a pad that
 * comes or goes moves no other player. SDL must be initialized before calling
 * it. tests/test_pad_sdl3_slots.c runs it against stand-ins for SDL. */
#ifndef PS3RECOMP_PAD_SDL3_SLOTS_H
#define PS3RECOMP_PAD_SDL3_SLOTS_H
static void pad_sdl3_sync_slots(SDL_Gamepad** slots, int count)
{
    SDL_LockJoysticks();
    /* Release lost pads before looking for replacements in this sweep. */
    for (int slot = 0; slot < count; ++slot) {
        if (slots[slot] && !SDL_GamepadConnected(slots[slot])) {
            SDL_CloseGamepad(slots[slot]);
            slots[slot] = NULL;
        }
    }
    int num = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&num);
    for (int k = 0; ids && k < num; ++k) {
        int vacant = -1, present = 0;
        for (int slot = 0; slot < count; ++slot) {
            if (!slots[slot]) {
                if (vacant < 0) vacant = slot;
            } else if (SDL_GetGamepadID(slots[slot]) == ids[k]) {
                present = 1;
                break;
            }
        }
        if (!present && vacant >= 0) slots[vacant] = SDL_OpenGamepad(ids[k]);
    }
    SDL_free(ids);
    SDL_UnlockJoysticks();
}
#endif
