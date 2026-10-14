/*
 * sail_movie -- host playback of a PAMF movie for cellSail (see sail_movie.h).
 *
 * One thread per open movie reads the program stream a 2048-byte pack at a
 * time:
 *   - video (PES 0xE0, MPEG-2) goes to the Media Foundation MPEG-2 decoder
 *     (msmpeg2vdec, part of Windows); decoded NV12 pictures are converted to
 *     the guest's 32-bit layout (BT.709, limited range) and queued, a few
 *     frames deep -- the thread waits while the queue is full;
 *   - LPCM (private stream 1, sub-stream 0x40|ch: one id byte, three header
 *     bytes, then 16-bit big-endian interleaved samples) is converted to float
 *     into a ring of a few seconds; if the title stops pulling audio the
 *     oldest samples are dropped rather than stalling the video.
 * Frame n is due n * fps_den / fps_num seconds after sail_movie_start.
 *
 * PAMF header (as cellPamf.c reads it): header size and data size in 2048-
 * byte units at 0x08 / 0x0C, stream count at 0x87, 48-byte stream headers
 * from 0x88: +0 coding type (0x02 M2V, 0x80 LPCM), +16 codec info -- M2V
 * +2 b0-3 frame-rate code, +12/+14 exact width/height; LPCM +2 channels,
 * +3 sampling-frequency code (1 = 48 kHz), +4 b6-7 bit-length code (1 = 16).
 */
#include "sail_movie.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

const size_t kVideoQueue = 6;       /* decoded frames held ahead (~200 ms at 30 fps) */
const int    kAudioSeconds = 4;     /* LPCM ring */

struct Frame { std::vector<uint8_t> px; int no; };

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

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

    LARGE_INTEGER qf{}, t0{};
    bool        started = false;

    /* decoder state (thread-only) */
    IMFTransform* mft = nullptr;
    bool        provides = false;
    DWORD       out_cb = 0;
    UINT32      out_w = 0, out_h = 0;
    LONG        out_stride = 0;
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
                m->info.channels = d[2] & 0x0F;
                m->info.rate = 48000;
            } else {
                fprintf(stderr, "[sail_movie] LPCM fs code %d bits code %d not handled%c", fs, bits, 10);
            }
        }
    }
    return m->data_end > m->data_off;
}

/* ---- video ------------------------------------------------------------ */

bool set_output_type(SailMovie* m)
{
    for (DWORD i = 0;; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(m->mft->GetOutputAvailableType(0, i, &t))) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12 && SUCCEEDED(m->mft->SetOutputType(0, t, 0))) {
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &m->out_w, &m->out_h);
            UINT32 st = 0;
            m->out_stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &st)) ? (LONG)st : (LONG)m->out_w;
            t->Release();
            MFT_OUTPUT_STREAM_INFO osi{};
            m->mft->GetOutputStreamInfo(0, &osi);
            m->provides = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                          MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            m->out_cb = osi.cbSize;
            return true;
        }
        t->Release();
    }
}

bool open_decoder(SailMovie* m)
{
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_MPEG2 };
    IMFActivate** acts = nullptr;
    UINT32 n = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                         MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &in, nullptr, &acts, &n)) || n == 0) {
        fprintf(stderr, "[sail_movie] no MPEG-2 video decoder on this system%c", 10);
        return false;
    }
    HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&m->mft));
    for (UINT32 i = 0; i < n; i++) acts[i]->Release();
    CoTaskMemFree(acts);
    if (FAILED(hr)) return false;

    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MPEG2);
    MFSetAttributeSize(t, MF_MT_FRAME_SIZE, (UINT32)m->info.width, (UINT32)m->info.height);
    MFSetAttributeRatio(t, MF_MT_FRAME_RATE, (UINT32)m->info.fps_num, (UINT32)m->info.fps_den);
    hr = m->mft->SetInputType(0, t, 0);
    t->Release();
    if (FAILED(hr) || !set_output_type(m)) {
        fprintf(stderr, "[sail_movie] MPEG-2 decoder refused the stream (0x%08lX)%c", (unsigned long)hr, 10);
        release(m->mft);
        return false;
    }
    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

/* NV12 -> 32-bit ARGB/RGBA, BT.709 limited range. */
void convert(const SailMovie* m, const uint8_t* y0, LONG pitch, UINT32 plane_h, uint8_t* out)
{
    const int w = m->info.width, h = m->info.height;
    const uint8_t* uv0 = y0 + (size_t)pitch * plane_h;
    for (int r = 0; r < h; r++) {
        const uint8_t* yr = y0 + (size_t)pitch * r;
        const uint8_t* uvr = uv0 + (size_t)pitch * (r >> 1);
        uint8_t* o = out + (size_t)r * w * 4;
        for (int c = 0; c < w; c++) {
            const int Y = 298 * (yr[c] - 16);
            const int U = uvr[c & ~1] - 128, V = uvr[(c & ~1) + 1] - 128;
            const uint8_t R = clamp8((Y + 459 * V + 128) >> 8);
            const uint8_t G = clamp8((Y - 55 * U - 136 * V + 128) >> 8);
            const uint8_t B = clamp8((Y + 541 * U + 128) >> 8);
            if (m->coding == SAIL_MOVIE_RGBA) { o[0] = R; o[1] = G; o[2] = B; o[3] = 0xFF; }
            else                              { o[0] = 0xFF; o[1] = R; o[2] = G; o[3] = B; }
            o += 4;
        }
    }
}

void emit_frame(SailMovie* m, IMFSample* s)
{
    IMFMediaBuffer* b = nullptr;
    if (FAILED(s->ConvertToContiguousBuffer(&b))) return;
    std::vector<uint8_t> px;
    {
        std::lock_guard<std::mutex> lk(m->mu);
        if (!m->pool.empty()) { px.swap(m->pool.back()); m->pool.pop_back(); }
    }
    px.resize((size_t)m->info.width * m->info.height * 4);
    IMF2DBuffer* b2 = nullptr;
    BYTE* scan0 = nullptr; LONG pitch = 0;
    if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch)) && pitch > 0) {
        convert(m, scan0, pitch, m->out_h, px.data());
        b2->Unlock2D();
    } else {
        BYTE* p = nullptr; DWORD len = 0;
        if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
            const LONG st = m->out_stride > 0 ? m->out_stride : (LONG)m->out_w;
            if ((size_t)len >= (size_t)st * m->out_h * 3 / 2) convert(m, p, st, m->out_h, px.data());
            b->Unlock();
        }
    }
    release(b2);
    release(b);

    std::unique_lock<std::mutex> lk(m->mu);
    m->cv.wait(lk, [&] { return m->stop || m->vq.size() < kVideoQueue; });
    if (m->stop) return;
    m->vq.push_back(Frame{ std::move(px), m->decoded++ });
    if (m->decoded == 1 || m->decoded % 300 == 0)
        fprintf(stderr, "[sail_movie] decoded %d frames%c", m->decoded, 10);
}

void pump_output(SailMovie* m)
{
    while (!m->stop) {
        MFT_OUTPUT_DATA_BUFFER ob{};
        IMFSample* mine = nullptr;
        if (!m->provides) {
            IMFMediaBuffer* buf = nullptr;
            MFCreateSample(&mine);
            MFCreateMemoryBuffer(m->out_cb ? m->out_cb : (DWORD)(m->out_w * m->out_h * 2), &buf);
            mine->AddBuffer(buf);
            buf->Release();
            ob.pSample = mine;
        }
        DWORD status = 0;
        const HRESULT hr = m->mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { release(mine); if (!set_output_type(m)) return; continue; }
        if (FAILED(hr)) {
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT)
                fprintf(stderr, "[sail_movie] ProcessOutput 0x%08lX%c", (unsigned long)hr, 10);
            release(mine);
            return;
        }
        if (ob.pSample) emit_frame(m, ob.pSample);
        if (ob.pSample && ob.pSample != mine) ob.pSample->Release();
        release(mine);
    }
}

void feed_video(SailMovie* m, const uint8_t* p, size_t n)
{
    if (!m->mft || !n) return;
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    MFCreateSample(&s);
    MFCreateMemoryBuffer((DWORD)n, &b);
    BYTE* d = nullptr;
    b->Lock(&d, nullptr, nullptr);
    memcpy(d, p, n);
    b->Unlock();
    b->SetCurrentLength((DWORD)n);
    s->AddBuffer(b);
    b->Release();
    for (int tries = 0; tries < 2 && !m->stop; tries++) {
        const HRESULT hr = m->mft->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) { pump_output(m); continue; }
        if (FAILED(hr)) fprintf(stderr, "[sail_movie] ProcessInput 0x%08lX%c", (unsigned long)hr, 10);
        break;
    }
    s->Release();
    pump_output(m);
}

/* ---- audio ------------------------------------------------------------ */

void feed_lpcm(SailMovie* m, const uint8_t* p, size_t n)
{
    const int ch = m->info.channels;
    if (ch <= 0) return;
    const size_t samples = n / 2;
    std::lock_guard<std::mutex> lk(m->mu);
    const size_t cap = m->aring.size();
    for (size_t i = 0; i < samples; i++) {
        const int16_t v = (int16_t)(p[2*i] << 8 | p[2*i+1]);
        if (m->a_cnt == cap) { m->a_rd = (m->a_rd + 1) % cap; m->a_cnt--; }   /* drop oldest */
        m->aring[(m->a_rd + m->a_cnt) % cap] = v * (1.0f / 32768.0f);
        m->a_cnt++;
    }
}

/* ---- demux thread ----------------------------------------------------- */

void demux(SailMovie* m)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (m->has_video && !open_decoder(m)) m->has_video = false;

    _fseeki64(m->f, m->data_off, SEEK_SET);
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
            if (sid == 0xE0 && m->has_video) feed_video(m, pl, pn);
            else if (sid == 0xBD && pn > 4 && (int)pl[0] == m->lpcm_sub) feed_lpcm(m, pl + 4, pn - 4);
        }
    }
    if (m->mft && !m->stop) {
        m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        pump_output(m);
    }
    release(m->mft);
    {
        std::lock_guard<std::mutex> lk(m->mu);
        m->demux_done = true;
    }
    fprintf(stderr, "[sail_movie] stream read: %d frames decoded%c", m->decoded, 10);
    MFShutdown();
    CoUninitialize();
}

} // namespace

extern "C" SailMovie* sail_movie_open(const char* host_path, int coding)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return nullptr;
    SailMovie* m = new SailMovie();
    m->f = f;
    m->coding = coding;
    if (!read_header(m) || (!m->has_video && m->lpcm_sub < 0)) {
        fclose(f);
        delete m;
        return nullptr;
    }
    if (m->info.channels > 0) m->aring.resize((size_t)m->info.rate * m->info.channels * kAudioSeconds);
    QueryPerformanceFrequency(&m->qf);
    m->th = std::thread(demux, m);
    fprintf(stderr, "[sail_movie] %s: video %dx%d @ %d/%d, LPCM %d ch%c", host_path,
            m->info.width, m->info.height, m->info.fps_num, m->info.fps_den, m->info.channels, 10);
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
    QueryPerformanceCounter(&m->t0);
    m->started = true;
}

extern "C" int sail_movie_video(SailMovie* m, uint8_t* dst, int pitch, int* frame_no, int* eos)
{
    std::unique_lock<std::mutex> lk(m->mu);
    *eos = 0;
    if (!m->started) return 0;
    if (!m->has_video) { *eos = m->demux_done && m->a_cnt == 0; return 0; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double t = (double)(now.QuadPart - m->t0.QuadPart) / (double)m->qf.QuadPart;
    const int due = (int)(t * m->info.fps_num / m->info.fps_den);
    bool got = false;
    Frame cur;
    while (!m->vq.empty() && m->vq.front().no <= due) {
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
    for (size_t i = 0; i < want; i++) {
        float v = m->aring[(m->a_rd + i) % cap];
        if (big_endian) {
            uint32_t u; memcpy(&u, &v, 4);
            u = _byteswap_ulong(u);
            memcpy(&v, &u, 4);
        }
        dst[i] = v;
    }
    m->a_rd = (m->a_rd + want) % cap;
    m->a_cnt -= want;
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

#else  /* no decoder on this host: cellSail keeps ending movies at once */

extern "C" SailMovie* sail_movie_open(const char*, int) { return nullptr; }
extern "C" void sail_movie_info(const SailMovie*, SailMovieInfo* out) { *out = SailMovieInfo{}; }
extern "C" void sail_movie_start(SailMovie*) {}
extern "C" int sail_movie_video(SailMovie*, uint8_t*, int, int*, int* eos) { *eos = 1; return 0; }
extern "C" int sail_movie_audio(SailMovie*, float*, int, int, int* eos) { *eos = 1; return 0; }
extern "C" void sail_movie_close(SailMovie*) {}

#endif
