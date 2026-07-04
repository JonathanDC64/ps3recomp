/*
 * ps3recomp - cellSysutil HLE implementation
 *
 * System callbacks, parameter queries, BGM control, system cache,
 * and disc game check.
 */

#include "cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Guest callback dispatch hook (set by the game's host code at startup).
 * -----------------------------------------------------------------------*/
ps3_guest_caller_fn g_ps3_guest_caller = NULL;

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Stored as a guest OPD address rather than a host function pointer:
     * the recompiled game passes its OPD addr through Register, and the
     * dispatcher hook resolves the OPD when actually invoking. The
     * CellSysutilCallback typedef maps to a pointer, so we cast through
     * uintptr_t at the API boundary. */
    uint32_t            guest_opd;
    uint32_t            userdata;     /* guest pointer; opaque to the runtime */
    int                 registered;
} SysutilCallbackSlot;

static SysutilCallbackSlot s_callbacks[CELL_SYSUTIL_MAX_CALLBACKS];

/* ---------------------------------------------------------------------------
 * Sysutil event queue.
 *
 * The OS would deliver events asynchronously (XMB open/close, drawing
 * begin/end, save-data complete, etc.). Games drain them by calling
 * cellSysutilCheckCallback periodically. We accept queued events from
 * the host side via cellSysutilQueueEvent() and drain them here.
 * -----------------------------------------------------------------------*/

typedef struct {
    int      slot;
    uint32_t status;
    uint32_t param;
} SysutilEvent;

#define SYSUTIL_EVENT_QUEUE_SIZE 32
static SysutilEvent s_event_queue[SYSUTIL_EVENT_QUEUE_SIZE];
static int          s_event_head = 0;  /* next read */
static int          s_event_tail = 0;  /* next write */

void cellSysutilQueueEvent(int slot, uint32_t status, uint32_t param)
{
    int next = (s_event_tail + 1) % SYSUTIL_EVENT_QUEUE_SIZE;
    if (next == s_event_head) {
        printf("[cellSysutil] event queue full — dropping status=0x%X\n", status);
        return;
    }
    s_event_queue[s_event_tail].slot   = slot;
    s_event_queue[s_event_tail].status = status;
    s_event_queue[s_event_tail].param  = param;
    s_event_tail = next;
}

static s32 s_bgm_enabled = 1;
static s32 s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
static char s_cache_path[CELL_SYSCACHE_PATH_MAX];
static int s_cache_mounted = 0;

static void (*s_disc_change_cb)(void*) = NULL;
static void* s_disc_change_arg = NULL;

/* ---------------------------------------------------------------------------
 * Core callbacks & params
 * -----------------------------------------------------------------------*/

s32 cellSysutilRegisterCallback(s32 slot, CellSysutilCallback func, void* userdata)
{
    /* `func` and `userdata` are guest VM addresses; reinterpret without
     * touching them. NULL func means "unregister this slot" — some games
     * (notably flOw) call Register with func=NULL as a clear-on-poll. */
    uint32_t func_addr     = (uint32_t)(uintptr_t)func;
    uint32_t userdata_addr = (uint32_t)(uintptr_t)userdata;

    printf("[cellSysutil] RegisterCallback(slot=%d, func=0x%08X)\n",
           slot, func_addr);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    if (func_addr == 0) {
        /* Treat as Unregister — matches observed game behaviour. */
        s_callbacks[slot].guest_opd  = 0;
        s_callbacks[slot].userdata   = 0;
        s_callbacks[slot].registered = 0;
        return CELL_OK;
    }

    s_callbacks[slot].guest_opd  = func_addr;
    s_callbacks[slot].userdata   = userdata_addr;
    s_callbacks[slot].registered = 1;

    return CELL_OK;
}

s32 cellSysutilUnregisterCallback(s32 slot)
{
    printf("[cellSysutil] UnregisterCallback(slot=%d)\n", slot);

    if (slot < 0 || slot >= CELL_SYSUTIL_MAX_CALLBACKS)
        return CELL_SYSUTIL_ERROR_NUM;

    s_callbacks[slot].guest_opd  = 0;
    s_callbacks[slot].userdata   = 0;
    s_callbacks[slot].registered = 0;

    return CELL_OK;
}

s32 cellSysutilCheckCallback(void)
{
    /* Drain the event queue, dispatching each event into guest code via
     * the registered ps3_guest_caller hook. Standard PS3 sysutil callback
     * signature is:
     *   void cb(uint64_t status, uint64_t param, void* userdata)
     * which maps to PPC r3=status, r4=param, r5=userdata; r6 is unused.
     *
     * Stops when the queue empties OR when no guest caller is installed
     * (in which case we drop pending events to avoid stalling).
     */
    /* Livelock guard: if a dispatched callback keeps re-queueing events the drain
     * never ends and the calling thread wedges here (observed: AsyncGetHddFreeSizeThread
     * spins at guest 0x00B5D8C0 = this call). Cap the drain per call; if exceeded, log
     * the offending callback + break (also breaks the wedge). Zero normal-case overhead
     * (a plain counter). A healthy drain processes only a handful of events. */
    int iters = 0;
    while (s_event_head != s_event_tail) {
        SysutilEvent e = s_event_queue[s_event_head];
        s_event_head = (s_event_head + 1) % SYSUTIL_EVENT_QUEUE_SIZE;

        if (e.slot < 0 || e.slot >= CELL_SYSUTIL_MAX_CALLBACKS) continue;
        if (!s_callbacks[e.slot].registered) continue;
        if (!s_callbacks[e.slot].guest_opd) continue;

        if (++iters > 256) {
            fprintf(stderr, "[cellSysutil] CheckCallback DRAIN RUNAWAY (%d) slot=%d opd=0x%08X status=0x%X -- breaking\n",
                    iters, e.slot, s_callbacks[e.slot].guest_opd, e.status); fflush(stderr);
            break;
        }
        if (g_ps3_guest_caller) {
            g_ps3_guest_caller(s_callbacks[e.slot].guest_opd,
                               (uint64_t)e.status,
                               (uint64_t)e.param,
                               (uint64_t)s_callbacks[e.slot].userdata,
                               0);
        }
    }
    return CELL_OK;
}

s32 cellSysutilGetSystemParamInt(s32 id, s32* value)
{
    /* `value` arrives as a raw GUEST address (the HLE dispatch passes r4 verbatim), NOT a
     * host pointer. Translate it into the VM and write the result big-endian (PS3 is BE).
     * Dereferencing it directly faulted (guest addr used as host addr). */
    extern uint8_t* vm_base;
    uint32_t gaddr = (uint32_t)(uintptr_t)value;
    if (!gaddr)
        return CELL_SYSUTIL_ERROR_VALUE;

    s32 v;
    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_LANG:                        v = CELL_SYSUTIL_LANG_ENGLISH_US; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN:        v = CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT:                v = 0; break; /* YYYYMMDD */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT:                v = 0; break; /* 24-hour */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_TIMEZONE:                   v = 0; break; /* UTC */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME:                 v = 0; break; /* No DST */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL:        v = 0; break; /* No restriction */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_GAME_PARENTAL_LEVEL0_RESTRICT: v = 0; break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USER_HAS_NP_ACCOUNT: v = 1; break; /* Has NP account */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CAMERA_PLFREQ:              v = 0; break; /* 60Hz */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_RUMBLE:                 v = 1; break; /* Rumble on */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_KEYBOARD_TYPE:              v = 0; break; /* US/101 */
    case CELL_SYSUTIL_SYSTEMPARAM_ID_PAD_AUTOOFF:                v = 0; break; /* Disabled */
    default:
        printf("[cellSysutil] GetSystemParamInt: unknown id 0x%04X\n", id);
        v = 0;
        break;
    }

    /* big-endian store into guest memory */
    uint8_t* p = vm_base + gaddr;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
    return CELL_OK;
}

s32 cellSysutilGetSystemParamString(s32 id, char* buf_guest, u32 bufsize)
{
    /* buf_guest is a raw GUEST address; translate into the VM. (byte string -> no bswap) */
    extern uint8_t* vm_base;
    if (!(uint32_t)(uintptr_t)buf_guest || bufsize == 0)
        return CELL_SYSUTIL_ERROR_VALUE;
    char* buf = (char*)(vm_base + (uint32_t)(uintptr_t)buf_guest);

    switch (id) {
    case CELL_SYSUTIL_SYSTEMPARAM_ID_NICKNAME:
        strncpy(buf, "ps3recomp_user", bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    case CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME:
        strncpy(buf, "User", bufsize - 1);
        buf[bufsize - 1] = '\0';
        break;
    default:
        printf("[cellSysutil] GetSystemParamString: unknown id 0x%04X\n", id);
        buf[0] = '\0';
        break;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * BGM playback control
 * -----------------------------------------------------------------------*/

s32 cellSysutilEnableBgmPlayback(void)
{
    printf("[cellSysutil] EnableBgmPlayback()\n");
    s_bgm_enabled = 1;
    return CELL_OK;
}

s32 cellSysutilDisableBgmPlayback(void)
{
    printf("[cellSysutil] DisableBgmPlayback()\n");
    s_bgm_enabled = 0;
    s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
    return CELL_OK;
}

s32 cellSysutilGetBgmPlaybackStatus(s32* status)
{
    if (!status)
        return CELL_SYSUTIL_ERROR_VALUE;

    *status = s_bgm_status;
    return CELL_OK;
}

s32 cellSysutilSetBgmPlaybackExtraParam(void* param)
{
    (void)param;
    printf("[cellSysutil] SetBgmPlaybackExtraParam()\n");
    return CELL_OK;
}

s32 cellSysutilEnableBgmPlaybackEx(s32 param)
{
    (void)param;
    printf("[cellSysutil] EnableBgmPlaybackEx(%d)\n", param);
    s_bgm_enabled = 1;
    return CELL_OK;
}

s32 cellSysutilDisableBgmPlaybackEx(void)
{
    printf("[cellSysutil] DisableBgmPlaybackEx()\n");
    s_bgm_enabled = 0;
    s_bgm_status = CELL_SYSUTIL_BGMPLAYBACK_STATUS_STOP;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * System cache
 * -----------------------------------------------------------------------*/

s32 cellSysCacheMount(char* cachePath_g)
{
    extern uint8_t* vm_base;
    printf("[cellSysutil] SysCacheMount()\n");

    if (!(uint32_t)(uintptr_t)cachePath_g)
        return CELL_EINVAL;
    char* cachePath = (char*)(vm_base + (uint32_t)(uintptr_t)cachePath_g); /* guest addr -> host */

    /* Provide a temp directory path */
    strncpy(s_cache_path, "/dev_hdd1/caches", CELL_SYSCACHE_PATH_MAX - 1);
    s_cache_path[CELL_SYSCACHE_PATH_MAX - 1] = '\0';
    strncpy(cachePath, s_cache_path, CELL_SYSCACHE_PATH_MAX - 1);
    cachePath[CELL_SYSCACHE_PATH_MAX - 1] = '\0';
    s_cache_mounted = 1;

    return CELL_OK;
}

s32 cellSysCacheClear(void)
{
    printf("[cellSysutil] SysCacheClear()\n");
    s_cache_mounted = 0;
    s_cache_path[0] = '\0';
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Disc game check
 * -----------------------------------------------------------------------*/

s32 cellDiscGameGetBootDiscInfo(u32* type_g, char* titleId_g, u32 titleIdSize)
{
    extern uint8_t* vm_base;
    printf("[cellSysutil] DiscGameGetBootDiscInfo()\n");

    if ((uint32_t)(uintptr_t)type_g) {
        uint8_t* p = vm_base + (uint32_t)(uintptr_t)type_g;     /* BE u32 store */
        uint32_t t = CELL_DISCGAME_TYPE_HDD;
        p[0]=(uint8_t)(t>>24); p[1]=(uint8_t)(t>>16); p[2]=(uint8_t)(t>>8); p[3]=(uint8_t)t;
    }

    if ((uint32_t)(uintptr_t)titleId_g && titleIdSize > 0) {
        char* titleId = (char*)(vm_base + (uint32_t)(uintptr_t)titleId_g);
        strncpy(titleId, "GAME00000", titleIdSize - 1);
        titleId[titleIdSize - 1] = '\0';
    }

    return CELL_OK;
}

s32 cellDiscGameRegisterDiscChangeCallback(void (*callback)(void*), void* arg)
{
    printf("[cellSysutil] DiscGameRegisterDiscChangeCallback()\n");
    s_disc_change_cb = callback;
    s_disc_change_arg = arg;
    return CELL_OK;
}

s32 cellDiscGameUnregisterDiscChangeCallback(void)
{
    printf("[cellSysutil] DiscGameUnregisterDiscChangeCallback()\n");
    s_disc_change_cb = NULL;
    s_disc_change_arg = NULL;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Misc utilities
 * -----------------------------------------------------------------------*/

s32 cellSysutilGetLicenseArea(void)
{
    /* Return 'A' for America */
    return 'A';
}

s32 cellSysutilIsMeetingApp(void)
{
    return 0; /* Not a meeting app */
}
