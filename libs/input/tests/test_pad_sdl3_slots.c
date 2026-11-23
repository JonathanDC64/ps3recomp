/* pad_sdl3_slots.h against stand-ins for SDL3's gamepad list; no hardware or SDL library needed.
 * cc -std=c17 -I libs/input libs/input/tests/test_pad_sdl3_slots.c -o test_pad_sdl3_slots
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned SDL_JoystickID;
typedef struct { SDL_JoystickID id; int connected, opens; } SDL_Gamepad;
static SDL_Gamepad pads[8];
static SDL_JoystickID order[8];   /* SDL's list, in its order */
static int num, open_fail;
static SDL_JoystickID* SDL_GetGamepads(int* count)
{
    SDL_JoystickID* a = (SDL_JoystickID*)malloc(sizeof(SDL_JoystickID) * (size_t)(num + 1));
    memcpy(a, order, sizeof(SDL_JoystickID) * (size_t)num);
    a[num] = 0;
    *count = num;
    return a;
}
static void SDL_free(void* p) { free(p); }
static SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id)
{
    if (open_fail) return NULL;
    SDL_Gamepad* p = &pads[id];
    ++p->opens;
    return p;
}
static int SDL_GamepadConnected(SDL_Gamepad* p) { return p->connected; }
static SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* p) { return p->id; }
static void SDL_CloseGamepad(SDL_Gamepad* p) { --p->opens; }
static void SDL_LockJoysticks(void) {}
static void SDL_UnlockJoysticks(void) {}
#include "pad_sdl3_slots.h"

static void add(SDL_JoystickID id)
{
    pads[id] = (SDL_Gamepad){ id, 1, 0 };
    order[num++] = id;
}
static void remove_pad(SDL_JoystickID id)
{
    pads[id].connected = 0;
    for (int i = 0; i < num; ++i)
        if (order[i] == id) {
            memmove(order + i, order + i + 1, (size_t)(num - i - 1) * sizeof order[0]);
            --num;
            break;
        }
}

int main(void)
{
    SDL_Gamepad* slots[4] = { 0 };
    pad_sdl3_sync_slots(slots, 4);
    assert(!slots[0]);
    add(3);   /* a pad switched on after startup */
    pad_sdl3_sync_slots(slots, 4);
    assert(slots[0] == &pads[3] && !slots[1]);
    puts("PASS: a late pad takes player one");
    add(5);
    pad_sdl3_sync_slots(slots, 4);
    pad_sdl3_sync_slots(slots, 4);   /* again: nothing opens twice */
    assert(slots[1] == &pads[5] && pads[3].opens == 1 && pads[5].opens == 1);
    puts("PASS: each pad opens once and keeps its slot");
    remove_pad(3);
    pad_sdl3_sync_slots(slots, 4);
    assert(!slots[0] && slots[1] == &pads[5] && !pads[3].opens);
    puts("PASS: a pad that goes away is closed, and the others stay where they are");
    add(6);
    pad_sdl3_sync_slots(slots, 4);
    assert(slots[0] == &pads[6] && slots[1] == &pads[5]);
    puts("PASS: a new pad takes the empty slot");
    add(1); add(2); add(4);   /* five pads, four slots */
    pad_sdl3_sync_slots(slots, 4);
    assert(slots[2] == &pads[1] && slots[3] == &pads[2] && !pads[4].opens);
    puts("PASS: a fifth pad waits for a slot");
    remove_pad(1);
    pad_sdl3_sync_slots(slots, 4);
    assert(slots[2] == &pads[4]);
    puts("PASS: it takes the first slot that empties");
    remove_pad(6);
    open_fail = 1;   /* a pad SDL lists but can't open stays out, and its slot stays free */
    add(7);
    pad_sdl3_sync_slots(slots, 4);
    assert(!slots[0]);
    open_fail = 0;
    pad_sdl3_sync_slots(slots, 4);
    assert(slots[0] == &pads[7]);
    puts("PASS: a pad that fails to open is tried again next sweep");
    return 0;
}
