/*
 * pad_stick -- one host stick to the PS3's two 0-255 axis bytes, as RPCS3's
 * pad handlers make them: a radial deadzone (NormalizeStickDeadzone), then
 * squircling (ConvertToSquirclePoint).
 *
 * The deadzone acts on the stick's distance from centre, never on one axis
 * alone, so the direction comes through as the host reads it. The per-axis
 * deadzones this replaces held one axis at centre while the other was
 * outside: a stick swept past up, down, left or right read as exactly that
 * direction for a while and then jumped by the deadzone's width. The game
 * turns the character to the stick direction at once, so the turn looked
 * jerky (more so at high frame rates, where each step is drawn).
 *
 * Squircling: a DualShock 3 reports a full push on a diagonal out near the
 * corner of its square range, and games were made with that. Today's pads
 * stop on a circle, about 0.71 on each axis. Squircling stretches the circle
 * toward the square: the distance is multiplied by
 * 1 + sin^2(2 * angle) / (factor / 1000). Up, down, left and right stay as
 * they are, the direction never changes, and a full diagonal comes out at
 * 1.125 times the circle with a factor of 8000 (0.80 on each axis) and 1.25
 * with 4000 (0.88). RPCS3 squircles every pad but the DualShock 3: 8000 in
 * its older profiles, 4000 in newer ones.
 *
 * x, y: -1..1, right and down positive (the PS3 reads up as 0).
 * deadzone: 0..1 of full travel. anti: 0..1, the output at the deadzone's
 * edge. Inside the deadzone the distance goes from 0 to anti, outside from
 * anti to 1, so there is no step anywhere. RPCS3's defaults are a deadzone
 * of 40/255 for the DualSense and DualShock 4 and 7849/32767 for XInput,
 * with anti 13% for both. squircle: the factor / 1000 (8 for 8000), 0 off.
 * Without squircling a push past the circle keeps its length, as RPCS3
 * keeps it and a DualShock 3 reports it; squircling starts from the circle.
 */
#ifndef PS3RECOMP_PAD_STICK_H
#define PS3RECOMP_PAD_STICK_H

#include <math.h>
#include <stdint.h>

/* PAD_STICK_SQUIRCLE's default, the factor in RPCS3's older DualSense,
 * DualShock 4 and XInput profiles. */
#define PAD_STICK_SQUIRCLE_DEFAULT 8000

static inline uint8_t pad_stick_axis_u8(float v)
{
    long b = 128 + lroundf(v * 128.0f);
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

static inline void pad_stick_to_u8(float x, float y, float deadzone, float anti, float squircle,
                                   uint8_t* out_x, uint8_t* out_y)
{
    x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
    const float mag = sqrtf(x * x + y * y);
    if (mag > 0.0f) {
        const float m = mag < 1.0f ? mag : 1.0f;
        float pos;
        if (deadzone <= 0.0f || m > deadzone)
            pos = deadzone >= 1.0f ? 1.0f : anti + (1.0f - anti) * (m - deadzone) / (1.0f - deadzone);
        else
            pos = anti * m / deadzone;
        float len = pos * mag / m;              /* past the circle: the push keeps its length */
        if (squircle > 0.0f) {
            const float s2 = 2.0f * x * y / (mag * mag);   /* sin(2 * angle) */
            len = pos * (1.0f + s2 * s2 / squircle);       /* pos: the distance held to the circle */
        }
        x *= len / mag;
        y *= len / mag;
    }
    *out_x = pad_stick_axis_u8(x);
    *out_y = pad_stick_axis_u8(y);
}

#endif
