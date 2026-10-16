/*
 * ps3recomp - raw DEFLATE decoder, and the native hook for SPU zlib inflate
 *
 * SPU code inflates with a statically linked zlib (PlayStation Edge zlib, for
 * one). Lifted, that runs several times slower than a host decoder, and on a
 * title that streams its data compressed it is most of the loading time. The
 * lifter's --native-hook replaces an image's inflate wrapper with a host call;
 * the wrapper's arguments and result are per title, so each hook is a small
 * adapter over ps3_inflate_raw.
 */
#ifndef PS3_SPU_INFLATE_H
#define PS3_SPU_INFLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inflate the raw DEFLATE stream (no zlib or gzip header) src[0..src_len)
 * into dst[0..dst_cap). Returns 0 when the stream's final block ended inside
 * both buffers, with the bytes written in *out_len; a negative value when the
 * stream is malformed, reads past src_len, or needs more than dst_cap bytes.
 * dst is written up to the error either way. */
int ps3_inflate_raw(uint8_t* dst, uint32_t dst_cap, const uint8_t* src,
                    uint32_t src_len, uint32_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* PS3_SPU_INFLATE_H */
