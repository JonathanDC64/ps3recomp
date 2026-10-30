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
#define rsx_d3d12_backend_init          rsx_out_backend_init
#define rsx_d3d12_backend_present       rsx_out_backend_present
#define rsx_d3d12_backend_pump_messages rsx_out_backend_pump_messages
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
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    for (int c = 0; c < ncaps; c++) {
        uint32_t* m = NULL;
        const int n = cellGcm_replay_load(argv[1 + c], &m);
        if (n < 0) { fprintf(stderr, "rsx_replay: cannot read %s\n", argv[1 + c]); return 1; }
        printf("[replay] %s: %d methods\n", argv[1 + c], n);
        for (int k = 0; frames == 0 || k < frames; k++) {
            QueryPerformanceCounter(&t0);
            cellGcm_replay_reset();
            for (int i = 0; i < n; i++) cellGcm_replay_method(m[2 * i], m[2 * i + 1]);
            rsx_d3d12_backend_present();
            QueryPerformanceCounter(&t1);
            if (k < 3 || frames == 0 && k % 120 == 0)
                printf("[replay] frame %d: %.1f ms\n", k, 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart);
            if (rsx_d3d12_backend_pump_messages() != 0) { c = ncaps; break; }
        }
    }
    fflush(stdout);
    /* The backend's threads and device are torn down with the process. */
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
