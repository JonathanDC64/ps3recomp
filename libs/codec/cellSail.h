/*
 * ps3recomp - cellSail HLE
 *
 * High-level media player wrapping PAMF/demux/decode pipeline.
 * Stub — player lifecycle tracked, no actual media playback.
 */

#ifndef PS3RECOMP_CELL_SAIL_H
#define PS3RECOMP_CELL_SAIL_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes, as the SDK numbers them. The names this file used before
 * (NOT_INITIALIZED, INVALID_ARGUMENT, ...) are kept as aliases, but their
 * values had been assigned in order and matched nothing: INVALID_ARGUMENT was
 * 0x80610703, which is the SDK's UNSUPPORTED_STREAM. */
#define CELL_SAIL_ERROR_INVALID_ARG           0x80610701
#define CELL_SAIL_ERROR_INVALID_STATE         0x80610702
#define CELL_SAIL_ERROR_UNSUPPORTED_STREAM    0x80610703
#define CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE    0x80610704
#define CELL_SAIL_ERROR_EMPTY                 0x80610705
#define CELL_SAIL_ERROR_FULLED                0x80610706
#define CELL_SAIL_ERROR_USING                 0x80610707
#define CELL_SAIL_ERROR_NOT_AVAILABLE         0x80610708
#define CELL_SAIL_ERROR_CANCEL                0x80610709
#define CELL_SAIL_ERROR_MEMORY                0x806107F0
#define CELL_SAIL_ERROR_INVALID_FD            0x806107F1
#define CELL_SAIL_ERROR_FATAL                 0x806107FF

#define CELL_SAIL_ERROR_NOT_INITIALIZED       CELL_SAIL_ERROR_INVALID_STATE
#define CELL_SAIL_ERROR_ALREADY_INITIALIZED   CELL_SAIL_ERROR_INVALID_STATE
#define CELL_SAIL_ERROR_INVALID_ARGUMENT      CELL_SAIL_ERROR_INVALID_ARG
#define CELL_SAIL_ERROR_NOT_SUPPORTED         CELL_SAIL_ERROR_UNSUPPORTED_STREAM
#define CELL_SAIL_ERROR_NOT_FOUND             CELL_SAIL_ERROR_INDEX_OUT_OF_RANGE
#define CELL_SAIL_ERROR_OUT_OF_MEMORY         CELL_SAIL_ERROR_MEMORY

/* Constants */
#define CELL_SAIL_PLAYER_MAX    4

/* Player state */
#define CELL_SAIL_PLAYER_STATE_CLOSED     0
#define CELL_SAIL_PLAYER_STATE_INITIALIZED 1
#define CELL_SAIL_PLAYER_STATE_BOOT_TRANSITION 2
#define CELL_SAIL_PLAYER_STATE_RUNNING    3
#define CELL_SAIL_PLAYER_STATE_PAUSE      4
#define CELL_SAIL_PLAYER_STATE_FINISHED   5

/* Stream types */
#define CELL_SAIL_STREAM_VIDEO   0
#define CELL_SAIL_STREAM_AUDIO   1
#define CELL_SAIL_STREAM_USER    2

/* Types */
typedef u32 CellSailPlayerHandle;

typedef struct CellSailPlayerAttribute {
    s32 threadPriority;
    u32 threadStackSize;
    u32 reserved[4];
} CellSailPlayerAttribute;

typedef struct CellSailPlayerResource {
    void* memAddr;
    u32 memSize;
    u32 reserved;
} CellSailPlayerResource;

typedef struct CellSailStreamInfo {
    u32 type;
    u32 codecType;
    u32 width;
    u32 height;
    u32 sampleRate;
    u32 channels;
    u32 reserved[4];
} CellSailStreamInfo;

typedef void (*CellSailPlayerCallback)(CellSailPlayerHandle handle,
                                         s32 event, s32 param,
                                         void* arg);

/* Functions */
s32 cellSailInit(void);
s32 cellSailTerm(void);

s32 cellSailPlayerCreate(const CellSailPlayerAttribute* attr,
                           const CellSailPlayerResource* resource,
                           CellSailPlayerCallback callback,
                           void* callbackArg,
                           CellSailPlayerHandle* handle);
s32 cellSailPlayerDestroy(CellSailPlayerHandle handle);

/* Every asynchronous player call takes a 64-bit userParam last; the player
 * hands it back as arg1 of the CALL_COMPLETED event for that call. */
s32 cellSailPlayerBoot(CellSailPlayerHandle handle, u64 userParam);
s32 cellSailPlayerOpenStream(CellSailPlayerHandle handle, u64 userParam);
s32 cellSailPlayerCloseStream(CellSailPlayerHandle handle, u64 userParam);
s32 cellSailPlayerOpenEsVideo(CellSailPlayerHandle handle, s32 index, u32 pSpecifier,
                              u32 flags, u64 userParam);
s32 cellSailPlayerOpenEsAudio(CellSailPlayerHandle handle, s32 index, u32 pSpecifier,
                              u32 flags, u64 userParam);
s32 cellSailPlayerSetEsAudioMuted(CellSailPlayerHandle handle, s32 index, s32 muted);

s32 cellSailPlayerStart(CellSailPlayerHandle handle, u32 pCommand, u64 userParam);
s32 cellSailPlayerStop(CellSailPlayerHandle handle, u64 userParam);
s32 cellSailPlayerSetPaused(CellSailPlayerHandle handle, s32 paused);
s32 cellSailPlayerFinalize(CellSailPlayerHandle handle);
s32 cellSailPlayerReplaceEventHandler(CellSailPlayerHandle handle, u32 pNewCallback,
                                      u32 pNewArg, u32 ppOldCallback, u32 ppOldArg);
s32 cellSailPlayerSetParameter(CellSailPlayerHandle handle, s32 type, u64 param0, u64 param1);
s32 cellSailPlayerUnsubscribeEvent(CellSailPlayerHandle handle, s32 eventType);

/* CellSailFuture: a one-shot result the player's completion handler fills in. */
s32 cellSailFutureInitialize(u32 pSelf);
s32 cellSailFutureFinalize(u32 pSelf);
s32 cellSailFutureReset(u32 pSelf, s32 wait);
s32 cellSailFutureSet(u32 pSelf, s32 result);
s32 cellSailFutureGet(u32 pSelf, u64 timeout, u32 pResult);
s32 cellSailFutureIsDone(u32 pSelf, u32 pResult);

s32 cellSailGraphicsAdapterGetFrame(u32 pSelf, u32 pInfo);
s32 cellSailGraphicsAdapterUpdateAvSync(u32 pSelf, u64 pts);
s32 cellSailGraphicsAdapterFinalize(u32 pSelf);
s32 cellSailSoundAdapterGetFrame(u32 pSelf, u32 samples, u32 pInfo);
s32 cellSailSoundAdapterUpdateAvSync(u32 pSelf, u64 pts);
s32 cellSailSoundAdapterFinalize(u32 pSelf);
s32 cellSailPlayerPause(CellSailPlayerHandle handle);

s32 cellSailPlayerGetState(CellSailPlayerHandle handle, s32* state);
s32 cellSailPlayerGetStreamNum(CellSailPlayerHandle handle, u32* streamNum);
s32 cellSailPlayerGetStreamInfo(CellSailPlayerHandle handle, u32 streamIndex,
                                  CellSailStreamInfo* info);

s32 cellSailPlayerSetSoundAdapter(CellSailPlayerHandle handle, u32 index, void* adapter);
s32 cellSailPlayerSetGraphicsAdapter(CellSailPlayerHandle handle, u32 index, void* adapter);

s32 cellSailPlayerCancel(CellSailPlayerHandle handle);
s32 cellSailPlayerIsPaused(CellSailPlayerHandle handle);
s32 cellSailPlayerSetRepeatMode(CellSailPlayerHandle handle, s32 repeatMode, void* command);

/* Descriptor management (for multi-stream sources) */
typedef u32 CellSailDescriptorHandle;

s32 cellSailPlayerCreateDescriptor(CellSailPlayerHandle handle,
                                     s32 streamType, void* mediaInfo,
                                     char* uri,
                                     CellSailDescriptorHandle* desc);
s32 cellSailPlayerDestroyDescriptor(CellSailPlayerHandle handle,
                                      CellSailDescriptorHandle desc);
s32 cellSailPlayerAddDescriptor(CellSailPlayerHandle handle,
                                  CellSailDescriptorHandle desc);
s32 cellSailPlayerRemoveDescriptor(CellSailPlayerHandle handle,
                                     CellSailDescriptorHandle desc);

s32 cellSailDescriptorCreateDatabase(CellSailDescriptorHandle desc,
                                       void* dbAddr, u32 dbSize,
                                       u64 arg);
s32 cellSailDescriptorDestroyDatabase(CellSailDescriptorHandle desc, u32 pDatabase);
s32 cellSailDescriptorGetStreamType(CellSailDescriptorHandle desc);
s32 cellSailDescriptorSetAutoSelection(CellSailDescriptorHandle desc, s32 enable);
s32 cellSailDescriptorGetUri(CellSailDescriptorHandle desc, char* uri, u32 maxLen);

/* Memory allocator */
typedef struct CellSailMemAllocator {
    void* (*alloc)(void* arg, u32 boundary, u32 size);
    void  (*free)(void* arg, u32 boundary, void* ptr);
    void* arg;
} CellSailMemAllocator;

/* Mirrors RPCS3's CellSailMemAllocatorFuncs: a guest struct of two callback
 * pointers, passed as one pointer arg (not two separate function pointers). */
typedef struct CellSailMemAllocatorFuncs {
    void* pAlloc;
    void* pFree;
} CellSailMemAllocatorFuncs;

/* Named pFuncs (not "callbacks") so the import-bridge generator's naming
 * heuristic (gen_imports.py param_marshal) treats this as a real guest
 * struct pointer needing host-pointer translation, not a raw callback value. */
s32 cellSailPlayerInitialize2(u32 pSelf, u32 pAllocator, u32 pCallback,
                              u32 pUserParam, u32 pAttribute, u32 pResource);
s32 cellSailSoundAdapterInitialize(u32 pSelf, u32 pCallbacks, u32 pArg);
s32 cellSailMemAllocatorInitialize(CellSailMemAllocator* allocator,
                                     CellSailMemAllocatorFuncs* pFuncs);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_SAIL_H */
