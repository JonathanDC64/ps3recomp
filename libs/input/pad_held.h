/* The log's lines for the controllers on the host ports (cellPad.c, pad_tell_devices): each one as it comes and goes,
 * and a button one has reported held from the moment it came, never let go. When a player finds a button pressed that
 * nobody presses, those lines say which device it is and which button. In a header of its own, with no pad, OS or SDL
 * in it, so tests/test_pad_held.c can run the rules.
 *
 * Each poll gives the device on each host port (pad_controller.h's; RUMBLE_NONE, 0, for none) and its buttons
 * (CELL_PAD_CTRL_* bits, before PAD_MAP). A device is followed by what it is, not by its port: a PlayStation pad over
 * HID moves along when an XInput pad takes its port. A button counts as held since the device came when it was down
 * in the device's first PAD_HELD_EARLY_MS and at every poll since. Once such a button has been down PAD_HELD_FOR_MS,
 * it is told, once per connection. */
#ifndef PS3RECOMP_PAD_HELD_H
#define PS3RECOMP_PAD_HELD_H

#define PAD_HELD_PORTS    4
#define PAD_HELD_EARLY_MS 1000ULL    /* down this soon after the device came (its first reports can be empty) */
#define PAD_HELD_FOR_MS   20000ULL   /* and still down this long after it came */

enum { PAD_HELD_CAME = 1, PAD_HELD_GONE = 2, PAD_HELD_STUCK = 3 };

typedef struct {
    long dev;                   /* the device followed; 0: a free entry */
    unsigned long long since;   /* when it came */
    unsigned held;              /* its buttons down since then, without a let-go */
    int told;                   /* its held line is out */
} PadHeldDevice;

typedef struct {
    int what;                   /* PAD_HELD_CAME, _GONE or _STUCK */
    long dev;
    int port;                   /* its host port now; -1 for PAD_HELD_GONE */
    unsigned buttons;           /* PAD_HELD_STUCK: the buttons held since it came */
} PadHeldEvent;

#define PAD_HELD_EVENTS (3 * PAD_HELD_PORTS)   /* the most one poll can give */

/* One poll. t: PAD_HELD_PORTS entries, zeroed before the first poll. devs and buttons: one per host port. What to tell
 * goes into out (PAD_HELD_EVENTS entries); returns how many. */
static int pad_held_sweep(PadHeldDevice* t, const long* devs, const unsigned* buttons, unsigned long long now,
                          PadHeldEvent* out)
{
    int n = 0, at[PAD_HELD_PORTS];   /* each entry's port at this poll, -1 for none */
    for (int e = 0; e < PAD_HELD_PORTS; e++) {   /* the devices followed: still here, or gone */
        at[e] = -1;
        for (int p = 0; t[e].dev && p < PAD_HELD_PORTS; p++)
            if (devs[p] == t[e].dev) {
                at[e] = p;
                break;
            }
        if (t[e].dev && at[e] < 0) {
            const PadHeldEvent gone = {PAD_HELD_GONE, t[e].dev, -1, 0};
            out[n++] = gone;
            t[e].dev = 0;
        }
    }
    for (int p = 0; p < PAD_HELD_PORTS; p++) {   /* the devices new at this poll */
        if (!devs[p]) continue;
        int known = 0, free_entry = -1;
        for (int e = 0; e < PAD_HELD_PORTS; e++) {
            if (t[e].dev == devs[p]) known = 1;
            else if (!t[e].dev && free_entry < 0) free_entry = e;
        }
        if (known || free_entry < 0) continue;
        const PadHeldDevice d = {devs[p], now, 0, 0};
        t[free_entry] = d;
        at[free_entry] = p;
        const PadHeldEvent came = {PAD_HELD_CAME, devs[p], p, 0};
        out[n++] = came;
    }
    for (int e = 0; e < PAD_HELD_PORTS; e++) {   /* the held buttons */
        if (at[e] < 0) continue;
        PadHeldDevice* d = &t[e];
        const unsigned b = buttons[at[e]];
        if (now - d->since < PAD_HELD_EARLY_MS) d->held |= b;
        d->held &= b;
        if (!d->told && d->held && now - d->since >= PAD_HELD_FOR_MS) {
            d->told = 1;
            const PadHeldEvent stuck = {PAD_HELD_STUCK, d->dev, at[e], d->held};
            out[n++] = stuck;
        }
    }
    return n;
}

#endif
