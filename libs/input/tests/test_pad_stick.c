/* pad_stick.h: the radial deadzone and RPCS3's squircling, one host stick to the PS3's axis bytes.
 *
 *   cc -std=gnu17 -I libs/input -o test_pad_stick libs/input/tests/test_pad_stick.c -lm
 *   clang-cl /I libs/input /Fe:test_pad_stick.exe libs/input/tests/test_pad_stick.c
 */
#include "pad_stick.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.14159265358979f

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { s_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                              printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The DualSense's settings (pad_hid_sony.c): deadzone 20/128, anti 13%. */
static const float DZ = 20.0f / 128.0f, ANTI = 0.13f;

static void conv(float x, float y, float sq, float* ox, float* oy)
{
    uint8_t bx, by;
    pad_stick_to_u8(x, y, DZ, ANTI, sq, &bx, &by);
    *ox = (bx - 128) / 128.0f;
    *oy = (by - 128) / 128.0f;
}

/* A title with its own deadzone, as Demon's Souls' stick function reads the two bytes: each to -1..1, then lengths up
 * to 0.37 are nothing, and the rest runs to 1 at 0.95. */
static const float GAME_ZONE = 0.37f, GAME_GAIN = 1.7241f;

static float game_length(uint8_t bx, uint8_t by)
{
    const float x = (bx / 255.0f - 0.5f) * 2.0f, y = (by / 255.0f - 0.5f) * 2.0f;
    return sqrtf(x * x + y * y);
}

static float game_output(uint8_t bx, uint8_t by)
{
    const float len = game_length(bx, by);
    if (len <= GAME_ZONE) return 0.0f;
    const float out = (len - GAME_ZONE) * GAME_GAIN;
    return out < 1.0f ? out : 1.0f;
}

/* A hash of every byte a deadzone gives over a grid of stick positions. */
static unsigned grid_hash(float dz, float anti, float sq)
{
    unsigned h = 2166136261u;
    for (int i = -64; i <= 64; i++)
        for (int j = -64; j <= 64; j++) {
            uint8_t b[2];
            pad_stick_to_u8(i / 64.0f, j / 64.0f, dz, anti, sq, &b[0], &b[1]);
            h = (h ^ b[0]) * 16777619u;
            h = (h ^ b[1]) * 16777619u;
        }
    return h;
}

/* Every deadzone from 0 to 70%: where the title's output first moves, and that it starts from nothing. */
static void check_tilt_zones(float sq)
{
    enum { STEPS = 2048 };
    for (int p = 0; p <= 70; p++) {
        const float tilt = p / 100.0f;
        for (int deg = 0; deg < 360; deg += 5) {
            const float a = deg * PI / 180.0f, ux = sinf(a), uy = -cosf(a);
            const int straight = deg % 90 == 0;
            float first = -1.0f, last = 0.0f, biggest = 0.0f;
            for (int k = 0; k <= STEPS; k++) {
                const float r = (float)k / STEPS;
                uint8_t bx, by;
                pad_stick_tilt_to_u8(r * ux, r * uy, tilt, GAME_ZONE, sq, &bx, &by);
                const float out = game_output(bx, by);
                if (r < tilt - 0.002f)
                    CHECK(bx == 128 && by == 128, "%d%% at %d deg: tilt %.3f is inside the zone, got (%u, %u)", p, deg, r, bx, by);
                if (out > 0.0f && first < 0.0f) {
                    first = r;
                    CHECK(out < 0.03f, "%d%% at %d deg: the first movement is already %.3f", p, deg, out);
                }
                const float step = fabsf(out - last);
                if (step > biggest) biggest = step;
                last = out;
            }
            CHECK(first >= tilt - 0.002f, "%d%% at %d deg: moved early, at %.4f", p, deg, first);
            CHECK(first <= tilt + (straight ? 0.003f : 0.016f), "%d%% at %d deg: first moved at %.4f", p, deg, first);
            CHECK(biggest < 0.03f, "%d%% at %d deg: a step of %.3f on the way out", p, deg, biggest);
            CHECK(last > 0.99f, "%d%% at %d deg: full tilt gives %.3f", p, deg, last);
        }
    }
}

int main(void)
{
    const float sqs[3] = {0.0f, 8.0f, 4.0f};
    uint8_t bx, by;

    /* At rest: centred, with or without squircling. */
    for (int i = 0; i < 3; i++) {
        pad_stick_to_u8(0.0f, 0.0f, DZ, ANTI, sqs[i], &bx, &by);
        CHECK(bx == 128 && by == 128, "rest with squircle %g: (%u, %u)", sqs[i], bx, by);
    }

    /* Up, down, left and right: squircling leaves them as they are. */
    for (int i = 0; i < 3; i++) {
        pad_stick_to_u8(1.0f, 0.0f, DZ, ANTI, sqs[i], &bx, &by);
        CHECK(bx == 255 && by == 128, "right with squircle %g: (%u, %u)", sqs[i], bx, by);
        pad_stick_to_u8(0.0f, -1.0f, DZ, ANTI, sqs[i], &bx, &by);
        CHECK(bx == 128 && by == 0, "up with squircle %g: (%u, %u)", sqs[i], bx, by);
        pad_stick_to_u8(-0.6f, 0.0f, DZ, ANTI, 0.0f, &bx, &by);
        uint8_t cx, cy;
        pad_stick_to_u8(-0.6f, 0.0f, DZ, ANTI, sqs[i], &cx, &cy);
        CHECK(bx == cx && by == cy, "left at 0.6 with squircle %g: (%u, %u) against (%u, %u)", sqs[i], cx, cy, bx, by);
    }

    /* A full push on the diagonal: 0.71 a side on the circle, 0.80 with 8000 (RPCS3's (229, 26) from the
     * DualSense), 0.88 with 4000. */
    {
        const float d = sqrtf(0.5f), want[3] = {0.7071f, 0.7955f, 0.8839f};
        for (int i = 0; i < 3; i++) {
            float ox, oy;
            conv(d, -d, sqs[i], &ox, &oy);
            CHECK(fabsf(ox - want[i]) < 0.01f && fabsf(oy + want[i]) < 0.01f,
                  "full diagonal with squircle %g: (%.3f, %.3f), want (%.3f, %.3f)", sqs[i], ox, oy, want[i], -want[i]);
        }
    }

    /* The direction never changes: every 3 degrees, at three distances, within the bytes' rounding. */
    for (int i = 0; i < 3; i++)
        for (int deg = 0; deg < 360; deg += 3)
            for (int k = 0; k < 3; k++) {
                const float r = k == 0 ? 1.0f : k == 1 ? 0.7f : 0.4f, a = deg * PI / 180.0f;
                float ox, oy;
                conv(r * sinf(a), -r * cosf(a), sqs[i], &ox, &oy);
                const float got = atan2f(ox, -oy), err = fabsf(remainderf(got - a, 2.0f * PI)) * 180.0f / PI;
                CHECK(err < 1.6f, "direction %d deg at %.1f with squircle %g: %.2f deg off", deg, r, sqs[i], err);
            }

    /* Squircling never shortens a push, and at most adds 1/factor. */
    for (int i = 1; i < 3; i++)
        for (int deg = 0; deg < 90; deg++) {
            const float a = deg * PI / 180.0f;
            float o0x, o0y, ox, oy;
            conv(0.8f * sinf(a), -0.8f * cosf(a), 0.0f, &o0x, &o0y);
            conv(0.8f * sinf(a), -0.8f * cosf(a), sqs[i], &ox, &oy);
            const float l0 = hypotf(o0x, o0y), l = hypotf(ox, oy);
            CHECK(l >= l0 - 0.012f && l <= l0 * (1.0f + 1.0f / sqs[i]) + 0.012f,
                  "length at %d deg with squircle %g: %.3f against %.3f", deg, sqs[i], l, l0);
        }

    /* A push past the circle (a DualSense's full diagonal reads about (25, 36)): kept as it is without
     * squircling, as RPCS3 keeps it; with squircling it starts from the circle. */
    {
        const float x = (25 - 128) / 127.0f, y = (36 - 128) / 127.0f;
        pad_stick_to_u8(x, y, DZ, ANTI, 0.0f, &bx, &by);
        CHECK(abs(bx - 25) <= 1 && abs(by - 36) <= 1, "past the circle, no squircle: (%u, %u), want (25, 36)", bx, by);
        float ox, oy;
        conv(x, y, 8.0f, &ox, &oy);
        const float a = atan2f(y, x), want = 1.0f + powf(sinf(2.0f * a), 2.0f) / 8.0f;
        CHECK(fabsf(hypotf(ox, oy) - want) < 0.012f, "past the circle, squircle 8: length %.3f, want %.3f",
              hypotf(ox, oy), want);
    }

    /* No step at the deadzone's edge, with or without squircling. */
    for (int i = 0; i < 3; i++) {
        float ix, iy, ox, oy;
        conv(DZ * 0.999f * sqrtf(0.5f), DZ * 0.999f * sqrtf(0.5f), sqs[i], &ix, &iy);
        conv(DZ * 1.001f * sqrtf(0.5f), DZ * 1.001f * sqrtf(0.5f), sqs[i], &ox, &oy);
        CHECK(fabsf(hypotf(ox, oy) - hypotf(ix, iy)) < 0.012f, "step at the deadzone's edge with squircle %g: %.3f to %.3f",
              sqs[i], hypotf(ix, iy), hypotf(ox, oy));
    }

    /* Today's deadzones, byte for byte, over a grid (the hashes come from the header before the tilt deadzone was
     * added). Unset, PAD_DEADZONE_LEFT and _RIGHT leave a stick on exactly this function. */
    CHECK(grid_hash(7849 / 32767.0f, ANTI, 8.0f) == 0xD62FE937u, "XInput and SDL's default bytes changed: 0x%08X",
          grid_hash(7849 / 32767.0f, ANTI, 8.0f));
    CHECK(grid_hash(DZ, ANTI, 8.0f) == 0x0D6158F3u, "the Sony path's default bytes changed: 0x%08X", grid_hash(DZ, ANTI, 8.0f));

    /* The knob: unset, empty or not a number stays on today's deadzone; 0 is a value. */
    CHECK(pad_stick_zone_percent(NULL) == -1, "unset");
    CHECK(pad_stick_zone_percent("") == -1, "empty");
    CHECK(pad_stick_zone_percent("x") == -1 && pad_stick_zone_percent("12x") == -1 && pad_stick_zone_percent("-5") == -1, "not a number");
    CHECK(pad_stick_zone_percent("0") == 0 && pad_stick_zone_percent("37") == 37 && pad_stick_zone_percent("70") == 70, "numbers");
    CHECK(pad_stick_zone_percent("150") == 100 && pad_stick_zone_percent("99999999999") == 100, "too big");

    /* Tilt deadzone: every value, with and without squircling, starts at its tilt and from nothing. */
    check_tilt_zones(8.0f);
    check_tilt_zones(0.0f);

    /* At rest and inside the zone the axes are exactly centred, so a title that checks for a centred stick sees one. */
    for (int p = 0; p <= 70; p += 7) {
        pad_stick_tilt_to_u8(0.0f, 0.0f, p / 100.0f, GAME_ZONE, 8.0f, &bx, &by);
        CHECK(bx == 128 && by == 128, "%d%% at rest: (%u, %u)", p, bx, by);
        if (p) {
            pad_stick_tilt_to_u8(p / 100.0f * 0.99f, 0.0f, p / 100.0f, GAME_ZONE, 8.0f, &bx, &by);
            CHECK(bx == 128 && by == 128, "%d%% just inside: (%u, %u)", p, bx, by);
        }
    }

    /* With the first-movement point of today's deadzone, the line above it is today's: the slopes and the end agree,
     * so a settings screen can honestly call those numbers the default. Straight pushes (squircling has no say). */
    {
        const struct { int p; float dz; } now[2] = {{45, 7849 / 32767.0f}, {39, DZ}};
        for (int i = 0; i < 2; i++)
            for (int k = now[i].p + 1; k <= 100; k++) {
                uint8_t tx, ty;
                pad_stick_tilt_to_u8(k / 100.0f, 0.0f, now[i].p / 100.0f, GAME_ZONE, 8.0f, &bx, &by);
                pad_stick_to_u8(k / 100.0f, 0.0f, now[i].dz, ANTI, 8.0f, &tx, &ty);
                CHECK(abs(bx - tx) <= 1 && by == ty, "%d%% at %d%%: %u against today's %u", now[i].p, k, bx, tx);
            }
    }

    /* The direction never changes past the zone, and squircling is in full at the rim only. */
    for (int deg = 0; deg < 360; deg += 3) {
        const float a = deg * PI / 180.0f;
        uint8_t ix, iy;
        pad_stick_tilt_to_u8(0.7f * sinf(a), -0.7f * cosf(a), 0.3f, GAME_ZONE, 8.0f, &ix, &iy);
        const float got = atan2f((ix - 128) / 128.0f, -(iy - 128) / 128.0f), err = fabsf(remainderf(got - a, 2.0f * PI)) * 180.0f / PI;
        CHECK(err < 1.6f, "direction %d deg: %.2f deg off", deg, err);
    }
    {
        const float d = sqrtf(0.5f);
        float fx, fy;
        pad_stick_tilt_to_u8(d, -d, 0.3f, GAME_ZONE, 8.0f, &bx, &by);
        fx = (bx - 128) / 128.0f;
        fy = (by - 128) / 128.0f;
        CHECK(fabsf(fx - 0.7955f) < 0.01f && fabsf(fy + 0.7955f) < 0.01f, "full diagonal: (%.3f, %.3f)", fx, fy);
        pad_stick_tilt_to_u8(0.301f * d, -0.301f * d, 0.3f, GAME_ZONE, 8.0f, &bx, &by);
        fx = (bx - 128) / 128.0f;
        fy = (by - 128) / 128.0f;
        CHECK(fabsf(sqrtf(fx * fx + fy * fy) - GAME_ZONE) < 0.012f, "diagonal at the edge: %.3f", sqrtf(fx * fx + fy * fy));
    }

    /* A corner of a square gate, past the circle, at the largest values: full, and never a division by zero. */
    pad_stick_tilt_to_u8(1.0f, 1.0f, 1.0f, GAME_ZONE, 8.0f, &bx, &by);
    CHECK(bx >= 217 && by >= 217, "corner at 100%%: (%u, %u)", bx, by);
    pad_stick_tilt_to_u8(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &bx, &by);
    CHECK(bx == 255 && by == 128, "full push with no zone: (%u, %u)", bx, by);
    pad_stick_tilt_to_u8(0.5f, 0.0f, 0.0f, 0.0f, 0.0f, &bx, &by);
    CHECK(bx == 192 && by == 128, "half push with no zone: (%u, %u)", bx, by);

    if (s_fail) { printf("%d failed\n", s_fail); return 1; }
    printf("pad_stick: all passed\n");
    return 0;
}
