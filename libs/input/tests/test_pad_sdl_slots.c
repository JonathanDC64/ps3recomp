/* Deterministic SDL device-list regression test; no hardware or SDL library needed.
 * cc -std=c17 -I libs/input libs/input/tests/test_pad_sdl_slots.c -o test_pad_sdl_slots
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int SDL_JoystickID;
typedef struct { int id, gamepad, attached, opens; } SDL_GameController;
static SDL_GameController devices[8];
static int order[8], num;
static int SDL_NumJoysticks(void) { return num; }
static int SDL_IsGameController(int i) { return i < num && devices[order[i]].gamepad; }
static SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int i) { return devices[order[i]].id; }
static SDL_GameController* SDL_GameControllerOpen(int i) {
    SDL_GameController* d = &devices[order[i]]; ++d->opens; return d;
}
static int SDL_GameControllerGetAttached(SDL_GameController* d) { return d->attached; }
static SDL_GameController* SDL_GameControllerGetJoystick(SDL_GameController* d) { return d; }
static SDL_JoystickID SDL_JoystickInstanceID(SDL_GameController* d) { return d->id; }
static void SDL_GameControllerClose(SDL_GameController* d) { --d->opens; }
static void SDL_LockJoysticks(void) {}
static void SDL_UnlockJoysticks(void) {}
#include "pad_sdl_slots.h"
static void add(int id, int gamepad) {
    devices[id] = (SDL_GameController){id, gamepad, 1, 0}; order[num++] = id;
}
static void remove_device(int id) {
    devices[id].attached = 0;
    for (int i = 0; i < num; ++i) if (order[i] == id) {
        memmove(order+i, order+i+1, (num-i-1)*sizeof(int)); --num; break;
    }
}
int main(void) {
    SDL_GameController* slots[4] = {0};
    add(0, 0); /* keyboard exposing a joystick interface */
    pad_sdl_sync_slots(slots, 4); assert(!slots[0]);
    add(1, 1); /* gamepad switched on after startup */
    pad_sdl_sync_slots(slots, 4);
    assert(slots[0] == &devices[1]); assert(!slots[1]);
    puts("PASS: late gamepad takes player one, skipping non-gamepads");
    add(2, 1); pad_sdl_sync_slots(slots, 4);
    assert(slots[1] == &devices[2]);
    remove_device(0); /* shifts all device indices, but not player assignments */
    pad_sdl_sync_slots(slots, 4);
    assert(slots[0] == &devices[1] && slots[1] == &devices[2]);
    assert(devices[1].opens == 1 && devices[2].opens == 1);
    puts("PASS: index changes do not duplicate controllers or move players");
    remove_device(1); pad_sdl_sync_slots(slots, 4);
    assert(!slots[0] && slots[1] == &devices[2] && !devices[1].opens);
    add(3, 1); pad_sdl_sync_slots(slots, 4);
    assert(slots[0] == &devices[3] && slots[1] == &devices[2]);
    puts("PASS: reconnection reuses the vacant player slot");
    remove_device(2); remove_device(3); pad_sdl_sync_slots(slots, 4);
    assert(!slots[0] && !slots[1]);
    puts("PASS: disconnected controllers are closed");
    return 0;
}
