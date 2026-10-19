/*
 * ps3recomp - sceNpTrophy HLE
 *
 * Trophy/achievement system: create contexts, unlock trophies, query progress.
 * The trophy set comes from the title's TROPHY.TRP (np_trophy_set.h).
 */

#ifndef PS3RECOMP_SCE_NP_TROPHY_H
#define PS3RECOMP_SCE_NP_TROPHY_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
#include "sceNp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (the firmware's 0x800229xx values)
 * -----------------------------------------------------------------------*/
#define SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED          0x80022901
#define SCE_NP_TROPHY_ERROR_NOT_INITIALIZED              0x80022902
#define SCE_NP_TROPHY_ERROR_NOT_SUPPORTED                0x80022903
#define SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED       0x80022904
#define SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY                0x80022905
#define SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT             0x80022906
#define SCE_NP_TROPHY_ERROR_EXCEEDS_MAX                  0x80022907
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT                 0x80022909
#define SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT              0x8002290A
#define SCE_NP_TROPHY_ERROR_INVALID_FORMAT               0x8002290B
#define SCE_NP_TROPHY_ERROR_BAD_RESPONSE                 0x8002290C
#define SCE_NP_TROPHY_ERROR_INVALID_GRADE                0x8002290D
#define SCE_NP_TROPHY_ERROR_INVALID_CONTEXT              0x8002290E
#define SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED           0x8002290F
#define SCE_NP_TROPHY_ERROR_ABORT                        0x80022910
#define SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE               0x80022911
#define SCE_NP_TROPHY_ERROR_LOCKED                       0x80022912
#define SCE_NP_TROPHY_ERROR_HIDDEN                       0x80022913
#define SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM       0x80022914
#define SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED             0x80022915
#define SCE_NP_TROPHY_ERROR_INVALID_TYPE                 0x80022916
#define SCE_NP_TROPHY_ERROR_INVALID_HANDLE               0x80022917
#define SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID           0x80022918
#define SCE_NP_TROPHY_ERROR_UNKNOWN_NP_COMM_ID           0x80022919
#define SCE_NP_TROPHY_ERROR_DISC_IO                      0x8002291A
#define SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST          0x8002291B
#define SCE_NP_TROPHY_ERROR_UNSUPPORTED_FORMAT           0x8002291C
#define SCE_NP_TROPHY_ERROR_ALREADY_INSTALLED            0x8002291D
#define SCE_NP_TROPHY_ERROR_BROKEN_DATA                  0x8002291E
#define SCE_NP_TROPHY_ERROR_VERIFICATION_FAILURE         0x8002291F
#define SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID            0x80022920
#define SCE_NP_TROPHY_ERROR_UNKNOWN_TROPHY_ID            0x80022921
#define SCE_NP_TROPHY_ERROR_UNKNOWN_TITLE                0x80022922
#define SCE_NP_TROPHY_ERROR_UNKNOWN_FILE                 0x80022923
#define SCE_NP_TROPHY_ERROR_DISC_NOT_MOUNTED             0x80022924
#define SCE_NP_TROPHY_ERROR_SHUTDOWN                     0x80022925
#define SCE_NP_TROPHY_ERROR_TITLE_ICON_NOT_FOUND         0x80022926
#define SCE_NP_TROPHY_ERROR_TROPHY_ICON_NOT_FOUND        0x80022927
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT_DISK_SPACE      0x80022928
#define SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE               0x8002292A
#define SCE_NP_TROPHY_ERROR_SAVEDATA_USER_DOES_NOT_MATCH 0x8002292B
#define SCE_NP_TROPHY_ERROR_TROPHY_ID_DOES_NOT_EXIST     0x8002292C
#define SCE_NP_TROPHY_ERROR_SERVICE_UNAVAILABLE          0x8002292D
#define SCE_NP_TROPHY_ERROR_UNKNOWN                      0x800229FF

/* Older names this header used */
#define SCE_NP_TROPHY_ERROR_PLATINUM_CANNOT_UNLOCK  SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM
#define SCE_NP_TROPHY_ERROR_INSUFFICIENT_SPACE      SCE_NP_TROPHY_ERROR_INSUFFICIENT_DISK_SPACE

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_TROPHY_INVALID_CONTEXT       ((SceNpTrophyContext)0)
#define SCE_NP_TROPHY_INVALID_HANDLE        ((SceNpTrophyHandle)0)
#define SCE_NP_TROPHY_INVALID_TROPHY_ID     ((SceNpTrophyId)-1)

#define SCE_NP_TROPHY_MAX_NUM_TROPHIES      128
#define SCE_NP_TROPHY_NAME_MAX_SIZE         128
#define SCE_NP_TROPHY_DESC_MAX_SIZE         1024
#define SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE   128
#define SCE_NP_TROPHY_GAME_DESC_MAX_SIZE    1024

#define SCE_NP_TROPHY_MAX_CONTEXTS          4
#define SCE_NP_TROPHY_MAX_HANDLES           4

#define SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY        1
#define SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT 1

/* Trophy grades */
#define SCE_NP_TROPHY_GRADE_UNKNOWN         0
#define SCE_NP_TROPHY_GRADE_PLATINUM        1
#define SCE_NP_TROPHY_GRADE_GOLD            2
#define SCE_NP_TROPHY_GRADE_SILVER          3
#define SCE_NP_TROPHY_GRADE_BRONZE          4

/* ---------------------------------------------------------------------------
 * Types
 *
 * The structs are the guest layouts, big-endian in guest memory. The HLE
 * writes them field by field with vm_write32/vm_write64 at these offsets; it
 * never stores through them on the host.
 * -----------------------------------------------------------------------*/
typedef s32 SceNpTrophyContext;
typedef s32 SceNpTrophyHandle;
typedef s32 SceNpTrophyId;

typedef struct SceNpTrophyDetails {     /* 1,164 bytes */
    SceNpTrophyId trophyId;
    u32  trophyGrade;
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];
    char description[SCE_NP_TROPHY_DESC_MAX_SIZE];
    b8   hidden;
    u8   reserved[3];
} SceNpTrophyDetails;

typedef struct SceNpTrophyData {        /* 16 bytes */
    u64  timestamp;     /* CellRtcTick (microseconds since year 1), 0 while locked */
    SceNpTrophyId trophyId;
    b8   unlocked;
    u8   reserved[3];
} SceNpTrophyData;

typedef struct SceNpTrophyGameDetails { /* 1,176 bytes */
    u32  numTrophies;
    u32  numPlatinum;
    u32  numGold;
    u32  numSilver;
    u32  numBronze;
    char title[SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE];
    char description[SCE_NP_TROPHY_GAME_DESC_MAX_SIZE];
    u8   reserved[4];
} SceNpTrophyGameDetails;

typedef struct SceNpTrophyGameData {    /* 20 bytes */
    u32  unlockedTrophies;
    u32  unlockedPlatinum;
    u32  unlockedGold;
    u32  unlockedSilver;
    u32  unlockedBronze;
} SceNpTrophyGameData;

/* Unlock state, one bit per trophy id: bit (id % 32) of flag_bits[id / 32] */
typedef struct SceNpTrophyFlagArray {
    u32 flag_bits[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32];
} SceNpTrophyFlagArray;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options);
s32 sceNpTrophyTerm(void);

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options);
s32 sceNpTrophyDestroyContext(SceNpTrophyContext context);

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle);
s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle);

/* Real ABI: (context, handle, statusCb, arg, options).  statusCb is a guest
 * OPD for SceNpTrophyStatusCallback(context, status, completed, total, arg);
 * the game blocks on registration-complete which is signalled via this cb. */
s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options);

/* SceNpTrophyStatus values fired to the status callback */
#define SCE_NP_TROPHY_STATUS_UNKNOWN             0
#define SCE_NP_TROPHY_STATUS_NOT_INSTALLED       1
#define SCE_NP_TROPHY_STATUS_DATA_CORRUPT        2
#define SCE_NP_TROPHY_STATUS_INSTALLED           3
#define SCE_NP_TROPHY_STATUS_REQUIRES_UPDATE     4
#define SCE_NP_TROPHY_STATUS_PROCESSING_SETUP    5
#define SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS 6
#define SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE 7
#define SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE 8
#define SCE_NP_TROPHY_STATUS_CHANGES_DETECTED    9

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options);

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data);

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data);

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId);

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count);

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage);

/* Sets the folder for the unlock files (default "gamedata/trophies", or
 * $PS3_TROPHY_DIR when that is set). The argument is a guest address. */
void sceNpTrophySetStoragePath(const char* path);

/* ---------------------------------------------------------------------------
 * Host API (for the port's UI; the guest never calls these)
 *
 * All of them take the module's lock, so any thread may call them.
 * -----------------------------------------------------------------------*/

typedef struct sceNpTrophyUnlockEvent {
    SceNpTrophyId trophyId;
    u32  grade;            /* SCE_NP_TROPHY_GRADE_* */
    u8   hidden;
    u8   platinum;         /* 1: the platinum, unlocked because this unlock completed it */
    u8   from_set;         /* 0: no TROPHY.TRP was found; name is a placeholder */
    u8   reserved;
    u64  unix_time;        /* seconds */
    char commId[16];       /* the TROPDIR folder name, "<comm id>_<nn>" */
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];          /* UTF-8 */
    char detail[SCE_NP_TROPHY_DESC_MAX_SIZE];
} sceNpTrophyUnlockEvent;

/* Called once per unlocked trophy, then once more for a platinum that the
 * unlock completed. It runs on the thread that unlocked (a guest thread), after
 * the unlock is saved and outside the module's lock: it must only queue work. */
typedef void (*sceNpTrophyUnlockHook)(const sceNpTrophyUnlockEvent* ev, void* user);
void sceNpTrophy_set_unlock_hook(sceNpTrophyUnlockHook hook, void* user);

typedef struct sceNpTrophySnapshotEntry {
    SceNpTrophyId trophyId;
    u32  grade;
    u8   hidden;
    u8   unlocked;
    u8   reserved[2];
    u64  unix_time;        /* 0 while locked */
    char name[SCE_NP_TROPHY_NAME_MAX_SIZE];
    char detail[SCE_NP_TROPHY_DESC_MAX_SIZE];
} sceNpTrophySnapshotEntry;

typedef struct sceNpTrophySnapshot {
    char commId[16];
    char title[SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE];
    char detail[SCE_NP_TROPHY_GAME_DESC_MAX_SIZE];
    u32  count;                    /* entries used in trophies[], in id order */
    u32  unlocked;
    u32  per_grade[5];             /* indexed by SCE_NP_TROPHY_GRADE_* */
    u32  unlocked_per_grade[5];
    s32  percent;                  /* rounded, as GetGameProgress reports it */
    sceNpTrophySnapshotEntry trophies[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
} sceNpTrophySnapshot;

/* The trophy list with its unlock state. It works before the game registers a
 * context: it then finds the set in /dev_bdvd/PS3_GAME/TROPDIR (or the game
 * folder's TROPDIR) and reads the unlock file. Returns 0, or -1 when no trophy
 * set can be found. */
int sceNpTrophy_snapshot(sceNpTrophySnapshot* out);

/* A trophy's PNG icon (trophyId -1: the set's ICON0.PNG), from the same set as
 * sceNpTrophy_snapshot. Returns 0 with a buffer to release with
 * sceNpTrophy_free, or -1. */
int sceNpTrophy_icon(SceNpTrophyId trophyId, void** png, u32* size);
void sceNpTrophy_free(void* p);

/* Unlocks a trophy on the first registered context exactly as
 * sceNpTrophyUnlockTrophy does (the platinum rule, the unlock file, the hook).
 * For test knobs. Returns CELL_OK or an SCE_NP_TROPHY_ERROR_* code; before the
 * game registers a context it returns NOT_INITIALIZED or
 * CONTEXT_NOT_REGISTERED without logging, so a caller can retry. */
s32 sceNpTrophy_debug_unlock(SceNpTrophyId trophyId);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_TROPHY_H */
