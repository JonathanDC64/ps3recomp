/*
 * ps3recomp - sceNpTrophy HLE implementation
 *
 * The trophy set (count, grades, names, the platinum links) is read from the
 * title's own TROPHY.TRP when RegisterContext runs (np_trophy_set.c). Without
 * one the module keeps its old placeholder set: 32 trophies reported, ids
 * 0-127 accepted, all bronze, generic names, no platinum.
 *
 * Unlocks persist in one small JSON file per communication id:
 *   <dir>/<comm id>.json        (<comm id>_<nn>.json when the number is not 0)
 *   { "trophies": [ { "id": N, "timestamp": T }, ... ] }
 * T is in Unix seconds. <dir> is what sceNpTrophySetStoragePath set, else
 * $PS3_TROPHY_DIR, else gamedata/trophies. The file is replaced through a
 * temporary file and a rename, so a crash mid-write cannot truncate it.
 *
 * Everything the guest reads is written big-endian at the firmware layouts
 * (sceNpTrophy.h), and the error codes and their order follow the firmware as
 * RPCS3 documents it (behaviour only; no code taken from it).
 */

#include "sceNpTrophy.h"
#include "np_trophy_set.h"
#include "../system/cellSysutil.h"
#include "ps3emu/guest_call.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_write32/64 (guest is big-endian) */
#include "../guest_struct.h"                /* GUEST_EA, offsetof */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
static SRWLOCK s_lock = SRWLOCK_INIT;
#define TROPHY_LOCK()   AcquireSRWLockExclusive(&s_lock)
#define TROPHY_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define TROPHY_LOCK()   pthread_mutex_lock(&s_lock)
#define TROPHY_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* runtime/syscalls/sys_fs.c: guest path -> host path, as the game's own reads go */
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);
/* libs/system/cellGame.c: the title id from PARAM.SFO */
const char* cellGame_get_title_id(void);

_Static_assert(sizeof(SceNpTrophyGameDetails) == 1176, "SceNpTrophyGameDetails layout");
_Static_assert(sizeof(SceNpTrophyGameData) == 20, "SceNpTrophyGameData layout");
_Static_assert(sizeof(SceNpTrophyDetails) == 1164, "SceNpTrophyDetails layout");
_Static_assert(offsetof(SceNpTrophyDetails, hidden) == 1160, "SceNpTrophyDetails.hidden");
_Static_assert(sizeof(SceNpTrophyData) == 16, "SceNpTrophyData layout");
_Static_assert(offsetof(SceNpTrophyData, unlocked) == 12, "SceNpTrophyData.unlocked");
_Static_assert(sizeof(SceNpTrophyFlagArray) == 16, "SceNpTrophyFlagArray layout");
/* A port may declare the hook's event itself rather than include this header. */
_Static_assert(sizeof(sceNpTrophyUnlockEvent) == 1192, "sceNpTrophyUnlockEvent layout");
_Static_assert(offsetof(sceNpTrophyUnlockEvent, name) == 40, "sceNpTrophyUnlockEvent.name");

/* English (US): the language cellSysutil reports for SYSTEMPARAM_ID_LANG. It
 * picks TROP_01.SFM over TROP.SFM when a set has one. */
#define TROPHY_SYSTEM_LANG      1
/* CellRtcTick (microseconds since 0001-01-01) of the Unix epoch */
#define TROPHY_TICK_UNIX_EPOCH  62135596800000000ULL
/* Without a trophy set: ids accepted and the count GetTrophyUnlockState reports */
#define TROPHY_PLACEHOLDER_IDS  128
#define TROPHY_FILE_MAX         (1u << 20)
/* Successful calls of the per-trophy queries that get a log line */
#define TROPHY_LOG_QUERIES      64

#define TLOG(...) do { printf("[sceNpTrophy] " __VA_ARGS__); fflush(stdout); } while (0)
#define RCF "0x%08X (%s)"
#define RCA(rc) (unsigned)(u32)(rc), trophy_err_name(rc)

/* ---------------------------------------------------------------------------
 * Internal state (all of it under s_lock)
 * -----------------------------------------------------------------------*/

typedef struct {
    u8  unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64 unlock_time[SCE_NP_TROPHY_MAX_NUM_TROPHIES];   /* Unix seconds */
} TrophyProgress;

typedef struct {
    int             in_use;
    int             registered;
    int             read_only;
    char            id[10];          /* the 9-character communication id */
    u8              num;             /* its number */
    char            name[16];        /* "<id>_<nn>": the TROPDIR folder */
    np_trophy_set*  set;             /* NULL: no TROPHY.TRP, placeholder set */
    int             set_tried;
    TrophyProgress  prog;
    int             file_unreadable; /* the unlock file exists but did not parse */
    int             dirty;           /* an unlock is not on disk yet */
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

static int s_trophy_initialized = 0;
static char s_storage_override[512];

/* ID zero is the firmware invalid sentinel; retain all four usable slots. */
static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

static sceNpTrophyUnlockHook s_hook;
static void*                 s_hook_user;

/* The set the host API found on disk when no registered context has one */
static np_trophy_set* s_host_set;
static char           s_host_name[16];

static unsigned s_logged_info;
static unsigned s_logged_game_info;

/* ---------------------------------------------------------------------------
 * Small helpers
 * -----------------------------------------------------------------------*/

static const char* trophy_err_name(s32 rc)
{
    switch ((u32)rc) {
    case CELL_OK:                                         return "OK";
    case SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED:         return "ALREADY_INITIALIZED";
    case SCE_NP_TROPHY_ERROR_NOT_INITIALIZED:             return "NOT_INITIALIZED";
    case SCE_NP_TROPHY_ERROR_NOT_SUPPORTED:               return "NOT_SUPPORTED";
    case SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED:      return "CONTEXT_NOT_REGISTERED";
    case SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY:               return "OUT_OF_MEMORY";
    case SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT:            return "INVALID_ARGUMENT";
    case SCE_NP_TROPHY_ERROR_EXCEEDS_MAX:                 return "EXCEEDS_MAX";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT:             return "UNKNOWN_CONTEXT";
    case SCE_NP_TROPHY_ERROR_INVALID_CONTEXT:             return "INVALID_CONTEXT";
    case SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE:              return "UNKNOWN_HANDLE";
    case SCE_NP_TROPHY_ERROR_HIDDEN:                      return "HIDDEN";
    case SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM:      return "CANNOT_UNLOCK_PLATINUM";
    case SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED:            return "ALREADY_UNLOCKED";
    case SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID:          return "INVALID_NP_COMM_ID";
    case SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID:           return "INVALID_TROPHY_ID";
    case SCE_NP_TROPHY_ERROR_UNKNOWN:                     return "UNKNOWN";
    default:                                              return "error";
    }
}

static const char* trophy_grade_name(u32 grade)
{
    switch (grade) {
    case SCE_NP_TROPHY_GRADE_PLATINUM: return "platinum";
    case SCE_NP_TROPHY_GRADE_GOLD:     return "gold";
    case SCE_NP_TROPHY_GRADE_SILVER:   return "silver";
    case SCE_NP_TROPHY_GRADE_BRONZE:   return "bronze";
    default:                           return "unknown";
    }
}

static u64 trophy_tick(u64 unix_seconds)
{
    return unix_seconds * 1000000ULL + TROPHY_TICK_UNIX_EPOCH;
}

/* In the set, or in the placeholder range when there is no set */
static int trophy_known(const TrophyContext* ctx, s32 id)
{
    if (id < 0 || id >= SCE_NP_TROPHY_MAX_NUM_TROPHIES)
        return 0;
    return ctx->set ? ctx->set->trophy[id].present : 1;
}

static u32 trophy_grade(const TrophyContext* ctx, s32 id)
{
    return ctx->set ? ctx->set->trophy[id].grade : SCE_NP_TROPHY_GRADE_BRONZE;
}

/* The trophy count GetTrophyUnlockState and GetGameProgress use */
static u32 trophy_total(const TrophyContext* ctx)
{
    return ctx->set ? ctx->set->count : TROPHY_PLACEHOLDER_IDS;
}

static u32 trophy_count_unlocked(const TrophyContext* ctx, u32 per_grade[5])
{
    u32 total = 0;
    if (per_grade)
        memset(per_grade, 0, 5 * sizeof(u32));
    for (s32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES; i++) {
        if (ctx->prog.unlocked[i] && trophy_known(ctx, i)) {
            total++;
            if (per_grade)
                per_grade[trophy_grade(ctx, i) % 5]++;
        }
    }
    return total;
}

/* Firmware order: context id range, context, handle id range, handle */
static s32 trophy_check(SceNpTrophyContext context, SceNpTrophyHandle handle)
{
    if (context < 1 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_contexts[context].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    if (handle < 1 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        return SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_handles[handle].in_use)
        return SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    return CELL_OK;
}

static void trophy_put_str(u32 ea, const char* s, size_t cap)
{
    size_t n = strlen(s);
    if (n > cap - 1)
        n = cap - 1;
    if (n)
        vm_memcpy_to(ea, s, n);
    vm_write8(ea + (u32)n, 0);
}

/* ---------------------------------------------------------------------------
 * The unlock file
 * -----------------------------------------------------------------------*/

static const char* trophy_storage_dir(void)
{
    const char* env;
    if (s_storage_override[0])
        return s_storage_override;
    env = getenv("PS3_TROPHY_DIR");
    return (env && *env) ? env : "gamedata/trophies";
}

static void trophy_ensure_dir(const char* dir)
{
    char tmp[512];
    char* p;

    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Create directories recursively */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_file_path(const char* id, u8 num, char* buf, size_t cap)
{
    char safe[10];
    size_t i;
    for (i = 0; i < 9 && id[i]; i++) {
        char c = id[i];
        safe[i] = (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    }
    safe[i] = '\0';
    if (num)
        snprintf(buf, cap, "%s/%s_%02u.json", trophy_storage_dir(), safe, (unsigned)num);
    else
        snprintf(buf, cap, "%s/%s.json", trophy_storage_dir(), safe);
}

/* The number after "key": inside [b, e), or -1 */
static long long trophy_json_number(const char* b, const char* e, const char* key)
{
    size_t kl = strlen(key);
    for (const char* p = b; (size_t)(e - p) >= kl; p++) {
        if (memcmp(p, key, kl) != 0)
            continue;
        p += kl;
        while (p < e && (isspace((unsigned char)*p) || *p == ':'))
            p++;
        if (p < e && isdigit((unsigned char)*p)) {
            long long v = 0;
            while (p < e && isdigit((unsigned char)*p) && v < 1000000000000000LL)
                v = v * 10 + (*p++ - '0');
            return v;
        }
        return -1;
    }
    return -1;
}

/* 1: read, 0: no file, -1: a file that is not one of ours */
static int trophy_load_file(const char* path, TrophyProgress* prog)
{
    FILE* fp = fopen(path, "rb");
    char* buf;
    size_t n;
    const char* end;
    const char* t;
    const char* p;

    if (!fp)
        return 0;
    buf = (char*)malloc(TROPHY_FILE_MAX + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    n = fread(buf, 1, TROPHY_FILE_MAX, fp);
    fclose(fp);
    buf[n] = '\0';
    end = buf + n;

    t = strstr(buf, "\"trophies\"");
    p = t ? strchr(t, '[') : NULL;
    if (!p) {
        free(buf);
        return -1;
    }
    for (p++;;) {
        const char* close;
        long long id, ts;
        while (p < end && (isspace((unsigned char)*p) || *p == ','))
            p++;
        if (p >= end || *p != '{')
            break;   /* ']' ends the list */
        close = (const char*)memchr(p, '}', (size_t)(end - p));
        if (!close)
            break;
        id = trophy_json_number(p, close, "\"id\"");
        ts = trophy_json_number(p, close, "\"timestamp\"");
        if (id >= 0 && id < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
            prog->unlocked[id] = 1;
            prog->unlock_time[id] = ts > 0 ? (u64)ts : 0;
        }
        p = close + 1;
    }
    free(buf);
    return 1;
}

static int trophy_file_exists(const TrophyContext* ctx)
{
    char path[1024];
    FILE* fp;
    trophy_file_path(ctx->id, ctx->num, path, sizeof path);
    fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fclose(fp);
    return 1;
}

static void trophy_load(TrophyContext* ctx)
{
    char path[1024];
    int rc;
    trophy_file_path(ctx->id, ctx->num, path, sizeof path);
    memset(&ctx->prog, 0, sizeof ctx->prog);
    rc = trophy_load_file(path, &ctx->prog);
    ctx->file_unreadable = (rc < 0);
    if (rc < 0)
        TLOG("WARNING: %s is not a trophy file; it is kept (renamed) before the first save\n", path);
}

static int trophy_save(TrophyContext* ctx)
{
    char path[1024], tmp[1100];
    FILE* fp;
    int ok, first = 1;

    trophy_ensure_dir(trophy_storage_dir());
    trophy_file_path(ctx->id, ctx->num, path, sizeof path);

    if (ctx->file_unreadable) {
        char bad[1100];
        snprintf(bad, sizeof bad, "%s.%llu.bad", path, (unsigned long long)time(NULL));
        if (rename(path, bad) == 0)
            TLOG("kept the unreadable trophy file as %s\n", bad);
        ctx->file_unreadable = 0;
    }

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        TLOG("WARNING: could not save trophies to %s\n", path);
        ctx->dirty = 1;
        return -1;
    }
    ok = fprintf(fp, "{\n  \"trophies\": [") >= 0;
    for (int i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES; i++) {
        if (!ctx->prog.unlocked[i])
            continue;
        ok &= fprintf(fp, "%s\n    { \"id\": %d, \"timestamp\": %llu }", first ? "" : ",",
                      i, (unsigned long long)ctx->prog.unlock_time[i]) >= 0;
        first = 0;
    }
    ok &= fprintf(fp, "\n  ]\n}\n") >= 0;
    ok &= fflush(fp) == 0;
    ok &= fclose(fp) == 0;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(tmp, path) == 0;
#endif
    }
    if (!ok) {
        remove(tmp);
        TLOG("WARNING: could not save trophies to %s\n", path);
        ctx->dirty = 1;
        return -1;
    }
    ctx->dirty = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Finding the trophy set
 * -----------------------------------------------------------------------*/

static void trophy_find_set(TrophyContext* ctx)
{
    char guest[256], host[1024];
    const char* tid = cellGame_get_title_id();
    np_trophy_set* set;

    if (ctx->set || ctx->set_tried)
        return;
    ctx->set_tried = 1;
    set = (np_trophy_set*)calloc(1, sizeof *set);
    if (!set) {
        TLOG("WARNING: out of memory reading the trophy set\n");
        return;
    }
    /* A disc title keeps TROPDIR on the disc; a PSN title in its game folder. */
    for (int i = 0; i < 2; i++) {
        int rc;
        if (i == 0)
            snprintf(guest, sizeof guest, "/dev_bdvd/PS3_GAME/TROPDIR/%s/TROPHY.TRP", ctx->name);
        else if (tid && *tid)
            snprintf(guest, sizeof guest, "/dev_hdd0/game/%s/TROPDIR/%s/TROPHY.TRP", tid, ctx->name);
        else
            break;
        sys_fs_translate_path(guest, host, (int)sizeof host);
        rc = np_trophy_set_load(set, host, TROPHY_SYSTEM_LANG);
        if (rc == 0) {
            ctx->set = set;
            TLOG("trophy set %s: %u trophies (P %u, G %u, S %u, B %u) from %s in %s\n",
                 ctx->name, set->count, set->per_grade[1], set->per_grade[2],
                 set->per_grade[3], set->per_grade[4], set->sfm, guest);
            return;
        }
        if (rc != NP_TROPHY_SET_ERR_OPEN)
            TLOG("WARNING: %s is not a usable trophy set (error %d)\n", guest, rc);
    }
    free(set);
    TLOG("no TROPHY.TRP for %s; using the placeholder set (32 trophies reported, "
         "ids 0-127 accepted, no names)\n", ctx->name);
}

/* For the host API, under the lock: a registered context's set, else a set
 * found on disk. *owner is that context, or NULL for the one found on disk. */
static const np_trophy_set* trophy_host_set(TrophyContext** owner)
{
    char guest[256], host[1024], folder[16], trp[1100];
    const char* tid;

    *owner = NULL;
    for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        TrophyContext* c = &s_contexts[i];
        if (c->in_use && c->registered && c->set) {
            *owner = c;
            return c->set;
        }
    }
    if (s_host_set)
        return s_host_set;

    tid = cellGame_get_title_id();
    for (int i = 0; i < 2; i++) {
        np_trophy_set* set;
        if (i == 0)
            snprintf(guest, sizeof guest, "/dev_bdvd/PS3_GAME/TROPDIR");
        else if (tid && *tid)
            snprintf(guest, sizeof guest, "/dev_hdd0/game/%s/TROPDIR", tid);
        else
            break;
        sys_fs_translate_path(guest, host, (int)sizeof host);
        if (np_trophy_set_find_folder(host, folder, sizeof folder) != 0)
            continue;
        set = (np_trophy_set*)calloc(1, sizeof *set);
        if (!set)
            return NULL;
        snprintf(trp, sizeof trp, "%s/%s/TROPHY.TRP", host, folder);
        if (np_trophy_set_load(set, trp, TROPHY_SYSTEM_LANG) == 0) {
            s_host_set = set;
            snprintf(s_host_name, sizeof s_host_name, "%s", folder);
            TLOG("found trophy set %s (%u trophies) in %s\n", folder, set->count, guest);
            return set;
        }
        free(set);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Guest struct writers
 * -----------------------------------------------------------------------*/

static void trophy_write_details(const TrophyContext* ctx, s32 id, u32 ea)
{
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE], desc[SCE_NP_TROPHY_DESC_MAX_SIZE];
    u8 hidden = 0;

    vm_memset(ea, 0, sizeof(SceNpTrophyDetails));
    vm_write32(ea + offsetof(SceNpTrophyDetails, trophyId), (u32)id);
    vm_write32(ea + offsetof(SceNpTrophyDetails, trophyGrade), trophy_grade(ctx, id));
    if (ctx->set) {
        trophy_put_str(ea + offsetof(SceNpTrophyDetails, name), ctx->set->trophy[id].name,
                       SCE_NP_TROPHY_NAME_MAX_SIZE);
        trophy_put_str(ea + offsetof(SceNpTrophyDetails, description),
                       ctx->set->trophy[id].detail, SCE_NP_TROPHY_DESC_MAX_SIZE);
        hidden = ctx->set->trophy[id].hidden;
    } else {
        snprintf(name, sizeof name, "Trophy %d", id);
        snprintf(desc, sizeof desc, "Trophy #%d description", id);
        trophy_put_str(ea + offsetof(SceNpTrophyDetails, name), name, sizeof name);
        trophy_put_str(ea + offsetof(SceNpTrophyDetails, description), desc, sizeof desc);
    }
    vm_write8(ea + offsetof(SceNpTrophyDetails, hidden), hidden);
}

static void trophy_write_data(const TrophyContext* ctx, s32 id, u32 ea)
{
    u8 unlocked = ctx->prog.unlocked[id] ? 1 : 0;
    vm_memset(ea, 0, sizeof(SceNpTrophyData));
    vm_write64(ea + offsetof(SceNpTrophyData, timestamp),
               unlocked ? trophy_tick(ctx->prog.unlock_time[id]) : 0);
    vm_write32(ea + offsetof(SceNpTrophyData, trophyId), (u32)id);
    vm_write8(ea + offsetof(SceNpTrophyData, unlocked), unlocked);
}

/* ---------------------------------------------------------------------------
 * Unlocking
 * -----------------------------------------------------------------------*/

static void trophy_fill_event(const TrophyContext* ctx, s32 id, int platinum,
                              sceNpTrophyUnlockEvent* ev)
{
    memset(ev, 0, sizeof *ev);
    ev->trophyId = id;
    ev->grade = trophy_grade(ctx, id);
    ev->platinum = (u8)platinum;
    ev->from_set = ctx->set ? 1 : 0;
    ev->unix_time = ctx->prog.unlock_time[id];
    snprintf(ev->commId, sizeof ev->commId, "%s", ctx->name);
    if (ctx->set) {
        ev->hidden = ctx->set->trophy[id].hidden;
        snprintf(ev->name, sizeof ev->name, "%s", ctx->set->trophy[id].name);
        snprintf(ev->detail, sizeof ev->detail, "%s", ctx->set->trophy[id].detail);
    } else {
        snprintf(ev->name, sizeof ev->name, "Trophy %d", id);
    }
}

/* Under the lock. On success *plat is the platinum this unlock completed (or
 * SCE_NP_TROPHY_INVALID_TROPHY_ID) and ev holds *nev hook events. */
static s32 trophy_unlock_locked(TrophyContext* ctx, s32 id, s32* plat,
                                sceNpTrophyUnlockEvent ev[2], int* nev)
{
    const np_trophy_set* set = ctx->set;
    u64 now;

    *nev = 0;
    *plat = SCE_NP_TROPHY_INVALID_TROPHY_ID;
    if (!trophy_known(ctx, id))
        return SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
    if (set && set->trophy[id].grade == SCE_NP_TROPHY_GRADE_PLATINUM)
        return SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM;
    if (ctx->prog.unlocked[id])
        return SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    now = (u64)time(NULL);
    ctx->prog.unlocked[id] = 1;
    ctx->prog.unlock_time[id] = now;

    /* The platinum falls with the last trophy linked to it (pid). */
    if (set) {
        s32 p = set->trophy[id].pid;
        if (p >= 0 && p < SCE_NP_TROPHY_MAX_NUM_TROPHIES && set->trophy[p].present &&
            set->trophy[p].grade == SCE_NP_TROPHY_GRADE_PLATINUM && !ctx->prog.unlocked[p]) {
            int all = 1;
            for (s32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES && all; i++)
                if (i != p && set->trophy[i].present && set->trophy[i].pid == p &&
                    !ctx->prog.unlocked[i])
                    all = 0;
            if (all) {
                ctx->prog.unlocked[p] = 1;
                ctx->prog.unlock_time[p] = now;
                *plat = p;
            }
        }
    }

    trophy_save(ctx);
    trophy_fill_event(ctx, id, 0, &ev[(*nev)++]);
    if (*plat >= 0)
        trophy_fill_event(ctx, *plat, 1, &ev[(*nev)++]);
    return CELL_OK;
}

static void trophy_fire(sceNpTrophyUnlockHook hook, void* user,
                        const sceNpTrophyUnlockEvent* ev, int n)
{
    for (int i = 0; hook && i < n; i++)
        hook(&ev[i], user);
}

/* who is the call up to its last argument, e.g. "UnlockTrophy(ctx=1, handle=1" */
static void trophy_log_unlock(const char* who, s32 id, s32 rc, s32 plat,
                              const sceNpTrophyUnlockEvent* ev, int nev, u32 unlocked, u32 total)
{
    if (rc != CELL_OK) {
        TLOG("%s, id=%d) -> " RCF "\n", who, id, RCA(rc));
        return;
    }
    TLOG("%s, id=%d) -> " RCF ": UNLOCKED %s, %u of %u now\n", who, id, RCA(rc),
         nev ? trophy_grade_name(ev[0].grade) : "?", unlocked, total);
    if (plat >= 0)
        TLOG("platinum %d UNLOCKED: trophy %d was the last one linked to it\n", plat, id);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

void sceNpTrophySetStoragePath(const char* path)
{
    const char* p = GUEST_PTR(path, const char*);
    if (!p)
        return;
    TROPHY_LOCK();
    strncpy(s_storage_override, p, sizeof(s_storage_override) - 1);
    s_storage_override[sizeof(s_storage_override) - 1] = '\0';
    TROPHY_UNLOCK();
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    s32 rc;

    TROPHY_LOCK();
    if (s_trophy_initialized) {
        rc = SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;
    } else if (options > 0) {
        rc = SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
    } else {
        for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++)
            free(s_contexts[i].set);
        memset(s_contexts, 0, sizeof(s_contexts));
        memset(s_handles, 0, sizeof(s_handles));
        s_trophy_initialized = 1;
        rc = CELL_OK;
    }
    TROPHY_UNLOCK();

    TLOG("Init(pool=0x%08X, size=0x%X, container=0x%X, options=0x%llX) -> " RCF "\n",
         GUEST_EA(poolPtr), poolSize, containerId, (unsigned long long)options, RCA(rc));
    return rc;
}

s32 sceNpTrophyTerm(void)
{
    s32 rc = CELL_OK;

    TROPHY_LOCK();
    if (!s_trophy_initialized) {
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    } else {
        /* Unlocks are saved as they happen; this writes the file for a
         * registered context that has none yet, or a save that failed. */
        for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
            TrophyContext* c = &s_contexts[i];
            if (c->in_use && c->registered && (c->dirty || !trophy_file_exists(c)))
                trophy_save(c);
            free(c->set);
        }
        memset(s_contexts, 0, sizeof(s_contexts));
        memset(s_handles, 0, sizeof(s_handles));
        s_trophy_initialized = 0;
    }
    TROPHY_UNLOCK();

    TLOG("Term() -> " RCF "\n", RCA(rc));
    return rc;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    s32 rc = SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
    s32 slot = 0;
    char id[10] = "";
    u8 num = 0;

    /* The signature is not checked: nothing here talks to a server. */
    (void)commSign;

    TROPHY_LOCK();
    if (!s_trophy_initialized) {
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    } else if (!context || !commId) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    } else if (options > SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) {
        rc = SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
    } else {
        const SceNpCommunicationId* commId_h = GUEST_PTR(commId, const SceNpCommunicationId*);
        memcpy(id, commId_h->data, 9);
        id[9] = '\0';
        num = commId_h->num;
        if (num > 99) {
            rc = SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
        } else {
            for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
                TrophyContext* c = &s_contexts[i];
                if (c->in_use)
                    continue;
                memset(c, 0, sizeof(*c));
                c->in_use = 1;
                c->read_only = (options & SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY) ? 1 : 0;
                memcpy(c->id, id, sizeof c->id);
                c->num = num;
                snprintf(c->name, sizeof c->name, "%s_%02u", id, (unsigned)num);
                vm_write32(GUEST_EA(context), (u32)i);
                slot = i;
                rc = CELL_OK;
                break;
            }
        }
    }
    TROPHY_UNLOCK();

    if (rc == CELL_OK)
        TLOG("CreateContext(commId=\"%s\", num=%u) -> ctx=%d\n", id, (unsigned)num, slot);
    else
        TLOG("CreateContext(commId=\"%s\") -> " RCF "\n", id, RCA(rc));
    return rc;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    s32 rc = CELL_OK;

    TROPHY_LOCK();
    if (!s_trophy_initialized) {
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    } else if (context < 1 || context > SCE_NP_TROPHY_MAX_CONTEXTS) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    } else if (!s_contexts[context].in_use) {
        rc = SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    } else {
        TrophyContext* c = &s_contexts[context];
        if (c->registered && (c->dirty || !trophy_file_exists(c)))
            trophy_save(c);
        free(c->set);
        memset(c, 0, sizeof(*c));
    }
    TROPHY_UNLOCK();

    TLOG("DestroyContext(ctx=%d) -> " RCF "\n", context, RCA(rc));
    return rc;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    s32 rc = SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
    s32 slot = 0;

    TROPHY_LOCK();
    if (!s_trophy_initialized) {
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    } else if (!handle) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    } else {
        for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
            if (!s_handles[i].in_use) {
                s_handles[i].in_use = 1;
                vm_write32(GUEST_EA(handle), (u32)i);
                slot = i;
                rc = CELL_OK;
                break;
            }
        }
    }
    TROPHY_UNLOCK();

    if (rc == CELL_OK)
        TLOG("CreateHandle() -> handle=%d\n", slot);
    else
        TLOG("CreateHandle() -> " RCF "\n", RCA(rc));
    return rc;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    s32 rc = CELL_OK;

    TROPHY_LOCK();
    if (!s_trophy_initialized)
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    else if (handle < 1 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    else if (!s_handles[handle].in_use)
        rc = SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    else
        s_handles[handle].in_use = 0;
    TROPHY_UNLOCK();

    TLOG("DestroyHandle(handle=%d) -> " RCF "\n", handle, RCA(rc));
    return rc;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    s32 rc;
    char path[1024] = "";
    u32 unlocked = 0, total = 0;
    int placeholder = 0;

    TROPHY_LOCK();
    rc = s_trophy_initialized ? trophy_check(context, handle) : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (rc == CELL_OK && !statusCb)
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (rc == CELL_OK && options > SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT)
        rc = SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
    if (rc == CELL_OK && !g_ps3_guest_caller)
        rc = SCE_NP_TROPHY_ERROR_UNKNOWN;
    if (rc == CELL_OK) {
        TrophyContext* c = &s_contexts[context];
        /* A second registration runs the status callbacks again on the state
         * already loaded. */
        if (!c->registered) {
            trophy_find_set(c);
            trophy_load(c);
            c->registered = 1;
        }
        trophy_file_path(c->id, c->num, path, sizeof path);
        unlocked = trophy_count_unlocked(c, NULL);
        total = c->set ? c->set->count : 32;
        placeholder = !c->set;
    }
    TROPHY_UNLOCK();

    if (rc != CELL_OK) {
        TLOG("RegisterContext failed: ctx=%d, handle=%d, statusCb=0x%08X -> " RCF "\n",
             context, handle, statusCb, RCA(rc));
        return rc;
    }

    TLOG("RegisterContext(ctx=%d, handle=%d, statusCb=0x%08X, arg=0x%08X) -> loaded saved "
         "data: %u of %u unlocked%s, file %s\n", context, handle, statusCb, arg, unlocked,
         total, placeholder ? " (placeholder set)" : "", path);

    /* Use the runner's OPD bridge, not the optional code/TOC scaffold entry
     * point. Registration status is synchronous; its terminal notification is
     * delivered through sysutil polling, after the HLE storage load completes.
     * Intermediate firmware installation progress is not simulated here. The
     * callback runs outside the lock: it may call back into this module. */
    g_ps3_guest_caller(statusCb, (u32)context, SCE_NP_TROPHY_STATUS_INSTALLED,
                       0, 0, arg, 0, 0, 0);

    TROPHY_LOCK();
    if (!s_trophy_initialized)
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    else if (!s_contexts[context].in_use)
        rc = SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    else if (!s_handles[handle].in_use)
        rc = SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    TROPHY_UNLOCK();
    if (rc != CELL_OK) {
        TLOG("RegisterContext: ctx=%d gone during the status callback -> " RCF "\n",
             context, RCA(rc));
        return rc;
    }

    {
        const u64 completion[8] = {(u32)context, SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE,
                                   0, 0, arg, 0, 0, 0};
        if (cellSysutilQueueGuestCallbackArgs(statusCb, completion) != CELL_OK) {
            TROPHY_LOCK();
            s_contexts[context].registered = 0;
            TROPHY_UNLOCK();
            rc = SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
            TLOG("RegisterContext: completion not queued -> " RCF "\n", RCA(rc));
            return rc;
        }
    }
    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    s32 rc;

    if (!reqSpace) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    } else if (options > 0) {
        rc = SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
    } else {
        TROPHY_LOCK();
        rc = s_trophy_initialized ? trophy_check(context, handle)
                                  : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
        TROPHY_UNLOCK();
    }

    /* Typical trophy pack size, as a big-endian u64. A host-endian 0x100000
     * reads back as 2^44, and LBP feeds this value straight into its save-data
     * free-space accounting: the budget underflows and boot bails. */
    if (rc == CELL_OK)
        vm_write64(GUEST_EA(reqSpace), (u64)(1024 * 1024));
    TLOG("GetRequiredDiskSpace(ctx=%d, handle=%d) -> " RCF "%s\n", context, handle, RCA(rc),
         rc == CELL_OK ? ", 1 MB" : "");
    return rc;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    s32 rc;
    u32 unlocked = 0, total = 0;
    int log_it = 1;

    TROPHY_LOCK();
    rc = s_trophy_initialized ? trophy_check(context, handle) : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (rc == CELL_OK && !s_contexts[context].registered)
        rc = SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    if (rc == CELL_OK && !details && !data)
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (rc == CELL_OK) {
        const TrophyContext* c = &s_contexts[context];
        const np_trophy_set* set = c->set;
        u32 per_grade[5];

        unlocked = trophy_count_unlocked(c, per_grade);
        if (details) {
            u32 ea = GUEST_EA(details);
            vm_memset(ea, 0, sizeof(SceNpTrophyGameDetails));
            if (set) {
                total = set->count;
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numTrophies), set->count);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numPlatinum), set->per_grade[1]);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numGold), set->per_grade[2]);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numSilver), set->per_grade[3]);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numBronze), set->per_grade[4]);
                trophy_put_str(ea + offsetof(SceNpTrophyGameDetails, title), set->title,
                               SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE);
                trophy_put_str(ea + offsetof(SceNpTrophyGameDetails, description),
                               set->title_detail, SCE_NP_TROPHY_GAME_DESC_MAX_SIZE);
            } else {
                /* The placeholder set's numbers, as before the TRP reader */
                total = 32;
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numTrophies), 32);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numPlatinum), 1);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numGold), 2);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numSilver), 8);
                vm_write32(ea + offsetof(SceNpTrophyGameDetails, numBronze), 21);
                trophy_put_str(ea + offsetof(SceNpTrophyGameDetails, title), "PS3 Game",
                               SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE);
                trophy_put_str(ea + offsetof(SceNpTrophyGameDetails, description), "Trophy set",
                               SCE_NP_TROPHY_GAME_DESC_MAX_SIZE);
            }
        }
        if (data) {
            u32 ea = GUEST_EA(data);
            vm_memset(ea, 0, sizeof(SceNpTrophyGameData));
            vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedTrophies), unlocked);
            vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedPlatinum), per_grade[1]);
            vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedGold), per_grade[2]);
            vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedSilver), per_grade[3]);
            vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedBronze), per_grade[4]);
        }
        log_it = s_logged_game_info++ < TROPHY_LOG_QUERIES;
    }
    TROPHY_UNLOCK();

    if (log_it)
        TLOG("GetGameInfo(ctx=%d, handle=%d) -> " RCF ": %u unlocked, %u in the set\n",
             context, handle, RCA(rc), unlocked, total);
    return rc;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    s32 rc;
    int log_it = 1;
    u32 grade = 0;
    u8 unlocked = 0;

    if (trophyId < 0 || trophyId >= SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
        TLOG("GetTrophyInfo(ctx=%d, id=%d) -> " RCF "\n", context, trophyId, RCA(rc));
        return rc;
    }

    TROPHY_LOCK();
    rc = s_trophy_initialized ? trophy_check(context, handle) : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (rc == CELL_OK && !details && !data)
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (rc == CELL_OK && !s_contexts[context].registered)
        rc = SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    if (rc == CELL_OK) {
        const TrophyContext* c = &s_contexts[context];
        if (!trophy_known(c, trophyId)) {
            rc = SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
        } else if (c->set && c->set->trophy[trophyId].hidden && !c->prog.unlocked[trophyId]) {
            rc = SCE_NP_TROPHY_ERROR_HIDDEN;
        } else {
            if (details)
                trophy_write_details(c, trophyId, GUEST_EA(details));
            if (data)
                trophy_write_data(c, trophyId, GUEST_EA(data));
            grade = trophy_grade(c, trophyId);
            unlocked = c->prog.unlocked[trophyId];
            log_it = s_logged_info++ < TROPHY_LOG_QUERIES;
        }
    }
    TROPHY_UNLOCK();

    if (log_it && rc == CELL_OK)
        TLOG("GetTrophyInfo(ctx=%d, id=%d) -> " RCF ": %s, %s\n", context, trophyId, RCA(rc),
             trophy_grade_name(grade), unlocked ? "unlocked" : "locked");
    else if (log_it)
        TLOG("GetTrophyInfo(ctx=%d, id=%d) -> " RCF "\n", context, trophyId, RCA(rc));
    return rc;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    sceNpTrophyUnlockEvent ev[2];
    int nev = 0;
    s32 rc, plat = SCE_NP_TROPHY_INVALID_TROPHY_ID;
    sceNpTrophyUnlockHook hook;
    void* user;
    u32 unlocked = 0, total = 0;
    char who[48];

    TROPHY_LOCK();
    rc = s_trophy_initialized ? trophy_check(context, handle) : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (rc == CELL_OK && s_contexts[context].read_only)
        rc = SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (rc == CELL_OK && !s_contexts[context].registered)
        rc = SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    if (rc == CELL_OK) {
        TrophyContext* c = &s_contexts[context];
        rc = trophy_unlock_locked(c, trophyId, &plat, ev, &nev);
        /* platinumId is only written on success: the platinum this unlock
         * completed, or SCE_NP_TROPHY_INVALID_TROPHY_ID. */
        if (rc == CELL_OK && platinumId)
            vm_write32(GUEST_EA(platinumId), (u32)plat);
        unlocked = trophy_count_unlocked(c, NULL);
        total = c->set ? c->set->count : 32;
    }
    hook = s_hook;
    user = s_hook_user;
    TROPHY_UNLOCK();

    snprintf(who, sizeof who, "UnlockTrophy(ctx=%d, handle=%d", context, handle);
    trophy_log_unlock(who, trophyId, rc, plat, ev, nev, unlocked, total);
    trophy_fire(hook, user, ev, nev);
    return rc;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    s32 rc;
    u32 total = 0, unlocked = 0;

    if (!flags || !count) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
        TLOG("GetTrophyUnlockState(ctx=%d) -> " RCF "\n", context, RCA(rc));
        return rc;
    }

    TROPHY_LOCK();
    rc = s_trophy_initialized ? trophy_check(context, handle) : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (rc == CELL_OK && !s_contexts[context].registered)
        rc = SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    if (rc == CELL_OK) {
        const TrophyContext* c = &s_contexts[context];
        u32 flags_ea = GUEST_EA(flags);
        u32 words[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32] = {0};
        for (s32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES; i++) {
            if (c->prog.unlocked[i] && trophy_known(c, i)) {
                words[i / 32] |= 1u << (i % 32);
                unlocked++;
            }
        }
        for (u32 w = 0; w < SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32; w++)
            vm_write32(flags_ea + w * 4, words[w]);
        total = trophy_total(c);
        vm_write32(GUEST_EA(count), total);
    }
    TROPHY_UNLOCK();

    TLOG("GetTrophyUnlockState(ctx=%d, handle=%d) -> " RCF ": %u of %u\n", context, handle,
         RCA(rc), unlocked, total);
    return rc;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    s32 rc, pct = 0;

    if (!percentage) {
        rc = SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
        TLOG("GetGameProgress(ctx=%d) -> " RCF "\n", context, RCA(rc));
        return rc;
    }

    TROPHY_LOCK();
    rc = s_trophy_initialized ? trophy_check(context, handle) : SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (rc == CELL_OK && !s_contexts[context].registered)
        rc = SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    if (rc == CELL_OK) {
        const TrophyContext* c = &s_contexts[context];
        u32 total = trophy_total(c);
        u32 unlocked = trophy_count_unlocked(c, NULL);
        /* rounded to the nearest percent */
        pct = total ? (s32)((unlocked * 100 + total / 2) / total) : 0;
        vm_write32(GUEST_EA(percentage), (u32)pct);
    }
    TROPHY_UNLOCK();

    TLOG("GetGameProgress(ctx=%d, handle=%d) -> " RCF ": %d%%\n", context, handle, RCA(rc), pct);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Host API
 * -----------------------------------------------------------------------*/

void sceNpTrophy_set_unlock_hook(sceNpTrophyUnlockHook hook, void* user)
{
    TROPHY_LOCK();
    s_hook = hook;
    s_hook_user = user;
    TROPHY_UNLOCK();
}

int sceNpTrophy_snapshot(sceNpTrophySnapshot* out)
{
    TrophyContext* owner;
    const np_trophy_set* set;
    TrophyProgress* prog;
    char path[1024];
    u32 n = 0;

    if (!out)
        return -1;
    prog = (TrophyProgress*)calloc(1, sizeof *prog);
    if (!prog)
        return -1;

    TROPHY_LOCK();
    set = trophy_host_set(&owner);
    if (!set) {
        TROPHY_UNLOCK();
        free(prog);
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (owner) {
        *prog = owner->prog;
        snprintf(out->commId, sizeof out->commId, "%s", owner->name);
    } else {
        char id[10];
        memcpy(id, s_host_name, 9);
        id[9] = '\0';
        trophy_file_path(id, (u8)atoi(s_host_name + 10), path, sizeof path);
        trophy_load_file(path, prog);
        snprintf(out->commId, sizeof out->commId, "%s", s_host_name);
    }
    snprintf(out->title, sizeof out->title, "%s", set->title);
    snprintf(out->detail, sizeof out->detail, "%s", set->title_detail);
    for (s32 i = 0; i < SCE_NP_TROPHY_MAX_NUM_TROPHIES; i++) {
        const np_trophy_info* t = &set->trophy[i];
        sceNpTrophySnapshotEntry* e;
        if (!t->present)
            continue;
        e = &out->trophies[n++];
        e->trophyId = i;
        e->grade = t->grade;
        e->hidden = t->hidden;
        e->unlocked = prog->unlocked[i] ? 1 : 0;
        e->unix_time = e->unlocked ? prog->unlock_time[i] : 0;
        snprintf(e->name, sizeof e->name, "%s", t->name);
        snprintf(e->detail, sizeof e->detail, "%s", t->detail);
        out->per_grade[t->grade % 5]++;
        if (e->unlocked) {
            out->unlocked++;
            out->unlocked_per_grade[t->grade % 5]++;
        }
    }
    out->count = n;
    out->percent = n ? (s32)((out->unlocked * 100 + n / 2) / n) : 0;
    TROPHY_UNLOCK();

    free(prog);
    return 0;
}

int sceNpTrophy_icon(SceNpTrophyId trophyId, void** png, u32* size)
{
    TrophyContext* owner;
    const np_trophy_set* set;
    char path[1024] = "";
    u64 off = 0, len = 0;
    void* buf;
    size_t n;

    if (!png || !size)
        return -1;
    *png = NULL;
    *size = 0;

    TROPHY_LOCK();
    set = trophy_host_set(&owner);
    if (set) {
        if (trophyId == -1) {
            off = set->icon0_offset;
            len = set->icon0_size;
        } else if (trophyId >= 0 && trophyId < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
            off = set->trophy[trophyId].icon_offset;
            len = set->trophy[trophyId].icon_size;
        }
        snprintf(path, sizeof path, "%s", set->trp_path);
    }
    TROPHY_UNLOCK();

    /* Read outside the lock: the set may go away, its path and offsets cannot. */
    if (!len || np_trophy_set_read_blob(path, off, len, &buf, &n) != 0)
        return -1;
    *png = buf;
    *size = (u32)n;
    return 0;
}

void sceNpTrophy_free(void* p)
{
    free(p);
}

s32 sceNpTrophy_debug_unlock(SceNpTrophyId trophyId)
{
    sceNpTrophyUnlockEvent ev[2];
    int nev = 0, slot = 0;
    s32 rc, plat = SCE_NP_TROPHY_INVALID_TROPHY_ID;
    sceNpTrophyUnlockHook hook;
    void* user;
    u32 unlocked = 0, total = 0;
    char who[48];

    TROPHY_LOCK();
    if (!s_trophy_initialized) {
        rc = SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    } else {
        for (int i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS && !slot; i++)
            if (s_contexts[i].in_use && s_contexts[i].registered && !s_contexts[i].read_only)
                slot = i;
        if (!slot) {
            rc = SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
        } else {
            TrophyContext* c = &s_contexts[slot];
            rc = trophy_unlock_locked(c, trophyId, &plat, ev, &nev);
            unlocked = trophy_count_unlocked(c, NULL);
            total = c->set ? c->set->count : 32;
        }
    }
    hook = s_hook;
    user = s_hook_user;
    TROPHY_UNLOCK();

    /* Quiet while nothing is registered: a test knob retries until the game
     * has registered its context. */
    if (slot) {
        snprintf(who, sizeof who, "debug unlock(ctx=%d", slot);
        trophy_log_unlock(who, trophyId, rc, plat, ev, nev, unlocked, total);
    }
    trophy_fire(hook, user, ev, nev);
    return rc;
}
