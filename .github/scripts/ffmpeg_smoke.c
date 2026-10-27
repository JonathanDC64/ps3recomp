/*
 * ffmpeg_smoke.c -- the Windows check of ffmpeg-archive.yml. Built with clang-cl against the package's headers,
 * together with libs/codec/ffmpeg_runtime.c, and run with the package's DLLs beside it, where a game build puts
 * them. ps3_ffmpeg_get() must load them (the majors the headers name), and the MPEG-2 decoder and parser that
 * cellSail's movies use must open.
 */

#include <stdio.h>

#include "ffmpeg_runtime.h"

static int fail(const char* what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    return 1;
}

int main(void)
{
    const PS3FFmpeg* ff = ps3_ffmpeg_get();
    if (!ff) return fail("ps3_ffmpeg_get() found no usable FFmpeg beside the test");
    const unsigned c = ff->avcodec_version(), u = ff->avutil_version();
    printf("libavcodec %u.%u.%u, libavutil %u.%u.%u\n", c >> 16, (c >> 8) & 0xFF, c & 0xFF, u >> 16,
           (u >> 8) & 0xFF, u & 0xFF);

    const AVCodec* dec = ff->avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!dec) return fail("no MPEG-2 video decoder");
    AVCodecContext* ctx = ff->avcodec_alloc_context3(dec);
    if (!ctx) return fail("avcodec_alloc_context3");
    if (ff->avcodec_open2(ctx, dec, NULL) < 0) return fail("avcodec_open2 on the MPEG-2 decoder");
    AVCodecParserContext* parser = ff->av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    if (!parser) return fail("no MPEG video parser");
    AVPacket* pkt = ff->av_packet_alloc();
    AVFrame* frame = ff->av_frame_alloc();
    if (!pkt || !frame) return fail("av_packet_alloc / av_frame_alloc");

    ff->av_frame_free(&frame);
    ff->av_packet_free(&pkt);
    ff->av_parser_close(parser);
    ff->avcodec_free_context(&ctx);
    printf("PASS: the MPEG-2 decoder and parser open\n");
    return 0;
}
