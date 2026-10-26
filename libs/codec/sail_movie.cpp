/*
 * sail_movie -- host playback of a PAMF movie for cellSail (see sail_movie.h).
 *
 * One thread per open movie reads the program stream a 2048-byte pack at a
 * time:
 *   - video (PES 0xE0, MPEG-2) goes to an MPEG-2 decoder: FFmpeg's libavcodec,
 *     loaded at run time (ffmpeg_runtime.h), in a build with
 *     PS3RECOMP_FFMPEG_RUNTIME; on Windows, Media Foundation's when FFmpeg's
 *     cannot be loaded (or the build has none). Decoded 4:2:0 pictures are
 *     converted to the guest's 32-bit layout (BT.709, limited range) and
 *     queued, a few frames deep -- the thread waits while the queue is full;
 *   - LPCM (private stream 1, sub-stream 0x40|ch: one id byte, three header
 *     bytes, then 16-bit big-endian interleaved samples) is converted to float
 *     into a ring of a few seconds; if the title stops pulling audio the
 *     oldest samples are dropped rather than stalling the video.
 * Frame n is due n * fps_den / fps_num seconds after sail_movie_start.
 *
 * Only the decoder differs (video_open, video_feed, video_drain, video_close
 * below pick FFmpeg's or Media Foundation's for each movie); the demuxer, the
 * audio, the queue and the clock are the same everywhere.
 *
 * PAMF header (as cellPamf.c reads it): header size and data size in 2048-
 * byte units at 0x08 / 0x0C, stream count at 0x87, 48-byte stream headers
 * from 0x88: +0 coding type (0x02 M2V, 0x80 LPCM), +16 codec info -- M2V
 * +2 b0-3 frame-rate code, +12/+14 exact width/height; LPCM +2 channels,
 * +3 sampling-frequency code (1 = 48 kHz), +4 b6-7 bit-length code (1 = 16).
 */
#include "sail_movie.h"

#include <cstdlib>

#if defined(_WIN32)
#define SAIL_MOVIE_MF 1
#endif
#if defined(PS3RECOMP_FFMPEG_RUNTIME)
#define SAIL_MOVIE_FFMPEG 1
#endif

#if defined(SAIL_MOVIE_MF) || defined(SAIL_MOVIE_FFMPEG)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef SAIL_MOVIE_MF
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#endif
#ifdef SAIL_MOVIE_FFMPEG
#include "ffmpeg_runtime.h"
#endif

namespace {

const size_t kVideoQueue = 6;       /* decoded frames held ahead (~200 ms at 30 fps) */
const int    kAudioSeconds = 4;     /* LPCM ring */

struct Frame { std::vector<uint8_t> px; int no; };

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* One decoded 4:2:0 picture. Media Foundation's NV12 has U and V
 * interleaved in one plane (uv_step 2, v = u + 1); FFmpeg's YUV420P has
 * them in planes of their own (uv_step 1). */
struct Picture {
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
    size_t y_pitch, uv_pitch;
    int uv_step;
};

#ifdef SAIL_MOVIE_MF
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
#endif

enum class Decoder { none, ffmpeg, mf };

struct VideoDecoder {
    Decoder     in_use = Decoder::none;
#ifdef SAIL_MOVIE_MF
    bool        com = false;            /* COM and Media Foundation started on the thread */
    IMFTransform* mft = nullptr;
    bool        provides = false;
    DWORD       out_cb = 0;
    UINT32      out_w = 0, out_h = 0;
    LONG        out_stride = 0;
#endif
#ifdef SAIL_MOVIE_FFMPEG
    const PS3FFmpeg* ff = nullptr;
    AVCodecContext* ctx = nullptr;
    AVCodecParserContext* parser = nullptr;
    AVPacket*   pkt = nullptr;
    AVFrame*    frame = nullptr;
    bool        warned = false;         /* an unexpected picture layout was reported */
#endif
};

} // namespace

struct SailMovie {
    FILE*       f = nullptr;
    long long   data_off = 0, data_end = 0;
    int         coding = SAIL_MOVIE_ARGB;
    SailMovieInfo info{};
    int         lpcm_sub = -1;          /* private_stream_id of the LPCM track */
    bool        has_video = false;

    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex  mu;
    std::condition_variable cv;
    std::deque<Frame> vq;
    std::vector<std::vector<uint8_t>> pool;   /* recycled frame buffers */
    bool        demux_done = false;           /* file read and decoder drained */
    int         decoded = 0;                  /* frames produced so far */
    int         last_out = -1;                /* last frame number handed out */

    std::vector<float> aring;
    size_t      a_rd = 0, a_cnt = 0;          /* in floats */
    int         a_half = -1;                  /* first byte of a sample split across packets */
    int         src_ch = 0;                   /* channels in the file (info.channels: handed out) */
    float       a_frame[8] = {};              /* one input frame being assembled */
    int         a_fill = 0;
    size_t      tone_pos = 0;                 /* SAIL_AUDIO_TONE phase, in frames */

    std::chrono::steady_clock::time_point t0;
    bool        started = false;

    VideoDecoder dec;                         /* thread-only */
};

namespace {

bool read_header(SailMovie* m)
{
    uint8_t h[0x800];
    if (fread(h, 1, sizeof h, m->f) != sizeof h || memcmp(h, "PAMF", 4) != 0) return false;
    auto be32 = [&](int o) { return (uint32_t)h[o] << 24 | h[o+1] << 16 | h[o+2] << 8 | h[o+3]; };
    auto be16 = [&](int o) { return (uint32_t)h[o] << 8 | h[o+1]; };
    m->data_off = (long long)be32(0x08) * 0x800;
    m->data_end = m->data_off + (long long)be32(0x0C) * 0x800;
    const int n = h[0x87];
    for (int i = 0; i < n && 0x88 + (i + 1) * 48 <= 0x800; i++) {
        const uint8_t* s = h + 0x88 + i * 48;
        const uint8_t* d = s + 16;
        if (s[0] == 0x02 && !m->has_video) {
            static const int rates[9][2] = { {0,0}, {24000,1001}, {24,1}, {25,1}, {30000,1001},
                                             {30,1}, {50,1}, {60000,1001}, {60,1} };
            const int rc = d[2] & 0x0F;
            m->has_video = true;
            m->info.width = (int)be16(16 + 0x88 + i * 48 + 12);
            m->info.height = (int)be16(16 + 0x88 + i * 48 + 14);
            m->info.fps_num = rc >= 1 && rc <= 8 ? rates[rc][0] : 30000;
            m->info.fps_den = rc >= 1 && rc <= 8 ? rates[rc][1] : 1001;
        } else if (s[0] == 0x80 && m->lpcm_sub < 0) {
            const int fs = d[3] & 0x0F, bits = d[4] >> 6;
            if (fs == 1 && bits == 1) {          /* 48 kHz, 16-bit: all PAMF LPCM in practice */
                m->lpcm_sub = s[5];
                m->src_ch = d[2] & 0x0F;
                m->info.channels = m->src_ch;
                m->info.rate = 48000;
            } else {
                fprintf(stderr, "[sail_movie] LPCM fs code %d bits code %d not handled%c", fs, bits, 10);
            }
        }
    }
    return m->data_end > m->data_off;
}

int seek_to(FILE* f, long long off)
{
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* ---- video ------------------------------------------------------------ */

/* 4:2:0 -> 32-bit ARGB/RGBA, BT.709 limited range. */
void convert(const SailMovie* m, const Picture& p, uint8_t* out)
{
    const int w = m->info.width, h = m->info.height;
    for (int r = 0; r < h; r++) {
        const uint8_t* yr = p.y + p.y_pitch * r;
        const uint8_t* ur = p.u + p.uv_pitch * (r >> 1);
        const uint8_t* vr = p.v + p.uv_pitch * (r >> 1);
        uint8_t* o = out + (size_t)r * w * 4;
        for (int c = 0; c < w; c++) {
            const int k = (c >> 1) * p.uv_step;
            const int Y = 298 * (yr[c] - 16);
            const int U = ur[k] - 128, V = vr[k] - 128;
            const uint8_t R = clamp8((Y + 459 * V + 128) >> 8);
            const uint8_t G = clamp8((Y - 55 * U - 136 * V + 128) >> 8);
            const uint8_t B = clamp8((Y + 541 * U + 128) >> 8);
            if (m->coding == SAIL_MOVIE_RGBA) { o[0] = R; o[1] = G; o[2] = B; o[3] = 0xFF; }
            else                              { o[0] = 0xFF; o[1] = R; o[2] = G; o[3] = B; }
            o += 4;
        }
    }
}

/* A frame's buffer, recycled from the pool when there is one. */
std::vector<uint8_t> frame_buffer(SailMovie* m)
{
    std::vector<uint8_t> px;
    {
        std::lock_guard<std::mutex> lk(m->mu);
        if (!m->pool.empty()) { px.swap(m->pool.back()); m->pool.pop_back(); }
    }
    px.resize((size_t)m->info.width * m->info.height * 4);
    return px;
}

/* For a picture that could not be read: opaque black in the guest's layout. */
void fill_black(const SailMovie* m, std::vector<uint8_t>& px)
{
    const size_t a = m->coding == SAIL_MOVIE_RGBA ? 3 : 0;   /* where the alpha byte is */
    memset(px.data(), 0, px.size());
    for (size_t i = a; i < px.size(); i += 4) px[i] = 0xFF;
}

/* Queues a converted frame, waiting while the queue is full. */
void queue_frame(SailMovie* m, std::vector<uint8_t> px)
{
    std::unique_lock<std::mutex> lk(m->mu);
    m->cv.wait(lk, [&] { return m->stop || m->vq.size() < kVideoQueue; });
    if (m->stop) return;
    m->vq.push_back(Frame{ std::move(px), m->decoded++ });
    if (m->decoded == 1 || m->decoded % 300 == 0)
        fprintf(stderr, "[sail_movie] decoded %d frames%c", m->decoded, 10);
}

#ifdef SAIL_MOVIE_MF

/* Media Foundation's decoder is looked for on the movie's thread (it needs
 * COM there); without one the movie plays its sound alone. */
bool video_decoder_present() { return true; }

bool set_output_type(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(d.mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12 && SUCCEEDED(d.mft->SetOutputType(0, t, 0))) {
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &d.out_w, &d.out_h);
            UINT32 st = 0;
            d.out_stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)st : (LONG)d.out_w;
            t->Release();
            MFT_OUTPUT_STREAM_INFO osi{};
            d.mft->GetOutputStreamInfo(0, &osi);
            d.provides = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            d.out_cb = osi.cbSize;
            return true;
        }
        t->Release();
    }
}

bool open_decoder(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    d.com = true;
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_MPEG2 };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &in, nullptr, &acts, &n)) || n == 0) {
        fprintf(stderr, "[sail_movie] no MPEG-2 video decoder on this system%c", 10);
        return false;
    }
    HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&d.mft));
    for (UINT32 i = 0; i < n; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return false;

    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MPEG2);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, (UINT32)m->info.width, (UINT32)m->info.height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, (UINT32)m->info.fps_num, (UINT32)m->info.fps_den);
    hr = d.mft->SetInputType(0, t, 0);
    t->Release();
    if (FAILED(hr) || !set_output_type(m)) {
        fprintf(stderr, "[sail_movie] MPEG-2 decoder refused the stream (0x%08lX)%c", (unsigned long)hr, 10);
        release(d.mft);
        return false;
    }
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

void emit_sample(SailMovie* m, IMFSample* s)
{
    const VideoDecoder& d = m->dec;
    IMFMediaBuffer* b = nullptr;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    std::vector<uint8_t> px = frame_buffer(m);
    bool done = false;
    auto nv12 = [&](const BYTE* y0, LONG pitch) {
        const uint8_t* uv0 = y0 + (size_t)pitch * d.out_h;
        convert(m, Picture{ y0, uv0, uv0 + 1, (size_t)pitch, (size_t)pitch, 2 }, px.data());
        done = true;
    };
    IMF2DBuffer* b2 = nullptr;
    BYTE* scan0 = nullptr; LONG pitch = 0;
    if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch)) && pitch > 0) {
        nv12(scan0, pitch);
        b2->Unlock2D();
    } else {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
            const LONG st = d.out_stride > 0 ? d.out_stride : (LONG)d.out_w;
            if ((size_t)len >= (size_t)st * d.out_h * 3 / 2) nv12(p, st);
            b->Unlock();
        }
    }
    release(b2);
    release(b);
    if (!done) fill_black(m, px);
    queue_frame(m, std::move(px));
}

void pump_output(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    while (!m->stop) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample* mine = nullptr;
        if (!d.provides) {
            IMFMediaBuffer* buf = nullptr;
            MFCreateSample(&mine);
            MFCreateMemoryBuffer(d.out_cb ? d.out_cb : (DWORD)(d.out_w * d.out_h * 2), &buf);
            mine->AddBuffer(buf);
            buf->Release();
            ob.pSample = mine;
        }
        DWORD status = 0;
        const HRESULT hr = d.mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { release(mine); if (!set_output_type(m)) return; continue; }
        if (FAILED(hr)) {
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT)
                fprintf(stderr, "[sail_movie] ProcessOutput 0x%08lX%c", (unsigned long)hr, 10);
            release(mine);
            return;
        }
        if (ob.pSample) emit_sample(m, ob.pSample);
        if (ob.pSample && ob.pSample != mine) ob.pSample->Release();
        release(mine);
    }
}

void feed_video(SailMovie* m, const uint8_t* p, size_t n)
{
    VideoDecoder& d = m->dec;
    if (!d.mft || !n) return;
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    MFCreateSample(&s);
    MFCreateMemoryBuffer((DWORD)n, &b);
    BYTE* dst = nullptr;
    b->Lock(&dst, nullptr, nullptr);
    memcpy(dst, p, n);
    b->Unlock();
    b->SetCurrentLength((DWORD)n);
    s->AddBuffer(b);
    b->Release();
    for (int tries = 0; tries < 2 && !m->stop; tries++) {
        const HRESULT hr = d.mft->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) { pump_output(m); continue; }
        if (FAILED(hr)) fprintf(stderr, "[sail_movie] ProcessInput 0x%08lX%c", (unsigned long)hr, 10);
        break;
    }
    s->Release();
    pump_output(m);
}

void drain_decoder(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    if (!d.mft) return;
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    pump_output(m);
}

void close_decoder(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    release(d.mft);
    if (d.com) {
        MFShutdown();
        CoUninitialize();
        d.com = false;
    }
}

#endif  /* SAIL_MOVIE_MF */

#ifdef SAIL_MOVIE_FFMPEG

/* FFmpeg's decoder, loaded at run time. Off Windows it is the only one:
 * without it (not installed, another major version, PS3RECOMP_FFMPEG=0) a
 * movie with video is not opened, and ends at once as it did before FFmpeg
 * was used here. */
#ifndef SAIL_MOVIE_MF
bool ff_video_decoder_present()
{
    const PS3FFmpeg* ff = ps3_ffmpeg_get();
    if (ff && ff->avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO)) return true;
    fprintf(stderr, "[sail_movie] no MPEG-2 video decoder (FFmpeg's libavcodec %s)%c",
            ff ? "has none" : "was not loaded", 10);
    return false;
}
#endif

void ff_close_decoder(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    if (!d.ff) return;
    if (d.parser) d.ff->av_parser_close(d.parser);
    if (d.ctx) d.ff->avcodec_free_context(&d.ctx);
    if (d.pkt) d.ff->av_packet_free(&d.pkt);
    if (d.frame) d.ff->av_frame_free(&d.frame);
    d.ff = nullptr;
    d.parser = nullptr;
    d.warned = false;
}

bool ff_open_decoder(SailMovie* m)
{
    VideoDecoder& d = m->dec;
    d.ff = ps3_ffmpeg_get();
    const AVCodec* c = d.ff ? d.ff->avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO) : nullptr;
    if (!c) return false;
    d.ctx = d.ff->avcodec_alloc_context3(c);
    d.parser = d.ff->av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    d.pkt = d.ff->av_packet_alloc();
    d.frame = d.ff->av_frame_alloc();
    if (!d.ctx || !d.parser || !d.pkt || !d.frame || d.ff->avcodec_open2(d.ctx, c, nullptr) < 0) {
        fprintf(stderr, "[sail_movie] FFmpeg's MPEG-2 decoder did not open%c", 10);
        ff_close_decoder(m);
        return false;
    }
    return true;
}

void emit_frame(SailMovie* m, const AVFrame* f)
{
    VideoDecoder& d = m->dec;
    std::vector<uint8_t> px = frame_buffer(m);
    if (f->format == AV_PIX_FMT_YUV420P && f->width >= m->info.width && f->height >= m->info.height) {
        convert(m, Picture{ f->data[0], f->data[1], f->data[2], (size_t)f->linesize[0], (size_t)f->linesize[1], 1 },
                px.data());
    } else {
        if (!d.warned)
            fprintf(stderr, "[sail_movie] picture %dx%d in format %d, the stream says %dx%d 4:2:0: shown black%c",
                    f->width, f->height, f->format, m->info.width, m->info.height, 10);
        d.warned = true;
        fill_black(m, px);
    }
    queue_frame(m, std::move(px));
}

/* One whole picture's bytes to the decoder (nullptr: the end, drain it),
 * then every picture it has ready. */
void decode(SailMovie* m, const uint8_t* data, int size)
{
    VideoDecoder& d = m->dec;
    if (data) {
        d.pkt->data = const_cast<uint8_t*>(data);
        d.pkt->size = size;
    }
    if (d.ff->avcodec_send_packet(d.ctx, data ? d.pkt : nullptr) < 0) return;
    while (!m->stop && d.ff->avcodec_receive_frame(d.ctx, d.frame) >= 0) {
        emit_frame(m, d.frame);
        d.ff->av_frame_unref(d.frame);
    }
}

/* A PES payload ends anywhere in a picture, so the parser gathers the
 * elementary stream into whole pictures first. n == 0: the stream has
 * ended, and the parser hands over the picture it is still holding. */
void parse(SailMovie* m, const uint8_t* p, size_t n)
{
    VideoDecoder& d = m->dec;
    const bool end = n == 0;
    while ((n > 0 || end) && !m->stop) {
        uint8_t* out = nullptr;
        int out_n = 0;
        const int used = d.ff->av_parser_parse2(d.parser, d.ctx, &out, &out_n, p, (int)n,
                                                AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) return;
        p += used;
        n -= (size_t)used;
        if (out_n > 0) decode(m, out, out_n);
        else if (end) break;
    }
}

void ff_feed_video(SailMovie* m, const uint8_t* p, size_t n)
{
    if (m->dec.ctx && n) parse(m, p, n);
}

void ff_drain_decoder(SailMovie* m)
{
    if (!m->dec.ctx) return;
    parse(m, nullptr, 0);
    decode(m, nullptr, 0);
}

#endif  /* SAIL_MOVIE_FFMPEG */

/* ---- the decoder in use ------------------------------------------------ */

/* Whether a movie with video can open at all. On Windows, Media Foundation's
 * answer (yes: its decoder is looked for on the movie's own thread). */
bool video_present()
{
#if defined(SAIL_MOVIE_MF)
    return video_decoder_present();
#else
    return ff_video_decoder_present();
#endif
}

/* FFmpeg's decoder where the build has it and it loads (on Windows it ships
 * beside the program), else Media Foundation's. FFmpeg's plays the same on
 * every PC; Windows' own MPEG-2 decoder needs an extension from the
 * Microsoft Store. PS3RECOMP_FFMPEG=0 skips FFmpeg. */
bool video_open(SailMovie* m)
{
    VideoDecoder& d = m->dec;
#ifdef SAIL_MOVIE_FFMPEG
    if (ff_open_decoder(m)) {
        d.in_use = Decoder::ffmpeg;
        return true;
    }
#endif
#ifdef SAIL_MOVIE_MF
#ifdef SAIL_MOVIE_FFMPEG
    { static bool said = false;
      if (!said) { said = true;
          fprintf(stderr, "[sail_movie] no FFmpeg decoder: Windows' own (Media Foundation) instead%c", 10); } }
#endif
    if (open_decoder(m)) {
        d.in_use = Decoder::mf;
        return true;
    }
#endif
    return false;
}

void video_feed(SailMovie* m, const uint8_t* p, size_t n)
{
#ifdef SAIL_MOVIE_FFMPEG
    if (m->dec.in_use == Decoder::ffmpeg) ff_feed_video(m, p, n);
#endif
#ifdef SAIL_MOVIE_MF
    if (m->dec.in_use == Decoder::mf) feed_video(m, p, n);
#endif
}

void video_drain(SailMovie* m)
{
#ifdef SAIL_MOVIE_FFMPEG
    if (m->dec.in_use == Decoder::ffmpeg) ff_drain_decoder(m);
#endif
#ifdef SAIL_MOVIE_MF
    if (m->dec.in_use == Decoder::mf) drain_decoder(m);
#endif
}

/* Both, whichever was used: a decoder that failed to open may have left
 * something started (Media Foundation's COM, FFmpeg's context). */
void video_close(SailMovie* m)
{
#ifdef SAIL_MOVIE_FFMPEG
    ff_close_decoder(m);
#endif
#ifdef SAIL_MOVIE_MF
    close_decoder(m);
#endif
    m->dec.in_use = Decoder::none;
}

/* ---- audio ------------------------------------------------------------ */

/* One input frame -> the frame handed to the title. PAMF LPCM 5.1 is L R C
 * Ls Rs LFE. Demon's Souls reads only stereo correctly (a 5.1 advert came out
 * as a mono mash at -45 dB), so by default a track of more than two channels
 * is folded to stereo -- ITU-R BS.775 weights, scaled so it cannot clip, LFE
 * dropped. SAIL_AUDIO_DOWNMIX=0 hands 5.1 over in the order cellSail's layout
 * 8 promises (L C R Ls Rs LFE), for a title that mixes it itself. */
void audio_frame(SailMovie* m, const float* f, float* out)
{
    const int ch = m->src_ch;
    if (m->info.channels == ch) {
        for (int c = 0; c < ch; c++) out[c] = f[c];
        if (ch == 6) { out[1] = f[2]; out[2] = f[1]; }
        return;
    }
    const float k = 0.70710678f;
    const float c = ch > 2 ? f[2] : 0.0f;
    const float ls = ch > 3 ? f[3] : 0.0f, rs = ch > 4 ? f[4] : ls;
    const float g = 1.0f / (1.0f + k + (ch > 3 ? k : 0.0f));
    out[0] = (f[0] + k * c + k * ls) * g;
    out[1] = (f[1] + k * c + k * rs) * g;
}

void feed_lpcm(SailMovie* m, const uint8_t* p, size_t n)
{
    const int ch = m->src_ch, out_ch = m->info.channels;
    if (ch <= 0 || ch > 8) return;
    std::lock_guard<std::mutex> lk(m->mu);
    const size_t cap = m->aring.size();
    /* The PCM runs on across PES packets: a payload can end in the middle
     * of a sample (odd length), and its other byte opens the next one. Each
     * packet was decoded on its own, so after an odd one every sample was read
     * a byte off -- full-scale white noise until the next odd packet put it
     * back, which is what the movies played for seconds at a time. */
    for (size_t i = 0; i < n; i++) {
        if (m->a_half < 0) { m->a_half = p[i]; continue; }
        m->a_frame[m->a_fill++] = (int16_t)(m->a_half << 8 | p[i]) * (1.0f / 32768.0f);
        m->a_half = -1;
        if (m->a_fill < ch) continue;
        m->a_fill = 0;
        /* Full (the title stopped pulling sound): drop the oldest whole
         * frame -- a single sample would swap the channels from then on. */
        if (m->a_cnt + (size_t)out_ch > cap) {
            m->a_rd = (m->a_rd + (size_t)out_ch) % cap;
            m->a_cnt -= (size_t)out_ch;
        }
        float out[8];
        audio_frame(m, m->a_frame, out);
        for (int c = 0; c < out_ch; c++)
            m->aring[(m->a_rd + m->a_cnt++) % cap] = out[c];
    }
}

/* ---- demux thread ----------------------------------------------------- */

void demux(SailMovie* m)
{
    if (m->has_video && !video_open(m)) m->has_video = false;

    seek_to(m->f, m->data_off);
    uint8_t pack[0x800];
    long long pos = m->data_off;
    while (!m->stop && pos < m->data_end && fread(pack, 1, sizeof pack, m->f) == sizeof pack) {
        pos += sizeof pack;
        if (memcmp(pack, "\x00\x00\x01\xBA", 4) != 0) continue;
        size_t q = 14 + (pack[13] & 7);
        while (q + 6 <= sizeof pack && !m->stop) {
            if (pack[q] || pack[q+1] || pack[q+2] != 1) break;
            const uint8_t sid = pack[q+3];
            const size_t len = (size_t)pack[q+4] << 8 | pack[q+5];
            if (q + 6 + len > sizeof pack) break;
            const uint8_t* body = pack + q + 6;
            q += 6 + len;
            if (sid != 0xE0 && sid != 0xBD) continue;
            if (len < 3) continue;
            const size_t hl = 3 + body[2];
            if (hl > len) continue;
            const uint8_t* pl = body + hl;
            const size_t pn = len - hl;
            if (sid == 0xE0 && m->has_video) video_feed(m, pl, pn);
            else if (sid == 0xBD && pn > 4 && (int)pl[0] == m->lpcm_sub) feed_lpcm(m, pl + 4, pn - 4);
        }
    }
    if (m->has_video && !m->stop) video_drain(m);
    video_close(m);
    {
        std::lock_guard<std::mutex> lk(m->mu);
        m->demux_done = true;
    }
    fprintf(stderr, "[sail_movie] stream read: %d frames decoded%c", m->decoded, 10);
}

} // namespace

extern "C" SailMovie* sail_movie_open(const char* host_path, int coding)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return nullptr;
    SailMovie* m = new SailMovie();
    m->f = f;
    m->coding = coding;
    if (!read_header(m) || (!m->has_video && m->lpcm_sub < 0) || (m->has_video && !video_present())) {
        fclose(f);
        delete m;
        return nullptr;
    }
    if (m->src_ch > 2 && sail_movie_downmix()) m->info.channels = 2;
    if (m->info.channels > 0) m->aring.resize((size_t)m->info.rate * m->info.channels * kAudioSeconds);
    m->th = std::thread(demux, m);
    fprintf(stderr, "[sail_movie] %s: video %dx%d @ %d/%d, LPCM %d ch (handed out as %d)%c", host_path,
            m->info.width, m->info.height, m->info.fps_num, m->info.fps_den, m->src_ch, m->info.channels, 10);
    return m;
}

extern "C" void sail_movie_info(const SailMovie* m, SailMovieInfo* out)
{
    *out = m->info;
    if (!m->has_video) out->width = out->height = 0;
}

extern "C" void sail_movie_start(SailMovie* m)
{
    std::lock_guard<std::mutex> lk(m->mu);
    m->t0 = std::chrono::steady_clock::now();
    m->started = true;
}

extern "C" int sail_movie_video(SailMovie* m, uint8_t* dst, int pitch, int* frame_no, int* eos)
{
    std::unique_lock<std::mutex> lk(m->mu);
    *eos = 0;
    if (!m->started) return 0;
    if (!m->has_video) { *eos = m->demux_done && m->a_cnt == 0; return 0; }
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - m->t0).count();
    /* One frame per call, in order, half a frame early; frames are dropped
     * only to catch up more than two behind. Taking the newest due frame
     * instead made a title that fetches at the movie's own rate (Demon's
     * Souls: once per 29.97 Hz flip) land on either side of a frame boundary
     * by a millisecond's jitter: two frames in one call, none in the next,
     * a skip and a repeat several times a second. */
    const int due = (int)(t * m->info.fps_num / m->info.fps_den + 0.5);
    bool got = false;
    Frame cur;
    while (!m->vq.empty() && m->vq.front().no <= due && (!got || cur.no < due - 2)) {
        if (got) m->pool.push_back(std::move(cur.px));
        cur = std::move(m->vq.front());
        m->vq.pop_front();
        got = true;
    }
    if (got) m->cv.notify_all();
    if (!got) {
        *eos = m->demux_done && m->vq.empty();
        return 0;
    }
    lk.unlock();
    const size_t row = (size_t)m->info.width * 4;
    for (int r = 0; r < m->info.height; r++)
        memcpy(dst + (size_t)r * pitch, cur.px.data() + r * row, row);
    lk.lock();
    m->pool.push_back(std::move(cur.px));
    m->last_out = cur.no;
    *frame_no = cur.no;
    return 1;
}

extern "C" int sail_movie_audio(SailMovie* m, float* dst, int frames, int big_endian, int* eos)
{
    std::lock_guard<std::mutex> lk(m->mu);
    *eos = 0;
    const size_t want = (size_t)frames * m->info.channels;
    if (!m->started || !want) return 0;
    if (m->a_cnt < want) { *eos = m->demux_done; return 0; }
    const size_t cap = m->aring.size();
    /* SAIL_AUDIO_TONE=<peak> (diagnostic, e.g. 0.2): replace the track with a
     * sine per channel, 250 Hz x (channel + 1), to see in an AUDIO_WAV
     * capture where each channel lands in the title's mix and at what gain. */
    static const double tone = [] { const char* e = getenv("SAIL_AUDIO_TONE"); return e ? atof(e) : 0.0; }();
    for (size_t i = 0; i < want; i++) {
        float v = m->aring[(m->a_rd + i) % cap];
        if (tone > 0) {
            const int c = (int)(i % (size_t)m->info.channels);
            const double t = (double)(m->tone_pos + i / (size_t)m->info.channels) / m->info.rate;
            v = (float)(tone * sin(6.283185307179586 * 250.0 * (c + 1) * t));
        }
        if (big_endian) {
            uint32_t u; memcpy(&u, &v, 4);
            u = __builtin_bswap32(u);
            memcpy(&v, &u, 4);
        }
        dst[i] = v;
    }
    m->a_rd = (m->a_rd + want) % cap;
    m->a_cnt -= want;
    m->tone_pos += (size_t)frames;
    return frames;
}

extern "C" void sail_movie_close(SailMovie* m)
{
    if (!m) return;
    m->stop = true;
    m->cv.notify_all();
    if (m->th.joinable()) m->th.join();
    fclose(m->f);
    delete m;
}

#else  /* no decoder in this build: cellSail keeps ending movies at once */

extern "C" SailMovie* sail_movie_open(const char*, int) { return nullptr; }
extern "C" void sail_movie_info(const SailMovie*, SailMovieInfo* out) { *out = SailMovieInfo{}; }
extern "C" void sail_movie_start(SailMovie*) {}
extern "C" int sail_movie_video(SailMovie*, uint8_t*, int, int*, int* eos) { *eos = 1; return 0; }
extern "C" int sail_movie_audio(SailMovie*, float*, int, int, int* eos) { *eos = 1; return 0; }
extern "C" void sail_movie_close(SailMovie*) {}

#endif

extern "C" int sail_movie_downmix(void)
{
    static const int on = [] { const char* e = getenv("SAIL_AUDIO_DOWNMIX"); return !e || atoi(e) != 0; }();
    return on;
}
