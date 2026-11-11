/* The rules for the log's controller lines (pad_held.h): each device as it comes and goes, and a button held since it
 * came. No pad, OS or SDL needed.
 * cc -std=c17 -I libs/input libs/input/tests/test_pad_held.c -o test_pad_held
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pad_held.h"

#define RIGHT 0x0020u
#define CROSS 0x4000u
#define CIRCLE 0x2000u
#define PAD_A 0x102L   /* devices as pad_controller.h makes them: an SDL pad in slot 1, a HID pad in slot 0 */
#define PAD_B 0x10L

static PadHeldDevice t[PAD_HELD_PORTS];
static PadHeldEvent ev[PAD_HELD_EVENTS];
static long devs[PAD_HELD_PORTS];
static unsigned btns[PAD_HELD_PORTS];

/* One poll at `now`; returns the number of events. */
static int poll_at(unsigned long long now) { return pad_held_sweep(t, devs, btns, now, ev); }

/* Polls every 4 ms from `from` up to `to` (both included); returns the STUCK events seen, the last one in *last. */
static int run(unsigned long long from, unsigned long long to, PadHeldEvent* last)
{
    int stuck = 0;
    for (unsigned long long now = from; now <= to; now += 4) {
        const int n = poll_at(now);
        for (int k = 0; k < n; k++)
            if (ev[k].what == PAD_HELD_STUCK) {
                stuck++;
                if (last) *last = ev[k];
            }
    }
    return stuck;
}

static void reset(void)
{
    memset(t, 0, sizeof t);
    memset(devs, 0, sizeof devs);
    memset(btns, 0, sizeof btns);
}

int main(void)
{
    PadHeldEvent last;

    /* A device comes, then goes; nothing told while nothing changes. */
    reset();
    devs[0] = PAD_A;
    assert(poll_at(1000) == 1 && ev[0].what == PAD_HELD_CAME && ev[0].dev == PAD_A && ev[0].port == 0);
    assert(poll_at(1004) == 0);
    devs[0] = 0;
    assert(poll_at(1008) == 1 && ev[0].what == PAD_HELD_GONE && ev[0].dev == PAD_A && ev[0].port == -1);
    assert(poll_at(1012) == 0);
    puts("PASS: a device coming and going, once each");

    /* A button down from the start, never let go: told once, at PAD_HELD_FOR_MS. */
    reset();
    devs[0] = PAD_A;
    btns[0] = RIGHT;
    assert(run(0, PAD_HELD_FOR_MS - 4, NULL) == 0);
    assert(run(PAD_HELD_FOR_MS, PAD_HELD_FOR_MS + 30000, &last) == 1);
    assert(last.dev == PAD_A && last.port == 0 && last.buttons == RIGHT);
    puts("PASS: a button held since the device came, told once");

    /* Its first reports empty: a button down within PAD_HELD_EARLY_MS still counts. */
    reset();
    devs[0] = PAD_A;
    assert(run(0, 500, NULL) == 0);
    btns[0] = CROSS;
    assert(run(504, PAD_HELD_FOR_MS + 1000, &last) == 1 && last.buttons == CROSS);
    puts("PASS: a button down in the device's first second counts");

    /* Pressed later than that and held as long: a player's press, not told. */
    reset();
    devs[0] = PAD_A;
    assert(run(0, PAD_HELD_EARLY_MS + 100, NULL) == 0);
    btns[0] = CIRCLE;
    assert(run(PAD_HELD_EARLY_MS + 104, 3 * PAD_HELD_FOR_MS, NULL) == 0);
    puts("PASS: a button pressed after the first second is not told, however long it is held");

    /* Let go once: not told, even when it goes down again for good. */
    reset();
    devs[0] = PAD_A;
    btns[0] = RIGHT;
    assert(run(0, 5000, NULL) == 0);
    btns[0] = 0;
    assert(run(5004, 5004, NULL) == 0);
    btns[0] = RIGHT;
    assert(run(5008, 3 * PAD_HELD_FOR_MS, NULL) == 0);
    puts("PASS: a button let go once is not told");

    /* Two held from the start, one let go: the other is told alone. */
    reset();
    devs[0] = PAD_A;
    btns[0] = RIGHT | CROSS;
    assert(run(0, 2000, NULL) == 0);
    btns[0] = RIGHT;
    assert(run(2004, PAD_HELD_FOR_MS + 100, &last) == 1 && last.buttons == RIGHT);
    puts("PASS: of two held, the one never let go");

    /* A device that moves to another port (a HID pad when an XInput pad takes its port) is the same device. */
    reset();
    devs[0] = PAD_B;
    btns[0] = CROSS;
    assert(poll_at(0) == 1 && ev[0].what == PAD_HELD_CAME);
    assert(run(4, 3000, NULL) == 0);
    devs[0] = PAD_A;
    btns[0] = 0;
    devs[1] = PAD_B;
    btns[1] = CROSS;
    assert(poll_at(3004) == 1 && ev[0].what == PAD_HELD_CAME && ev[0].dev == PAD_A && ev[0].port == 0);
    assert(run(3008, PAD_HELD_FOR_MS + 100, &last) == 1 && last.dev == PAD_B && last.port == 1 && last.buttons == CROSS);
    puts("PASS: a device that moves port keeps its time");

    /* Gone and back: its time starts again, and it can be told again. */
    reset();
    devs[0] = PAD_A;
    btns[0] = RIGHT;
    assert(run(0, PAD_HELD_FOR_MS + 100, NULL) == 1);
    devs[0] = 0;
    btns[0] = 0;
    assert(poll_at(PAD_HELD_FOR_MS + 104) == 1 && ev[0].what == PAD_HELD_GONE);
    devs[0] = PAD_A;
    btns[0] = RIGHT;
    const unsigned long long back = PAD_HELD_FOR_MS + 108;
    assert(poll_at(back) == 1 && ev[0].what == PAD_HELD_CAME);
    assert(run(back + 4, back + PAD_HELD_FOR_MS - 4, NULL) == 0);
    assert(run(back + PAD_HELD_FOR_MS, back + PAD_HELD_FOR_MS + 100, NULL) == 1);
    puts("PASS: a device back after going is followed afresh");

    /* Four devices at once, and one gone while another comes in the same poll. */
    reset();
    for (int p = 0; p < PAD_HELD_PORTS; p++) devs[p] = 0x102L + 0x100L * p;
    assert(poll_at(0) == PAD_HELD_PORTS);
    devs[2] = PAD_B;
    const int n = poll_at(4);
    assert(n == 2 && ev[0].what == PAD_HELD_GONE && ev[0].dev == 0x302L && ev[1].what == PAD_HELD_CAME &&
           ev[1].dev == PAD_B && ev[1].port == 2);
    puts("PASS: four devices, and one taking another's place");

    puts("ALL PASS");
    return 0;
}
