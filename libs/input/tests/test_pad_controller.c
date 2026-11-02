/* PAD_CONTROLLER's rules (pad_controller.h); no pad, OS or SDL needed.
 * cc -std=c17 -I libs/input libs/input/tests/test_pad_controller.c -o test_pad_controller
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pad_controller.h"

int main(void) {
    /* Names: XInput users (or SDL slots) 1-4, PlayStation HID slots up to hid_max. */
    assert(pad_controller_parse(NULL, 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("", 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("auto", 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("controller1", 0, 4) == PAD_DEV(RUMBLE_XINPUT, 0));
    assert(pad_controller_parse("controller4", 0, 4) == PAD_DEV(RUMBLE_XINPUT, 3));
    assert(pad_controller_parse("controller2", 1, 0) == PAD_DEV(RUMBLE_SDL, 1));
    assert(pad_controller_parse("playstation1", 0, 4) == (RUMBLE_HID | 0));
    assert(pad_controller_parse("playstation4", 0, 4) == (RUMBLE_HID | 3));
    assert(pad_controller_parse("playstation1", 1, 0) == RUMBLE_NONE);   /* off Windows: SDL has every pad */
    assert(pad_controller_parse("controller0", 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("controller5", 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("controller12", 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("playstation5", 0, 4) == RUMBLE_NONE);
    assert(pad_controller_parse("Controller1", 0, 4) == RUMBLE_NONE);
    puts("PASS: names parse, and anything else is auto");

    /* No device of one kind reads as another's. */
    for (int i = 0; i < 4; i++) {
        assert(!(PAD_DEV(RUMBLE_XINPUT, i) & RUMBLE_HID) && !(PAD_DEV(RUMBLE_SDL, i) & RUMBLE_HID));
        assert(pad_dev_index(PAD_DEV(RUMBLE_XINPUT, i)) == i && pad_dev_index(PAD_DEV(RUMBLE_SDL, i)) == i);
        for (int j = 0; j < 4; j++) assert(PAD_DEV(RUMBLE_XINPUT, i) != (RUMBLE_HID | j));
    }
    puts("PASS: devices stay apart");

    /* An Xbox pad on port 0, a DualSense on port 1 (the ports XInput left empty). */
    const long devs[4] = {PAD_DEV(RUMBLE_XINPUT, 0), RUMBLE_HID | 0, RUMBLE_NONE, RUMBLE_NONE};
    assert(pad_controller_find(devs, 4, pad_controller_parse("playstation1", 0, 4)) == 1);
    assert(pad_controller_find(devs, 4, pad_controller_parse("controller1", 0, 4)) == 0);
    assert(pad_controller_find(devs, 4, pad_controller_parse("controller2", 0, 4)) == -1);
    assert(pad_controller_find(devs, 4, pad_controller_parse("playstation2", 0, 4)) == -1);
    assert(pad_controller_find(devs, 4, RUMBLE_NONE) == -1);   /* auto never moves a pad */
    puts("PASS: the chosen pad's port is found, or none");

    char name[16];
    assert(pad_controller_name(devs[0], name, sizeof name) && !strcmp(name, "controller1"));
    assert(pad_controller_name(devs[1], name, sizeof name) && !strcmp(name, "playstation1"));
    assert(pad_controller_name(PAD_DEV(RUMBLE_SDL, 2), name, sizeof name) && !strcmp(name, "controller3"));
    assert(!pad_controller_name(RUMBLE_NONE, name, sizeof name));
    for (int i = 0; i < 4; i++) {   /* a name reads back as its device */
        assert(pad_controller_name(PAD_DEV(RUMBLE_XINPUT, i), name, sizeof name));
        assert(pad_controller_parse(name, 0, 4) == PAD_DEV(RUMBLE_XINPUT, i));
        assert(pad_controller_name(RUMBLE_HID | i, name, sizeof name));
        assert(pad_controller_parse(name, 0, 4) == (RUMBLE_HID | i));
    }
    puts("PASS: names round-trip");
    return 0;
}
