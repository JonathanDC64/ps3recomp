/* L2 and R2 as buttons from how far the trigger is in (pad_trigger.h). */
#include "pad_trigger.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* The knob's text. */
    assert(pad_trigger_knob(NULL) == -1);
    assert(pad_trigger_knob("") == -1);
    assert(pad_trigger_knob("x") == -1);
    assert(pad_trigger_knob("30x") == -1);
    assert(pad_trigger_knob("30") == 30);
    assert(pad_trigger_knob("30 ") == 30);
    assert(pad_trigger_knob("0") == 0);
    assert(pad_trigger_knob("-5") == 0);
    assert(pad_trigger_knob("254") == 254);
    assert(pad_trigger_knob("999") == 254);

    /* The default, 0-255: a trigger resting 1-5% in (3-13) is up; 12% and past is down. */
    for (unsigned v = 0; v <= 30; v++) assert(!pad_trigger_down_255(v, -1));
    for (unsigned v = 31; v <= 255; v++) assert(pad_trigger_down_255(v, -1));

    /* The knob, 0-255: 0 is any movement; 254 still leaves a full press. */
    assert(!pad_trigger_down_255(0, 0));
    assert(pad_trigger_down_255(1, 0));
    assert(!pad_trigger_down_255(60, 60));
    assert(pad_trigger_down_255(61, 60));
    assert(!pad_trigger_down_255(254, 254));
    assert(pad_trigger_down_255(255, 254));

    /* SDL3's range: today's 3000 by default; the knob taken from 255 to 32767. */
    assert(!pad_trigger_down_32767(3000, -1));
    assert(pad_trigger_down_32767(3001, -1));
    assert(!pad_trigger_down_32767(0, 0));
    assert(pad_trigger_down_32767(1, 0));
    assert(!pad_trigger_down_32767(3855, 30));   /* 30 of 255 is 3855 of 32767 */
    assert(pad_trigger_down_32767(3856, 30));
    assert(pad_trigger_down_32767(32767, 254));
    for (int k = 0; k <= 254; k++) {   /* the same point on both ranges, within one step of SDL3's */
        const int t = (k * 32767 + 127) / 255;
        assert(!pad_trigger_down_32767(t, k) && pad_trigger_down_32767(t + 1, k));
        assert(t >= k * 128 && t <= (k + 1) * 129);
    }

    printf("pad_trigger: ok\n");
    return 0;
}
