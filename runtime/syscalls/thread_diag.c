/* thread_diag.c -- see thread_diag.h. */
#include "thread_diag.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define TDIAG_TLS __declspec(thread)
static unsigned long tdiag_tid(void) { return (unsigned long)GetCurrentThreadId(); }
static long tdiag_inc(volatile long* p) { return InterlockedIncrement(p) - 1; }
#else
#  include <pthread.h>
#  define TDIAG_TLS __thread
static unsigned long tdiag_tid(void) { return (unsigned long)(uintptr_t)pthread_self(); }
static long tdiag_inc(volatile long* p) { return __sync_fetch_and_add(p, 1); }
#endif

#define TDIAG_MAX 128

typedef struct {
    unsigned long tid;
    char          name[48];
    char          wtype[24];
    uint32_t      wobj;
    volatile long waiting;   /* 1 while blocked */
    uint64_t      seq;       /* number of waits entered */
    char          lasthle[40];
    uint64_t      hleseq;    /* number of HLE calls made */
} tdiag_rec;

static tdiag_rec   g_t[TDIAG_MAX];
static volatile long g_n = 0;
static TDIAG_TLS int s_slot = -1;

static int tdiag_slot(void)
{
    if (s_slot >= 0) return s_slot;
    long i = tdiag_inc(&g_n);
    if (i >= TDIAG_MAX) i = TDIAG_MAX - 1;
    s_slot = (int)i;
    g_t[i].tid = tdiag_tid();
    return s_slot;
}

void thrdiag_set_name(const char* name)
{
    int i = tdiag_slot();
    g_t[i].tid = tdiag_tid();
    strncpy(g_t[i].name, name ? name : "?", sizeof(g_t[i].name) - 1);
    g_t[i].name[sizeof(g_t[i].name) - 1] = '\0';
}

void thrdiag_wait(const char* type, uint32_t obj)
{
    int i = tdiag_slot();
    strncpy(g_t[i].wtype, type ? type : "?", sizeof(g_t[i].wtype) - 1);
    g_t[i].wtype[sizeof(g_t[i].wtype) - 1] = '\0';
    g_t[i].wobj    = obj;
    g_t[i].waiting = 1;
    g_t[i].seq++;
}

void thrdiag_wake(void)
{
    if (s_slot >= 0) g_t[s_slot].waiting = 0;
}

/* Return the wait-object (e.g. sema id) that a currently-blocked thread whose name
 * starts with `prefix` is waiting on, restricted to wait-type `type`; 0 if none.
 * Used by the vblank ticker to post exactly the sema HighGraphics is blocked on. */
uint32_t thrdiag_wobj_of(const char* prefix, const char* type)
{
    long n = g_n; if (n > TDIAG_MAX) n = TDIAG_MAX;
    size_t plen = 0; while (prefix && prefix[plen]) plen++;
    for (long i = 0; i < n; i++) {
        if (!g_t[i].waiting) continue;
        if (strncmp(g_t[i].name, prefix, plen) != 0) continue;
        if (type && strcmp(g_t[i].wtype, type) != 0) continue;
        return g_t[i].wobj;
    }
    return 0;
}

void thrdiag_hle(const char* name)
{
    int i = tdiag_slot();
    strncpy(g_t[i].lasthle, name ? name : "?", sizeof(g_t[i].lasthle) - 1);
    g_t[i].lasthle[sizeof(g_t[i].lasthle) - 1] = '\0';
    g_t[i].hleseq++;
}

/* Current thread's registered name (or "" if not set yet). Lets event syscalls
 * attribute a receive to a named guest thread (e.g. the SPURS service). */
const char* thrdiag_cur_name(void)
{
    if (s_slot < 0) return "";
    return g_t[s_slot].name;
}

void thrdiag_dump(void)
{
    long n = g_n; if (n > TDIAG_MAX) n = TDIAG_MAX;
    fprintf(stderr, "[thrdiag] === %ld guest threads ===\n", n);
    for (long i = 0; i < n; i++) {
        fprintf(stderr, "[thrdiag]   tid=%-6lu name=\"%s\" %-7s wait=%s obj=0x%08X waits=%llu lastHLE=%s(#%llu)\n",
                g_t[i].tid, g_t[i].name,
                g_t[i].waiting ? "WAITING" : "running",
                g_t[i].wtype[0] ? g_t[i].wtype : "-",
                g_t[i].wobj, (unsigned long long)g_t[i].seq,
                g_t[i].lasthle[0] ? g_t[i].lasthle : "-", (unsigned long long)g_t[i].hleseq);
    }
    fflush(stderr);
}
