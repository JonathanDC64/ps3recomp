/*
 * pad_stick -- one host stick to the PS3's two 0-255 axis bytes, with a
 * radial deadzone, as RPCS3's PadHandlerBase::NormalizeStickDeadzone does.
 *
 * The deadzone acts on the stick's distance from centre, never on one axis
 * alone, so the direction comes through as the host reads it. The per-axis
 * deadzones this replaces held one axis at centre while the other was
 * outside: a stick swept past up, down, left or right read as exactly that
 * direction for a while and then jumped by the deadzone's width. The game
 * turns the character to the stick direction at once, so the turn looked
 * jerky (more so at high frame rates, where each step is drawn).
 *
 * x, y: -1..1, right and down positive (the PS3 reads up as 0).
 * deadzone: 0..1 of full travel. anti: 0..1, the output at the deadzone's
 * edge. Inside the deadzone the distance goes from 0 to anti, outside from
 * anti to 1, so there is no step anywhere. RPCS3's defaults are a deadzone
 * of 40/255 for the DualSense and DualShock 4 and 7849/32767 for XInput,
 * with anti 13% for both; with those the DualSense's output is close to its
 * raw values, as on a PS3.
 */
#ifndef PS3RECOMP_PAD_STICK_H
#define PS3RECOMP_PAD_STICK_H

#include <math.h>
#include <stdint.h>

static inline uint8_t pad_stick_axis_u8(float v)
{
    long b = 128 + lroundf(v * 128.0f);
    return (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
}

static inline void pad_stick_to_u8(float x, float y, float deadzone, float anti,
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
        x *= pos / mag;
        y *= pos / mag;
    }
    *out_x = pad_stick_axis_u8(x);
    *out_y = pad_stick_axis_u8(y);
}

#endif
