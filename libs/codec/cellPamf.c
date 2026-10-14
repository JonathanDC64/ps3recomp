/*
 * ps3recomp - cellPamf HLE implementation
 *
 * Reads the real on-disc PAMF header: stream counts, per-stream codec
 * parameters, presentation times and entry points, as cellDmux/cellVdec/
 * cellAdec set-up asks for them. Return codes and argument checks follow the
 * real library (including a few of its quirks, noted where they matter).
 */

#include "cellPamf.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_ptr8 / vm_read* / vm_write*: guest EA access */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "../../runtime/platform/win32_compat.h"   /* SRWLOCK */
#endif

/* ---------------------------------------------------------------------------
 * PAMF header layout (big-endian, byte-packed; offsets from the file start)
 *
 * 0x00  4  magic "PAMF"
 * 0x04  4  version, ASCII "0040" or "0041"
 * 0x08  4  header_size  \  both in 2048-byte units; the elementary stream
 * 0x0C  4  data_size    /  data starts right after the header
 * 0x10  4  psmf_marks_offset, 0x14 psmf_marks_size  (0 in practice)
 * 0x18  4  unk_offset,        0x1C unk_size         (0 in practice)
 * 0x20 48  reserved
 * 0x50     sequence info:
 *   0x50 4  size (excluding this field)
 *   0x56 2  start_pts_high   0x58 4 start_pts_low     (90 kHz)
 *   0x5C 2  end_pts_high     0x5E 4 end_pts_low
 *   0x62 4  mux_rate_bound (low 22 bits, 50 bytes/s units)
 *   0x66 4  std_delay_bound
 *   0x6A 4  total_stream_num
 *   0x6F 1  grouping_period_num (always 1)
 * 0x70     grouping period:
 *   0x70 4  size   0x74 2+4 start pts   0x7A 2+4 end pts
 *   0x81 1  group_num (always 1)
 * 0x82     group:
 *   0x82 4  size   0x87 1 stream_num
 * 0x88     stream_num x 48-byte stream headers:
 *   +0  coding type (0x1B AVC, 0x02 M2V, 0xDC ATRAC3+, 0x80 LPCM, 0x81 AC3,
 *       0xDD user data)
 *   +4  PES stream_id (0xE0|ch for video, 0xBD private stream for the rest)
 *   +5  private_stream_id (ATRAC3+ ch, LPCM 0x40|ch, AC3 0x30|ch, user 0x20|ch)
 *   +6  P-STD buffer   +8 ep_offset (from file start)   +12 ep_num
 *   +16 32 bytes of codec-specific info (see cellPamfReaderGetStreamInfo)
 * EP tables: 12 bytes each, sorted by stream, right after the stream headers:
 *   +0 u16: 2 bits indexN-1, 13 bits nThRefPictureOffset/2048 - 1
 *   +2 u16 pts_high   +4 u32 pts_low   +8 u32 rpnOffset (2048-byte units)
 *
 * Only one grouping period with one group is ever produced or supported, so
 * the group's position is fixed and everything is read at fixed offsets.
 * -----------------------------------------------------------------------*/

#define PAMF_UNIT                 0x800u

#define PAMF_OFF_MAGIC            0x00
#define PAMF_OFF_VERSION          0x04
#define PAMF_OFF_HEADER_SIZE      0x08
#define PAMF_OFF_DATA_SIZE        0x0C
#define PAMF_OFF_MARKS_OFFSET     0x10
#define PAMF_OFF_MARKS_SIZE       0x14
#define PAMF_OFF_UNK_OFFSET       0x18
#define PAMF_OFF_UNK_SIZE         0x1C

#define PAMF_OFF_SEQ              0x50
#define PAMF_OFF_SEQ_START_HI     0x56
#define PAMF_OFF_SEQ_START_LO     0x58
#define PAMF_OFF_SEQ_END_HI       0x5C
#define PAMF_OFF_SEQ_END_LO       0x5E
#define PAMF_OFF_SEQ_MUX_RATE     0x62
#define PAMF_OFF_SEQ_TOTAL_NUM    0x6A
#define PAMF_OFF_SEQ_GP_NUM       0x6F

#define PAMF_OFF_GP               0x70
#define PAMF_OFF_GP_START_HI      0x74
#define PAMF_OFF_GP_START_LO      0x76
#define PAMF_OFF_GP_END_LO        0x7C
#define PAMF_OFF_GP_GROUP_NUM     0x81

#define PAMF_OFF_GROUP            0x82
#define PAMF_OFF_GROUP_STREAM_NUM 0x87

#define PAMF_OFF_STREAMS          0x88
#define PAMF_STREAM_SIZE          48
#define PAMF_EP_SIZE              12

/* Sizes inside the nested sections, as the real size checks use them */
#define PAMF_GP_HEAD              0x12   /* grouping period bytes before its group */
#define PAMF_SEQ_HEAD             0x20   /* sequence info bytes before its period */

/* Fixed part plus the largest stream table: what the real library may read
 * without consulting header_size. */
#define PAMF_MAX_FIXED            (PAMF_OFF_STREAMS + 255u * PAMF_STREAM_SIZE)

/* Stream header fields */
#define ST_CODING                 0
#define ST_STREAM_ID              4
#define ST_PRIVATE_ID             5
#define ST_EP_OFFSET              8
#define ST_EP_NUM                 12
#define ST_INFO                   16

#define PTS_MAX                   0xFFFFFFFFull   /* CODEC_TS_INVALID */

/* The guest-layout structs in cellPamf.h must keep the SDK sizes/offsets. */
_Static_assert(sizeof(CellPamfAvcInfo) == 0x20, "CellPamfAvcInfo size");
_Static_assert(sizeof(CellPamfM2vInfo) == 0x18, "CellPamfM2vInfo size");
_Static_assert(sizeof(CellPamfLpcmInfo) == 8 &&
               offsetof(CellPamfLpcmInfo, bitsPerSample) == 6, "CellPamfLpcmInfo layout");
_Static_assert(sizeof(CellPamfAtrac3plusInfo) == 8 && sizeof(CellPamfAc3Info) == 8,
               "audio info size");
_Static_assert(sizeof(CellPamfEp) == 0x18 && sizeof(CellPamfEpUnk) == 0x20, "CellPamfEp size");
_Static_assert(sizeof(CellPamfEpIterator) == 0x10, "CellPamfEpIterator size");

/* ---------------------------------------------------------------------------
 * Byte helpers
 * -----------------------------------------------------------------------*/
static u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static u32 be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void put16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8* p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

/* Pointer arguments arrive as raw guest EAs (ppu_hle.cpp's generic adapter). */
static u32 ea_of(const void* p) { return (u32)(uintptr_t)p; }

static u64 pts_of(const u8* hi, const u8* lo) { return ((u64)be16(hi) << 32) | be32(lo); }

/* ---------------------------------------------------------------------------
 * SDK stream type <-> on-disc stream identification
 * -----------------------------------------------------------------------*/

/* Per SDK type: coding type, stream_id and private_stream_id base. The
 * channel goes into stream_id for video (0xE0) and private_stream_id
 * otherwise. Types 6-9 are the PSMF flavours, coding type 0xFF. */
static const struct { u8 coding, sid, psid; } k_type_ids[10] = {
    { CELL_PAMF_CODEC_TYPE_AVC,        0xE0, 0x00 },
    { CELL_PAMF_CODEC_TYPE_M2V,        0xE0, 0x00 },
    { CELL_PAMF_CODEC_TYPE_ATRAC3PLUS, 0xBD, 0x00 },
    { CELL_PAMF_CODEC_TYPE_LPCM,       0xBD, 0x40 },
    { CELL_PAMF_CODEC_TYPE_AC3,        0xBD, 0x30 },
    { CELL_PAMF_CODEC_TYPE_USER_DATA,  0xBD, 0x20 },
    { CELL_PAMF_CODEC_TYPE_PSMF,       0xE0, 0x00 },
    { CELL_PAMF_CODEC_TYPE_PSMF,       0xBD, 0x00 },
    { CELL_PAMF_CODEC_TYPE_PSMF,       0xBD, 0x10 },
    { CELL_PAMF_CODEC_TYPE_PSMF,       0xBD, 0x20 },
};

static s32 pamf_type_to_stream(u8 type, u8 ch, u8* coding, u8* sid, u8* psid)
{
    /* The real library ORs any channel in unchecked; above 15 the ids collide
     * with other types, so refuse it. */
    if (type >= 10 || ch > 15)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const int video = k_type_ids[type].sid == 0xE0;
    if (coding) *coding = k_type_ids[type].coding;
    if (sid)    *sid    = (u8)(k_type_ids[type].sid | (video ? ch : 0));
    if (psid)   *psid   = (u8)(k_type_ids[type].psid | (video ? 0 : ch));
    return CELL_OK;
}

static s32 pamf_stream_to_type(u8 coding, u8 sid, u8 psid, u8* type, u8* ch)
{
    u8 t, c = (u8)(psid & 0x0F);

    switch (coding) {
    case CELL_PAMF_CODEC_TYPE_AVC:        t = CELL_PAMF_STREAM_TYPE_AVC; c = sid & 0x0F; break;
    case CELL_PAMF_CODEC_TYPE_M2V:        t = CELL_PAMF_STREAM_TYPE_M2V; c = sid & 0x0F; break;
    case CELL_PAMF_CODEC_TYPE_ATRAC3PLUS: t = CELL_PAMF_STREAM_TYPE_ATRAC3PLUS; break;
    case CELL_PAMF_CODEC_TYPE_LPCM:       t = CELL_PAMF_STREAM_TYPE_PAMF_LPCM; break;
    case CELL_PAMF_CODEC_TYPE_AC3:        t = CELL_PAMF_STREAM_TYPE_AC3; break;
    case CELL_PAMF_CODEC_TYPE_USER_DATA:  t = CELL_PAMF_STREAM_TYPE_USER_DATA; break;
    case CELL_PAMF_CODEC_TYPE_PSMF:
        if ((sid & 0xF0) == 0xE0) {
            if (psid != 0)
                return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
            t = CELL_PAMF_STREAM_TYPE_PSMF_AVC;
            c = sid & 0x0F;
        } else if (sid == 0xBD) {
            switch (psid & 0xF0) {
            case 0x00: t = CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS; break;
            /* 0x20 (user data) also reports PSMF_LPCM on the real library */
            case 0x10: case 0x20: t = CELL_PAMF_STREAM_TYPE_PSMF_LPCM; break;
            default: return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
            }
        } else {
            return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
        }
        break;
    default:
        return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
    }

    if (type) *type = t;
    if (ch)   *ch   = c;
    return CELL_OK;
}

/* Does stream type `type` satisfy the requested (possibly pseudo) type? */
static int pamf_same_type(u8 type, u8 req)
{
    switch (req) {
    case CELL_PAMF_STREAM_TYPE_VIDEO:
        return type == CELL_PAMF_STREAM_TYPE_AVC || type == CELL_PAMF_STREAM_TYPE_M2V;
    case CELL_PAMF_STREAM_TYPE_AUDIO:
        return type == CELL_PAMF_STREAM_TYPE_ATRAC3PLUS || type == CELL_PAMF_STREAM_TYPE_AC3 ||
               type == CELL_PAMF_STREAM_TYPE_PAMF_LPCM;
    case CELL_PAMF_STREAM_TYPE_UNK:
        return type == CELL_PAMF_STREAM_TYPE_PAMF_LPCM ||
               type == CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS;
    default:
        return type == req;
    }
}

static s32 stream_type_of(const u8* s, u8* type, u8* ch)
{
    return pamf_stream_to_type(s[ST_CODING], s[ST_STREAM_ID], s[ST_PRIVATE_ID], type, ch);
}

/* ---------------------------------------------------------------------------
 * Header validation
 * -----------------------------------------------------------------------*/

static s32 pamf_check_magic(const u8* h, u16* version)
{
    if (be32(h + PAMF_OFF_MAGIC) != CELL_PAMF_MAGIC)
        return (s32)CELL_PAMF_ERROR_UNKNOWN_TYPE;
    switch (be32(h + PAMF_OFF_VERSION)) {
    case CELL_PAMF_VERSION_0040: if (version) *version = 40; return CELL_OK;
    case CELL_PAMF_VERSION_0041: if (version) *version = 41; return CELL_OK;
    default: return (s32)CELL_PAMF_ERROR_UNSUPPORTED_VERSION;
    }
}

/* cellPamfGetStreamOffsetAndSize: magic, version and the two section sizes.
 * The elementary stream data starts at header_size. */
static s32 pamf_sizes(const u8* h, u64 fileSize, u64* headerSize, u64* dataSize)
{
    s32 rc = pamf_check_magic(h, NULL);
    if (rc != CELL_OK)
        return rc;
    const u64 hs = (u64)be32(h + PAMF_OFF_HEADER_SIZE) * PAMF_UNIT;
    const u64 ds = (u64)be32(h + PAMF_OFF_DATA_SIZE) * PAMF_UNIT;
    if (hs == 0 || (fileSize != 0 && hs + ds != fileSize))
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (headerSize) *headerSize = hs;
    if (dataSize)   *dataSize = ds;
    return CELL_OK;
}

/* Full validation (CELL_PAMF_ATTRIBUTE_VERIFY_ON, cellPamfVerify): the real
 * library's checks in its order, so a title gets the same verdict. Offsets
 * from the header start stand in for its absolute addresses (0 = unset is
 * still unambiguous: nothing it compares can sit at offset 0). `limit` is
 * how much of `h` may be read at all -- the real library trusts header_size,
 * a host buffer can't. */
static s32 pamf_verify(const u8* h, u32 limit, u64 fileSize, u32 attribute, u16* version)
{
    if (limit < PAMF_OFF_SEQ)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    s32 rc = pamf_check_magic(h, version);
    if (rc != CELL_OK)
        return rc;

    const u64 hs = (u64)be32(h + PAMF_OFF_HEADER_SIZE) * PAMF_UNIT;
    const u64 ds = (u64)be32(h + PAMF_OFF_DATA_SIZE) * PAMF_UNIT;
    if (hs == 0 || (fileSize != 0 && hs + ds != fileSize))
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    const u32 marks_off = be32(h + PAMF_OFF_MARKS_OFFSET);
    const u32 marks_size = be32(h + PAMF_OFF_MARKS_SIZE);
    const u32 unk_off = be32(h + PAMF_OFF_UNK_OFFSET);
    const u32 unk_size = be32(h + PAMF_OFF_UNK_SIZE);

    /* Optional sections: offset and size both set or both zero, inside the header */
    if (marks_off == 0 ? marks_size != 0
                       : (marks_size == 0 || hs < (u64)marks_off + marks_size))
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (unk_off == 0 ? unk_size != 0
                     : (unk_size == 0 || hs < (u64)unk_off + unk_size))
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (unk_off < (u64)marks_off + marks_size)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    if (limit < PAMF_OFF_STREAMS)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    /* Sequence info */
    const u32 seq_size = be32(h + PAMF_OFF_SEQ);
    if (PAMF_OFF_SEQ + 4 + (u64)seq_size > hs)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    const u64 start = pts_of(h + PAMF_OFF_SEQ_START_HI, h + PAMF_OFF_SEQ_START_LO);
    const u64 end   = pts_of(h + PAMF_OFF_SEQ_END_HI, h + PAMF_OFF_SEQ_END_LO);
    if (end > PTS_MAX || start >= end)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (h[PAMF_OFF_SEQ_GP_NUM] != 1)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    /* Grouping period. Its end pts is taken with the START high half -- a
     * real-library bug, harmless since the high halves are always 0. */
    const u32 gp_size = be32(h + PAMF_OFF_GP);
    if (PAMF_OFF_GP + 4 + (u64)gp_size > hs)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    const u64 gp_start = pts_of(h + PAMF_OFF_GP_START_HI, h + PAMF_OFF_GP_START_LO);
    const u64 gp_end   = pts_of(h + PAMF_OFF_GP_START_HI, h + PAMF_OFF_GP_END_LO);
    if (gp_end > PTS_MAX || gp_start >= gp_end || gp_start != start)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (h[PAMF_OFF_GP_GROUP_NUM] != 1)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    /* Group */
    const u32 group_size = be32(h + PAMF_OFF_GROUP);
    if (PAMF_OFF_GROUP + 4 + (u64)group_size > hs)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    const u32 stream_num = h[PAMF_OFF_GROUP_STREAM_NUM];
    if (stream_num == 0)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (PAMF_OFF_STREAMS + stream_num * PAMF_STREAM_SIZE > limit)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    /* Streams: known type, one channel per type, EP tables contiguous and in
     * stream order, EP pts valid and rpnOffsets ascending within the data. */
    u16 used[10] = { 0 };
    u32 eps_start = 0, next_eps = 0;

    for (u32 i = 0; i < stream_num; i++) {
        const u8* s = h + PAMF_OFF_STREAMS + i * PAMF_STREAM_SIZE;
        u8 type, ch;
        if (stream_type_of(s, &type, &ch) != CELL_OK)
            return (s32)CELL_PAMF_ERROR_UNKNOWN_STREAM;
        if (used[type] & (1u << ch))
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
        used[type] |= (u16)(1u << ch);

        const u32 ep_off = be32(s + ST_EP_OFFSET);
        const u32 ep_num = be32(s + ST_EP_NUM);
        if (ep_num == 0 ? ep_off != 0
                        : (ep_off == 0 || ep_off + (u64)ep_num * PAMF_EP_SIZE > hs))
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

        if (ep_off == 0 || (attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER))
            continue;
        if (ep_off + (u64)ep_num * PAMF_EP_SIZE > limit)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

        if (eps_start == 0)
            eps_start = next_eps = ep_off;
        else if (next_eps != ep_off)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

        u64 prev_rpn = 0;
        for (u32 e = 0; e < ep_num; e++) {
            const u8* ep = h + ep_off + e * PAMF_EP_SIZE;
            const u64 rpn = (u64)be32(ep + 8) * PAMF_UNIT;
            if (pts_of(ep + 2, ep + 4) > PTS_MAX || rpn > ds || rpn < prev_rpn)
                return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
            prev_rpn = rpn;
        }
        next_eps += ep_num * PAMF_EP_SIZE;
    }

    /* Nested section sizes must fit their parents. Arithmetic deliberately as
     * the real library does it: the left sides wrap at 32 bits, and a
     * too-small parent wraps the right side huge (the check then passes). */
    if ((u32)(group_size + 4) > (u64)gp_size - PAMF_GP_HEAD + 4 ||
        (u32)(gp_size + 4) > (u64)seq_size - PAMF_SEQ_HEAD + 4)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    if (be32(h + PAMF_OFF_SEQ_TOTAL_NUM) != stream_num)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    /* What follows the stream headers: EP tables, else marks, else unk. */
    if (eps_start == 0)
        eps_start = marks_off ? marks_off : unk_off;
    if (eps_start != 0 && PAMF_OFF_GP + 4 + (u64)gp_size != eps_start)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    if (next_eps != 0 && marks_off == 0) {
        if (unk_off != 0 && unk_off != next_eps)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    } else if (next_eps != 0 && marks_off != next_eps) {
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    } else if (marks_off != 0 && !(attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)) {
        if ((u64)marks_off + 8 > limit)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
        const u32 msize = be32(h + marks_off);
        if (msize + 4 != marks_size)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
        /* marks count read at +6 (not +4) -- as the real library does */
        if (2u + be16(h + marks_off + 6) * 0x28u != msize)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Reader state lives HOST-side, keyed by the guest EA.
 *
 * The title's CellPamfReader is 128 opaque bytes to it; we never write them
 * and keep the state here instead, keyed off the object's address. The header
 * itself stays where it is: in guest memory for cellPamfReaderInitialize (as
 * on the real library, which keeps a pointer), or in a host buffer we own for
 * pamf_reader_init_host.
 *
 * There is no reader finalise call, so a guest reader's slot is only reused
 * when the same object is initialised again, or evicted (oldest guest reader
 * first) once all slots are taken -- titles keep readers on the stack, so the
 * same few addresses come round again.
 * -----------------------------------------------------------------------*/
#define PAMF_MAX_READERS 16

typedef struct pamf_state {
    u32       ea;          /* guest EA of the CellPamfReader: the key */
    int       in_use;
    u32       seq;         /* initialisation order, for eviction */
    const u8* hdr;         /* host view of the header */
    u8*       owned;       /* == hdr when the header is ours to free */
    u32       hdr_ea;      /* guest EA of the header (0 when host-owned) */
    u32       limit;       /* bytes of hdr that may be read */
    u32       attribute;
    u16       version;     /* 40/41, set by verification */
    u64       headerSize;  /* set by verification, as on the real library */
    u64       dataSize;
    s32       cur;         /* current stream index, -1 = none */
} pamf_state;

static pamf_state s_readers[PAMF_MAX_READERS];
static u32 s_seq;
static SRWLOCK s_lock = SRWLOCK_INIT;   /* cellSail sets readers up from its own threads */

static void slots_lock(void)   { AcquireSRWLockExclusive(&s_lock); }
static void slots_unlock(void) { ReleaseSRWLockExclusive(&s_lock); }

static void slot_clear(pamf_state* st)
{
    free(st->owned);
    memset(st, 0, sizeof(*st));
}

static pamf_state* slot_find(u32 ea)
{
    for (int i = 0; i < PAMF_MAX_READERS; i++)
        if (s_readers[i].in_use && s_readers[i].ea == ea)
            return &s_readers[i];
    return NULL;
}

/* The slot for `ea`, emptied (its owned header freed) and ready to fill. */
static pamf_state* slot_claim(u32 ea)
{
    slots_lock();
    pamf_state* st = slot_find(ea);
    if (!st) {
        pamf_state* oldest_guest = NULL;
        pamf_state* oldest = NULL;
        for (int i = 0; i < PAMF_MAX_READERS && !st; i++) {
            pamf_state* c = &s_readers[i];
            if (!c->in_use) { st = c; break; }
            if (!c->owned && (!oldest_guest || c->seq < oldest_guest->seq)) oldest_guest = c;
            if (!oldest || c->seq < oldest->seq) oldest = c;
        }
        if (!st) {
            st = oldest_guest ? oldest_guest : oldest;
            printf("[cellPamf] reader slots full: evicting reader 0x%08X\n", st->ea);
        }
    }
    slot_clear(st);
    st->in_use = 1;
    st->ea = ea;
    st->seq = ++s_seq;
    st->cur = -1;
    slots_unlock();
    return st;
}

static void slot_release(pamf_state* st)
{
    slots_lock();
    slot_clear(st);
    slots_unlock();
}

static pamf_state* pamf_get(const CellPamfReader* reader)
{
    const u32 ea = ea_of(reader);
    if (!ea)
        return NULL;
    slots_lock();
    pamf_state* st = slot_find(ea);
    slots_unlock();
    return st;
}

static u32 st_stream_num(const pamf_state* st)
{
    return st->hdr[PAMF_OFF_GP_GROUP_NUM] ? st->hdr[PAMF_OFF_GROUP_STREAM_NUM] : 0;
}

static const u8* st_stream(const pamf_state* st, u32 i)
{
    return st->hdr + PAMF_OFF_STREAMS + i * PAMF_STREAM_SIZE;
}

static const u8* st_cur(const pamf_state* st)
{
    return st->cur >= 0 ? st_stream(st, (u32)st->cur) : NULL;
}

/* The current stream's packed EP table, NULL if it has none or the table is
 * not inside the readable header. */
static const u8* st_eps(const pamf_state* st, u32* ep_off)
{
    const u8* s = st_cur(st);
    if (!s)
        return NULL;
    const u32 off = be32(s + ST_EP_OFFSET), num = be32(s + ST_EP_NUM);
    if (num == 0 || off == 0 || off + (u64)num * PAMF_EP_SIZE > st->limit)
        return NULL;
    if (ep_off) *ep_off = off;
    return st->hdr + off;
}

static u32 st_ep_num(const pamf_state* st)
{
    const u8* s = st_cur(st);
    return s ? be32(s + ST_EP_NUM) : 0;
}

/* How much of a guest-resident header may be read: its header_size, but never
 * less than the fixed part plus a full stream table (the real library reads
 * those without looking at header_size), and never past the 4 GB guest space. */
static u32 guest_span(u32 ea)
{
    u64 span = (u64)vm_read32(ea + PAMF_OFF_HEADER_SIZE) * PAMF_UNIT;
    if (span < PAMF_MAX_FIXED)
        span = PAMF_MAX_FIXED;
    const u64 room = 0x100000000ull - ea;
    if (span > room)
        span = room;
    return span > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)span;
}

/* Common to both initialisers once hdr/limit are set. */
static s32 pamf_open(pamf_state* st, u64 fileSize, u32 attribute)
{
    st->attribute = attribute;
    if (st->limit < PAMF_OFF_STREAMS)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    if (attribute & CELL_PAMF_ATTRIBUTE_VERIFY_ON) {
        s32 rc = pamf_verify(st->hdr, st->limit, fileSize, attribute, &st->version);
        if (rc != CELL_OK)
            return rc;
        st->headerSize = (u64)be32(st->hdr + PAMF_OFF_HEADER_SIZE) * PAMF_UNIT;
        st->dataSize   = (u64)be32(st->hdr + PAMF_OFF_DATA_SIZE) * PAMF_UNIT;
    }

    if (st->hdr[PAMF_OFF_SEQ_GP_NUM] == 0)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    const u32 n = st_stream_num(st);
    if (PAMF_OFF_STREAMS + n * PAMF_STREAM_SIZE > st->limit)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    if (n)
        st->cur = 0;   /* the real library selects stream 0 */
    return CELL_OK;
}

static const char* type_name(u8 type)
{
    static const char* const names[] = { "avc", "m2v", "at3p", "lpcm", "ac3", "user",
                                         "psmf-avc", "psmf-at3p", "psmf-lpcm", "psmf-user" };
    return type < 10 ? names[type] : "?";
}

/* The one line per initialise: streams and time range. */
static void pamf_log_open(const pamf_state* st, s32 rc)
{
    if (rc != CELL_OK) {
        printf("[cellPamf] init reader 0x%08X (%s header): error 0x%08X\n",
               st->ea, st->owned ? "host" : "guest", (u32)rc);
        return;
    }
    char list[160];
    size_t len = 0;
    list[0] = '\0';
    const u32 n = st_stream_num(st);
    for (u32 i = 0; i < n && len + 16 < sizeof(list); i++) {
        u8 type = 0xFF, ch = 0;
        stream_type_of(st_stream(st, i), &type, &ch);
        len += (size_t)snprintf(list + len, sizeof(list) - len, "%s%s/%u",
                                i ? " " : "", type_name(type), ch);
    }
    const u8* h = st->hdr;
    printf("[cellPamf] init reader 0x%08X (%s header): %u streams [%s] pts %llu..%llu, "
           "header 0x%llX\n",
           st->ea, st->owned ? "host" : "guest", n, list,
           (unsigned long long)pts_of(h + PAMF_OFF_SEQ_START_HI, h + PAMF_OFF_SEQ_START_LO),
           (unsigned long long)pts_of(h + PAMF_OFF_SEQ_END_HI, h + PAMF_OFF_SEQ_END_LO),
           (unsigned long long)be32(h + PAMF_OFF_HEADER_SIZE) * PAMF_UNIT);
}

/* ---------------------------------------------------------------------------
 * Host-side entry points
 * -----------------------------------------------------------------------*/

s32 pamf_reader_init_host(u32 reader_ea, void* hdr, u32 size)
{
    if (!reader_ea || !hdr) {
        free(hdr);
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    }
    pamf_state* st = slot_claim(reader_ea);
    st->owned = (u8*)hdr;
    st->hdr = st->owned;
    st->limit = size;

    /* A buffer shorter than the header_size it declares holds no EP tables:
     * treat it as a minimum header, which is what the attribute means. */
    u32 attribute = CELL_PAMF_ATTRIBUTE_VERIFY_ON;
    if (size > PAMF_OFF_HEADER_SIZE + 4 &&
        (u64)be32(st->hdr + PAMF_OFF_HEADER_SIZE) * PAMF_UNIT > size)
        attribute |= CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER;

    const s32 rc = pamf_open(st, 0, attribute);
    pamf_log_open(st, rc);
    if (rc != CELL_OK)
        slot_release(st);
    return rc;
}

void pamf_reader_release(u32 reader_ea)
{
    if (!reader_ea)
        return;
    slots_lock();
    pamf_state* st = slot_find(reader_ea);
    if (st)
        slot_clear(st);
    slots_unlock();
}

/* ---------------------------------------------------------------------------
 * Header queries (no reader)
 * -----------------------------------------------------------------------*/

s32 cellPamfGetStreamOffsetAndSize(const void* pAddr, u64 fileSize, u64* pOffset, u64* pSize)
{
    const u32 ea = ea_of(pAddr);
    if (!ea)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    u64 hs, ds;
    s32 rc = pamf_sizes(vm_ptr8(ea), fileSize, &hs, &ds);
    if (rc != CELL_OK)
        return rc;
    if (pOffset) vm_write64(ea_of(pOffset), hs);
    if (pSize)   vm_write64(ea_of(pSize), ds);
    return CELL_OK;
}

s32 cellPamfGetHeaderSize(const void* pAddr, u64 fileSize, u64* pSize)
{
    return cellPamfGetStreamOffsetAndSize(pAddr, fileSize, pSize, NULL);
}

s32 cellPamfGetHeaderSize2(const void* pAddr, u64 fileSize, u32 attribute, u64* pSize)
{
    const u32 ea = ea_of(pAddr);
    if (!ea)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u8* h = vm_ptr8(ea);
    u64 hs;
    s32 rc = pamf_sizes(h, fileSize, &hs, NULL);
    if (rc != CELL_OK)
        return rc;
    if (attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER) {
        /* Everything up to the end of the sequence info: no EP tables */
        const u64 min = PAMF_OFF_SEQ + 4 + (u64)be32(h + PAMF_OFF_SEQ);
        if (min > hs)
            return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
        hs = min;
    }
    if (pSize)
        vm_write64(ea_of(pSize), hs);
    return CELL_OK;
}

s32 cellPamfVerify(const void* pAddr, u64 fileSize)
{
    const u32 ea = ea_of(pAddr);
    if (!ea)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    return pamf_verify(vm_ptr8(ea), guest_span(ea), fileSize,
                       CELL_PAMF_ATTRIBUTE_VERIFY_ON, NULL);
}

/* ---------------------------------------------------------------------------
 * Reader lifecycle
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderInitialize(CellPamfReader* reader, const void* pamfAddr,
                             u64 fileSize, u32 attribute)
{
    const u32 rea = ea_of(reader), hea = ea_of(pamfAddr);
    if (!rea || !hea)   /* the real library doesn't check either */
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    pamf_state* st = slot_claim(rea);
    st->hdr = vm_ptr8(hea);
    st->hdr_ea = hea;
    st->limit = guest_span(hea);

    const s32 rc = pamf_open(st, fileSize, attribute);
    pamf_log_open(st, rc);
    if (rc != CELL_OK)
        slot_release(st);
    return rc;
}

s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, CellCodecTimeStamp* pTimeStamp)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !pTimeStamp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    vm_write32(ea_of(pTimeStamp) + 0, be16(st->hdr + PAMF_OFF_SEQ_START_HI));
    vm_write32(ea_of(pTimeStamp) + 4, be32(st->hdr + PAMF_OFF_SEQ_START_LO));
    return CELL_OK;
}

s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, CellCodecTimeStamp* pTimeStamp)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !pTimeStamp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    vm_write32(ea_of(pTimeStamp) + 0, be16(st->hdr + PAMF_OFF_SEQ_END_HI));
    vm_write32(ea_of(pTimeStamp) + 4, be32(st->hdr + PAMF_OFF_SEQ_END_LO));
    return CELL_OK;
}

u32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader)
{
    pamf_state* st = pamf_get(reader);
    if (!st)
        return 0;
    return be32(st->hdr + PAMF_OFF_SEQ_MUX_RATE) & 0x003FFFFF;
}

/* ---------------------------------------------------------------------------
 * Stream queries
 * -----------------------------------------------------------------------*/

s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader)
{
    pamf_state* st = pamf_get(reader);
    if (!st)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    return (s32)st_stream_num(st);
}

s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType)
{
    pamf_state* st = pamf_get(reader);
    if (!st)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u32 n = st_stream_num(st);
    s32 found = 0;
    for (u32 i = 0; i < n; i++) {
        u8 type;
        if (stream_type_of(st_stream(st, i), &type, NULL) == CELL_OK)
            found += pamf_same_type(type, streamType);
    }
    return found;
}

s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u8 streamIndex)
{
    pamf_state* st = pamf_get(reader);
    if (!st || streamIndex >= st_stream_num(st))
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    st->cur = streamIndex;
    return CELL_OK;
}

/* Returns the selected stream's index on success. The real library scans
 * with the wrong stride (only stream 0 is ever matched properly); this does
 * the intended scan, which agrees with it wherever it worked. */
s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u8 ch)
{
    pamf_state* st = pamf_get(reader);
    u8 coding, sid, psid;
    if (!st || pamf_type_to_stream(streamType, ch, &coding, &sid, &psid) != CELL_OK)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    const u32 n = st_stream_num(st);
    for (u32 i = 0; i < n; i++) {
        const u8* s = st_stream(st, i);
        if (s[ST_CODING] == coding && s[ST_STREAM_ID] == sid && s[ST_PRIVATE_ID] == psid) {
            st->cur = (s32)i;
            return (s32)i;
        }
    }
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}

/* Selects the streamIndex-th stream matching streamType (pseudo types
 * allowed) and returns its overall index. */
s32 cellPamfReaderSetStreamWithTypeAndIndex(CellPamfReader* reader, u8 streamType, u8 streamIndex)
{
    pamf_state* st = pamf_get(reader);
    if (!st)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u32 n = st_stream_num(st);
    if (streamIndex >= n)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    u32 found = 0;
    for (u32 i = 0; i < n; i++) {
        u8 type;
        if (stream_type_of(st_stream(st, i), &type, NULL) != CELL_OK)
            continue;
        found += (u32)pamf_same_type(type, streamType);
        if (found > streamIndex) {
            st->cur = (s32)i;
            return (s32)i;
        }
    }
    return (s32)CELL_PAMF_ERROR_STREAM_NOT_FOUND;
}

/* Not an SDK export (an earlier name here); same as SetStreamWithTypeAndIndex. */
s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u8 streamIndex)
{
    return cellPamfReaderSetStreamWithTypeAndIndex(reader, streamType, streamIndex);
}

s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader)
{
    pamf_state* st = pamf_get(reader);
    if (!st)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    return st->cur;
}

s32 cellPamfReaderGetStreamTypeAndChannel(CellPamfReader* reader, u8* pType, u8* pCh)
{
    pamf_state* st = pamf_get(reader);
    const u8* s = st ? st_cur(st) : NULL;
    if (!s)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    u8 type, ch;
    s32 rc = stream_type_of(s, &type, &ch);
    if (rc != CELL_OK)
        return rc;
    if (pType) vm_write8(ea_of(pType), type);
    if (pCh)   vm_write8(ea_of(pCh), ch);
    return CELL_OK;
}

static void es_filter_write(u32 ea, u8 sid, u8 psid, int avc)
{
    vm_write32(ea + 0, sid);
    vm_write32(ea + 4, psid);
    vm_write32(ea + 8, avc ? 1u : 0u);
    vm_write32(ea + 12, 0);
}

s32 cellPamfReaderGetEsFilterId(CellPamfReader* reader, CellCodecEsFilterId* pEsFilterId)
{
    pamf_state* st = pamf_get(reader);
    const u8* s = st ? st_cur(st) : NULL;
    if (!s || !pEsFilterId)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    es_filter_write(ea_of(pEsFilterId), s[ST_STREAM_ID], s[ST_PRIVATE_ID],
                    s[ST_CODING] == CELL_PAMF_CODEC_TYPE_AVC);
    return CELL_OK;
}

s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 ch, CellCodecEsFilterId* pEsFilterId)
{
    u8 sid, psid;
    if (!pEsFilterId || pamf_type_to_stream(streamType, ch, NULL, &sid, &psid) != CELL_OK)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    es_filter_write(ea_of(pEsFilterId), sid, psid, streamType == CELL_PAMF_STREAM_TYPE_AVC);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Stream info -- the 32 codec-specific bytes at +16 of the stream header,
 * unpacked into the SDK struct for the stream's type:
 *
 * AVC:  +0 profileIdc  +1 levelIdc  +2 flags (b7 frameMbsOnly, b6 videoSignalInfo,
 *       b0-3 frameRateInfo+1)  +3 aspectRatioIdc  +4/+6 sarWidth/Height (only
 *       meaningful when aspectRatioIdc == 255)  +9/+11 width/height in 16-px
 *       units  +12..+19 frame crop L/R/T/B (u16)  +20 b5-7 videoFormat,
 *       b4 fullRange  +21..+23 colour/transfer/matrix  +24 b7 entropy,
 *       b6 deblocking, b4-5 minNumSlicePerPicture, b0-1 nfwIdc  +25 maxMeanBitrate
 * M2V:  +0 profile/level (0x44 -> MP@HL, 0x48 -> MP@ML)  +2 flags (b7
 *       progressive, b6 videoSignalInfo, b0-3 frameRateInfo)  +3 aspect
 *       +4/+6 sar  +9/+11 size in 16-px units  +12/+14 exact size
 *       +20..+23 as AVC
 * Audio: +2 channels  +3 sampling-frequency code  +4 b6-7 LPCM bit-length code
 * -----------------------------------------------------------------------*/

#define AVC_AT(f) (out + offsetof(CellPamfAvcInfo, f))
#define M2V_AT(f) (out + offsetof(CellPamfM2vInfo, f))

s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize)
{
    pamf_state* st = pamf_get(reader);
    const u8* s = st ? st_cur(st) : NULL;
    if (!s || !info)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    u8 type;
    if (stream_type_of(s, &type, NULL) != CELL_OK)
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;

    const u8* d = s + ST_INFO;
    u8 out[0x20];
    u32 n;
    memset(out, 0, sizeof(out));

    switch (type) {
    case CELL_PAMF_STREAM_TYPE_AVC: {
        n = sizeof(CellPamfAvcInfo);
        if (infoSize < n)
            return (s32)CELL_PAMF_ERROR_INVALID_ARG;
        *AVC_AT(profileIdc)          = d[0];
        *AVC_AT(levelIdc)            = d[1];
        *AVC_AT(frameMbsOnlyFlag)    = (d[2] >> 7) & 1;
        *AVC_AT(videoSignalInfoFlag) = (d[2] >> 6) & 1;
        *AVC_AT(frameRateInfo)       = (u8)((d[2] & 0x0F) - 1);
        *AVC_AT(aspectRatioIdc)      = d[3];
        if (d[3] == 0xFF) {
            put16(AVC_AT(sarWidth), be16(d + 4));
            put16(AVC_AT(sarHeight), be16(d + 6));
        }
        put16(AVC_AT(horizontalSize), d[9] * 16u);
        put16(AVC_AT(verticalSize), d[11] * 16u);
        put16(AVC_AT(frameCropLeftOffset), be16(d + 12));
        put16(AVC_AT(frameCropRightOffset), be16(d + 14));
        put16(AVC_AT(frameCropTopOffset), be16(d + 16));
        put16(AVC_AT(frameCropBottomOffset), be16(d + 18));
        if (d[2] & 0x40) {
            *AVC_AT(videoFormat)             = d[20] >> 5;
            *AVC_AT(videoFullRangeFlag)      = (d[20] >> 4) & 1;
            *AVC_AT(colourPrimaries)         = d[21];
            *AVC_AT(transferCharacteristics) = d[22];
            *AVC_AT(matrixCoefficients)      = d[23];
        }
        *AVC_AT(entropyCodingModeFlag)    = (d[24] >> 7) & 1;
        *AVC_AT(deblockingFilterFlag)     = (d[24] >> 6) & 1;
        *AVC_AT(minNumSlicePerPictureIdc) = (d[24] >> 4) & 3;
        *AVC_AT(nfwIdc)                   = d[24] & 3;
        *AVC_AT(maxMeanBitrate)           = d[25];
        break;
    }

    case CELL_PAMF_STREAM_TYPE_M2V: {
        n = sizeof(CellPamfM2vInfo);
        if (infoSize < n)
            return (s32)CELL_PAMF_ERROR_INVALID_ARG;
        *M2V_AT(profileAndLevelIndication) = d[0] == 0x44 ? CELL_PAMF_M2V_MP_HL
                                           : d[0] == 0x48 ? CELL_PAMF_M2V_MP_ML
                                           : CELL_PAMF_M2V_UNKNOWN;
        *M2V_AT(progressiveSequence) = (d[2] >> 7) & 1;
        *M2V_AT(videoSignalInfoFlag) = (d[2] >> 6) & 1;
        *M2V_AT(frameRateInfo)       = d[2] & 0x0F;
        *M2V_AT(aspectRatioIdc)      = d[3];
        if (d[3] == 0xFF) {
            put16(M2V_AT(sarWidth), be16(d + 4));
            put16(M2V_AT(sarHeight), be16(d + 6));
        }
        put16(M2V_AT(horizontalSize), d[9] * 16u);
        put16(M2V_AT(verticalSize), d[11] * 16u);
        put16(M2V_AT(horizontalSizeValue), be16(d + 12));
        put16(M2V_AT(verticalSizeValue), be16(d + 14));
        if (d[2] & 0x40) {
            *M2V_AT(videoFormat)             = d[20] >> 5;
            *M2V_AT(videoFullRangeFlag)      = (d[20] >> 4) & 1;
            *M2V_AT(colourPrimaries)         = d[21];
            *M2V_AT(transferCharacteristics) = d[22];
            *M2V_AT(matrixCoefficients)      = d[23];
        }
        break;
    }

    case CELL_PAMF_STREAM_TYPE_ATRAC3PLUS:
    case CELL_PAMF_STREAM_TYPE_AC3:
    case CELL_PAMF_STREAM_TYPE_PAMF_LPCM:
        /* All three are 8 bytes: freq code u32 @0, channels u8 @4, and for
         * LPCM the bit-length code u16 @6. */
        n = sizeof(CellPamfLpcmInfo);
        if (infoSize < n)
            return (s32)CELL_PAMF_ERROR_INVALID_ARG;
        put32(out + offsetof(CellPamfLpcmInfo, samplingFrequency), d[3] & 0x0F);
        out[offsetof(CellPamfLpcmInfo, numberOfChannels)] = d[2] & 0x0F;
        if (type == CELL_PAMF_STREAM_TYPE_PAMF_LPCM)
            put16(out + offsetof(CellPamfLpcmInfo, bitsPerSample), d[4] >> 6);
        break;

    /* PSMF streams (never produced by the PAMF tools) read the 48-byte entry
     * with the PSMF stream-header layout, as the real library does. */
    case CELL_PAMF_STREAM_TYPE_PSMF_AVC:
        n = 4;
        if (infoSize < n)
            return (s32)CELL_PAMF_ERROR_INVALID_ARG;
        put16(out + 0, s[12] * 16u);
        put16(out + 2, s[13] * 16u);
        break;

    case CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS:
    case CELL_PAMF_STREAM_TYPE_PSMF_LPCM:
        n = 2;
        if (infoSize < n)
            return (s32)CELL_PAMF_ERROR_INVALID_ARG;
        out[0] = s[14];
        out[1] = s[15] & 0x0F;
        break;

    case CELL_PAMF_STREAM_TYPE_USER_DATA:
    case CELL_PAMF_STREAM_TYPE_PSMF_USER_DATA:
        /* User-data streams have no info struct */
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;

    default:
        return (s32)CELL_PAMF_ERROR_INVALID_PAMF;
    }

    vm_memcpy_to(ea_of(info), out, n);
    return CELL_OK;
}

#undef AVC_AT
#undef M2V_AT

/* ---------------------------------------------------------------------------
 * Entry point navigation
 * -----------------------------------------------------------------------*/

/* Unpack one 12-byte EP into a guest CellPamfEp. */
static void ep_write(u32 ea, const u8* e)
{
    const u16 v0 = be16(e);
    vm_write32(ea + 0, (u32)(v0 >> 14) + 1);                         /* indexN */
    vm_write32(ea + 4, (u32)(v0 & 0x1FFF) * PAMF_UNIT + PAMF_UNIT);  /* nThRefPictureOffset */
    vm_write32(ea + 8, be16(e + 2));                                 /* pts.upper */
    vm_write32(ea + 12, be32(e + 4));                                /* pts.lower */
    vm_write64(ea + 16, (u64)be32(e + 8) * PAMF_UNIT);               /* rpnOffset */
}

/* Index of the last EP at or before the time stamp, 0 if none is. */
static u32 ep_find(const u8* eps, u32 num, u32 ts_ea)
{
    const u64 want = vm_read64(ts_ea);   /* {upper, lower} == one big-endian u64 */
    u32 i = num - 1;
    for (; i >= 1; i--)
        if (pts_of(eps + i * PAMF_EP_SIZE + 2, eps + i * PAMF_EP_SIZE + 4) <= want)
            break;
    return i;
}

/* Iterators carry pCur, a guest EA of the packed EP, as on the real library.
 * A host-owned header has no guest EA: there pCur is the EP's offset in the
 * header and reserved[0] names the reader slot (index + 1). Titles treat the
 * iterator as opaque. */
static void it_write(u32 it, const pamf_state* st, u32 index, u32 num, u32 ep_off)
{
    vm_write8(it + 0, 1);   /* isPamf */
    vm_write8(it + 1, st->owned ? (u8)(st - s_readers + 1) : 0);
    vm_write16(it + 2, 0);
    vm_write32(it + 4, index);
    vm_write32(it + 8, num);
    vm_write32(it + 12, st->owned ? ep_off : st->hdr_ea + ep_off);
}

static const u8* it_resolve(u32 it, u32 pcur)
{
    const u8 tag = vm_read8(it + 1);
    if (!tag)
        return pcur ? vm_ptr8(pcur) : NULL;
    if (tag > PAMF_MAX_READERS)
        return NULL;
    const pamf_state* st = &s_readers[tag - 1];
    if (!st->in_use || !st->owned || (u64)pcur + PAMF_EP_SIZE > st->limit)
        return NULL;
    return st->owned + pcur;
}

u32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader)
{
    pamf_state* st = pamf_get(reader);
    return st ? st_ep_num(st) : 0;
}

s32 cellPamfReaderGetEpIteratorWithIndex(CellPamfReader* reader, u32 epIndex,
                                         CellPamfEpIterator* pIt)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !pIt)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u32 num = st_ep_num(st);
    if (epIndex >= num)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    u32 off;
    if ((st->attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER) || !st_eps(st, &off))
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    it_write(ea_of(pIt), st, epIndex, num, off + epIndex * PAMF_EP_SIZE);
    return CELL_OK;
}

s32 cellPamfReaderGetEpIteratorWithTimeStamp(CellPamfReader* reader,
                                             CellCodecTimeStamp* pTimeStamp,
                                             CellPamfEpIterator* pIt)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !pTimeStamp || !pIt)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    if (st->attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;

    const u32 num = st_ep_num(st);
    vm_write8(ea_of(pIt) + 0, 1);
    vm_write32(ea_of(pIt) + 8, num);
    if (num == 0)
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;
    u32 off;
    const u8* eps = st_eps(st, &off);
    if (!eps)
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;

    const u32 i = ep_find(eps, num, ea_of(pTimeStamp));
    it_write(ea_of(pIt), st, i, num, off + i * PAMF_EP_SIZE);
    return CELL_OK;
}

s32 cellPamfEpIteratorGetEp(CellPamfEpIterator* pIt, CellPamfEp* pEp)
{
    if (!pIt || !pEp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u8* e = it_resolve(ea_of(pIt), vm_read32(ea_of(pIt) + 12));
    if (!e)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    ep_write(ea_of(pEp), e);
    return CELL_OK;
}

/* Moves by up to `steps`, clamped to the table, and returns the steps taken. */
s32 cellPamfEpIteratorMove(CellPamfEpIterator* pIt, s32 steps, CellPamfEp* pEp)
{
    if (!pIt)
        return 0;
    const u32 it = ea_of(pIt);
    const u32 index = vm_read32(it + 4), num = vm_read32(it + 8);

    u32 next = index + (u32)steps;
    if ((s32)next < 0) {
        steps = -(s32)index;
        next = 0;
    } else if (next >= num) {
        steps = (s32)(num - index) - 1;
        next = index + (u32)steps;
    }

    const u32 pcur = vm_read32(it + 12) + (u32)steps * PAMF_EP_SIZE;
    vm_write32(it + 4, next);
    vm_write32(it + 12, pcur);
    if (pEp) {
        const u8* e = it_resolve(it, pcur);
        if (e)
            ep_write(ea_of(pEp), e);
    }
    return steps;
}

/* Undocumented. The real library finds the EP but never writes it; only
 * nextRpnOffset is stored (when epSize says the struct has room), and that
 * one unscaled -- reproduced as is. */
s32 cellPamfReaderGetEpWithTimeStamp(CellPamfReader* reader, CellCodecTimeStamp* pTimeStamp,
                                     CellPamfEpUnk* pEp, u32 epSize)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !pTimeStamp || !pEp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    if (st->attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER)
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;

    const u32 num = st_ep_num(st);
    if (num == 0)
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;
    const u8* eps = st_eps(st, NULL);
    if (!eps)
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;

    const u32 i = ep_find(eps, num, ea_of(pTimeStamp));
    const u64 next = i < num - 1 ? be32(eps + (i + 1) * PAMF_EP_SIZE + 8) : st->dataSize;
    if (epSize == sizeof(CellPamfEpUnk))
        vm_write64(ea_of(pEp) + offsetof(CellPamfEpUnk, nextRpnOffset), next);
    return CELL_OK;
}

s32 cellPamfReaderGetEpWithIndex(CellPamfReader* reader, u32 epIndex,
                                 CellPamfEpUnk* pEp, u32 epSize)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !pEp)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u32 num = st_ep_num(st);
    if (epIndex >= num)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    const u8* eps;
    if ((st->attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER) || !(eps = st_eps(st, NULL)))
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;

    const u8* e = eps + epIndex * PAMF_EP_SIZE;
    ep_write(ea_of(pEp), e);
    const u64 next = epIndex < num - 1 ? (u64)be32(e + PAMF_EP_SIZE + 8) * PAMF_UNIT
                                       : st->dataSize;
    if (epSize == sizeof(CellPamfEpUnk))
        vm_write64(ea_of(pEp) + offsetof(CellPamfEpUnk, nextRpnOffset), next);
    return CELL_OK;
}

/* Not an SDK export (an earlier name here): one EP of the current stream. */
s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep)
{
    pamf_state* st = pamf_get(reader);
    if (!st || !ep)
        return (s32)CELL_PAMF_ERROR_INVALID_ARG;
    if (epIndex >= st_ep_num(st))
        return (s32)CELL_PAMF_ERROR_EP_NOT_FOUND;
    const u8* eps;
    if ((st->attribute & CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER) || !(eps = st_eps(st, NULL)))
        return (s32)CELL_PAMF_ERROR_NOT_AVAILABLE;
    ep_write(ea_of(ep), eps + epIndex * PAMF_EP_SIZE);
    return CELL_OK;
}
