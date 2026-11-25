/* cellAudio's peak limiter (audio_limit.h) on synthetic signals, in calls of
 * one mixer block: quiet music with bursts up to 3x full scale, a one-sample
 * spike and a NaN. Each output frame must be the input from AUDIO_LIMIT_AHEAD
 * frames earlier times a gain in (0, 1], at most AUDIO_LIMIT_CEIL, never NaN;
 * the gain may not step faster than the look-ahead ramp allows; and away from
 * the loud parts (before the first, and once the release is done after each)
 * the samples must be the input's own, bit for bit. With the limiter off:
 * the hard clip at +-1, no delay.
 *
 *   cc -std=gnu17 -I include -o /tmp/test_audio_limit libs/audio/tests/test_audio_limit.c -lm
 */
#include "../audio_limit.h"

#include <stdio.h>
#include <stdlib.h>

#define BLOCK  256
#define BLOCKS 2000                 /* 10.7 s at 48 kHz */
#define FRAMES (BLOCK * BLOCKS)

static float in[FRAMES * 2], out[FRAMES * 2];
static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int quiet_at(double t)   /* before the first burst, or a second after one ended */
{
    return (t > 0.5 && t < 1.9) || (t > 3.5 && t < 4.9) || t > 9.0;
}

int main(void)
{
    srand(7);
    for (unsigned i = 0; i < FRAMES; i++) {
        const double t = i / 48000.0;
        double amp = 0.3;
        if (t > 2.0 && t < 2.3) amp = 3.0;
        if (t > 5.0 && t < 5.01) amp = 1.5;
        double l = amp * sin(6.283185307179586 * 440.0 * t) + 0.02 * (rand() / (double)RAND_MAX - 0.5);
        double r = amp * sin(6.283185307179586 * 660.0 * t);
        if (i == 48000 * 7) l = 2.5;    /* a one-sample spike */
        in[i * 2] = (float)l;
        in[i * 2 + 1] = (float)r;
    }
    in[48000 * 8 * 2] = NAN;

    for (unsigned b = 0; b < BLOCKS; b++) {
        memcpy(&out[b * BLOCK * 2], &in[b * BLOCK * 2], BLOCK * 2 * sizeof(float));
        audio_limit_block(&out[b * BLOCK * 2], BLOCK, 1);
    }

    const unsigned D = AUDIO_LIMIT_AHEAD;
    unsigned quiet = 0, exact = 0, limited = 0;
    double prev = 1.0, step = 0.0;
    unsigned prev_k = 0;
    for (unsigned k = D; k < FRAMES; k++) {
        for (int c = 0; c < 2; c++) {
            const float o = out[k * 2 + c];
            CHECK(o == o, "frame %u: NaN out", k);
            CHECK(fabsf(o) <= AUDIO_LIMIT_CEIL + 1e-6f, "frame %u: %f over the ceiling", k, o);
        }
        const float x = in[(k - D) * 2];
        if (x == x && fabsf(x) > 0.05f) {
            const double g = out[k * 2] / x;
            CHECK(g > 0.0 && g <= 1.0 + 1e-6, "frame %u: gain %f", k, g);
            if (g < 0.999) limited++;
            if (k == prev_k + 1 && fabs(g - prev) > step) step = fabs(g - prev);
            prev = g;
            prev_k = k;
        }
        if (quiet_at(k / 48000.0)) {
            quiet++;
            if (out[k * 2] == in[(k - D) * 2] && out[k * 2 + 1] == in[(k - D) * 2 + 1]) exact++;
        }
    }
    /* The steepest the gain may fall: from 1 to the lowest need over AHEAD frames. */
    const double worst = (1.0 - AUDIO_LIMIT_CEIL / 3.0) / AUDIO_LIMIT_AHEAD;
    CHECK(step <= worst * 1.01, "the gain stepped %f in a frame (at most %f)", step, worst);
    CHECK(exact == quiet, "%u of %u quiet frames not the input's own", quiet - exact, quiet);
    CHECK(limited > 10000, "only %u frames limited: the bursts were not reached", limited);

    /* Off: the hard clip, no delay. */
    float blk[BLOCK * 2];
    for (unsigned i = 0; i < BLOCK * 2; i++) blk[i] = in[(48000 * 2 + 100) * 2 + i];
    blk[3] = NAN;
    audio_limit_block(blk, BLOCK, 0);
    for (unsigned i = 0; i < BLOCK * 2; i++) {
        const float x = in[(48000 * 2 + 100) * 2 + i];
        const float want = i == 3 ? 0.0f : x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
        CHECK(blk[i] == want, "off, sample %u: %f, want %f", i, blk[i], want);
    }

    if (fails) {
        printf("audio limiter: %d failed\n", fails);
        return 1;
    }
    printf("audio limiter: %u frames limited, steepest gain step %.4f, %u quiet frames exact, off = hard clip\n",
           limited, step, quiet);
    return 0;
}
