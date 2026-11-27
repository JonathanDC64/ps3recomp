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

/* A deadzone given as real stick travel: tilt (0 to 1) is how far from centre the stick starts counting, the same
 * on every controller. Inside it the axes are exactly centred. Past it the distance runs from anti to 1 over the
 * travel left, so a title whose own deadzone ends at anti sees movement start at tilt, from nothing, with no jump.
 * Squircling grows in over that travel too (t: 0 at tilt, 1 at the rim); at full strength from the start a diagonal
 * would clear the title's zone before a straight push did. */
static inline void pad_stick_tilt_to_u8(float x, float y, float tilt, float anti, float squircle,
                                        uint8_t* out_x, uint8_t* out_y)
{
    x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
    y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
    const float mag = sqrtf(x * x + y * y);
    if (mag <= tilt) {
        *out_x = *out_y = 128;
        return;
    }
    const float m = mag < 1.0f ? mag : 1.0f;
    const float t = m >= 1.0f ? 1.0f : (m - tilt) / (1.0f - tilt);
    const float pos = anti + (1.0f - anti) * t;
    float len = pos * mag / m;                  /* past the circle: the push keeps its length */
    if (squircle > 0.0f) {
        const float s2 = 2.0f * x * y / (mag * mag);        /* sin(2 * angle) */
        len = pos * (1.0f + t * s2 * s2 / squircle);        /* pos: the distance held to the circle */
    }
    x *= len / mag;
    y *= len / mag;
    *out_x = pad_stick_axis_u8(x);
    *out_y = pad_stick_axis_u8(y);
}

/* An axial deadzone, for a stick that wavers a little sideways while it is pushed straight ahead (a worn or noisy
 * stick). The radial deadzone above acts on the distance from centre, so once the stick is past it every waver goes
 * through, and a title that turns the character to the stick direction at once shows each one, more often the higher
 * the frame rate. Here each axis closer to centre than zone (0 to 1 of full travel) reads 0, and past it the axis runs
 * from 0 to its end over the travel left, so a stick swept slowly across an axis has no step. It runs on the stick as
 * the host reads it, before the radial deadzone. 0: off. */
static inline void pad_stick_axial(float* x, float* y, float zone)
{
    if (zone <= 0.0f) return;
    if (zone >= 1.0f) {
        *x = *y = 0.0f;
        return;
    }
    const float k = 1.0f / (1.0f - zone);
    const float ax = fabsf(*x), ay = fabsf(*y);
    *x = ax <= zone ? 0.0f : copysignf((ax - zone) * k, *x);
    *y = ay <= zone ? 0.0f : copysignf((ay - zone) * k, *y);
}

/* Smoothing for one stick: a One-Euro filter (Casiez, Roussel and Vogel, CHI 2012). It is a low-pass filter whose
 * cutoff rises with the stick's speed: a stick held still or moved slowly is smoothed a lot, so a noisy stick's
 * flicker goes, and a quick push hardly at all, so a turn keeps its pace. Both axes share the cutoff, from the speed
 * of the stick as a whole, so the filter never bends the direction. The speed comes from the last reading, as in the
 * paper, and is itself smoothed at 1 Hz. Time is in seconds, so it smooths the same at any frame rate.
 *
 * level 1 light, 2 medium, 3 strong: a cutoff of 1.5, 1 and 0.5 Hz at rest, plus 6, 4 and 2 Hz for each unit of
 * travel a second. Measured at 30, 60 and 120 readings a second (test_pad_stick.c):
 * - held full forward with a sideways waver of up to 3% of travel at each reading, the facing that waver gives
 *   spreads 2.5 to 9 times less (the stronger the level and the more readings, the less);
 * - a 40 ms flick from forward to right arrives 0 to 17 ms later than unsmoothed;
 * - a stick moved slowly and steadily (half its travel a second) trails by 35, 53 and 107 ms, the same at every rate.
 * 0 (or anything else): off.
 *
 * dt: the seconds since the last call for this stick. The first call, and one after a gap of more than a quarter of a
 * second (the title stopped reading the pad), starts from the reading as it is. */
typedef struct {
    float x, y;       /* the output */
    float px, py;     /* the last reading */
    float dx, dy;     /* the speed, smoothed */
    int primed;
} PadStickSmooth;

static inline void pad_stick_smooth(PadStickSmooth* f, int level, float dt, float* x, float* y)
{
    static const float min_hz[4] = {0.0f, 1.5f, 1.0f, 0.5f}, per_speed[4] = {0.0f, 6.0f, 4.0f, 2.0f};
    if (level < 1 || level > 3) {
        f->primed = 0;
        return;
    }
    if (!f->primed || !(dt > 0.0f) || dt > 0.25f) {
        f->x = f->px = *x;
        f->y = f->py = *y;
        f->dx = f->dy = 0.0f;
        f->primed = 1;
        return;
    }
    const float two_pi = 6.28318531f;
    const float ad = 1.0f / (1.0f + 1.0f / (two_pi * 1.0f * dt));
    f->dx += ad * ((*x - f->px) / dt - f->dx);
    f->dy += ad * ((*y - f->py) / dt - f->dy);
    const float hz = min_hz[level] + per_speed[level] * sqrtf(f->dx * f->dx + f->dy * f->dy);
    const float a = 1.0f / (1.0f + 1.0f / (two_pi * hz * dt));
    f->px = *x;
    f->py = *y;
    f->x += a * (*x - f->x);
    f->y += a * (*y - f->y);
    *x = f->x;
    *y = f->y;
}

/* A deadzone percent from the environment, or -1 when it isn't one (unset, empty, not a number): -1 leaves the stick
 * on pad_stick_to_u8. Above 100 counts as 100. */
static inline int pad_stick_zone_percent(const char* e)
{
    if (!e || *e < '0' || *e > '9') return -1;
    int v = 0;
    for (; *e >= '0' && *e <= '9'; e++)
        if (v < 1000) v = v * 10 + (*e - '0');
    return *e ? -1 : v > 100 ? 100 : v;
}

#endif
