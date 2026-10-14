/*
 * ps3recomp - cellSail HLE implementation
 *
 * Player lifecycle, state and events are modelled call by call. A PAMF
 * stream with MPEG-2 video and LPCM audio is played for real (sail_movie.c*:
 * Media Foundation on Windows): the graphics adapter hands out decoded frames
 * in buffers the title allocates through its own pAlloc callback, the sound
 * adapter hands out float PCM, and the end of the stream is reported when the
 * last frame has been shown. Anything else -- or PS3_SAIL_MOVIES=0, or a host
 * without a decoder -- ends at once, as before.
 */

#include "ps3emu/guest_call.h"
#include "cellSail.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_read32 / vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   /* getenv -- Clang errors on the implicit declaration MSVC allows */
#include "../guest_struct.h"   /* GUEST_EA, guest_struct_load/store */
#include "cellPamf.h"           /* pamf_reader_init_host: a PAMF database is a reader */
#include "sail_movie.h"
#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "ps3emu/ps3_knob.h"            /* ps3_env_int */
/* runtime/syscalls/sys_fs.c: the guest -> host path mapping cellFs uses too. */
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);
#ifdef _WIN32
#include <windows.h>
#else
#include "../../runtime/platform/win32_compat.h"   /* SRWLOCK, CONDITION_VARIABLE */
#endif

/* Internal state */

static int s_initialized = 0;

typedef struct {
    int in_use;
    s32 state;
    CellSailPlayerCallback callback;
    void* callbackArg;
} SailPlayer;

static SailPlayer s_players[CELL_SAIL_PLAYER_MAX];

/* Players registered by cellSailPlayerInitialize2, keyed by the guest struct EA
 * the title passed as `pSelf`. Declared here because the adapter setters below
 * need it -- they take that same pointer, not an index into s_players[]. */
static struct { u32 self_ea, allocator_ea, callback_ea, arg_ea; int in_use; int state;
                u32 desc; /* descriptor added with AddDescriptor (0 = none) */
                u32 gfx_ea, snd_ea;   /* adapters set with Set*Adapter (0 = none) */
                SailMovie* movie;     /* open stream being played (NULL = none) */
                u32 session;          /* FrameInfo.sessionId of the open stream */
                int playing;          /* started and its end not reported yet */ }
    s_sail_players[CELL_SAIL_PLAYER_MAX];

/* A CellSailPlayer is a GUEST POINTER, not a small integer handle.
 *
 * Every player entry point below used to validate its first argument as an
 * index (`handle >= CELL_SAIL_PLAYER_MAX`) into the handle-keyed s_players[]
 * table, which rejects every real call: a guest pointer is always larger than
 * the table. cellSailPlayerInitialize2 has always registered the player by its
 * `pSelf` EA, so the two halves of this file disagreed about what a handle is.
 *
 * That is not a harmless error return. Tokyo Jungle sets its sound adapter
 * during audio init; on failure it abandons the path, closes a resource it
 * never opened ("sgxResClose unknown ID (0)"), terminates its SGX sound system
 * and then blocks forever joining an audio thread that will not exit.
 *
 * One lookup, used by all of them, so the disagreement cannot come back. */
static int sail_player_slot(u32 self_ea)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use && s_sail_players[i].self_ea == self_ea)
            return i;
    return -1;
}

/* Lifecycle */

s32 cellSailInit(void)
{
    printf("[cellSail] Init()\n");
    if (s_initialized)
        return (s32)CELL_SAIL_ERROR_ALREADY_INITIALIZED;
    memset(s_players, 0, sizeof(s_players));
    s_initialized = 1;
    return CELL_OK;
}

s32 cellSailTerm(void)
{
    printf("[cellSail] Term()\n");
    s_initialized = 0;
    return CELL_OK;
}

/* Player management */

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle)
{
    (void)attr; (void)resource;
    printf("[cellSail] PlayerCreate()\n");

    if (!s_initialized) return (s32)CELL_SAIL_ERROR_NOT_INITIALIZED;
    if (!handle) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_players[i].in_use) {
            memset(&s_players[i], 0, sizeof(SailPlayer));
            s_players[i].in_use = 1;
            s_players[i].state = CELL_SAIL_PLAYER_STATE_INITIALIZED;
            s_players[i].callback = callback;
            s_players[i].callbackArg = callbackArg;
            vm_write32((u32)(uintptr_t)handle, (u32)i);   /* guest out-param */
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

s32 cellSailPlayerDestroy(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerDestroy(%u)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].in_use = 0;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_CLOSED;
    return CELL_OK;
}

/* CellSailEvent is {be32 major; be32 minor} -- one 64-bit register, major in
 * the high half. The handler is called (pArg, event, arg0, arg1).
 *
 * Async player calls report completion as major=2 (CALL_COMPLETED), minor =
 * the CellSailPlayerCall id that finished, arg0 = the call's result and
 * arg1 = the userParam the title passed to that call. Titles pass a
 * CellSailFuture there: Demon's Souls' handler does
 * cellSailFutureSet(arg1, arg0) on BOOT and later cellSailFutureGet()s it, so
 * a completion that dropped arg1 set a NULL future.
 *
 * We run every call synchronously and notify before it returns. That is safe
 * for titles that queue the event and act on it later (Demon's Souls posts
 * each one to its own event port and drains one per frame), which is also
 * the only way they can call back into the player. */
#define SAIL_EV_CALL_COMPLETED 2
enum { SAIL_CALL_BOOT = 1, SAIL_CALL_OPEN_STREAM = 2, SAIL_CALL_CLOSE_STREAM = 3,
       SAIL_CALL_OPEN_ES_AUDIO = 4, SAIL_CALL_OPEN_ES_VIDEO = 5,
       SAIL_CALL_START = 10, SAIL_CALL_STOP = 11 };

/* The other events the stream lifecycle produces. arg0 of STREAM_OPENED /
 * STREAM_CLOSED is the descriptor; arg0 of ES_OPENED is the CellSailEsType
 * (AUDIO 0, VIDEO 1). Demon's Souls answers STREAM_OPENED by opening the
 * video ES, the video ES_OPENED by opening its audio ES, and treats the audio
 * ES_OPENED as "ready to start"; it closes a movie only after STREAM_CLOSED
 * (it destroys the descriptor there) and the CLOSED state have both arrived. */
#define SAIL_EV_STREAM_OPENED        4
#define SAIL_EV_STREAM_CLOSED        5
#define SAIL_EV_PAUSE_STATE_CHANGED  7
#define SAIL_EV_SOURCE_EOS           8
#define SAIL_EV_ES_OPENED            9
enum { SAIL_ES_AUDIO = 0, SAIL_ES_VIDEO = 1 };

static void sail_notify2(int slot, u32 major, u32 minor, u64 arg0, u64 arg1)
{
    if (slot < 0 || !g_ps3_guest_caller) return;
    const u32 cb = s_sail_players[slot].callback_ea;
    if (!cb) return;
    g_ps3_guest_caller(cb, s_sail_players[slot].arg_ea,
                       ((u64)major << 32) | minor, arg0, arg1, 0, 0, 0, 0);
}

static void sail_notify(int slot, u32 major, u32 minor, u64 arg0)
{
    sail_notify2(slot, major, minor, arg0, 0);
}

static void sail_completed(int slot, u32 call, u64 userParam)
{
    sail_notify2(slot, SAIL_EV_CALL_COMPLETED, call, 0 /* CELL_OK */, userParam);
}

/* The player also reports every state it passes through: major=3
 * (PLAYER_STATE_CHANGED), minor 0, arg0 = the new state in the SDK's
 * numbering -- not the CELL_SAIL_PLAYER_STATE_* values in cellSail.h, which
 * are this file's own bookkeeping. Midway Arcade Origins maps arg0 through a
 * nine-entry table (CLOSED=2, OPENED=4, RUNNING=6, ...) and opens nothing until
 * boot has reported CLOSED; without these events it boots the player and then
 * waits on a black screen for good. */
#define SAIL_EV_STATE_CHANGED 3
enum { SAIL_ST_BOOT_TRANSITION = 1, SAIL_ST_CLOSED = 2, SAIL_ST_OPEN_TRANSITION = 3,
       SAIL_ST_OPENED = 4, SAIL_ST_START_TRANSITION = 5, SAIL_ST_RUNNING = 6,
       SAIL_ST_STOP_TRANSITION = 7, SAIL_ST_CLOSE_TRANSITION = 8 };

/* One synchronous call: transition state, completion, settled state. */
static void sail_call(int slot, u32 call, u64 userParam, u32 via, u32 to)
{
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, via);
    sail_completed(slot, call, userParam);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, to);
}

/* A finished stream: the source reports end of stream and the player drops
 * back to OPENED, which is what a title waiting out a movie (Midway's logo
 * AVIs, Demon's Souls' intro) watches for. */
static void sail_ended(int slot)
{
    s_sail_players[slot].playing = 0;
    s_sail_players[slot].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    sail_notify(slot, SAIL_EV_SOURCE_EOS, 0, 0);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_STOP_TRANSITION);
    sail_notify(slot, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED);
}

/* ---------------------------------------------------------------------------
 * Adapters
 *
 * The title's adapter structs are zeroed and our state is kept here, keyed by
 * their EA. CellSailGraphicsAdapterFuncs is {pMakeup, pCleanup, pFormatChanged,
 * pAlloc, pFree} (guest OPDs); pAlloc(pArg, size, num, ppFrame) returns how
 * many frame buffers it put in ppFrame[]. Frame and sound buffers are handed
 * to the title as CellSailGraphicsFrameInfo / CellSailSoundFrameInfo:
 * {u32 pBuffer; u32 sessionId; u32 tag; s32 status; u64 pts}.
 * -----------------------------------------------------------------------*/
#define SAIL_ADAPTERS     8
#define SAIL_GFX_FRAMES   4      /* frame buffers asked of pAlloc per stream */
#define SAIL_SND_SLOTS    4      /* sound buffers rotated through */
#define SAIL_SND_SLOT     0x10000u
/* Guest scratch for the pAlloc out-array and the sound buffers: past the
 * cellAudio port window (0x58000000 + 8 x 2 MB), outside sys_memory, the HLE
 * heap and sys_vm. One 1 MB page per player. */
#define SAIL_SCRATCH      0x59800000u
#define SAIL_SCRATCH_SIZE 0x100000u

typedef struct {
    u32 self_ea, funcs_ea, arg_ea;
    int is_gfx;
    u8  pref[32];           /* CellSailVideoFormat from SetPreferredFormat */
    int have_pref;
    u32 frame[SAIL_GFX_FRAMES];
    int nframe, next;       /* frames allocated for `session`, next to fill */
    u32 frame_session;
    int samples;            /* sound: sampleNum of the last GetFrame */
    u32 tag;
    u32 last_buf;           /* graphics: frame handed out last, and its pts */
    u64 last_pts;
} SailAdapter;

static SailAdapter s_adapters[SAIL_ADAPTERS];

static SailAdapter* sail_adapter(u32 ea, int create, int is_gfx)
{
    for (int i = 0; i < SAIL_ADAPTERS; i++)
        if (s_adapters[i].self_ea == ea) return &s_adapters[i];
    if (!create) return NULL;
    for (int i = 0; i < SAIL_ADAPTERS; i++)
        if (!s_adapters[i].self_ea) {
            memset(&s_adapters[i], 0, sizeof s_adapters[i]);
            s_adapters[i].self_ea = ea;
            s_adapters[i].is_gfx = is_gfx;
            return &s_adapters[i];
        }
    return NULL;
}

/* The player an adapter is attached to (slot), or -1. */
static int sail_adapter_player(u32 ea)
{
    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++)
        if (s_sail_players[i].in_use &&
            (s_sail_players[i].gfx_ea == ea || s_sail_players[i].snd_ea == ea))
            return i;
    return -1;
}

static u32 sail_scratch(int slot)
{
    static int committed[CELL_SAIL_PLAYER_MAX];
    const u32 ea = SAIL_SCRATCH + (u32)slot * SAIL_SCRATCH_SIZE;
    if (!committed[slot]) {
        if (vm_commit(ea, SAIL_SCRATCH_SIZE) != CELL_OK) return 0;
        committed[slot] = 1;
    }
    return ea;
}

/* PS3_SAIL_MOVIES=0: never play, every stream ends at once (the old answer). */
static int sail_movies_on(void)
{
    static int on = -1;
    if (on < 0) on = ps3_env_int("PS3_SAIL_MOVIES", 1) != 0;
    return on;
}

static const char* sail_desc_uri(u32 desc);   /* descriptors are below */

static void sail_free_frames(int slot);       /* with the adapters, below */

/* The frame fetches run on the title's audio and render threads while
 * CloseStream / Finalize run on another: Demon's Souls closes a movie that is
 * skipped with START while its mixer is inside cellSailSoundAdapterGetFrame.
 * The movie pointer is read under this lock (shared) and replaced under it
 * (exclusive). Guest callbacks are never made while it is held, so a
 * callback that closes the stream cannot deadlock against it. */
static SRWLOCK s_movie_lock = SRWLOCK_INIT;

static SailMovie* sail_movie_get(int slot)   /* returns with the lock shared */
{
    AcquireSRWLockShared(&s_movie_lock);
    return s_sail_players[slot].movie;
}

static void sail_movie_put(void)
{
    ReleaseSRWLockShared(&s_movie_lock);
}

/* The open movie's stream info and session; 0 when none is open. */
static int sail_movie_info_of(int slot, SailMovieInfo* mi, u32* session)
{
    memset(mi, 0, sizeof *mi);
    SailMovie* m = sail_movie_get(slot);
    if (m) sail_movie_info(m, mi);
    if (session) *session = s_sail_players[slot].session;
    sail_movie_put();
    return m != NULL;
}

static void sail_close_movie(int slot)
{
    sail_free_frames(slot);
    AcquireSRWLockExclusive(&s_movie_lock);
    SailMovie* m = s_sail_players[slot].movie;
    s_sail_players[slot].movie = NULL;
    ReleaseSRWLockExclusive(&s_movie_lock);
    sail_movie_close(m);   /* nothing can reach it now */
    s_sail_players[slot].playing = 0;
}

/* Opens the descriptor's stream for playback, if it is one we can play. */
static void sail_open_movie(int slot)
{
    sail_close_movie(slot);
    const u32 desc = s_sail_players[slot].desc;
    if (!sail_movies_on() || !desc) return;
    const char* uri = sail_desc_uri(desc);
    if (!uri) return;
    static const char scheme[] = "x-cell-fs://";
    const char* guest_path = strncmp(uri, scheme, sizeof scheme - 1) == 0 ? uri + sizeof scheme - 1 : uri;
    char host[1024];
    sys_fs_translate_path(guest_path, host, (int)sizeof host);
    int coding = SAIL_MOVIE_ARGB;
    SailAdapter* g = sail_adapter(s_sail_players[slot].gfx_ea, 0, 1);
    if (g && g->have_pref && g->pref[0] == 1 /* RGBA_INTERLEAVED */) coding = SAIL_MOVIE_RGBA;
    SailMovie* m = sail_movie_open(host, coding);
    static u32 session;
    AcquireSRWLockExclusive(&s_movie_lock);
    s_sail_players[slot].movie = m;
    s_sail_players[slot].session = ++session;
    ReleaseSRWLockExclusive(&s_movie_lock);
    if (!m)
        printf("[cellSail] %s: not playable here, it will end at once\n", guest_path);
}

/* Called from the frame fetches: once the movie has run out, report its end
 * (on the title's thread, as the real player's events would arrive). */
static void sail_check_end(int slot, int eos)
{
    if (eos && InterlockedExchange((volatile LONG*)&s_sail_players[slot].playing, 0)) {
        printf("[cellSail] stream ended\n");
        sail_ended(slot);
    }
}

s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam)
{
    printf("[cellSail] PlayerBoot(0x%08X, user=0x%llX)\n", handle, (unsigned long long)userParam);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    sail_call(_sp, SAIL_CALL_BOOT, userParam, SAIL_ST_BOOT_TRANSITION, SAIL_ST_CLOSED);
    return CELL_OK;
}

/* Opens the descriptor the title added with cellSailPlayerAddDescriptor. Its
 * second argument is the userParam, not a path: this used to read it as a
 * guest string and always printed "null". */
s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, u64 userParam)
{
    const int _sp = sail_player_slot((u32)handle);
    printf("[cellSail] PlayerOpenStream(0x%08X, desc=%u, user=0x%llX)\n", handle,
           _sp < 0 ? 0u : s_sail_players[_sp].desc, (unsigned long long)userParam);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPEN_TRANSITION);
    sail_open_movie(_sp);
    if (s_sail_players[_sp].desc)
        sail_notify(_sp, SAIL_EV_STREAM_OPENED, 0, s_sail_players[_sp].desc);
    sail_completed(_sp, SAIL_CALL_OPEN_STREAM, userParam);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle, u64 userParam)
{
    printf("[cellSail] PlayerCloseStream(0x%08X, user=0x%llX)\n", handle,
           (unsigned long long)userParam);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_CLOSE_TRANSITION);
    sail_close_movie(_sp);
    if (s_sail_players[_sp].desc)
        sail_notify(_sp, SAIL_EV_STREAM_CLOSED, 0, s_sail_players[_sp].desc);
    sail_completed(_sp, SAIL_CALL_CLOSE_STREAM, userParam);
    sail_notify(_sp, SAIL_EV_STATE_CHANGED, 0, SAIL_ST_CLOSED);
    return CELL_OK;
}

/* cellSailPlayerOpenEsVideo/Audio(pSelf, index, pSpecifier, flags, userParam).
 * pSpecifier is what the title selected the stream with (Demon's Souls passes
 * the PAMF reader from CreateDatabase); nothing here decodes, so it is only
 * acknowledged. */
static s32 sail_open_es(CellSailPlayerHandle handle, const char* what, u32 es,
                        u32 call, s32 index, u64 userParam)
{
    printf("[cellSail] PlayerOpenEs%s(0x%08X, index=%d, user=0x%llX)\n", what, handle,
           index, (unsigned long long)userParam);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_notify(_sp, SAIL_EV_ES_OPENED, 0, es);
    sail_completed(_sp, call, userParam);
    return CELL_OK;
}

s32 cellSailPlayerOpenEsVideo(CellSailPlayerHandle handle, s32 index, u32 pSpecifier,
                              u32 flags, u64 userParam)
{
    (void)pSpecifier; (void)flags;
    return sail_open_es(handle, "Video", SAIL_ES_VIDEO, SAIL_CALL_OPEN_ES_VIDEO, index, userParam);
}

s32 cellSailPlayerOpenEsAudio(CellSailPlayerHandle handle, s32 index, u32 pSpecifier,
                              u32 flags, u64 userParam)
{
    (void)pSpecifier; (void)flags;
    return sail_open_es(handle, "Audio", SAIL_ES_AUDIO, SAIL_CALL_OPEN_ES_AUDIO, index, userParam);
}

s32 cellSailPlayerSetEsAudioMuted(CellSailPlayerHandle handle, s32 index, s32 muted)
{
    (void)index; (void)muted;
    return sail_player_slot((u32)handle) < 0 ? (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT : CELL_OK;
}

s32 cellSailPlayerStart(CellSailPlayerHandle handle, u32 pCommand, u64 userParam)
{
    (void)pCommand;
    printf("[cellSail] PlayerStart(0x%08X, user=0x%llX)\n", handle, (unsigned long long)userParam);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_RUNNING;
    sail_call(_sp, SAIL_CALL_START, userParam, SAIL_ST_START_TRANSITION, SAIL_ST_RUNNING);
    s_sail_players[_sp].playing = 1;
    SailMovie* m = sail_movie_get(_sp);
    if (m) sail_movie_start(m);                        /* ends from the frame fetches */
    sail_movie_put();
    if (!m) sail_ended(_sp);                           /* nothing to play: over already */
    return CELL_OK;
}

s32 cellSailPlayerStop(CellSailPlayerHandle handle, u64 userParam)
{
    printf("[cellSail] PlayerStop(0x%08X, user=0x%llX)\n", handle, (unsigned long long)userParam);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    s_sail_players[_sp].playing = 0;
    sail_call(_sp, SAIL_CALL_STOP, userParam, SAIL_ST_STOP_TRANSITION, SAIL_ST_OPENED);
    return CELL_OK;
}

s32 cellSailPlayerSetPaused(CellSailPlayerHandle handle, s32 paused)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_notify(_sp, SAIL_EV_PAUSE_STATE_CHANGED, 0, paused ? 1 : 0);
    return CELL_OK;
}

/* cellSailPlayerReplaceEventHandler(pSelf, pNewCallback, pNewArg,
 * ppOldCallback, ppOldArg). Demon's Souls swaps its handler from inside the
 * BOOT completion; every later event must reach the new one. */
s32 cellSailPlayerReplaceEventHandler(CellSailPlayerHandle handle, u32 pNewCallback,
                                      u32 pNewArg, u32 ppOldCallback, u32 ppOldArg)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0 || !pNewCallback)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (ppOldCallback) vm_write32(ppOldCallback, s_sail_players[_sp].callback_ea);
    if (ppOldArg)      vm_write32(ppOldArg, s_sail_players[_sp].arg_ea);
    s_sail_players[_sp].callback_ea = pNewCallback;
    s_sail_players[_sp].arg_ea      = pNewArg;
    return CELL_OK;
}

/* Player options we do not model. They must still answer for a valid player:
 * Demon's Souls gives up on its movie when SetParameter returns an error. */
s32 cellSailPlayerSetParameter(CellSailPlayerHandle handle, s32 type, u64 param0, u64 param1)
{
    (void)type; (void)param0; (void)param1;
    return sail_player_slot((u32)handle) < 0 ? (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT : CELL_OK;
}

s32 cellSailPlayerUnsubscribeEvent(CellSailPlayerHandle handle, s32 eventType)
{
    (void)eventType;
    return sail_player_slot((u32)handle) < 0 ? (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT : CELL_OK;
}

s32 cellSailPlayerFinalize(CellSailPlayerHandle handle)
{
    printf("[cellSail] PlayerFinalize(0x%08X)\n", handle);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_close_movie(_sp);
    memset(&s_sail_players[_sp], 0, sizeof(s_sail_players[_sp]));
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * CellSailFuture {be32 mutex_id; be32 cond_id; be32 flags; be32 result;
 *                 be64 userParam}
 *
 * The real library backs each future with an lv2 mutex + cond. The waiting
 * guest thread is a host thread here, so one host lock and condition variable
 * for all futures does the same job; the state itself stays in the guest
 * struct (flags bit 0 = done, result) where the title can see it.
 * -----------------------------------------------------------------------*/
#define SAIL_FUT_FLAGS   8
#define SAIL_FUT_RESULT  12
#define SAIL_FUT_DONE    1u

static SRWLOCK            s_fut_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_fut_cv   = CONDITION_VARIABLE_INIT;

s32 cellSailFutureInitialize(u32 pSelf)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    AcquireSRWLockExclusive(&s_fut_lock);
    for (u32 i = 0; i < 0x18; i += 4) vm_write32(pSelf + i, 0);
    ReleaseSRWLockExclusive(&s_fut_lock);
    return CELL_OK;
}

s32 cellSailFutureFinalize(u32 pSelf)
{
    return pSelf ? CELL_OK : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailFutureReset(u32 pSelf, s32 wait)
{
    (void)wait;
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    AcquireSRWLockExclusive(&s_fut_lock);
    vm_write32(pSelf + SAIL_FUT_FLAGS, 0);
    vm_write32(pSelf + SAIL_FUT_RESULT, 0);
    ReleaseSRWLockExclusive(&s_fut_lock);
    return CELL_OK;
}

s32 cellSailFutureSet(u32 pSelf, s32 result)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    AcquireSRWLockExclusive(&s_fut_lock);
    vm_write32(pSelf + SAIL_FUT_RESULT, (u32)result);
    vm_write32(pSelf + SAIL_FUT_FLAGS, vm_read32(pSelf + SAIL_FUT_FLAGS) | SAIL_FUT_DONE);
    ReleaseSRWLockExclusive(&s_fut_lock);
    WakeAllConditionVariable(&s_fut_cv);
    return CELL_OK;
}

/* Waits until the future is set. timeout is in microseconds, 0 = no limit
 * (the lv2 cond convention the real library passes it to). On expiry it
 * returns lv2's ETIMEDOUT, which is what that cond wait reports. */
s32 cellSailFutureGet(u32 pSelf, u64 timeout, u32 pResult)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    const ULONGLONG deadline = timeout ? GetTickCount64() + (timeout + 999) / 1000 : 0;
    s32 rc = CELL_OK;
    AcquireSRWLockExclusive(&s_fut_lock);
    while (!(vm_read32(pSelf + SAIL_FUT_FLAGS) & SAIL_FUT_DONE)) {
        DWORD ms = INFINITE;
        if (timeout) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) { rc = (s32)0x8001000Bu; break; }   /* CELL_ETIMEDOUT */
            ms = (DWORD)(deadline - now);
        }
        SleepConditionVariableSRW(&s_fut_cv, &s_fut_lock, ms, 0);
    }
    if (rc == CELL_OK && pResult)
        vm_write32(pResult, vm_read32(pSelf + SAIL_FUT_RESULT));
    ReleaseSRWLockExclusive(&s_fut_lock);
    return rc;
}

s32 cellSailFutureIsDone(u32 pSelf, u32 pResult)
{
    if (!pSelf || !pResult) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    AcquireSRWLockExclusive(&s_fut_lock);
    vm_write32(pResult, (vm_read32(pSelf + SAIL_FUT_FLAGS) & SAIL_FUT_DONE) ? 1u : 0u);
    ReleaseSRWLockExclusive(&s_fut_lock);
    return CELL_OK;
}

s32 cellSailPlayerPause(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_PAUSE;
    return CELL_OK;
}

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!state) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)state, (u32)s_sail_players[_sp].state);
    return CELL_OK;
}

s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!streamNum) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    vm_write32((u32)(uintptr_t)streamNum, 0);   /* no streams in stub */
    return CELL_OK;
}

s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info)
{
    (void)streamIndex;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (!info) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return (s32)CELL_SAIL_ERROR_NOT_FOUND;
}

/* r3 is a CellSailPlayer* -- a GUEST POINTER, the same `pSelf` that
 * cellSailPlayerInitialize2 registered -- not a small integer handle. Validating
 * it as an index (`handle >= CELL_SAIL_PLAYER_MAX`) rejects every real call,
 * because a guest pointer is always larger than the table size.
 *
 * That is not a harmless error return: Tokyo Jungle sets its sound adapter
 * during audio init, and on failure abandons the whole path -- it closes a
 * resource it never opened ("sgxResClose unknown ID (0)"), terminates its SGX
 * sound system, and then blocks forever joining an audio thread that is not
 * going to exit. Look the player up the way Initialize2 stored it. */
s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index, void* adapter)
{
    (void)index;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].snd_ea = (u32)(uintptr_t)adapter;
    return CELL_OK;
}

/* r3 is a CellSailPlayer* -- a GUEST POINTER, the same `pSelf` that
 * cellSailPlayerInitialize2 registered -- not a small integer handle. Validating
 * it as an index (`handle >= CELL_SAIL_PLAYER_MAX`) rejects every real call,
 * because a guest pointer is always larger than the table size.
 *
 * That is not a harmless error return: Tokyo Jungle sets its sound adapter
 * during audio init, and on failure abandons the whole path -- it closes a
 * resource it never opened ("sgxResClose unknown ID (0)"), terminates its SGX
 * sound system, and then blocks forever joining an audio thread that is not
 * going to exit. Look the player up the way Initialize2 stored it. */
s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index, void* adapter)
{
    (void)index;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].gfx_ea = (u32)(uintptr_t)adapter;
    return CELL_OK;
}

s32 cellSailPlayerCancel(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_sail_players[_sp].state = CELL_SAIL_PLAYER_STATE_FINISHED;
    return CELL_OK;
}

s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle)
{
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return 0;
    return (s_sail_players[_sp].state == CELL_SAIL_PLAYER_STATE_PAUSE) ? 1 : 0;
}

s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode, void* command)
{
    (void)command;
    printf("[cellSail] SetRepeatMode(%u, %d)\n", handle, repeatMode);
    const int _sp = sail_player_slot((u32)handle);
    if (_sp < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    /* Real ABI returns the (now-set) repeat mode in r3, not an error code. */
    return repeatMode;
}

/* ---------------------------------------------------------------------------
 * Descriptor management
 * -----------------------------------------------------------------------*/

#define MAX_DESCRIPTORS 16

typedef struct {
    int in_use;
    s32 streamType;
    CellSailPlayerHandle player;
    char uri[512];
} SailDescriptor;

static SailDescriptor s_descs[MAX_DESCRIPTORS];

/* Descriptor handles are slot + 1. The title stores the handle where the real
 * library puts a descriptor POINTER and may test it against 0 ("no stream"),
 * so slot 0 must not come out as handle 0. Returns the slot, or -1. */
static int sail_desc_slot(CellSailDescriptorHandle desc)
{
    if (desc == 0 || desc > MAX_DESCRIPTORS || !s_descs[desc - 1].in_use)
        return -1;
    return (int)desc - 1;
}

s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* mediaInfo,
                                     char* uri,
                                     CellSailDescriptorHandle* desc)
{
    (void)mediaInfo;

    /* uri is a GUEST address. Printing it as a host char* faulted the moment
     * the title opened a movie -- the log line stopped mid-token at
     * 'uri="' and the process died with 0xC0000005. Copy it out of guest
     * memory first. The descriptor has always had a uri[512] field; nothing
     * ever filled it, so the URI was discarded even when it did not crash. */
    char uri_host[512];
    uri_host[0] = '\0';
    if (uri) {
        u32 ea = (u32)(uintptr_t)uri;
        size_t n = 0;
        while (n < sizeof(uri_host) - 1) {
            char c = (char)vm_read8(ea + (u32)n);
            if (!c) break;
            uri_host[n++] = c;
        }
        uri_host[n] = '\0';
    }

    printf("[cellSail] CreateDescriptor(player=%u, type=%d, uri=\"%s\")\n", handle,
           streamType, uri ? uri_host : "null");
    if (!desc) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    for (int i = 0; i < MAX_DESCRIPTORS; i++) {
        if (!s_descs[i].in_use) {
            memset(&s_descs[i], 0, sizeof(SailDescriptor));
            s_descs[i].in_use = 1;
            s_descs[i].streamType = streamType;
            s_descs[i].player = handle;
            snprintf(s_descs[i].uri, sizeof(s_descs[i].uri), "%s", uri_host);
            vm_write32((u32)(uintptr_t)desc, (u32)i + 1);   /* guest out-param */
            return CELL_OK;
        }
    }
    return (s32)CELL_SAIL_ERROR_OUT_OF_MEMORY;
}

static const char* sail_desc_uri(u32 desc)
{
    const int d = sail_desc_slot(desc);
    return d < 0 ? NULL : s_descs[d].uri;
}

s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc)
{
    const int d = sail_desc_slot(desc);
    if (d < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    s_descs[d].in_use = 0;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp >= 0 && s_sail_players[_sp].desc == desc)
        s_sail_players[_sp].desc = 0;
    return CELL_OK;
}

/* The player opens the descriptor added last; OpenStream reports it back in
 * STREAM_OPENED. */
s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc)
{
    printf("[cellSail] AddDescriptor(player=0x%08X, desc=%u)\n", handle, desc);
    if (sail_desc_slot(desc) < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp >= 0)
        s_sail_players[_sp].desc = desc;
    return CELL_OK;
}

s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle desc)
{
    if (sail_desc_slot(desc) < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    const int _sp = sail_player_slot((u32)handle);
    if (_sp >= 0 && s_sail_players[_sp].desc == desc)
        s_sail_players[_sp].desc = 0;
    return CELL_OK;
}

/* Read the header of the PAMF file a descriptor names. Its first 16 bytes give
 * the header size (offset 8, in 2048-byte units); the header is that many
 * bytes from the start of the file. Returns a malloc'd copy, or NULL. */
static void* sail_read_pamf_header(const char* uri, u32* out_size)
{
    static const char scheme[] = "x-cell-fs://";
    const char* guest_path = strncmp(uri, scheme, sizeof scheme - 1) == 0
                           ? uri + sizeof scheme - 1 : uri;
    char host[1024];
    sys_fs_translate_path(guest_path, host, (int)sizeof host);
    FILE* f = fopen(host, "rb");
    if (!f) {
        printf("[cellSail] cannot open %s (host %s)\n", guest_path, host);
        return NULL;
    }
    unsigned char head[16];
    void* buf = NULL;
    if (fread(head, 1, sizeof head, f) == sizeof head) {
        const u32 units = ((u32)head[8] << 24) | ((u32)head[9] << 16) |
                          ((u32)head[10] << 8) | head[11];
        /* A PAMF header is a few KB; anything past 16 MB is not a header. */
        const u32 size = units * 2048u;
        if (units && size <= (16u << 20) && (buf = malloc(size)) != NULL) {
            fseek(f, 0, SEEK_SET);
            if (fread(buf, 1, size, f) == size) *out_size = size;
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

/* cellSailDescriptorCreateDatabase(pSelf, pDatabase, size, arg).
 *
 * For a PAMF stream the database IS a CellPamfReader: Demon's Souls passes a
 * 0x80-byte buffer here and then calls cellPamfReader* on it directly (stream
 * counts, type and channel, LPCM info) to choose its audio track. So it must
 * be a reader initialised on the stream's real header. Other stream types
 * keep the old answer: nothing to build, CELL_OK. */
s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize, u64 arg)
{
    (void)arg;
    const int d = sail_desc_slot(desc);
    printf("[cellSail] DescriptorCreateDatabase(desc=%u, db=0x%08X, size=0x%X)\n",
           desc, (u32)(uintptr_t)dbAddr, dbSize);
    if (d < 0 || !dbAddr)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (s_descs[d].streamType != 0 /* CELL_SAIL_STREAM_PAMF */)
        return CELL_OK;
    u32 size = 0;
    void* hdr = sail_read_pamf_header(s_descs[d].uri, &size);
    if (!hdr)
        return (s32)CELL_SAIL_ERROR_INVALID_FD;
    /* Movie sound comes out as stereo when it is downmixed (sail_movie.h).
     * Demon's Souls reads the LPCM channel count from this reader and sets
     * up a 5.1 or a stereo mix to match, so it must see the same 2. */
    if (sail_movie_downmix() && size >= 0x88) {
        u8* h = (u8*)hdr;
        for (u32 i = 0; i < h[0x87] && 0x88 + (i + 1) * 48 <= size; i++) {
            u8* e = h + 0x88 + i * 48;
            if (e[0] == 0x80 /* LPCM */ && (e[16 + 2] & 0x0F) > 2)
                e[16 + 2] = (u8)((e[16 + 2] & 0xF0) | 2);
        }
    }
    const s32 rc = pamf_reader_init_host((u32)(uintptr_t)dbAddr, hdr, size);
    if (rc != CELL_OK)
        printf("[cellSail] PAMF header of %s rejected: 0x%08X\n", s_descs[d].uri, (u32)rc);
    return rc;
}

s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc, u32 pDatabase)
{
    const int d = sail_desc_slot(desc);
    if (d < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (pDatabase && s_descs[d].streamType == 0 /* CELL_SAIL_STREAM_PAMF */)
        pamf_reader_release(pDatabase);
    return CELL_OK;
}

/* Returns the CellSailStreamType itself (PAMF 0, MP4 1, AVI 2); Demon's
 * Souls branches on the return value. This used to return CELL_OK and write
 * the type through r4, which the title never set -- a store to whatever
 * address r4 happened to hold. */
s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc)
{
    const int d = sail_desc_slot(desc);
    if (d < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return s_descs[d].streamType;
}

s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable)
{
    (void)enable;
    if (sail_desc_slot(desc) < 0)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    return CELL_OK;
}

s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen)
{
    const int d = sail_desc_slot(desc);
    if (d < 0 || !uri || !maxLen)
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    /* uri is a guest address: copy byte by byte, terminator included. */
    const u32 ea = (u32)(uintptr_t)uri;
    u32 n = 0;
    for (; n + 1 < maxLen && s_descs[d].uri[n]; n++)
        vm_write8(ea + n, (u8)s_descs[d].uri[n]);
    vm_write8(ea + n, 0);
    return CELL_OK;
}

/* Memory allocator */

/* cellSailPlayerInitialize2(pSelf, pAllocator, pCallback, pUserParam,
 *                          pAttribute, pResource)  -- NID 0x23654375
 *
 * This was unimplemented, so it took the generic "return success and touch
 * nothing" path. That is the worst answer for an initialiser: the guest
 * believes its player struct is set up and reads whatever happened to be in
 * that memory. Tokyo Jungle then dereferenced one of those stale words and
 * faulted reading guest 0x10F000B4, well past the end of main memory.
 *
 * The struct fields are not documented here, and guessing offsets to write
 * would just trade one wrong value for another. What IS safe and sufficient is
 * to ZERO it: every pointer the guest reads out is then NULL, which callers
 * check, instead of a garbage address that faults. The handles we were given
 * are kept on our side, keyed by the struct EA, so no guest layout is assumed.
 */
#define SAIL_PLAYER_CLEAR   0x200u   /* well clear of the caller's next object */
#define SAIL_ADAPTER_CLEAR  0x100u


static void sail_zero_guest(u32 ea, u32 n)
{
    for (u32 i = 0; i < n; i += 4) vm_write32(ea + i, 0);
}

s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pUserParam, u32 pAttribute, u32 pResource)
{
    (void)pAttribute; (void)pResource;
    printf("[cellSail] PlayerInitialize2(self=0x%08X alloc=0x%08X cb=0x%08X)\n",
           pSelf, pAllocator, pCallback);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;

    /* PS3_NO_SAIL=1: report that the player cannot be created. On hardware
     * this call spawns the SAIL player's own worker threads, and a title that
     * then waits for one of them to signal readiness will wait forever against
     * a stub that spawns nothing. Failing cleanly lets a title take its
     * "no video" path instead of deadlocking during setup. */
    if (getenv("PS3_NO_SAIL")) {
        printf("[cellSail] PS3_NO_SAIL -- reporting player creation failure\n");
        return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    }

    sail_zero_guest(pSelf, SAIL_PLAYER_CLEAR);

    for (int i = 0; i < CELL_SAIL_PLAYER_MAX; i++) {
        if (!s_sail_players[i].in_use) {
            s_sail_players[i].in_use       = 1;
            s_sail_players[i].self_ea      = pSelf;
            s_sail_players[i].allocator_ea = pAllocator;
            s_sail_players[i].callback_ea  = pCallback;
            s_sail_players[i].arg_ea       = pUserParam;
            s_sail_players[i].desc         = 0;
            break;
        }
    }
    return CELL_OK;
}

/* cellSailSoundAdapterInitialize(pSelf, pCallbacks, pArg) -- NID 0x3D0D3B72.
 * Same reasoning: zero it rather than leave the guest reading stale memory.
 * Our side of the adapter is kept in s_adapters. */
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] SoundAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    SailAdapter* a = sail_adapter(pSelf, 1, 0);
    if (a) { a->funcs_ea = pCallbacks; a->arg_ea = pArg; }
    return CELL_OK;
}

/* cellSailGraphicsAdapterInitialize(pSelf, pCallbacks, pArg)
 *
 * The video-side twin of cellSailSoundAdapterInitialize, and it was missing --
 * so the import returned whatever was in r3 and the guest read an adapter it
 * believed was initialised. Same treatment as the sound adapter: zero the guest
 * struct so every pointer the title reads back is NULL (which callers check)
 * rather than a stale address that faults or is silently followed. */
s32 cellSailGraphicsAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg)
{
    printf("[cellSail] GraphicsAdapterInitialize(self=0x%08X funcs=0x%08X)\n",
           pSelf, pCallbacks);
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    sail_zero_guest(pSelf, SAIL_ADAPTER_CLEAR);
    SailAdapter* a = sail_adapter(pSelf, 1, 1);
    if (a) { a->funcs_ea = pCallbacks; a->arg_ea = pArg; }
    return CELL_OK;
}

/* cellSailGraphicsAdapterSetPreferredFormat(pSelf, pFormat)
 *
 * pFormat is a CellSailVideoFormat (32 bytes): s8 coding, scan, bitsPerColor,
 * frameRate; s16 width, height; s32 pitch, alpha; s8 colorMatrix, aspectRatio,
 * colorRange, reserved1; s32 reserved2; s64 reserved3. Kept for the coding
 * (ARGB or RGBA) and as the template GetFormat answers from. It must answer
 * CELL_OK for a valid adapter: a title that reads an error here concludes its
 * graphics adapter is unusable. */
s32 cellSailGraphicsAdapterSetPreferredFormat(u32 pSelf, u32 pFormat)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    SailAdapter* a = sail_adapter(pSelf, 1, 1);
    if (a && pFormat) {
        for (int i = 0; i < 32; i++) a->pref[i] = vm_read8(pFormat + (u32)i);
        a->have_pref = 1;
    }
    printf("[cellSail] GraphicsAdapterSetPreferredFormat(self=0x%08X coding=%d %dx%d pitch=%d)\n",
           pSelf, a ? (s8)a->pref[0] : -1, a ? (s16)(a->pref[4] << 8 | a->pref[5]) : 0,
           a ? (s16)(a->pref[6] << 8 | a->pref[7]) : 0,
           a ? (s32)((u32)a->pref[8] << 24 | a->pref[9] << 16 | a->pref[10] << 8 | a->pref[11]) : 0);
    return CELL_OK;
}

/* Frame buffers for the open stream, asked of the title's pAlloc the first
 * time a frame is due. */
static void sail_alloc_frames(int slot, SailAdapter* a, u32 size)
{
    if (a->frame_session == s_sail_players[slot].session && a->nframe > 0) return;
    a->nframe = 0;
    a->next = 0;
    a->last_buf = 0;
    a->frame_session = s_sail_players[slot].session;
    const u32 pAlloc = a->funcs_ea ? vm_read32(a->funcs_ea + 12) : 0;
    const u32 out = sail_scratch(slot);
    if (!pAlloc || !out || !g_ps3_guest_caller) return;
    for (int i = 0; i < SAIL_GFX_FRAMES; i++) vm_write32(out + 4u * i, 0);
    /* The guest call returns nothing to us: count what pAlloc filled in. */
    g_ps3_guest_caller(pAlloc, a->arg_ea, size, SAIL_GFX_FRAMES, out, 0, 0, 0, 0);
    for (int i = 0; i < SAIL_GFX_FRAMES; i++) {
        const u32 f = vm_read32(out + 4u * i);
        if (!f) break;
        a->frame[a->nframe++] = f;
    }
    printf("[cellSail] %d frame buffers of 0x%X bytes from the title, first at 0x%08X\n",
           a->nframe, size, a->frame[0]);
}

/* Gives the frame buffers back through pFree when the stream closes. */
static void sail_free_frames(int slot)
{
    SailAdapter* a = sail_adapter(s_sail_players[slot].gfx_ea, 0, 1);
    if (!a || !a->nframe) return;
    const u32 pFree = a->funcs_ea ? vm_read32(a->funcs_ea + 16) : 0;
    const u32 out = sail_scratch(slot);
    if (pFree && out && g_ps3_guest_caller) {
        for (int i = 0; i < a->nframe; i++) vm_write32(out + 4u * i, a->frame[i]);
        g_ps3_guest_caller(pFree, a->arg_ea, (u64)a->nframe, out, 0, 0, 0, 0, 0);
    }
    a->nframe = 0;
    a->frame_session = 0;
}

static void sail_frame_info(u32 pInfo, u32 buf, u32 session, u32 tag, u64 pts)
{
    vm_write32(pInfo + 0, buf);
    vm_write32(pInfo + 4, session);
    vm_write32(pInfo + 8, tag);
    vm_write32(pInfo + 12, 0);
    vm_write64(pInfo + 16, pts);
}

/* The newest frame that is due, in the next of the title's frame buffers;
 * EMPTY while none is (titles poll past that). */
s32 cellSailGraphicsAdapterGetFrame(u32 pSelf, u32 pInfo)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    const int slot = sail_adapter_player(pSelf);
    SailAdapter* a = sail_adapter(pSelf, 0, 1);
    SailMovieInfo mi;
    u32 session = 0;
    if (slot < 0 || !a || !pInfo || !sail_movie_info_of(slot, &mi, &session))
        return (s32)CELL_SAIL_ERROR_EMPTY;
    int no = 0, eos = 0;
    static unsigned calls, handed, handed0;
    static ULONGLONG t0;
    if (++calls == 1 || calls % 300 == 0) {
        const ULONGLONG now = GetTickCount64();
        const double dt = t0 ? (double)(now - t0) / 1000.0 : 0.0;
        printf("[cellSail] video GetFrame #%u: %u frames handed out, playing=%d, "
               "last 300 calls %.1f/s, new frames %.1f/s\n",
               calls, handed, s_sail_players[slot].playing, dt > 0 ? 300.0 / dt : 0.0,
               dt > 0 ? (handed - handed0) / dt : 0.0);
        t0 = now; handed0 = handed;
    }
    if (mi.width > 0) {
        const u32 pitch = (u32)mi.width * 4u;
        sail_alloc_frames(slot, a, pitch * (u32)mi.height);   /* a guest call: unlocked */
        if (a->nframe > 0) {
            const u32 buf = a->frame[a->next];
            SailMovie* m = sail_movie_get(slot);
            const int got = m && s_sail_players[slot].session == session &&
                            sail_movie_video(m, vm_ptr8(buf), (int)pitch, &no, &eos);
            sail_movie_put();
            if (!m || s_sail_players[slot].session != session) return (s32)CELL_SAIL_ERROR_EMPTY;
            if (got) {
                a->next = (a->next + 1) % a->nframe;
                handed++;
                a->last_buf = buf;
                a->last_pts = (u64)no * 90000u * (u64)mi.fps_den / (u64)mi.fps_num;
                sail_frame_info(pInfo, buf, s_sail_players[slot].session, ++a->tag, a->last_pts);
                return CELL_OK;
            }
            /* No new frame due: hand the current one out again. Demon's Souls
             * draws the movie quad only on a frame GetFrame filled, so EMPTY
             * between two movie frames flashed the screen black whenever the
             * game ran faster than the movie. */
            if (!eos && a->last_buf && a->frame_session == s_sail_players[slot].session) {
                sail_frame_info(pInfo, a->last_buf, s_sail_players[slot].session, a->tag, a->last_pts);
                return CELL_OK;
            }
        }
    } else {
        SailMovie* m = sail_movie_get(slot);
        if (m) sail_movie_video(m, NULL, 0, &no, &eos);   /* audio-only: still reports the end */
        sail_movie_put();
    }
    sail_check_end(slot, eos);
    return (s32)CELL_SAIL_ERROR_EMPTY;
}

/* cellSailGraphicsAdapterGetFormat(pSelf, pFormat): the format of the frames
 * the adapter hands out. Titles read it when FrameInfo.sessionId changes. */
s32 cellSailGraphicsAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    if (!pSelf || !pFormat) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    const int slot = sail_adapter_player(pSelf);
    SailAdapter* a = sail_adapter(pSelf, 0, 1);
    SailMovieInfo mi = {0};
    if (slot >= 0) sail_movie_info_of(slot, &mi, NULL);
    u8 f[32];
    memset(f, 0, sizeof f);
    if (a && a->have_pref) memcpy(f, a->pref, sizeof f);
    f[0] = (a && a->have_pref && a->pref[0] == 1) ? 1 : 0;          /* RGBA / ARGB interleaved */
    f[1] = 0;                                                      /* progressive */
    f[2] = 8;                                                      /* bits per colour */
    f[3] = mi.fps_num == 30000 ? 3 : mi.fps_num == 24000 ? 0 : mi.fps_num == 25 ? 2
         : mi.fps_num == 30 ? 4 : mi.fps_num == 24 ? 1 : (u8)0xFF;
    f[4] = (u8)(mi.width >> 8);  f[5] = (u8)mi.width;
    f[6] = (u8)(mi.height >> 8); f[7] = (u8)mi.height;
    const u32 pitch = (u32)mi.width * 4u;
    f[8] = (u8)(pitch >> 24); f[9] = (u8)(pitch >> 16); f[10] = (u8)(pitch >> 8); f[11] = (u8)pitch;
    f[12] = 0; f[13] = 0; f[14] = 0; f[15] = 0xFF;                 /* alpha 255 */
    f[16] = 1;                                                     /* BT.709 */
    f[17] = 1;                                                     /* square pixels */
    f[18] = 0;                                                     /* full range (converted) */
    for (int i = 0; i < 32; i++) vm_write8(pFormat + (u32)i, f[i]);
    return CELL_OK;
}

/* cellSailSoundAdapterGetFrame(pSelf, samples, pInfo): `samples` frames of
 * interleaved big-endian float PCM in a buffer of ours. */
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo)
{
    if (!pSelf) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    const int slot = sail_adapter_player(pSelf);
    SailAdapter* a = sail_adapter(pSelf, 1, 0);
    if (slot < 0 || !a || !pInfo || !samples) return (s32)CELL_SAIL_ERROR_EMPTY;
    const u32 base = sail_scratch(slot);
    SailMovie* m = sail_movie_get(slot);     /* held to the end: no guest calls below */
    SailMovieInfo mi;
    if (m) sail_movie_info(m, &mi);
    if (!m || mi.channels <= 0 || mi.rate <= 0 || !base) {
        sail_movie_put();
        return (s32)CELL_SAIL_ERROR_EMPTY;
    }
    if (samples * (u32)mi.channels * 4u > SAIL_SND_SLOT) samples = SAIL_SND_SLOT / (4u * (u32)mi.channels);
    if (a->samples != (int)samples) {
        printf("[cellSail] sound frames of %u samples x %d ch\n", samples, mi.channels);
        a->samples = (int)samples;
    }
    const u32 buf = base + SAIL_SND_SLOT * (1u + (u32)a->next);   /* slot 0 holds ppFrame */
    int eos = 0;
    static unsigned calls, filled;
    if (++calls % 2000 == 0)
        printf("[cellSail] sound GetFrame #%u: %u filled\n", calls, filled);
    const int got = sail_movie_audio(m, (float*)vm_ptr8(buf), (int)samples, 1, &eos) > 0;
    const u32 session = s_sail_players[slot].session;
    sail_movie_put();
    if (got) {
        filled++;
        a->next = (a->next + 1) % SAIL_SND_SLOTS;
        sail_frame_info(pInfo, buf, session, ++a->tag,
                        (u64)a->tag * samples * 90000u / (u64)mi.rate);
        return CELL_OK;
    }
    if (mi.width <= 0) sail_check_end(slot, eos);   /* video reports the end when there is one */
    return (s32)CELL_SAIL_ERROR_EMPTY;
}

/* cellSailSoundAdapterGetFormat(pSelf, pFormat): CellSailAudioFormat {s8
 * coding (LPCM_FLOAT32 1), chNum; s16 sampleNum; s32 fs, chLayout, reserved0;
 * s64 reserved1}. PAMF LPCM is in Blu-ray order: L R, or L C R Ls Rs LFE. */
s32 cellSailSoundAdapterGetFormat(u32 pSelf, u32 pFormat)
{
    if (!pSelf || !pFormat) return (s32)CELL_SAIL_ERROR_INVALID_ARG;
    const int slot = sail_adapter_player(pSelf);
    SailAdapter* a = sail_adapter(pSelf, 0, 0);
    SailMovieInfo mi = {0};
    if (slot >= 0) sail_movie_info_of(slot, &mi, NULL);
    const int ch = mi.channels;
    const int layout = ch == 1 ? 1 : ch == 2 ? 2 : ch == 6 ? 8 : ch == 8 ? 11 : 0;
    vm_write8(pFormat + 0, 1);
    vm_write8(pFormat + 1, (u8)ch);
    vm_write16(pFormat + 2, (u16)(a ? a->samples : 0));
    vm_write32(pFormat + 4, (u32)mi.rate);
    vm_write32(pFormat + 8, (u32)layout);
    vm_write32(pFormat + 12, 0xFFFFFFFFu);
    vm_write64(pFormat + 16, 0);
    return CELL_OK;
}

s32 cellSailGraphicsAdapterUpdateAvSync(u32 pSelf, u64 pts)
{
    (void)pts;
    return pSelf ? CELL_OK : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u64 pts)
{
    (void)pts;
    return pSelf ? CELL_OK : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailGraphicsAdapterFinalize(u32 pSelf)
{
    SailAdapter* a = sail_adapter(pSelf, 0, 1);
    if (a) memset(a, 0, sizeof *a);
    return pSelf ? CELL_OK : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailSoundAdapterFinalize(u32 pSelf)
{
    SailAdapter* a = sail_adapter(pSelf, 0, 0);
    if (a) memset(a, 0, sizeof *a);
    return pSelf ? CELL_OK : (s32)CELL_SAIL_ERROR_INVALID_ARG;
}

s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     CellSailMemAllocatorFuncs* pFuncs)
{
    /* Both parameters are GUEST addresses, and both structs live in GUEST
     * memory with 32-bit big-endian pointers. Dereferencing them as host
     * structs was wrong twice over: the address was never translated, and the
     * host struct has 64-bit fields, so even the offsets did not line up. The
     * values stored here are guest OPD addresses that SAIL calls back into. */
    u32 alloc_ea = (u32)(uintptr_t)allocator;
    u32 funcs_ea = (u32)(uintptr_t)pFuncs;
    printf("[cellSail] MemAllocatorInitialize(allocator=0x%08X funcs=0x%08X)\n",
           alloc_ea, funcs_ea);
    if (!alloc_ea) return (s32)CELL_SAIL_ERROR_INVALID_ARGUMENT;
    if (funcs_ea) {
        vm_write32(alloc_ea + 0, vm_read32(funcs_ea + 0));   /* pAlloc */
        vm_write32(alloc_ea + 4, vm_read32(funcs_ea + 4));   /* pFree  */
    }
    return CELL_OK;
}
