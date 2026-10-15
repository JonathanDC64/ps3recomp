/* Console `waits`: time inside each lv2 syscall and HLE call per guest thread
 * (ppu_loader.cpp). Armed by the debug console; see ppu_waits_add. */
#pragma once
#include <stdint.h>

#define PPU_WAITS_N 65536u   /* entries; a power of two */

typedef struct ppu_waits_ent {
    uint32_t tid;          /* guest thread id */
    int32_t  hle;          /* 0 = lv2 syscall (key = number), 1 = HLE (key = NID) */
    uint32_t key;
    uint32_t obj;          /* r3 on entry */
    const char* name;      /* HLE name, or null */
    uint64_t ticks;        /* QPC ticks inside the call */
    uint32_t n;            /* calls; 0 = free slot */
} ppu_waits_ent;

#ifdef __cplusplus
extern "C" {
#endif
extern volatile int g_waits_armed;
extern ppu_waits_ent g_waits[PPU_WAITS_N];
extern volatile uint32_t g_waits_dropped;   /* calls a full table lost */
uint64_t ppu_waits_now(void);
void ppu_waits_add(uint32_t tid, int hle, uint32_t key, uint32_t obj,
                   const char* name, uint64_t ticks);

/* Console `trace`: the same calls as a timeline (start, end, thread, call, r3)
 * plus marks from host threads (ppu_trace_mark). See ppu_trace_run. */
extern volatile int g_trace_armed;
void ppu_trace_add(uint32_t tid, int hle, uint32_t key, uint32_t obj,
                   const char* name, uint64_t t0, uint64_t t1);
void ppu_trace_mark(const char* name, uint32_t a, uint32_t b);
int ppu_trace_run(unsigned sec, const char* path);
#ifdef __cplusplus
}
#endif
