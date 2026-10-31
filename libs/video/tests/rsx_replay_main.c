/*
 * rsx_replay -- play back one captured frame through the D3D12 backend.
 *
 * A capture (RSX_REPLAY_CAP=<file>, see cellGcmSys.c) holds one frame's RSX
 * method stream, the RSX and GCM state at its start, and the guest memory it
 * reads. This replays it through the same method dispatch and the same
 * backend a title runs, with no game, no PPU and no boot: a renderer change
 * is rebuilt and checked against a real in-game frame in seconds.
 *
 *   rsx_replay <capture> [frames]
 *   rsx_replay <capture> <capture> ... [frames]
 *
 * plays the frame `frames` times (default 1; 0 = until the window is
 * closed) and exits. With several captures each is played `frames` times in
 * turn through the same backend, so a frame starts with whatever the earlier
 * ones left on the GPU (targets, depth surfaces, cached textures and
 * pipelines), as a frame in a running game does; a single replay starts
 * with none of it. Guest memory is not cleared between captures: a page the
 * next capture does not hold keeps the previous one's bytes. A capture's
 * first frame shows the picture its snapshot wrote back, so in a chain only
 * the later frames of each capture show what the GPU state did. The
 * backend's own diagnostics work as in a game run, e.g. CELLMARK_DUMP=1
 * CELLMARK_DUMP_DIR=<dir> writes the presented frame as a BMP and RSX_CAP=0
 * RSX_CAP_DIR=<dir> the per-op manifest. The capture and
 * the replayer must come from the same build (raw state structs).
 *
 * Each frame's time is printed for frames 0-2 (and every 120th when playing
 * until closed). RSX_REPLAY_TIMES=1 prints every frame instead, with the
 * replaying thread's CPU time beside the wall time, and after each capture
 * the means over its frames after the first:
 *   [replay] frame 7: 5.123 ms, cpu 4.688 ms
 *   [replay] times: 199 frames after the first: wall 5.140 ms, cpu 4.981 ms a frame
 * Windows counts a thread's time in clock ticks (about 15.6 ms), so one
 * frame's cpu figure is coarse; the mean over many frames is not.
 *
 * The process ends without the renderer's shutdown. RSX_REPLAY_SHUTDOWN=1,
 * on the new path's routes only (RSX_BACKEND=hal-d3d12 or null), runs it
 * after the last frame first: the GPU idle, every object freed and the
 * device destroyed, which the null backend checks (its last line counts
 * the failed checks; GPU_NULL_DUMP's file ends with the stream's hash).
 *   [replay] shutdown: done
 * It also saves the new path's pipeline stores and prints its "pipelines
 * this run" line. RSX_REPLAY_INDEX=<rows file> hands the renderer pipeline
 * index rows to build (below).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

uint8_t* vm_base = NULL;

#if defined(PS3RECOMP_RSX_HAL)
/* Built with the portable renderer: RSX_BACKEND picks the renderer the
 * capture plays through (rsx_out.h). */
#include "rsx_out.h"
#include "rsx_d3d12_backend.h"   /* (rsx_d3d12_overlay, for RSX_REPLAY_OVERLAY_GPU) */
#include <string.h>
#define rsx_d3d12_backend_init          rsx_out_backend_init
#define rsx_d3d12_backend_present       rsx_out_backend_present
#define rsx_d3d12_backend_pump_messages rsx_out_backend_pump_messages
#include "gpu/gpu.h"
#include "gpu/gpu_vt_cmd.h"

/* RSX_REPLAY_OVERLAY_GPU=1 on the new path's routes: overlay v2 (rsx_out.h)
 * under test. Each frame's image gets a 64x64 square in its top left corner,
 * its colour by frame, recorded into the core's recorder; every 4th frame the
 * recording is flushed once in between, as a read back does. The last line
 * says how many frames it drew in and what failed. */
static unsigned s_ov_frames, s_ov_flushes, s_ov_failed;
static void ov_overlay_v1(void* user, void* back_buffer, unsigned index, unsigned width, unsigned height)
{
    (void)user; (void)back_buffer; (void)index; (void)width; (void)height;
    s_ov_failed++;   /* (v2 is set: the core must not call v1's frame) */
}
static void ov_frame(void* user, struct gpu_cmd* c, uint32_t target, uint32_t format, unsigned index,
                     unsigned width, unsigned height)
{
    (void)user; (void)format; (void)index;
    gpu_device* const d = rsx_out_gpu_device();
    if (!d || !c || width < 64 || height < 64) { s_ov_failed++; return; }
    const gpu_texture t = {target};
    gpu_view_desc v;
    memset(&v, 0, sizeof v);
    v.texture = t;
    v.kind = GPU_VIEW_RTV;
    v.dim = GPU_DIM_2D;
    const gpu_view rtv = gpu_view_get(d, &v);
    if (!GPU_VALID(rtv)) { s_ov_failed++; return; }
    const float k = (float)(s_ov_frames % 8) / 7.0f;
    const float rgba[4] = {k, 1.0f - k, 0.5f, 1.0f};
    const gpu_rect r = {0, 0, 64, 64};
    gpu_pass pass;
    memset(&pass, 0, sizeof pass);
    pass.nrt = 1;
    pass.rt[0] = rtv;
    gpu_cmd_barrier(c, t, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
    gpu_cmd_pass_begin(c, &pass);
    gpu_cmd_clear_color(c, rtv, rgba, &r);
    gpu_cmd_pass_end(c);
    gpu_cmd_barrier(c, t, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
    if (s_ov_frames % 4 == 3) {
        c = rsx_out_overlay_gpu_flush();
        if (!c) { s_ov_failed++; return; }
        s_ov_flushes++;
        gpu_cmd_barrier(c, t, GPU_STATE_PRESENT, GPU_STATE_RENDER_TARGET);
        gpu_cmd_pass_begin(c, &pass);
        const gpu_rect r2 = {64, 0, 128, 64};
        gpu_cmd_clear_color(c, rtv, rgba, &r2);
        gpu_cmd_pass_end(c);
        gpu_cmd_barrier(c, t, GPU_STATE_RENDER_TARGET, GPU_STATE_PRESENT);
    }
    s_ov_frames++;
}
#else
int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
void rsx_d3d12_backend_present(void);
int  rsx_d3d12_backend_pump_messages(void);
#endif
int  cellGcm_replay_load(const char* file, uint32_t** methods);
void cellGcm_replay_reset(void);
void cellGcm_replay_method(uint32_t mfull, uint32_t data);

#define VM_SPAN 0x100010000ull   /* the 4 GB guest space plus a guard, as boot_main */

/* What the runtime library expects from a game's loader (ppu_loader.cpp) and
 * runtime/host/host_stubs.c does not cover. No guest code runs here. */
const uint64_t function_table[2] = { 0, 0 };   /* backtrace symbolisation: empty */
const uint64_t function_table_count = 0;

/* Commit guest pages on first touch, as the game's runtime does: the backend
 * may read past what the capture held (it held only non-zero pages), and
 * that memory reads as zero. */
static LONG WINAPI commit_on_touch(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
    if (a < vm_base || a >= vm_base + VM_SPAN) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* page = (uint8_t*)((uintptr_t)a & ~(uintptr_t)0xFFFF);
    return VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION
                                                                   : EXCEPTION_CONTINUE_SEARCH;
}

/* The calling thread's CPU time so far (user + kernel), in ms. */
static double thread_cpu_ms(void)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0.0;
    const ULONGLONG t = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                        ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    return (double)t / 1e4;   /* 100 ns units */
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: rsx_replay <capture> [<capture> ...] [frames]   (0 = until the window closes)\n");
        return 2;
    }
    /* A trailing number is the frame count; every other argument is a capture. */
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep progress lines in order with the backend's stderr */
    int ncaps = argc - 1, frames = 1;
    { char* end = NULL; long v = strtol(argv[argc - 1], &end, 10);
      if (argc > 2 && end && *end == 0) { frames = (int)v; ncaps--; } }
    if (frames == 0 && ncaps > 1) frames = 1;   /* "until closed" only for one capture */
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SPAN, MEM_RESERVE, PAGE_READWRITE);
    if (!vm_base) { fprintf(stderr, "rsx_replay: cannot reserve the guest space\n"); return 1; }
    AddVectoredExceptionHandler(1, commit_on_touch);

    if (rsx_d3d12_backend_init(1280, 720, "rsx_replay") != 0) {
        fprintf(stderr, "rsx_replay: backend init failed\n"); return 1;
    }
#if defined(PS3RECOMP_RSX_HAL)
    /* RSX_REPLAY_INDEX=<file>: pipeline index rows for the renderer to build,
     * as setup's pass hands them over, from the rows file precompile_compare
     * --rows writes ("add <vfetch> <vs key> <clamp> <ps key> <blend> <nrt>
     * <rtfmt> <cmask> <cull> <depth>", then "ready <hash>"). With
     * PS3_PSO_WAIT=1 and RSX_REPLAY_SHUTDOWN=1 the build runs to its end. */
    { char ip[MAX_PATH] = "";
      const DWORD il = GetEnvironmentVariableA("RSX_REPLAY_INDEX", ip, sizeof ip);
      FILE* fi = il && il < sizeof ip ? fopen(ip, "rb") : NULL;
      if (il && !fi) { fprintf(stderr, "rsx_replay: cannot read %s\n", ip); return 1; }
      if (fi) {
          char line[256];
          unsigned rows = 0, ready = 0;
          unsigned long long hash = 0;
          while (fgets(line, sizeof line, fi)) {
              int fetch, clamp, nrt;
              unsigned long long vk, pk;
              unsigned blend, rtfmt, cmask, cull, depth;
              if (sscanf(line, "add %d %llx %d %llx %x %d %x %x %x %x", &fetch, &vk, &clamp, &pk, &blend, &nrt,
                         &rtfmt, &cmask, &cull, &depth) == 10) {
                  rsx_out_pso_index_add(fetch, vk, clamp, pk, blend, nrt, rtfmt, cmask, cull, depth);
                  rows++;
              } else if (sscanf(line, "ready %llx", &hash) == 1) ready = 1;
          }
          fclose(fi);
          rsx_out_pso_index_ready(ready ? hash : 0);
          printf("[replay] pipeline index: %u rows handed over from %s\n", rows, ip);
      } }
    char ovv[16] = "";
    const int overlay_gpu = GetEnvironmentVariableA("RSX_REPLAY_OVERLAY_GPU", ovv, sizeof ovv) < sizeof ovv &&
                            atoi(ovv) > 0 && strcmp(rsx_out_renderer(), "d3d12") != 0;
    if (overlay_gpu) {
        static const rsx_d3d12_overlay v1 = {NULL, NULL, ov_overlay_v1, NULL};   /* (v2 set beside it, as des_ui) */
        static const rsx_out_overlay_gpu v2 = {NULL, ov_frame};
        rsx_out_set_overlay(&v1);
        rsx_out_set_overlay_gpu(&v2);
        if (!rsx_out_gpu_device()) s_ov_failed++;
    }
#endif
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    char tv[16] = "";
    const int times = GetEnvironmentVariableA("RSX_REPLAY_TIMES", tv, sizeof tv) < sizeof tv && atoi(tv) > 0;
    for (int c = 0; c < ncaps; c++) {
        uint32_t* m = NULL;
        const int n = cellGcm_replay_load(argv[1 + c], &m);
        if (n < 0) { fprintf(stderr, "rsx_replay: cannot read %s\n", argv[1 + c]); return 1; }
        printf("[replay] %s: %d methods\n", argv[1 + c], n);
        double wall_sum = 0.0, cpu_sum = 0.0;
        int timed = 0;
        for (int k = 0; frames == 0 || k < frames; k++) {
            const double cpu0 = times ? thread_cpu_ms() : 0.0;
            QueryPerformanceCounter(&t0);
            cellGcm_replay_reset();
            for (int i = 0; i < n; i++) cellGcm_replay_method(m[2 * i], m[2 * i + 1]);
            rsx_d3d12_backend_present();
            QueryPerformanceCounter(&t1);
            if (times) {
                const double wall = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
                const double cpu = thread_cpu_ms() - cpu0;
                printf("[replay] frame %d: %.3f ms, cpu %.3f ms\n", k, wall, cpu);
                if (k > 0) { wall_sum += wall; cpu_sum += cpu; timed++; }
            } else if (k < 3 || frames == 0 && k % 120 == 0)
                printf("[replay] frame %d: %.1f ms\n", k, 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
            if (rsx_d3d12_backend_pump_messages() != 0) { c = ncaps; break; }
        }
        if (timed)
            printf("[replay] times: %d frames after the first: wall %.3f ms, cpu %.3f ms a frame\n",
                   timed, wall_sum / timed, cpu_sum / timed);
    }
#if defined(PS3RECOMP_RSX_HAL)
    if (overlay_gpu)
        printf("[replay] overlay v2: %u frames drawn over, %u flushes, %u failed\n", s_ov_frames, s_ov_flushes,
               s_ov_failed);
#endif
    fflush(stdout);
#if defined(PS3RECOMP_RSX_HAL)
    /* RSX_REPLAY_SHUTDOWN=1 on the new path's routes: its shutdown first. */
    { char sv[16] = "";
      if (GetEnvironmentVariableA("RSX_REPLAY_SHUTDOWN", sv, sizeof sv) < sizeof sv && atoi(sv) > 0 &&
          strcmp(rsx_out_renderer(), "d3d12") != 0) {
          rsx_out_backend_shutdown();
          printf("[replay] shutdown: done\n");
          fflush(stdout);
      } }
#endif
    /* The backend's threads and device are torn down with the process. */
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
