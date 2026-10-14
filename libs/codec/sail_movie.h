/*
 * sail_movie -- host playback of a PAMF movie for cellSail.
 *
 * A PAMF file is a small header followed by an MPEG program stream: MPEG-2 or
 * AVC video (stream 0xE0) and LPCM / ATRAC3+ / AC3 audio in private stream 1.
 * This module demuxes it on a thread of its own, decodes MPEG-2 video with
 * the operating system's decoder (Media Foundation on Windows) and 16-bit
 * LPCM itself, and hands out frames on the wall clock from sail_movie_start.
 * cellSail's adapters copy them into the guest buffers the title reads.
 *
 * Only MPEG-2 video and LPCM audio for now (what Demon's Souls ships); a
 * stream with anything else opens without that track. Where there is no
 * decoder (non-Windows hosts), sail_movie_open returns NULL and cellSail
 * keeps its old behaviour (the movie ends at once).
 */
#ifndef PS3RECOMP_SAIL_MOVIE_H
#define PS3RECOMP_SAIL_MOVIE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SailMovie SailMovie;

/* Pixel layouts for decoded frames (CellSailVideoCoding values). */
enum { SAIL_MOVIE_ARGB = 0, SAIL_MOVIE_RGBA = 1 };

typedef struct {
    int width, height;          /* 0 when there is no video track */
    int fps_num, fps_den;
    int channels, rate;         /* 0 channels when there is no audio track */
} SailMovieInfo;

/* Opens host_path, reads the stream headers and starts demuxing/decoding
 * ahead into bounded queues. coding is SAIL_MOVIE_ARGB or SAIL_MOVIE_RGBA.
 * NULL if the file cannot be read or nothing in it can be decoded. */
SailMovie* sail_movie_open(const char* host_path, int coding);
void sail_movie_info(const SailMovie* m, SailMovieInfo* out);

/* The playback clock starts (frame 0 is due now). */
void sail_movie_start(SailMovie* m);

/* The newest frame that is due and has not been handed out yet, copied to dst
 * (height rows of pitch bytes, 4 bytes per pixel). Returns 1 with *frame_no
 * set, 0 when no new frame is due. *eos becomes 1 once the last frame has
 * been handed out and the stream is exhausted. */
int sail_movie_video(SailMovie* m, uint8_t* dst, int pitch, int* frame_no, int* eos);

/* Up to `frames` interleaved float frames; returns how many were copied
 * (0 when the queue is short). big_endian: store each float byte-swapped,
 * the way the guest reads it. *eos as above. */
int sail_movie_audio(SailMovie* m, float* dst, int frames, int big_endian, int* eos);

void sail_movie_close(SailMovie* m);

#ifdef __cplusplus
}
#endif

#endif
