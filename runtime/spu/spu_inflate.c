/*
 * ps3recomp - raw DEFLATE decoder (RFC 1951)
 *
 * See spu_inflate.h. Canonical Huffman codes decoded through a 10-bit lookup
 * table, with a bit-at-a-time walk for the longer codes; a 64-bit bit buffer
 * refilled a byte at a time. Nothing here allocates or keeps state between
 * calls, so any number of SPU threads can be in it at once.
 */

#include <string.h>
#include "spu_inflate.h"

#define FAST_BITS 10
#define MAX_BITS  15

typedef struct {
    uint16_t fast[1u << FAST_BITS];   /* len << 9 | symbol; len 0: longer code */
    uint16_t count[MAX_BITS + 1];     /* codes of each length */
    uint16_t sym[288];                /* symbols by code length, then value */
} huff;

typedef struct {
    const uint8_t* in;
    const uint8_t* in_end;
    uint64_t bits;
    unsigned nbits;
    unsigned fake;    /* zero bits fed past the end of the input */
} bitin;

static void refill(bitin* s)
{
    while (s->nbits <= 56) {
        if (s->in < s->in_end) s->bits |= (uint64_t)*s->in++ << s->nbits;
        else s->fake += 8;
        s->nbits += 8;
    }
}

static unsigned getbits(bitin* s, unsigned n)   /* n <= 32 */
{
    if (s->nbits < n) refill(s);
    const unsigned v = (unsigned)(s->bits & ((1ull << n) - 1));
    s->bits >>= n; s->nbits -= n;
    return v;
}

/* 0, or -1 for an over-subscribed set of lengths. An incomplete set is
 * accepted (a distance code with one symbol is one); a code it leaves out
 * decodes to an error. */
static int build(huff* h, const uint8_t* len, unsigned n)
{
    uint16_t offs[MAX_BITS + 2];
    memset(h->count, 0, sizeof h->count);
    for (unsigned i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (unsigned l = 1; l <= MAX_BITS; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (unsigned l = 1; l <= MAX_BITS; l++) offs[l + 1] = offs[l] + h->count[l];
    for (unsigned i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;

    memset(h->fast, 0, sizeof h->fast);
    unsigned code = 0, k = 0;
    for (unsigned l = 1; l <= MAX_BITS; l++) {
        for (unsigned c = 0; c < h->count[l]; c++, k++, code++) {
            if (l > FAST_BITS) continue;
            unsigned rev = 0;   /* codes are sent most significant bit first */
            for (unsigned b = 0; b < l; b++) rev |= ((code >> b) & 1u) << (l - 1 - b);
            for (unsigned j = rev; j < (1u << FAST_BITS); j += 1u << l)
                h->fast[j] = (uint16_t)(l << 9 | h->sym[k]);
        }
        code <<= 1;
    }
    return 0;
}

static int decode(bitin* s, const huff* h)
{
    if (s->nbits < MAX_BITS) refill(s);
    const unsigned e = h->fast[s->bits & ((1u << FAST_BITS) - 1)];
    if (e >> 9) {
        s->bits >>= e >> 9; s->nbits -= e >> 9;
        return (int)(e & 511);
    }
    int code = 0, first = 0, index = 0;
    for (unsigned l = 1; l <= MAX_BITS; l++) {
        code |= (int)(s->bits & 1); s->bits >>= 1; s->nbits--;
        const int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count;
        first <<= 1; code <<= 1;
    }
    return -1;
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(bitin* s, const huff* lit, const huff* dist,
                 uint8_t* dst, uint32_t cap, uint32_t* pos)
{
    uint32_t o = *pos;
    for (;;) {
        int sym = decode(s, lit);
        if (sym < 256) {
            if (sym < 0) return -1;
            if (o >= cap) return -2;
            dst[o++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) break;
        sym -= 257;
        if (sym >= 29) return -1;
        const uint32_t n = len_base[sym] + getbits(s, len_extra[sym]);
        const int ds = decode(s, dist);
        if (ds < 0 || ds >= 30) return -1;
        const uint32_t d = dist_base[ds] + getbits(s, dist_extra[ds]);
        if (d > o) return -1;
        if (n > cap - o) return -2;
        const uint8_t* from = dst + o - d;
        uint8_t* to = dst + o;
        if (d >= n) memcpy(to, from, n);
        else for (uint32_t i = 0; i < n; i++) to[i] = from[i];   /* overlapping run */
        o += n;
    }
    *pos = o;
    return 0;
}

static void fixed_tables(huff* lit, huff* dist)
{
    uint8_t l[288];
    unsigned i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build(lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build(dist, l, 30);
}

static int dynamic_tables(bitin* s, huff* lit, huff* dist)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint8_t l[288 + 32];
    const unsigned nlen = getbits(s, 5) + 257;
    const unsigned ndist = getbits(s, 5) + 1;
    const unsigned ncode = getbits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    memset(l, 0, 19);
    for (unsigned i = 0; i < ncode; i++) l[order[i]] = (uint8_t)getbits(s, 3);
    huff lc;
    if (build(&lc, l, 19)) return -1;
    for (unsigned i = 0; i < nlen + ndist;) {
        const int sym = decode(s, &lc);
        if (sym < 0) return -1;
        if (sym < 16) { l[i++] = (uint8_t)sym; continue; }
        uint8_t v = 0;
        unsigned rep;
        if (sym == 16) {
            if (i == 0) return -1;
            v = l[i - 1];
            rep = 3 + getbits(s, 2);
        } else if (sym == 17) rep = 3 + getbits(s, 3);
        else rep = 11 + getbits(s, 7);
        if (i + rep > nlen + ndist) return -1;
        while (rep--) l[i++] = v;
    }
    if (l[256] == 0) return -1;   /* no end-of-block code */
    if (build(lit, l, nlen) || build(dist, l + nlen, ndist)) return -1;
    return 0;
}

int ps3_inflate_raw(uint8_t* dst, uint32_t dst_cap, const uint8_t* src,
                    uint32_t src_len, uint32_t* out_len)
{
    bitin s = { src, src + src_len, 0, 0, 0 };
    uint32_t o = 0;
    huff lit, dist;
    unsigned last;
    *out_len = 0;
    do {
        last = getbits(&s, 1);
        const unsigned type = getbits(&s, 2);
        if (type == 0) {
            getbits(&s, s.nbits & 7);   /* to a byte boundary */
            if (s.fake > s.nbits) return -1;
            s.in -= (s.nbits - s.fake) / 8;   /* hand the buffered bytes back */
            s.bits = 0; s.nbits = 0; s.fake = 0;
            if (s.in_end - s.in < 4) return -1;
            const unsigned n = s.in[0] | s.in[1] << 8;
            if ((n ^ (s.in[2] | s.in[3] << 8)) != 0xFFFF) return -1;
            s.in += 4;
            if ((uint32_t)(s.in_end - s.in) < n) return -1;
            if (n > dst_cap - o) return -2;
            memcpy(dst + o, s.in, n);
            s.in += n; o += n;
        } else if (type == 1 || type == 2) {
            if (type == 1) fixed_tables(&lit, &dist);
            else if (dynamic_tables(&s, &lit, &dist)) return -1;
            const int r = codes(&s, &lit, &dist, dst, dst_cap, &o);
            if (r) { *out_len = o; return r; }
        } else {
            return -1;
        }
        if (s.fake > s.nbits) return -1;   /* read past the end of the input */
    } while (!last);
    *out_len = o;
    return 0;
}
