/* RSX_GPU_TIMES: the GPU's own time in each recording, from timestamps the
 * D3D12 and Vulkan backends write at the same calls: cmd_begin, cmd_end and,
 * with RSX_GPU_TIMES=2, each pass_begin. A slot's stamps are read when the
 * slot is begun again (the caller has seen its fence) and at wait_idle, and
 * added up for gpu_gpu_ns (rsx_replay's "gpu" figure, the core's
 * [gpu-times] line). With 2 the steps between a recording's stamps are
 * averaged over 600 recordings of 8 or more stamps (the frames, not a blit
 * or the menu) and printed, step 0 from the start to the first pass:
 *   [gpu-times] 600 recordings: 812.4 us a recording; steps (us): 0:12.1 1:3.0 ...
 * Off (the default), no stamps are written. Backend-internal. */
#ifndef PS3RECOMP_GPU_TIMES_H
#define PS3RECOMP_GPU_TIMES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPU_TIMES_MAX 512u   /* stamps a recording: its start, its passes, its end */

/* 0 off, 1 a recording's start and end, 2 each pass_begin too. */
static inline int gpu_times_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char* e = getenv("RSX_GPU_TIMES");
        const int v = e && *e ? atoi(e) : 0;
        m = v < 0 ? 0 : v > 2 ? 2 : v;
    }
    return m;
}

typedef struct gpu_times {
    uint64_t ns, n;                  /* the recordings read so far (gpu_gpu_ns) */
    double   step[GPU_TIMES_MAX];    /* RSX_GPU_TIMES=2: each step's microseconds, summed */
    uint32_t steps, recs;            /* the most steps a summed recording had; recordings summed */
} gpu_times;

/* One recording's n stamps, in ticks of ns_per_tick nanoseconds; mask holds
 * the bits a stamp keeps (a Vulkan queue may have fewer than 64). */
static inline void gpu_times_take(gpu_times* g, const uint64_t* ts, uint32_t n, double ns_per_tick, uint64_t mask)
{
    if (n < 2) return;
    g->ns += (uint64_t)((double)((ts[n - 1] - ts[0]) & mask) * ns_per_tick + 0.5);
    g->n++;
    if (gpu_times_mode() < 2 || n < 8) return;
    for (uint32_t k = 1; k < n; k++) g->step[k - 1] += (double)((ts[k] - ts[k - 1]) & mask) * ns_per_tick * 1e-3;
    if (n - 1 > g->steps) g->steps = n - 1;
    if (++g->recs < 600) return;
    double tot = 0.0;
    for (uint32_t k = 0; k < g->steps; k++) tot += g->step[k];
    printf("[gpu-times] %u recordings: %.1f us a recording; steps (us):", g->recs, tot / g->recs);
    for (uint32_t k = 0; k < g->steps; k++) printf(" %u:%.1f", k, g->step[k] / g->recs);
    printf("\n");
    fflush(stdout);
    memset(g->step, 0, sizeof g->step);
    g->steps = g->recs = 0;
}

#endif
