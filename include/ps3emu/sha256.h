/*
 * ps3recomp - SHA-256 (FIPS 180-4), for the network sign-in's proof of work
 * (libs/network/ps3_net_signin.c). Header-only, like nid.h's SHA-1.
 */

#ifndef PS3EMU_SHA256_H
#define PS3EMU_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_sha256_ctx {
    uint32_t state[8];
    uint64_t count;       /* bytes hashed */
    uint8_t  buffer[64];
} ps3_sha256_ctx;

static const uint32_t ps3_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ps3_sha256_ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static inline void ps3_sha256_init(ps3_sha256_ctx* ctx)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(ctx->state, iv, sizeof(iv));
    ctx->count = 0;
}

static inline void ps3_sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 |
               block[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ps3_sha256_ror(w[i - 15], 7) ^ ps3_sha256_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ps3_sha256_ror(w[i - 2], 17) ^ ps3_sha256_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ps3_sha256_ror(e, 6) ^ ps3_sha256_ror(e, 11) ^ ps3_sha256_ror(e, 25)) +
                      ((e & f) ^ (~e & g)) + ps3_sha256_k[i] + w[i];
        uint32_t t2 = (ps3_sha256_ror(a, 2) ^ ps3_sha256_ror(a, 13) ^ ps3_sha256_ror(a, 22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static inline void ps3_sha256_update(ps3_sha256_ctx* ctx, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t used = (size_t)(ctx->count & 63);
    ctx->count += len;
    if (used) {
        size_t take = 64 - used < len ? 64 - used : len;
        memcpy(ctx->buffer + used, p, take);
        p += take;
        len -= take;
        if (used + take < 64) return;
        ps3_sha256_transform(ctx->state, ctx->buffer);
    }
    for (; len >= 64; p += 64, len -= 64) ps3_sha256_transform(ctx->state, p);
    memcpy(ctx->buffer, p, len);
}

static inline void ps3_sha256_final(ps3_sha256_ctx* ctx, uint8_t out[32])
{
    uint64_t bits = ctx->count * 8;
    uint8_t pad[72] = {0x80};
    size_t used = (size_t)(ctx->count & 63);
    size_t n = (used < 56 ? 56 : 120) - used;
    for (int i = 0; i < 8; i++) pad[n + i] = (uint8_t)(bits >> (56 - 8 * i));
    ps3_sha256_update(ctx, pad, n + 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

static inline void ps3_sha256(const void* data, size_t len, uint8_t out[32])
{
    ps3_sha256_ctx c;
    ps3_sha256_init(&c);
    ps3_sha256_update(&c, data, len);
    ps3_sha256_final(&c, out);
}

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_SHA256_H */
