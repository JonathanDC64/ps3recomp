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

    if (s_fail) { printf("%d failed\n", s_fail); return 1; }
    printf("pad_stick: all passed\n");
    return 0;
}
