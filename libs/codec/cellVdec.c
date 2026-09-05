/*
 * ps3recomp - cellVdec HLE implementation
 *
 * Stub video decoder. Accepts AU data and delivers AUDONE callbacks
 * but does not perform actual H.264/MPEG-2 decoding. Games that
 * require video playback will need an FFmpeg/libav integration here.
 */

#include "cellVdec.h"
#include <stdio.h>
#include <string.h>
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_VDEC 4

typedef struct {
    int in_use;
    u32 codecType;
    CellVdecCbMsg cbFunc;
    void* cbArg;
    int seqStarted;
    CellVdecPicItem lastPic;
    int hasPic;
    u32 auCount;        /* total AUs decoded */
    u16 width;          /* configured resolution (0 = use default) */
    u16 height;
} VdecSlot;

static VdecSlot s_vdec[MAX_VDEC];

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  CellVdecCbMsg cbFunc, void* cbArg, CellVdecHandle* handle)
{
    (void)res;

    u32 codec_type = type ? vm_read32(GUEST_EA(type)) : 0;   /* codecType */
    printf("[cellVdec] Open(codecType=%u)\n", codec_type);

    if (!type || !handle)
        return (s32)CELL_VDEC_ERROR_ARG;

    for (int i = 0; i < MAX_VDEC; i++) {
        if (!s_vdec[i].in_use) {
            memset(&s_vdec[i], 0, sizeof(VdecSlot));
            s_vdec[i].in_use = 1;
            s_vdec[i].codecType = codec_type;
            s_vdec[i].cbFunc = cbFunc;
            s_vdec[i].cbArg = cbArg;
            vm_write32((u32)(uintptr_t)handle, (u32)i);
            printf("[cellVdec] Open -> handle=%u\n", i);
            return CELL_OK;
        }
    }
    return (s32)CELL_VDEC_ERROR_BUSY;
}

s32 cellVdecClose(CellVdecHandle handle)
{
    printf("[cellVdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].in_use = 0;
    return CELL_OK;
}

s32 cellVdecStartSeq(CellVdecHandle handle)
{
    printf("[cellVdec] StartSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 1;
    return CELL_OK;
}

s32 cellVdecEndSeq(CellVdecHandle handle)
{
    printf("[cellVdec] EndSeq(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    s_vdec[handle].seqStarted = 0;

    /* Notify sequence done */
    if (s_vdec[handle].cbFunc)
        s_vdec[handle].cbFunc(handle, CELL_VDEC_MSG_TYPE_SEQDONE,
                               CELL_OK, s_vdec[handle].cbArg);

    return CELL_OK;
}

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo)
{
    (void)mode;

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_VDEC_ERROR_ARG;

    VdecSlot* v = &s_vdec[handle];

    /* CellVdecAuInfo mixes u32 and u64, so it comes across field by field: a
     * big-endian u64 is high word first, and a word-at-a-time copy would put
     * the two halves the wrong way round on a little-endian host. */
    u32 au_ea = GUEST_EA(auInfo);
    CellVdecAuInfo au;
    au.startAddr = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, startAddr));
    au.size      = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, size));
    au.pts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, pts));
    au.dts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, dts));
    au.userData  = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, userData));

    printf("[cellVdec] DecodeAu(handle=%u, addr=0x%X, size=%u, pts=%llu)\n",
           handle, au.startAddr, au.size,
           (unsigned long long)au.pts);

    /* Step 1: Report AU consumed */
    if (v->cbFunc)
        v->cbFunc(handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK, v->cbArg);

    /* Step 2: Generate a dummy PICOUT callback.
     * Without FFmpeg, we can't actually decode video. But games expect the
     * PICOUT callback to fire for each AU, so we generate a placeholder
     * picture. Games that only check for completion (not pixel data) will
     * proceed correctly. */
    v->auCount++;
    memset(&v->lastPic, 0, sizeof(v->lastPic));
    v->lastPic.codecType = v->codecType;
    v->lastPic.startAddr = au.startAddr;
    v->lastPic.size      = au.size;
    v->lastPic.auNum     = v->auCount;
    v->lastPic.pts       = au.pts;
    v->lastPic.dts       = au.dts;
    v->lastPic.userData  = au.userData;
    v->lastPic.status    = 0; /* OK */
    v->lastPic.picFmt    = CELL_VDEC_PIC_FMT_YUV420P;
    v->lastPic.width     = v->width ? v->width : 1280;
    v->lastPic.height    = v->height ? v->height : 720;
    v->hasPic = 1;

    if (v->cbFunc)
        v->cbFunc(handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK, v->cbArg);

    return CELL_OK;
}

s32 cellVdecGetPicture(CellVdecHandle handle, const CellVdecPicItem** picItem)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    if (!s_vdec[handle].hasPic)
        return (s32)CELL_VDEC_ERROR_EMPTY;

    if (picItem)
        *picItem = &s_vdec[handle].lastPic;

    s_vdec[handle].hasPic = 0;
    return CELL_OK;
}

s32 cellVdecGetPicItem(CellVdecHandle handle, const CellVdecPicItem** picItem)
{
    return cellVdecGetPicture(handle, picItem);
}

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode)
{
    printf("[cellVdec] SetFrameRate(handle=%u, code=%u)\n",
           handle, frameRateCode);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;

    return CELL_OK;
}
