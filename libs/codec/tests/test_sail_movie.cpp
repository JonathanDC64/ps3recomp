/*
 * sail_movie (cellSail's movie player) off Windows, end to end: the MPEG-2
 * video through FFmpeg loaded at run time, as cellSail plays a movie.
 *
 * The test makes its own movie. It encodes pictures of known flat colours
 * with libavcodec (two B-frames between references, so the decoder hands
 * pictures out in another order than they arrive) and writes a PAMF file
 * around them: the header, then 2048-byte packs whose video PES payloads are
 * cut at sizes that split pictures, and a stereo LPCM track whose payloads
 * have odd lengths, so samples straddle packets. Then it plays the file
 * through sail_movie and checks
 *   - the stream info read from the header;
 *   - every frame handed out once, in order, with its own colour, in both
 *     the RGBA and the ARGB layout;
 *   - the LPCM samples, across the odd splits;
 *   - the end of the stream.
 * A movie with sound and no picture plays its sound in full: 6 s of it, more
 * than the reader's 4 s ring, taken as fast as the test can.
 *
 *   test_sail_movie play <dir>        play a movie written to <dir>
 *   test_sail_movie nodecoder <dir>   with PS3RECOMP_FFMPEG=0: a movie with
 *                                     video does not open (it ends at once)
 *   test_sail_movie sound <dir>       a movie with only an LPCM track
 */
#include "sail_movie.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

const int kW = 64, kH = 48;          /* four by three macroblocks */
const int kFrames = 10;
const int kAudioFrames = 18 * 256;   /* whole 256-frame pulls, as Demon's Souls' mixer asks */
const int kSoundOnlyFrames = 1125 * 256;   /* 6 s at 48 kHz */
const int kLpcmSub = 0x40;

/* Limited-range Y'CbCr: red, green, blue, yellow, cyan, magenta, white, grey. */
const int kYuv[8][3] = { {81, 90, 240}, {145, 54, 34}, {41, 240, 110}, {210, 16, 146},
                         {170, 166, 16}, {106, 202, 222}, {235, 128, 128}, {126, 128, 128} };

int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                       printf(__VA_ARGS__); printf("\n"); } } while (0)

/* BT.709 limited range, in floating point (sail_movie works in integers). */
void expected_rgb(int frame, int rgb[3])
{
    const double y = 1.164383 * (kYuv[frame % 8][0] - 16);
    const double u = kYuv[frame % 8][1] - 128, v = kYuv[frame % 8][2] - 128;
    const double c[3] = { y + 1.792741 * v, y - 0.213249 * u - 0.532909 * v, y + 2.112402 * u };
    for (int i = 0; i < 3; i++) rgb[i] = (int)std::lround(c[i] < 0 ? 0 : c[i] > 255 ? 255 : c[i]);
}

int16_t sample(int k) { return (int16_t)(((k * 37) & 0x7FFF) - 0x4000); }

std::vector<uint8_t> encode()
{
    std::vector<uint8_t> es;
    const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!c) { printf("FAIL: libavcodec has no MPEG-2 encoder\n"); return es; }
    AVCodecContext* ctx = avcodec_alloc_context3(c);
    ctx->width = kW;
    ctx->height = kH;
    ctx->time_base = AVRational{ 1001, 30000 };
    ctx->framerate = AVRational{ 30000, 1001 };
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->gop_size = 6;
    ctx->max_b_frames = 2;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = FF_QP2LAMBDA * 2;
    if (avcodec_open2(ctx, c, nullptr) < 0) { printf("FAIL: the MPEG-2 encoder did not open\n"); return es; }
    AVFrame* f = av_frame_alloc();
    f->format = ctx->pix_fmt;
    f->width = kW;
    f->height = kH;
    av_frame_get_buffer(f, 0);
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(ctx, pkt) >= 0) {
            es.insert(es.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    };
    for (int i = 0; i < kFrames; i++) {
        av_frame_make_writable(f);
        for (int p = 0; p < 3; p++) {
            const int h = p ? kH / 2 : kH, w = p ? kW / 2 : kW;
            for (int r = 0; r < h; r++) memset(f->data[p] + (size_t)r * f->linesize[p], kYuv[i % 8][p], (size_t)w);
        }
        f->pts = i;
        avcodec_send_frame(ctx, f);
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&ctx);
    return es;
}

void be16(uint8_t* p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void be32(uint8_t* p, unsigned v) { be16(p, v >> 16); be16(p + 2, v & 0xFFFF); }

/* A pack holding one PES packet: stream id, then the payload. */
std::vector<uint8_t> pack(uint8_t sid, const uint8_t* payload, size_t n)
{
    std::vector<uint8_t> p(0x800, 0);
    p[0] = 0; p[1] = 0; p[2] = 1; p[3] = 0xBA;
    p[4] = 0x44;                 /* an MPEG-2 pack header; the SCR is not read */
    p[13] = 0xF8;                /* no stuffing */
    uint8_t* e = p.data() + 14;
    e[0] = 0; e[1] = 0; e[2] = 1; e[3] = sid;
    be16(e + 4, (unsigned)(3 + n));
    e[6] = 0x81; e[7] = 0; e[8] = 0;   /* no PTS, no header data */
    memcpy(e + 9, payload, n);
    return p;
}

/* audio_frames of stereo LPCM, with the video track or without one. */
bool write_movie(const std::string& path, bool video = true, int audio_frames = kAudioFrames)
{
    const std::vector<uint8_t> es = video ? encode() : std::vector<uint8_t>();
    if (video && es.empty()) return false;
    std::vector<std::vector<uint8_t>> packs;

    /* Video, cut at sizes that have nothing to do with where pictures end
     * (flat pictures are a few hundred bytes each, so most of them span
     * several packets). */
    static const size_t cuts[] = { 97, 300, 64, 211, 33, 150 };
    size_t at = 0;
    for (int i = 0; at < es.size(); i++) {
        const size_t n = std::min(cuts[i % 6], es.size() - at);
        packs.push_back(pack(0xE0, es.data() + at, n));
        at += n;
    }

    /* LPCM: big-endian 16-bit stereo behind the 4-byte sub-stream header,
     * in payloads of odd and even lengths, a pack after every video pack. */
    std::vector<uint8_t> pcm;
    for (int k = 0; k < audio_frames * 2; k++) {
        const uint16_t s = (uint16_t)sample(k);
        pcm.push_back((uint8_t)(s >> 8));
        pcm.push_back((uint8_t)s);
    }
    static const size_t acuts[] = { 1001, 999, 1500, 1333 };
    std::vector<std::vector<uint8_t>> apacks;
    at = 0;
    for (int i = 0; at < pcm.size(); i++) {
        const size_t n = std::min(acuts[i % 4], pcm.size() - at);
        std::vector<uint8_t> pl = { (uint8_t)kLpcmSub, 0, 0, 0 };
        pl.insert(pl.end(), pcm.begin() + (long)at, pcm.begin() + (long)(at + n));
        apacks.push_back(pack(0xBD, pl.data(), pl.size()));
        at += n;
    }
    std::vector<std::vector<uint8_t>> all;
    for (size_t i = 0; i < std::max(packs.size(), apacks.size()); i++) {
        if (i < packs.size()) all.push_back(packs[i]);
        if (i < apacks.size()) all.push_back(apacks[i]);
    }

    uint8_t h[0x800] = {};
    memcpy(h, "PAMF", 4);
    be32(h + 0x08, 1);                        /* header: one 2048-byte unit */
    be32(h + 0x0C, (unsigned)all.size());     /* data: one unit per pack */
    h[0x87] = video ? 2 : 1;
    if (video) {
        uint8_t* v = h + 0x88;                /* M2V: 29.97 fps, the exact size */
        v[0] = 0x02;
        v[16 + 2] = 4;
        be16(v + 16 + 12, kW);
        be16(v + 16 + 14, kH);
    }
    uint8_t* a = h + 0x88 + (video ? 48 : 0); /* LPCM: 2 ch, 48 kHz, 16-bit */
    a[0] = 0x80;
    a[5] = kLpcmSub;
    a[16 + 2] = 2;
    a[16 + 3] = 1;
    a[16 + 4] = 1 << 6;

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { printf("FAIL: cannot write %s\n", path.c_str()); return false; }
    fwrite(h, 1, sizeof h, f);
    for (const auto& p : all) fwrite(p.data(), 1, p.size(), f);
    fclose(f);
    printf("movie: %zu bytes of MPEG-2 in %zu packs, %zu LPCM packs\n", es.size(), packs.size(), apacks.size());
    return true;
}

bool near(int a, int b) { return std::abs(a - b) <= 8; }

void play(const std::string& path, int coding)
{
    const char* name = coding == SAIL_MOVIE_RGBA ? "RGBA" : "ARGB";
    SailMovie* m = sail_movie_open(path.c_str(), coding);
    CHECK(m, "%s: the movie did not open", name);
    if (!m) return;
    SailMovieInfo info;
    sail_movie_info(m, &info);
    CHECK(info.width == kW && info.height == kH, "size %dx%d", info.width, info.height);
    CHECK(info.fps_num == 30000 && info.fps_den == 1001, "rate %d/%d", info.fps_num, info.fps_den);
    CHECK(info.channels == 2 && info.rate == 48000, "audio %d ch at %d", info.channels, info.rate);

    sail_movie_start(m);
    std::vector<uint8_t> px((size_t)kW * kH * 4);
    std::vector<float> pcm(256 * 2);
    int next = 0, audio_k = 0, audio_bad = 0;
    bool veos = false, aeos = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (!(veos && aeos) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        int no = -1, eos = 0;
        if (sail_movie_video(m, px.data(), kW * 4, &no, &eos)) {
            CHECK(no == next, "%s: frame %d handed out, %d expected", name, no, next);
            int want[3];
            expected_rgb(no, want);
            for (int at : { (kH / 2) * kW + kW / 2, 0, kW * kH - 1 }) {
                const uint8_t* p = px.data() + (size_t)at * 4;
                const int r = coding == SAIL_MOVIE_RGBA ? p[0] : p[1], g = coding == SAIL_MOVIE_RGBA ? p[1] : p[2];
                const int b = coding == SAIL_MOVIE_RGBA ? p[2] : p[3], al = coding == SAIL_MOVIE_RGBA ? p[3] : p[0];
                CHECK(near(r, want[0]) && near(g, want[1]) && near(b, want[2]) && al == 0xFF,
                      "%s: frame %d pixel %d is %d,%d,%d alpha %d, want %d,%d,%d", name, no, at, r, g, b, al,
                      want[0], want[1], want[2]);
            }
            next = no + 1;
        }
        veos = veos || eos;
        int e2 = 0;
        if (sail_movie_audio(m, pcm.data(), 256, 0, &e2) == 256) {
            for (int i = 0; i < 512; i++, audio_k++)
                if (pcm[(size_t)i] != sample(audio_k) / 32768.0f) audio_bad++;
        }
        aeos = aeos || e2;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(veos, "%s: the video never ended", name);
    CHECK(next == kFrames, "%s: %d frames handed out, %d expected", name, next, kFrames);
    CHECK(audio_k == kAudioFrames * 2, "%s: %d samples handed out, %d expected", name, audio_k, kAudioFrames * 2);
    CHECK(audio_bad == 0, "%s: %d samples differ from the source", name, audio_bad);
    sail_movie_close(m);
    printf("%s: %d frames, %d samples\n", name, next, audio_k);
}

/* No picture: nothing paces the reader but the sound taken. Every sample is
 * handed out once, in order, and the end comes after the last. */
void play_sound_only(const std::string& path)
{
    SailMovie* m = sail_movie_open(path.c_str(), SAIL_MOVIE_RGBA);
    CHECK(m, "sound only: the movie did not open");
    if (!m) return;
    SailMovieInfo info;
    sail_movie_info(m, &info);
    CHECK(info.width == 0 && info.height == 0, "sound only: size %dx%d", info.width, info.height);
    CHECK(info.channels == 2 && info.rate == 48000, "sound only: audio %d ch at %d", info.channels, info.rate);
    sail_movie_start(m);
    /* The sound is taken from a moment later on, as a title's mixer does: a
     * reader that ran ahead would have dropped the start by then. */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::vector<float> pcm(256 * 2);
    int audio_k = 0, audio_bad = 0, first_bad = -1;
    bool aeos = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (!aeos && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        int e2 = 0;
        if (sail_movie_audio(m, pcm.data(), 256, 0, &e2) == 256) {
            for (int i = 0; i < 512; i++, audio_k++)
                if (pcm[(size_t)i] != sample(audio_k) / 32768.0f && audio_bad++ == 0) first_bad = audio_k;
        } else if (!e2) {
            std::this_thread::yield();
        }
        aeos = aeos || e2;
    }
    int no = -1, veos = 0;
    sail_movie_video(m, nullptr, 0, &no, &veos);
    CHECK(aeos, "sound only: the sound never ended");
    CHECK(veos, "sound only: the stream did not end with its sound");
    CHECK(audio_k == kSoundOnlyFrames * 2, "sound only: %d samples handed out, %d expected", audio_k,
          kSoundOnlyFrames * 2);
    CHECK(audio_bad == 0, "sound only: %d samples differ from the source, the first at %d", audio_bad, first_bad);
    sail_movie_close(m);
    printf("sound only: %d samples\n", audio_k);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: test_sail_movie play|nodecoder|sound <dir>\n"); return 2; }
    const std::string mode = argv[1], path = std::string(argv[2]) + "/sail_movie_test_" + mode + ".pam";
    if (!write_movie(path, mode != "sound", mode == "sound" ? kSoundOnlyFrames : kAudioFrames)) return 1;
    if (mode == "sound") {
        play_sound_only(path);
    } else if (mode == "play") {
        play(path, SAIL_MOVIE_RGBA);
        play(path, SAIL_MOVIE_ARGB);
    } else if (mode == "nodecoder") {
        SailMovie* m = sail_movie_open(path.c_str(), SAIL_MOVIE_RGBA);
        CHECK(!m, "a movie with video opened with no decoder");
        sail_movie_close(m);
    } else {
        printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    remove(path.c_str());
    if (failures) printf("FAILED (%d)\n", failures);
    else printf("PASS\n");
    return failures ? 1 : 0;
}
