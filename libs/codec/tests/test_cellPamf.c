/*
 * ps3recomp -- cellPamf against a synthetic PAMF header
 *
 * Builds a header in the real on-disc layout (AVC + M2V + ATRAC3plus + LPCM,
 * with an EP table on the AVC stream) in a fake guest memory and drives the
 * guest API over it: size queries, verification (and that broken headers are
 * caught), stream selection by index/type/channel including the VIDEO/AUDIO
 * pseudo types, every info struct byte by byte, EP iterators, and the
 * host-owned reader that cellSail uses.
 *
 * With a file argument it instead loads that .pam's header and prints what
 * the reader makes of it (no game data ships with the repo).
 *
 * Build (from the repo root; no runtime lib needed):
 *   clang-cl /O2 -w /Iinclude libs/codec/tests/test_cellPamf.c
 *   test_cellPamf.exe [file.pam]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
#include "../cellPamf.c"

/* The guest-memory accessors' hooks into the PPU/SPU runtime; inert here. */
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_coh_notify_write(uint32_t a) { (void)a; }

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define CHECK_RC(e, want) do { s32 _r = (s32)(e); if (_r != (s32)(want)) { \
    printf("FAIL %s:%d: %s = 0x%08X, want 0x%08X\n", __FILE__, __LINE__, #e, (u32)_r, (u32)(want)); \
    exit(1); } } while (0)

enum {
    MEM = 16u << 20,
    G_HDR = 0x10000,      /* PAMF header */
    G_RDR = 0x1000,       /* CellPamfReader objects, 0x80 apart */
    G_OUT = 0x2000,       /* out-params */
    G_IT  = 0x2100,       /* CellPamfEpIterator */
    G_TS  = 0x2200,       /* CellCodecTimeStamp */
    G_EP  = 0x2300,       /* CellPamfEp / CellPamfEpUnk */
    G_BIG = 0x100000,     /* a real file's header */
};

#define RDR(n) ((CellPamfReader*)(uintptr_t)(G_RDR + (n) * 0x80))
#define GP(ea) ((void*)(uintptr_t)(ea))

static u8* g(u32 ea) { return vm_base + ea; }
static void w16(u32 ea, u32 v) { put16(g(ea), v); }
static void w32(u32 ea, u32 v) { put32(g(ea), v); }

/* Stream headers start at 0x88; layout per cellPamf.c. */
#define NSTREAMS   4
#define END_STREAMS (0x88 + NSTREAMS * 48)   /* 0x148: EP tables follow */
#define NEPS       3
#define START_PTS  90000u
#define END_PTS    (START_PTS + 900000u)
#define DATA_UNITS 0x10u

static void stream(u32 i, u8 coding, u8 sid, u8 psid, u32 ep_off, u32 ep_num)
{
    const u32 s = G_HDR + 0x88 + i * 48;
    g(s)[0] = coding;
    g(s)[4] = sid;
    g(s)[5] = psid;
    w16(s + 6, 0x2108);
    w32(s + 8, ep_off);
    w32(s + 12, ep_num);
}

static void build_header(void)
{
    memset(g(G_HDR), 0, 0x800);
    memcpy(g(G_HDR), "PAMF0041", 8);
    w32(G_HDR + 0x08, 1);                     /* header 0x800 bytes */
    w32(G_HDR + 0x0C, DATA_UNITS);            /* data 0x8000 bytes */
    w32(G_HDR + 0x50, END_STREAMS - 0x54);    /* seq info size */
    w32(G_HDR + 0x58, START_PTS);
    w32(G_HDR + 0x5E, END_PTS);
    w32(G_HDR + 0x62, 0x1D4C0);               /* mux rate */
    w32(G_HDR + 0x66, 90000);
    w32(G_HDR + 0x6A, NSTREAMS);
    g(G_HDR + 0x6F)[0] = 1;                   /* grouping periods */
    w32(G_HDR + 0x70, END_STREAMS - 0x74);
    w32(G_HDR + 0x76, START_PTS);
    w32(G_HDR + 0x7C, END_PTS);
    g(G_HDR + 0x81)[0] = 1;                   /* groups */
    w32(G_HDR + 0x82, END_STREAMS - 0x86);
    g(G_HDR + 0x87)[0] = NSTREAMS;

    /* 0: AVC ch0, with the EP table */
    stream(0, 0x1B, 0xE0, 0x00, END_STREAMS, NEPS);
    u8* d = g(G_HDR + 0x88 + 16);
    d[0] = 100; d[1] = 41; d[2] = 0x80 | 0x40 | 0x04; d[3] = 1;
    d[9] = 1920 / 16; d[11] = 1088 / 16;
    put16(d + 18, 8);                         /* crop bottom */
    d[20] = 0xA0 | 0x10; d[21] = 1; d[22] = 1; d[23] = 1;
    d[24] = 0x80 | 0x40 | 0x10 | 0x01; d[25] = 0x20;

    /* 1: M2V ch1 */
    stream(1, 0x02, 0xE1, 0x00, 0, 0);
    d = g(G_HDR + 0x88 + 48 + 16);
    d[0] = 0x48; d[2] = 0xC4; d[3] = 0xFF;
    put16(d + 4, 10); put16(d + 6, 11);
    d[9] = 720 / 16; d[11] = 480 / 16;
    put16(d + 12, 720); put16(d + 14, 480);
    d[20] = 0xA0; d[21] = 6; d[22] = 6; d[23] = 6;

    /* 2: ATRAC3plus ch0, 3: LPCM ch0 (6 ch, 48 kHz, 16-bit) */
    stream(2, 0xDC, 0xBD, 0x00, 0, 0);
    d = g(G_HDR + 0x88 + 2 * 48 + 16);
    d[2] = 2; d[3] = 1;
    stream(3, 0x80, 0xBD, 0x40, 0, 0);
    d = g(G_HDR + 0x88 + 3 * 48 + 16);
    d[2] = 6; d[3] = 1; d[4] = 0x40;

    /* EPs at +0, +0.5 s, +1 s; rpnOffset 0/4/8 units */
    for (u32 i = 0; i < NEPS; i++) {
        const u32 e = G_HDR + END_STREAMS + i * 12;
        w16(e, i ? (1u << 14) | 3u : 0);
        w32(e + 4, START_PTS + i * 45000);
        w32(e + 8, i * 4);
    }
}

#define FILE_SIZE ((0x800ull) + DATA_UNITS * 0x800ull)

static void test_header_queries(void)
{
    CHECK_RC(cellPamfGetHeaderSize(GP(G_HDR), FILE_SIZE, GP(G_OUT)), CELL_OK);
    CHECK(vm_read64(G_OUT) == 0x800);
    CHECK_RC(cellPamfGetHeaderSize2(GP(G_HDR), 0, CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER, GP(G_OUT)), CELL_OK);
    CHECK(vm_read64(G_OUT) == END_STREAMS);
    CHECK_RC(cellPamfGetStreamOffsetAndSize(GP(G_HDR), FILE_SIZE, GP(G_OUT), GP(G_OUT + 8)), CELL_OK);
    CHECK(vm_read64(G_OUT) == 0x800 && vm_read64(G_OUT + 8) == DATA_UNITS * 0x800u);
    CHECK_RC(cellPamfGetHeaderSize(GP(G_HDR), FILE_SIZE + 1, GP(G_OUT)), CELL_PAMF_ERROR_INVALID_PAMF);

    CHECK_RC(cellPamfVerify(GP(G_HDR), FILE_SIZE), CELL_OK);
    CHECK_RC(cellPamfVerify(GP(G_HDR), 0), CELL_OK);

    /* Each corruption is caught with the real library's code, then undone. */
    struct { u32 off; u8 val; s32 want; } bad[] = {
        { 0x00, 'X',  (s32)CELL_PAMF_ERROR_UNKNOWN_TYPE },
        { 0x07, '9',  (s32)CELL_PAMF_ERROR_UNSUPPORTED_VERSION },
        { 0x6F, 2,    (s32)CELL_PAMF_ERROR_INVALID_PAMF },          /* grouping periods */
        { 0x87, 0,    (s32)CELL_PAMF_ERROR_INVALID_PAMF },          /* no streams */
        { 0x6D, 3,    (s32)CELL_PAMF_ERROR_INVALID_PAMF },          /* total_stream_num */
        { 0x88 + 48, 0x55, (s32)CELL_PAMF_ERROR_UNKNOWN_STREAM },   /* coding type */
        { 0x88 + 48 + 4, 0xE0, (s32)CELL_PAMF_ERROR_INVALID_PAMF }, /* M2V reuses ch0... */
        { END_STREAMS + 12 + 11, 9, (s32)CELL_PAMF_ERROR_INVALID_PAMF }, /* rpn not ascending */
        { 0x58, 0xFF, (s32)CELL_PAMF_ERROR_INVALID_PAMF },          /* start pts past end */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const u8 keep = g(G_HDR + bad[i].off)[0];
        g(G_HDR + bad[i].off)[0] = bad[i].val;
        s32 rc = cellPamfVerify(GP(G_HDR), FILE_SIZE);
        if (bad[i].off == 0x88 + 48 + 4) {
            /* ...but as AVC/M2V are different types, ch0 twice is fine */
            CHECK_RC(rc, CELL_OK);
        } else {
            CHECK_RC(rc, bad[i].want);
        }
        g(G_HDR + bad[i].off)[0] = keep;
    }
    /* Two LPCM streams on one channel is the real duplicate */
    g(G_HDR + 0x88 + 2 * 48)[0] = 0x80; g(G_HDR + 0x88 + 2 * 48 + 5)[0] = 0x40;
    CHECK_RC(cellPamfVerify(GP(G_HDR), FILE_SIZE), CELL_PAMF_ERROR_INVALID_PAMF);
    g(G_HDR + 0x88 + 2 * 48)[0] = 0xDC; g(G_HDR + 0x88 + 2 * 48 + 5)[0] = 0x00;
    CHECK_RC(cellPamfVerify(GP(G_HDR), FILE_SIZE), CELL_OK);
}

static void test_streams(CellPamfReader* r)
{
    CHECK_RC(cellPamfReaderGetNumberOfStreams(r), NSTREAMS);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(r, CELL_PAMF_STREAM_TYPE_VIDEO), 2);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(r, CELL_PAMF_STREAM_TYPE_AUDIO), 2);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(r, CELL_PAMF_STREAM_TYPE_UNK), 1);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(r, CELL_PAMF_STREAM_TYPE_AVC), 1);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(r, CELL_PAMF_STREAM_TYPE_PAMF_LPCM), 1);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(r, CELL_PAMF_STREAM_TYPE_USER_DATA), 0);

    CHECK_RC(cellPamfReaderGetPresentationStartTime(r, GP(G_TS)), CELL_OK);
    CHECK(vm_read32(G_TS) == 0 && vm_read32(G_TS + 4) == START_PTS);
    CHECK_RC(cellPamfReaderGetPresentationEndTime(r, GP(G_TS)), CELL_OK);
    CHECK(vm_read32(G_TS) == 0 && vm_read32(G_TS + 4) == END_PTS);
    CHECK(cellPamfReaderGetMuxRateBound(r) == 0x1D4C0);

    /* Stream 0 is selected on initialise */
    CHECK_RC(cellPamfReaderGetStreamIndex(r), 0);
    vm_write16(G_OUT, 0xFFFF);
    CHECK_RC(cellPamfReaderGetStreamTypeAndChannel(r, GP(G_OUT), GP(G_OUT + 1)), CELL_OK);
    CHECK(vm_read8(G_OUT) == CELL_PAMF_STREAM_TYPE_AVC && vm_read8(G_OUT + 1) == 0);

    /* AVC info, byte for byte */
    u8* o = g(G_OUT);
    memset(o, 0xEE, 0x40);
    CHECK_RC(cellPamfReaderGetStreamInfo(r, GP(G_OUT), 0x1F), CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(cellPamfReaderGetStreamInfo(r, GP(G_OUT), 0x20), CELL_OK);
    static const u8 avc[0x20] = {
        100, 41, 1, 1, 3, 1, 0, 0, 0, 0,   /* sar 0 unless aspect 255 */
        0x07, 0x80, 0x04, 0x40,            /* 1920 x 1088 */
        0, 0, 0, 0, 0, 0, 0, 8,            /* crop L R T B */
        5, 1, 1, 1, 1,                     /* format, fullRange, colour/transfer/matrix */
        1, 1, 1, 1, 0x20,                  /* entropy, deblock, minSlice, nfw, bitrate */
    };
    CHECK(memcmp(o, avc, 0x20) == 0);
    CHECK(o[0x20] == 0xEE);

    /* By type and index, pseudo types included; returns the overall index */
    CHECK_RC(cellPamfReaderSetStreamWithTypeAndIndex(r, CELL_PAMF_STREAM_TYPE_AUDIO, 1), 3);
    CHECK_RC(cellPamfReaderGetStreamIndex(r), 3);
    memset(o, 0xEE, 0x40);
    CHECK_RC(cellPamfReaderGetStreamInfo(r, GP(G_OUT), 7), CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(cellPamfReaderGetStreamInfo(r, GP(G_OUT), 8), CELL_OK);
    static const u8 lpcm[8] = { 0, 0, 0, CELL_PAMF_FS_48kHz, 6, 0, 0, CELL_PAMF_BIT_LENGTH_16 };
    CHECK(memcmp(o, lpcm, 8) == 0 && o[8] == 0xEE);

    CHECK_RC(cellPamfReaderSetStreamWithTypeAndIndex(r, CELL_PAMF_STREAM_TYPE_AUDIO, 0), 2);
    CHECK_RC(cellPamfReaderGetStreamInfo(r, GP(G_OUT), 8), CELL_OK);
    static const u8 at3[8] = { 0, 0, 0, 1, 2, 0, 0, 0 };
    CHECK(memcmp(o, at3, 8) == 0);
    CHECK_RC(cellPamfReaderGetEsFilterId(r, GP(G_OUT)), CELL_OK);
    CHECK(vm_read32(G_OUT) == 0xBD && vm_read32(G_OUT + 4) == 0 &&
          vm_read32(G_OUT + 8) == 0 && vm_read32(G_OUT + 12) == 0);

    CHECK_RC(cellPamfReaderSetStreamWithTypeAndIndex(r, CELL_PAMF_STREAM_TYPE_VIDEO, 2),
             CELL_PAMF_ERROR_STREAM_NOT_FOUND);
    CHECK_RC(cellPamfReaderSetStreamWithTypeAndIndex(r, CELL_PAMF_STREAM_TYPE_AUDIO, 4),
             CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(cellPamfReaderGetStreamIndex(r), 2);   /* unchanged by failures */

    /* By type and channel */
    CHECK_RC(cellPamfReaderSetStreamWithTypeAndChannel(r, CELL_PAMF_STREAM_TYPE_M2V, 1), 1);
    CHECK_RC(cellPamfReaderSetStreamWithTypeAndChannel(r, CELL_PAMF_STREAM_TYPE_M2V, 0),
             CELL_PAMF_ERROR_STREAM_NOT_FOUND);
    CHECK_RC(cellPamfReaderSetStreamWithTypeAndChannel(r, 42, 0), CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(cellPamfReaderGetStreamTypeAndChannel(r, GP(G_OUT), GP(G_OUT + 1)), CELL_OK);
    CHECK(vm_read8(G_OUT) == CELL_PAMF_STREAM_TYPE_M2V && vm_read8(G_OUT + 1) == 1);
    memset(o, 0xEE, 0x40);
    CHECK_RC(cellPamfReaderGetStreamInfo(r, GP(G_OUT), 0x18), CELL_OK);
    static const u8 m2v[0x18] = {
        CELL_PAMF_M2V_MP_ML, 1, 1, 4, 0xFF, 0, 0, 10, 0, 11,
        0x02, 0xD0, 0x01, 0xE0, 0x02, 0xD0, 0x01, 0xE0,   /* 720x480 twice */
        5, 0, 6, 6, 6, 0,
    };
    CHECK(memcmp(o, m2v, 0x18) == 0 && o[0x18] == 0xEE);

    CHECK_RC(cellPamfReaderSetStreamWithIndex(r, NSTREAMS), CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(cellPamfReaderSetStreamWithIndex(r, 3), CELL_OK);
    CHECK_RC(cellPamfReaderGetEsFilterId(r, GP(G_OUT)), CELL_OK);
    CHECK(vm_read32(G_OUT) == 0xBD && vm_read32(G_OUT + 4) == 0x40);

    CHECK_RC(cellPamfStreamTypeToEsFilterId(CELL_PAMF_STREAM_TYPE_AVC, 2, GP(G_OUT)), CELL_OK);
    CHECK(vm_read32(G_OUT) == 0xE2 && vm_read32(G_OUT + 4) == 0 && vm_read32(G_OUT + 8) == 1);
    CHECK_RC(cellPamfStreamTypeToEsFilterId(CELL_PAMF_STREAM_TYPE_PAMF_LPCM, 1, GP(G_OUT)), CELL_OK);
    CHECK(vm_read32(G_OUT) == 0xBD && vm_read32(G_OUT + 4) == 0x41 && vm_read32(G_OUT + 8) == 0);
    CHECK_RC(cellPamfStreamTypeToEsFilterId(CELL_PAMF_STREAM_TYPE_VIDEO, 0, GP(G_OUT)),
             CELL_PAMF_ERROR_INVALID_ARG);
}

static void check_ep(u32 ea, u32 indexN, u32 nth, u32 pts, u64 rpn)
{
    CHECK(vm_read32(ea) == indexN && vm_read32(ea + 4) == nth);
    CHECK(vm_read32(ea + 8) == 0 && vm_read32(ea + 12) == pts);
    CHECK(vm_read64(ea + 16) == rpn);
}

static void test_eps(CellPamfReader* r)
{
    CHECK_RC(cellPamfReaderSetStreamWithIndex(r, 1), CELL_OK);
    CHECK(cellPamfReaderGetNumberOfEp(r) == 0);
    CHECK_RC(cellPamfReaderSetStreamWithIndex(r, 0), CELL_OK);
    CHECK(cellPamfReaderGetNumberOfEp(r) == NEPS);

    CHECK_RC(cellPamfReaderGetEpIteratorWithIndex(r, NEPS, GP(G_IT)), CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(cellPamfReaderGetEpIteratorWithIndex(r, 1, GP(G_IT)), CELL_OK);
    CHECK(vm_read32(G_IT + 4) == 1 && vm_read32(G_IT + 8) == NEPS);
    CHECK_RC(cellPamfEpIteratorGetEp(GP(G_IT), GP(G_EP)), CELL_OK);
    check_ep(G_EP, 2, 4 * 0x800, START_PTS + 45000, 4 * 0x800);

    CHECK_RC(cellPamfEpIteratorMove(GP(G_IT), 5, GP(G_EP)), 1);       /* clamped to the last */
    check_ep(G_EP, 2, 4 * 0x800, START_PTS + 90000, 8 * 0x800);
    CHECK_RC(cellPamfEpIteratorMove(GP(G_IT), -9, GP(G_EP)), -2);     /* clamped to the first */
    check_ep(G_EP, 1, 0x800, START_PTS, 0);

    vm_write32(G_TS, 0); vm_write32(G_TS + 4, START_PTS + 60000);
    CHECK_RC(cellPamfReaderGetEpIteratorWithTimeStamp(r, GP(G_TS), GP(G_IT)), CELL_OK);
    CHECK(vm_read32(G_IT + 4) == 1);
    vm_write32(G_TS + 4, 1);   /* before the first EP: still EP 0 */
    CHECK_RC(cellPamfReaderGetEpIteratorWithTimeStamp(r, GP(G_TS), GP(G_IT)), CELL_OK);
    CHECK(vm_read32(G_IT + 4) == 0);

    vm_write64(G_EP + 0x18, 0);
    CHECK_RC(cellPamfReaderGetEpWithIndex(r, 0, GP(G_EP), 0x20), CELL_OK);
    check_ep(G_EP, 1, 0x800, START_PTS, 0);
    CHECK(vm_read64(G_EP + 0x18) == 4 * 0x800);
    CHECK_RC(cellPamfReaderGetEpWithIndex(r, 2, GP(G_EP), 0x20), CELL_OK);
    CHECK(vm_read64(G_EP + 0x18) == DATA_UNITS * 0x800u);   /* last: the data size */
}

static void test_host_reader(void)
{
    const u32 key = G_RDR + 0x400;
    u8* copy = (u8*)malloc(0x800);
    memcpy(copy, g(G_HDR), 0x800);
    CHECK_RC(pamf_reader_init_host(key, copy, 0x800), CELL_OK);
    test_streams((CellPamfReader*)(uintptr_t)key);
    test_eps((CellPamfReader*)(uintptr_t)key);

    /* The header really is the host copy: change the guest one under it */
    g(G_HDR + 0x87)[0] = 1;
    CHECK_RC(cellPamfReaderGetNumberOfStreams((CellPamfReader*)(uintptr_t)key), NSTREAMS);
    g(G_HDR + 0x87)[0] = NSTREAMS;

    pamf_reader_release(key);
    CHECK_RC(cellPamfReaderGetNumberOfStreams((CellPamfReader*)(uintptr_t)key),
             CELL_PAMF_ERROR_INVALID_ARG);

    /* Only the part up to the EP tables: a minimum header */
    copy = (u8*)malloc(END_STREAMS);
    memcpy(copy, g(G_HDR), END_STREAMS);
    CHECK_RC(pamf_reader_init_host(key, copy, END_STREAMS), CELL_OK);
    CHECK_RC(cellPamfReaderGetNumberOfStreams((CellPamfReader*)(uintptr_t)key), NSTREAMS);
    CHECK_RC(cellPamfReaderGetEpIteratorWithIndex((CellPamfReader*)(uintptr_t)key, 0, GP(G_IT)),
             CELL_PAMF_ERROR_NOT_AVAILABLE);

    /* Re-initialising frees the previous header; a bad one is refused (and freed) */
    copy = (u8*)malloc(0x800);
    memcpy(copy, g(G_HDR), 0x800);
    copy[0] = 'X';
    CHECK_RC(pamf_reader_init_host(key, copy, 0x800), CELL_PAMF_ERROR_UNKNOWN_TYPE);
    CHECK_RC(cellPamfReaderGetNumberOfStreams((CellPamfReader*)(uintptr_t)key),
             CELL_PAMF_ERROR_INVALID_ARG);
    CHECK_RC(pamf_reader_init_host(key, NULL, 0), CELL_PAMF_ERROR_INVALID_ARG);
}

static void test_slots(void)
{
    /* A host reader survives a flood of guest readers (oldest guest evicted) */
    const u32 key = G_RDR + 0x400;
    u8* copy = (u8*)malloc(0x800);
    memcpy(copy, g(G_HDR), 0x800);
    CHECK_RC(pamf_reader_init_host(key, copy, 0x800), CELL_OK);
    for (u32 i = 0; i < 3 * PAMF_MAX_READERS; i++)
        CHECK_RC(cellPamfReaderInitialize((CellPamfReader*)(uintptr_t)(0x200000 + i * 0x80),
                                          GP(G_HDR), 0, 0), CELL_OK);
    CHECK_RC(cellPamfReaderGetNumberOfStreams((CellPamfReader*)(uintptr_t)key), NSTREAMS);
    pamf_reader_release(key);
}

static int dump_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    const u64 size = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);
    const size_t n = fread(g(G_BIG), 1, 0x80000, f);
    fclose(f);
    if (n < 0x100) { printf("%s: too short\n", path); return 1; }

    CHECK_RC(cellPamfGetHeaderSize(GP(G_BIG), size, GP(G_OUT)), CELL_OK);
    printf("%s: %llu bytes, header 0x%llX, verify %08X\n", path, (unsigned long long)size,
           (unsigned long long)vm_read64(G_OUT), (u32)cellPamfVerify(GP(G_BIG), size));
    CellPamfReader* r = RDR(7);
    CHECK_RC(cellPamfReaderInitialize(r, GP(G_BIG), size, CELL_PAMF_ATTRIBUTE_VERIFY_ON), CELL_OK);
    cellPamfReaderGetPresentationStartTime(r, GP(G_TS));
    const u32 start = vm_read32(G_TS + 4);
    cellPamfReaderGetPresentationEndTime(r, GP(G_TS));
    printf("  pts %u..%u (%.2f s), mux rate bound %u\n", start, vm_read32(G_TS + 4),
           (vm_read32(G_TS + 4) - start) / 90000.0, cellPamfReaderGetMuxRateBound(r));
    const s32 ns = cellPamfReaderGetNumberOfStreams(r);
    for (s32 i = 0; i < ns; i++) {
        cellPamfReaderSetStreamWithIndex(r, (u8)i);
        cellPamfReaderGetStreamTypeAndChannel(r, GP(G_OUT), GP(G_OUT + 1));
        const u8 type = vm_read8(G_OUT), ch = vm_read8(G_OUT + 1);
        cellPamfReaderGetEsFilterId(r, GP(G_OUT));
        printf("  stream %d: type %u (%s) ch %u, es %02X/%02X, %u EPs:", i, type,
               type_name(type), ch, vm_read32(G_OUT), vm_read32(G_OUT + 4),
               cellPamfReaderGetNumberOfEp(r));
        memset(g(G_OUT), 0, 0x20);
        const s32 rc = cellPamfReaderGetStreamInfo(r, GP(G_OUT), 0x20);
        if (rc != CELL_OK) { printf(" info %08X\n", (u32)rc); continue; }
        const u8* o = g(G_OUT);
        if (type == CELL_PAMF_STREAM_TYPE_AVC || type == CELL_PAMF_STREAM_TYPE_M2V) {
            const size_t off = type == CELL_PAMF_STREAM_TYPE_AVC
                ? offsetof(CellPamfAvcInfo, horizontalSize) : offsetof(CellPamfM2vInfo, horizontalSize);
            printf(" %ux%u, frameRateInfo %u, profile/level 0x%02X\n", (u32)be16(o + off),
                   (u32)be16(o + off + 2),
                   (u32)(type == CELL_PAMF_STREAM_TYPE_AVC ? o[4] : o[3]), (u32)o[0]);
        } else {
            printf(" fs code %u, %u ch, bit-length code %u\n", be32(o), (u32)o[4],
                   (u32)be16(o + 6));
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    vm_base = (uint8_t*)calloc(1, MEM);
    CHECK(vm_base != NULL);
    if (argc > 1)
        return dump_file(argv[1]);

    build_header();
    test_header_queries();

    CellPamfReader* r = RDR(0);
    CHECK_RC(cellPamfReaderGetNumberOfStreams(r), CELL_PAMF_ERROR_INVALID_ARG);   /* not initialised */
    CHECK_RC(cellPamfReaderInitialize(r, GP(G_HDR), FILE_SIZE, CELL_PAMF_ATTRIBUTE_VERIFY_ON), CELL_OK);
    test_streams(r);
    test_eps(r);

    /* Verification failures reach the caller; without VERIFY_ON nothing is checked */
    g(G_HDR)[0] = 'X';
    CHECK_RC(cellPamfReaderInitialize(RDR(1), GP(G_HDR), 0, CELL_PAMF_ATTRIBUTE_VERIFY_ON),
             CELL_PAMF_ERROR_UNKNOWN_TYPE);
    CHECK_RC(cellPamfReaderInitialize(RDR(1), GP(G_HDR), 0, 0), CELL_OK);
    g(G_HDR)[0] = 'P';

    /* Minimum header: streams work, EPs don't */
    CHECK_RC(cellPamfReaderInitialize(RDR(2), GP(G_HDR), 0,
                                      CELL_PAMF_ATTRIBUTE_VERIFY_ON | CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER),
             CELL_OK);
    CHECK_RC(cellPamfReaderGetNumberOfSpecificStreams(RDR(2), CELL_PAMF_STREAM_TYPE_VIDEO), 2);
    CHECK(cellPamfReaderGetNumberOfEp(RDR(2)) == NEPS);
    CHECK_RC(cellPamfReaderGetEpIteratorWithIndex(RDR(2), 0, GP(G_IT)), CELL_PAMF_ERROR_NOT_AVAILABLE);

    test_host_reader();
    test_slots();

    printf("test_cellPamf: ok\n");
    return 0;
}
