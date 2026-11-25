/*
 * ps3recomp - cellAudio's peak limiter: the finished stereo mix into [-1, 1].
 *
 * Ports add up, and an 8-channel port's downmix adds its surround channels to
 * the fronts, so a loud moment can go past full scale; clipped there, it
 * crackles. A peak limiter with 1 ms of look-ahead instead: the output is the
 * mix from AUDIO_LIMIT_AHEAD frames ago, times a gain that falls in a straight
 * line over those frames to bring each frame's louder side to at most
 * AUDIO_LIMIT_CEIL as it leaves, and recovers toward 1 with an 80 ms time
 * constant. While no frame in reach is over the ceiling the gain is exactly 1,
 * so the samples are the mix's own, a millisecond later. A NaN leaves as 0.
 * With on = 0: the hard clip at +-1, with no delay.
 *
 * One stream: the state is this header's, for the one file that includes it
 * (cellAudio.c; libs/audio/tests/test_audio_limit.c tests it alone).
 */
#ifndef PS3RECOMP_AUDIO_LIMIT_H
#define PS3RECOMP_AUDIO_LIMIT_H

#include <math.h>
#include <string.h>

#define AUDIO_LIMIT_AHEAD      48      /* frames of look-ahead (1 ms at 48 kHz) */
#define AUDIO_LIMIT_CEIL       0.98f
#define AUDIO_LIMIT_MAX_FRAMES 256     /* the most frames one call takes */

/* mix: n stereo frames (n <= AUDIO_LIMIT_MAX_FRAMES), interleaved, in place. */
static void audio_limit_block(float* mix, unsigned n, int on)
{
    if (on) {
        /* buf: the previous call's last AHEAD frames, then this call's. need[k]:
         * the most gain frame k can leave with. */
        static float buf[(AUDIO_LIMIT_AHEAD + AUDIO_LIMIT_MAX_FRAMES) * 2];
        static float need[AUDIO_LIMIT_AHEAD + AUDIO_LIMIT_MAX_FRAMES];
        static float att;   /* 1 minus the gain, so the release can reach 0 in float */
        static int primed;
        if (!primed) {
            primed = 1;
            for (unsigned k = 0; k < AUDIO_LIMIT_AHEAD; k++) need[k] = 1.0f;
        }
        const float keep = expf(-1.0f / (0.080f * 48000.0f));
        memcpy(&buf[AUDIO_LIMIT_AHEAD * 2], mix, n * 2 * sizeof(float));
        for (unsigned k = 0; k < n; k++) {
            float l = buf[(AUDIO_LIMIT_AHEAD + k) * 2], r = buf[(AUDIO_LIMIT_AHEAD + k) * 2 + 1];
            if (l != l) buf[(AUDIO_LIMIT_AHEAD + k) * 2] = l = 0.0f;
            if (r != r) buf[(AUDIO_LIMIT_AHEAD + k) * 2 + 1] = r = 0.0f;
            const float pk = fmaxf(fabsf(l), fabsf(r));
            need[AUDIO_LIMIT_AHEAD + k] = pk > AUDIO_LIMIT_CEIL ? AUDIO_LIMIT_CEIL / pk : 1.0f;
        }
        for (unsigned j = 0; j < n; j++) {
            /* The release, then no more than any frame in reach allows: frame
             * j + d needs need[j + d] by the time it leaves, d frames on, so
             * the gain now may be at most the straight line from there back to
             * 1 at AHEAD frames before it. The release decays the attenuation
             * (1 - gain) toward 0: done on the gain itself, in float it stalls
             * at about 0.9999. */
            float rel = att * keep;
            if (rel < 1e-6f) rel = 0.0f;
            float a = 1.0f - rel;
            int held = 0;
            for (unsigned d = 0; d <= AUDIO_LIMIT_AHEAD; d++) {
                const float q = need[j + d];
                if (q < 1.0f) {
                    const float lim = q + (1.0f - q) * (float)d / (float)AUDIO_LIMIT_AHEAD;
                    if (lim < a) { a = lim; held = 1; }
                }
            }
            att = held ? 1.0f - a : rel;
            mix[j * 2 + 0] = buf[j * 2 + 0] * a;
            mix[j * 2 + 1] = buf[j * 2 + 1] * a;
        }
        memmove(buf, &buf[n * 2], AUDIO_LIMIT_AHEAD * 2 * sizeof(float));
        memmove(need, &need[n], AUDIO_LIMIT_AHEAD * sizeof(float));
    }
    for (unsigned i = 0; i < n * 2; i++) {
        float v = mix[i];
        if (v > 1.0f) v = 1.0f;
        else if (v < -1.0f) v = -1.0f;
        else if (v != v) v = 0.0f;
        mix[i] = v;
    }
}

#endif /* PS3RECOMP_AUDIO_LIMIT_H */
