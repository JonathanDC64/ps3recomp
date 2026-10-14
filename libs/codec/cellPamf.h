/*
 * ps3recomp - cellPamf HLE
 *
 * PAMF (PlayStation AV Media File) container header reader. PAMF files
 * carry an MPEG-2 program stream (AVC/M2V video, ATRAC3plus/LPCM/AC3 audio,
 * user data) behind a header describing each elementary stream; the reader
 * answers stream/codec/entry-point queries for cellDmux/cellVdec/cellAdec
 * set-up. Constants and info-struct layouts follow the SDK.
 */

#ifndef PS3RECOMP_CELL_PAMF_H
#define PS3RECOMP_CELL_PAMF_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_ERROR_STREAM_NOT_FOUND     0x80610501
#define CELL_PAMF_ERROR_INVALID_PAMF         0x80610502
#define CELL_PAMF_ERROR_INVALID_ARG          0x80610503
#define CELL_PAMF_ERROR_UNKNOWN_TYPE         0x80610504
#define CELL_PAMF_ERROR_UNSUPPORTED_VERSION  0x80610505
#define CELL_PAMF_ERROR_UNKNOWN_STREAM       0x80610506
#define CELL_PAMF_ERROR_EP_NOT_FOUND         0x80610507
#define CELL_PAMF_ERROR_NOT_AVAILABLE        0x80610508
/* Old name used before the SDK codes were checked; same meaning. */
#define CELL_PAMF_ERROR_INVALID_HEADER       CELL_PAMF_ERROR_INVALID_PAMF

/* cellPamfReaderInitialize / cellPamfGetHeaderSize2 attribute flags */
#define CELL_PAMF_ATTRIBUTE_VERIFY_ON        1   /* validate the whole header */
#define CELL_PAMF_ATTRIBUTE_MINIMUM_HEADER   2   /* header loaded without EP tables */

/* ---------------------------------------------------------------------------
 * Stream types (SDK values -- what the API takes and returns)
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_STREAM_TYPE_AVC              0   /* H.264/AVC video */
#define CELL_PAMF_STREAM_TYPE_M2V              1   /* MPEG-2 video */
#define CELL_PAMF_STREAM_TYPE_ATRAC3PLUS       2   /* ATRAC3plus audio */
#define CELL_PAMF_STREAM_TYPE_PAMF_LPCM        3   /* LPCM audio */
#define CELL_PAMF_STREAM_TYPE_AC3              4   /* AC3 audio */
#define CELL_PAMF_STREAM_TYPE_USER_DATA        5   /* User data */
#define CELL_PAMF_STREAM_TYPE_PSMF_AVC         6
#define CELL_PAMF_STREAM_TYPE_PSMF_ATRAC3PLUS  7
#define CELL_PAMF_STREAM_TYPE_PSMF_LPCM        8
#define CELL_PAMF_STREAM_TYPE_PSMF_USER_DATA   9
/* Query-only pseudo types (GetNumberOfSpecificStreams /
 * SetStreamWithTypeAndIndex): VIDEO = AVC or M2V, AUDIO = ATRAC3plus, AC3 or
 * LPCM. UNK matches LPCM or PSMF ATRAC3plus on the real library; its purpose
 * is unknown. There is no "all streams" type -- GetNumberOfStreams is that. */
#define CELL_PAMF_STREAM_TYPE_VIDEO           20
#define CELL_PAMF_STREAM_TYPE_AUDIO           21
#define CELL_PAMF_STREAM_TYPE_UNK             22

/* stream_coding_type byte in the on-disc stream header */
#define CELL_PAMF_CODEC_TYPE_M2V             0x02
#define CELL_PAMF_CODEC_TYPE_AVC             0x1B
#define CELL_PAMF_CODEC_TYPE_LPCM            0x80
#define CELL_PAMF_CODEC_TYPE_AC3             0x81
#define CELL_PAMF_CODEC_TYPE_ATRAC3PLUS      0xDC
#define CELL_PAMF_CODEC_TYPE_USER_DATA       0xDD
#define CELL_PAMF_CODEC_TYPE_PSMF            0xFF

/* Info-struct value codes (the API reports codes, not Hz / bits) */
#define CELL_PAMF_FS_48kHz                   1
#define CELL_PAMF_BIT_LENGTH_16              1
#define CELL_PAMF_BIT_LENGTH_24              3
#define CELL_PAMF_M2V_MP_ML                  1
#define CELL_PAMF_M2V_MP_H14                 2
#define CELL_PAMF_M2V_MP_HL                  3
#define CELL_PAMF_M2V_UNKNOWN                255

/* ---------------------------------------------------------------------------
 * PAMF header magic / versions ("PAMF", then "0040" or "0041" in ASCII)
 * -----------------------------------------------------------------------*/
#define CELL_PAMF_MAGIC         0x50414D46   /* "PAMF" */
#define CELL_PAMF_VERSION_0040  0x30303430   /* "0040" */
#define CELL_PAMF_VERSION_0041  0x30303431   /* "0041" */

/* ---------------------------------------------------------------------------
 * Types
 *
 * Every struct below describes GUEST memory: multi-byte fields are
 * big-endian there, and the C layouts (natural alignment) reproduce the SDK
 * offsets and sizes. The HLE writes them byte-wise at those offsets.
 * -----------------------------------------------------------------------*/

/* The title's reader object (128 bytes in guest memory). Opaque here: the
 * HLE keeps the reader state host-side, keyed by this object's address. */
typedef struct CellPamfReader {
    u32 opaque[32];
} CellPamfReader;

/* 90 kHz timestamp split in two big-endian words */
typedef struct CellCodecTimeStamp {
    u32 upper;
    u32 lower;
} CellCodecTimeStamp;

typedef struct CellCodecEsFilterId {
    u32 filterIdMajor;        /* PES stream_id */
    u32 filterIdMinor;        /* private_stream_id (0xBD streams) */
    u32 supplementalInfo1;    /* 1 for AVC */
    u32 supplementalInfo2;
} CellCodecEsFilterId;

typedef struct CellPamfEp {         /* 0x18 bytes */
    u32 indexN;
    u32 nThRefPictureOffset;
    CellCodecTimeStamp pts;
    u64 rpnOffset;                  /* byte offset into the stream data */
} CellPamfEp;

typedef struct CellPamfEpUnk {      /* 0x20: GetEpWithIndex/TimeStamp output */
    CellPamfEp ep;
    u64 nextRpnOffset;
} CellPamfEpUnk;

typedef struct CellPamfEpIterator { /* 0x10 bytes */
    u8  isPamf;
    u8  reserved[3];
    u32 index;
    u32 num;
    u32 pCur;                       /* guest EA of the current packed EP */
} CellPamfEpIterator;

typedef struct CellPamfAvcInfo {    /* 0x20 bytes */
    u8   profileIdc;
    u8   levelIdc;
    u8   frameMbsOnlyFlag;
    u8   videoSignalInfoFlag;
    u8   frameRateInfo;
    u8   aspectRatioIdc;
    u16  sarWidth;
    u16  sarHeight;
    u16  horizontalSize;
    u16  verticalSize;
    u16  frameCropLeftOffset;
    u16  frameCropRightOffset;
    u16  frameCropTopOffset;
    u16  frameCropBottomOffset;
    u8   videoFormat;
    u8   videoFullRangeFlag;
    u8   colourPrimaries;
    u8   transferCharacteristics;
    u8   matrixCoefficients;
    u8   entropyCodingModeFlag;
    u8   deblockingFilterFlag;
    u8   minNumSlicePerPictureIdc;
    u8   nfwIdc;
    u8   maxMeanBitrate;
} CellPamfAvcInfo;

typedef struct CellPamfM2vInfo {    /* 0x18 bytes */
    u8   profileAndLevelIndication;
    u8   progressiveSequence;
    u8   videoSignalInfoFlag;
    u8   frameRateInfo;
    u8   aspectRatioIdc;
    u16  sarWidth;
    u16  sarHeight;
    u16  horizontalSize;
    u16  verticalSize;
    u16  horizontalSizeValue;
    u16  verticalSizeValue;
    u8   videoFormat;
    u8   videoFullRangeFlag;
    u8   colourPrimaries;
    u8   transferCharacteristics;
    u8   matrixCoefficients;
} CellPamfM2vInfo;

typedef struct CellPamfAtrac3plusInfo {  /* 8 bytes */
    u32 samplingFrequency;          /* CELL_PAMF_FS_* code */
    u8  numberOfChannels;
} CellPamfAtrac3plusInfo;

typedef struct CellPamfLpcmInfo {   /* 8 bytes */
    u32 samplingFrequency;          /* CELL_PAMF_FS_* code */
    u8  numberOfChannels;
    u16 bitsPerSample;              /* CELL_PAMF_BIT_LENGTH_* code */
} CellPamfLpcmInfo;

typedef struct CellPamfAc3Info {    /* 8 bytes */
    u32 samplingFrequency;
    u8  numberOfChannels;
} CellPamfAc3Info;

/* ---------------------------------------------------------------------------
 * Functions (guest API; every pointer argument is a guest EA)
 * -----------------------------------------------------------------------*/

/* Header queries */
s32 cellPamfGetHeaderSize(const void* pAddr, u64 fileSize, u64* pSize);
s32 cellPamfGetHeaderSize2(const void* pAddr, u64 fileSize, u32 attribute, u64* pSize);
s32 cellPamfGetStreamOffsetAndSize(const void* pAddr, u64 fileSize,
                                   u64* pOffset, u64* pSize);
s32 cellPamfVerify(const void* pAddr, u64 fileSize);

/* Reader lifecycle */
s32 cellPamfReaderInitialize(CellPamfReader* reader, const void* pamfAddr,
                             u64 fileSize, u32 attribute);
s32 cellPamfReaderGetPresentationStartTime(CellPamfReader* reader, CellCodecTimeStamp* pTimeStamp);
s32 cellPamfReaderGetPresentationEndTime(CellPamfReader* reader, CellCodecTimeStamp* pTimeStamp);
u32 cellPamfReaderGetMuxRateBound(CellPamfReader* reader);

/* Stream queries */
s32 cellPamfReaderGetNumberOfStreams(CellPamfReader* reader);
s32 cellPamfReaderGetNumberOfSpecificStreams(CellPamfReader* reader, u8 streamType);
s32 cellPamfReaderSetStreamWithIndex(CellPamfReader* reader, u8 streamIndex);
s32 cellPamfReaderSetStreamWithTypeAndChannel(CellPamfReader* reader, u8 streamType, u8 ch);
s32 cellPamfReaderSetStreamWithTypeAndIndex(CellPamfReader* reader, u8 streamType, u8 streamIndex);
s32 cellPamfReaderSetStreamWithType(CellPamfReader* reader, u8 streamType, u8 streamIndex);
s32 cellPamfReaderGetStreamIndex(CellPamfReader* reader);
s32 cellPamfReaderGetStreamTypeAndChannel(CellPamfReader* reader, u8* pType, u8* pCh);
s32 cellPamfReaderGetEsFilterId(CellPamfReader* reader, CellCodecEsFilterId* pEsFilterId);
s32 cellPamfReaderGetStreamInfo(CellPamfReader* reader, void* info, u32 infoSize);
s32 cellPamfStreamTypeToEsFilterId(u8 streamType, u8 ch, CellCodecEsFilterId* pEsFilterId);

/* Entry point navigation */
u32 cellPamfReaderGetNumberOfEp(CellPamfReader* reader);
s32 cellPamfReaderGetEpIteratorWithIndex(CellPamfReader* reader, u32 epIndex,
                                         CellPamfEpIterator* pIt);
s32 cellPamfReaderGetEpIteratorWithTimeStamp(CellPamfReader* reader,
                                             CellCodecTimeStamp* pTimeStamp,
                                             CellPamfEpIterator* pIt);
s32 cellPamfEpIteratorGetEp(CellPamfEpIterator* pIt, CellPamfEp* pEp);
s32 cellPamfEpIteratorMove(CellPamfEpIterator* pIt, s32 steps, CellPamfEp* pEp);
s32 cellPamfReaderGetEpWithTimeStamp(CellPamfReader* reader, CellCodecTimeStamp* pTimeStamp,
                                     CellPamfEpUnk* pEp, u32 epSize);
s32 cellPamfReaderGetEpWithIndex(CellPamfReader* reader, u32 epIndex,
                                 CellPamfEpUnk* pEp, u32 epSize);
s32 cellPamfReaderGetEp(CellPamfReader* reader, u32 epIndex, CellPamfEp* ep);

/* ---------------------------------------------------------------------------
 * Host-side entry points (not guest imports)
 * -----------------------------------------------------------------------*/

/* Initialise the reader keyed by guest EA `reader_ea` from a HOST copy of a
 * PAMF header (e.g. read from disc by cellSail). The reader takes ownership of
 * `hdr` (malloc'd by the caller) and frees it on pamf_reader_release or on
 * re-initialisation -- also when this call fails. The header is fully
 * verified (CELL_PAMF_ATTRIBUTE_VERIFY_ON, fileSize unknown); `size` is the
 * number of valid bytes at `hdr`, and a header shorter than its header_size
 * field is accepted as a minimum header (no EP tables). After this the
 * cellPamfReader* calls work on `reader_ea` as for a guest-initialised
 * reader. Returns CELL_OK or a CELL_PAMF_ERROR_*. */
s32 pamf_reader_init_host(u32 reader_ea, void* hdr, u32 size);
void pamf_reader_release(u32 reader_ea);   /* frees the slot + owned header */

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_PAMF_H */
