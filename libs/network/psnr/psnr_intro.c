/*
 * psnr_intro.c - see psnr_intro.h. Building and reading the introductions' packets; no sockets here.
 */
#include "psnr_intro.h"
#include "psnr.h"   /* psnr_put / psnr_get */
#include "../../../third_party/monocypher/monocypher.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
/* (the CRT's: declared here, as a file that includes this one may have read stdlib.h without _CRT_RAND_S) */
extern int __cdecl rand_s(unsigned int* value);
#endif

static uint8_t* pi_head(uint8_t* out, uint8_t type)
{
    memcpy(out, "PSNR", 4);
    out[4] = type;
    return out + 5;
}

static uint8_t* pi_put_cand(uint8_t* p, const psnr_cand* c)
{
    *p++ = 4;
    memcpy(p, c->ip, 4);
    return psnr_put16(p + 4, c->port);
}

static uint8_t* pi_put_cands(uint8_t* p, const psnr_cand* c, int n, int most)
{
    if (n < 0) n = 0;
    if (n > most) n = most;
    *p++ = (uint8_t)n;
    for (int i = 0; i < n; i++) p = pi_put_cand(p, &c[i]);
    return p;
}

static size_t pi_padded(uint8_t* out, uint8_t* end)
{
    const size_t n = (size_t)(end - out);
    if (n < PSNR_INTRO_REQ_LEN) memset(end, 0, PSNR_INTRO_REQ_LEN - n);
    return PSNR_INTRO_REQ_LEN;
}

size_t psnr_intro_register(uint8_t out[PSNR_INTRO_REQ_LEN], uint16_t version, uint64_t room, uint64_t owner,
                           uint16_t room_port, const psnr_cand* cands, int n)
{
    uint8_t* p = pi_head(out, PSNR_PKT_REGISTER);
    p = psnr_put16(p, version);
    p = psnr_put64(p, room);
    p = psnr_put64(p, owner);
    p = psnr_put16(p, room_port);
    return pi_padded(out, pi_put_cands(p, cands, n, 2));
}

size_t psnr_intro_unregister(uint8_t out[PSNR_INTRO_REQ_LEN], uint64_t room, uint64_t owner)
{
    uint8_t* p = pi_head(out, PSNR_PKT_UNREGISTER);
    p = psnr_put64(p, room);
    return pi_padded(out, psnr_put64(p, owner));
}

size_t psnr_intro_lookup(uint8_t out[PSNR_INTRO_REQ_LEN], uint16_t version, uint64_t room, uint32_t nonce,
                         const psnr_cand* own)
{
    uint8_t* p = pi_head(out, PSNR_PKT_LOOKUP);
    p = psnr_put16(p, version);
    p = psnr_put64(p, room);
    p = psnr_put32(p, nonce);
    return pi_padded(out, pi_put_cands(p, own, own ? 1 : 0, 1));
}

size_t psnr_intro_open(uint8_t out[PSNR_INTRO_REQ_LEN], uint64_t room, uint64_t nonce)
{
    uint8_t* p = pi_head(out, PSNR_PKT_OPEN);
    p = psnr_put64(p, room);
    return pi_padded(out, psnr_put64(p, nonce));
}

size_t psnr_intro_open2(uint8_t out[PSNR_INTRO_REQ_LEN], uint64_t nonce, uint64_t cookie)
{
    uint8_t* p = pi_head(out, PSNR_PKT_OPEN2);
    p = psnr_put64(p, nonce);
    return pi_padded(out, psnr_put64(p, cookie));
}

size_t psnr_intro_registered(uint8_t* out, uint64_t room, uint8_t status, const psnr_cand* seen, uint8_t refresh_s)
{
    static const psnr_cand none;
    uint8_t* p = pi_head(out, PSNR_PKT_REGISTERED);
    p = psnr_put64(p, room);
    *p++ = status;
    p = pi_put_cand(p, seen ? seen : &none);
    *p++ = refresh_s;
    return (size_t)(p - out);
}

size_t psnr_intro_found(uint8_t* out, uint64_t room, uint32_t nonce, uint8_t status, uint8_t flags,
                        const psnr_cand* seen, uint16_t room_port, const psnr_cand* cands, int n)
{
    static const psnr_cand none;
    uint8_t* p = pi_head(out, PSNR_PKT_FOUND);
    p = psnr_put64(p, room);
    p = psnr_put32(p, nonce);
    *p++ = status;
    *p++ = flags;
    p = pi_put_cand(p, seen ? seen : &none);
    p = psnr_put16(p, room_port);
    return (size_t)(pi_put_cands(p, cands, n, 3) - out);
}

size_t psnr_intro_intro(uint8_t* out, uint64_t room, uint32_t nonce, uint8_t flags, const psnr_cand* cands, int n)
{
    uint8_t* p = pi_head(out, PSNR_PKT_INTRO);
    p = psnr_put64(p, room);
    p = psnr_put32(p, nonce);
    *p++ = flags;
    return (size_t)(pi_put_cands(p, cands, n, 2) - out);
}

size_t psnr_intro_knock(uint8_t* out, uint64_t room, uint32_t nonce)
{
    uint8_t* p = pi_head(out, PSNR_PKT_KNOCK);
    p = psnr_put64(p, room);
    return (size_t)(psnr_put32(p, nonce) - out);
}

size_t psnr_intro_cookie(uint8_t* out, uint64_t nonce, uint64_t cookie)
{
    uint8_t* p = pi_head(out, PSNR_PKT_COOKIE);
    p = psnr_put64(p, nonce);
    return (size_t)(psnr_put64(p, cookie) - out);
}

size_t psnr_intro_opened(uint8_t* out, uint64_t nonce, uint32_t session)
{
    uint8_t* p = pi_head(out, PSNR_PKT_OPENED);
    p = psnr_put64(p, nonce);
    return (size_t)(psnr_put32(p, session) - out);
}

/* ---- reading ---- */

typedef struct { const uint8_t* p; size_t n; int err; } pi_rd;

static const uint8_t* pi_take(pi_rd* r, size_t n)
{
    static const uint8_t zero[8];
    if (r->err || r->n < n) { r->err = 1; return zero; }
    const uint8_t* p = r->p;
    r->p += n;
    r->n -= n;
    return p;
}
static uint8_t  pi_r8(pi_rd* r)  { return *pi_take(r, 1); }
static uint16_t pi_r16(pi_rd* r) { return psnr_get16(pi_take(r, 2)); }
static uint32_t pi_r32(pi_rd* r) { return psnr_get32(pi_take(r, 4)); }
static uint64_t pi_r64(pi_rd* r) { return psnr_get64(pi_take(r, 8)); }

static void pi_r_cand(pi_rd* r, psnr_cand* c)
{
    c->family = pi_r8(r);
    memcpy(c->ip, pi_take(r, 4), 4);
    c->port = pi_r16(r);
    if (c->family != 4) r->err = 1;   /* (IPv4 only for now: the family byte leaves room for IPv6) */
}

static void pi_r_cands(pi_rd* r, psnr_intro_pkt* o, int most)
{
    const int n = pi_r8(r);
    if (n > most) { r->err = 1; return; }
    for (int i = 0; i < n && !r->err; i++) pi_r_cand(r, &o->cands[i]);
    o->n = r->err ? 0 : n;
}

int psnr_udp_type(const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    return len >= 5 && memcmp(p, "PSNR", 4) == 0 ? p[4] : -1;
}

int psnr_intro_read(const void* buf, size_t len, psnr_intro_pkt* o)
{
    const int type = psnr_udp_type(buf, len);
    pi_rd r = { (const uint8_t*)buf + 5, len >= 5 ? len - 5 : 0, 0 };
    memset(o, 0, sizeof *o);
    if (type < 0) return 0;
    o->type = (uint8_t)type;
    switch (type) {
    case PSNR_PKT_REGISTER:
        if (len < PSNR_INTRO_REQ_LEN) return 0;   /* (padded: no answer is bigger than a request) */
        o->version = pi_r16(&r); o->room = pi_r64(&r); o->owner = pi_r64(&r); o->room_port = pi_r16(&r);
        pi_r_cands(&r, o, 2);
        break;
    case PSNR_PKT_UNREGISTER:
        if (len < PSNR_INTRO_REQ_LEN) return 0;
        o->room = pi_r64(&r); o->owner = pi_r64(&r);
        break;
    case PSNR_PKT_LOOKUP:
        if (len < PSNR_INTRO_REQ_LEN) return 0;
        o->version = pi_r16(&r); o->room = pi_r64(&r); o->nonce = pi_r32(&r);
        pi_r_cands(&r, o, 1);
        break;
    case PSNR_PKT_REGISTERED:
        o->room = pi_r64(&r); o->status = pi_r8(&r);
        pi_r_cand(&r, &o->seen);
        o->refresh = pi_r8(&r);
        break;
    case PSNR_PKT_FOUND:
        o->room = pi_r64(&r); o->nonce = pi_r32(&r); o->status = pi_r8(&r);
        if (r.err) return 0;
        if (o->status != PSNR_INTRO_OK && r.n == 0) return 1;   /* (a refusal may stop after its status) */
        o->flags = pi_r8(&r);
        pi_r_cand(&r, &o->seen);
        o->room_port = pi_r16(&r);
        pi_r_cands(&r, o, 3);
        if (r.err && o->status != PSNR_INTRO_OK) { memset(&o->seen, 0, sizeof o->seen); o->n = 0; return 1; }
        break;
    case PSNR_PKT_INTRO:
        o->room = pi_r64(&r); o->nonce = pi_r32(&r); o->flags = pi_r8(&r);
        pi_r_cands(&r, o, 2);
        break;
    case PSNR_PKT_KNOCK:
        o->room = pi_r64(&r); o->nonce = pi_r32(&r);
        break;
    case PSNR_PKT_OPEN:
        if (len < PSNR_INTRO_REQ_LEN) return 0;
        o->room = pi_r64(&r); o->nonce64 = pi_r64(&r);
        break;
    case PSNR_PKT_OPEN2:
        if (len < PSNR_INTRO_REQ_LEN) return 0;
        o->nonce64 = pi_r64(&r); o->cookie = pi_r64(&r);
        break;
    case PSNR_PKT_COOKIE:
        o->nonce64 = pi_r64(&r); o->cookie = pi_r64(&r);
        break;
    case PSNR_PKT_OPENED:
        o->nonce64 = pi_r64(&r); o->session = pi_r32(&r);
        break;
    default:
        return 0;
    }
    return !r.err;
}

psnr_cand psnr_cand_of(const uint8_t ip[4], uint16_t port)
{
    psnr_cand c;
    c.family = 4;
    memcpy(c.ip, ip, 4);
    c.port = port;
    return c;
}

uint64_t psnr_intro_cookie_of(const uint8_t secret[32], const uint8_t ip[4], uint16_t port, uint64_t nonce,
                              uint64_t minute)
{
    uint8_t msg[4 + 2 + 8 + 8], h[8];
    memcpy(msg, ip, 4);
    psnr_put16(msg + 4, port);
    psnr_put64(msg + 6, nonce);
    psnr_put64(msg + 14, minute);
    crypto_blake2b_keyed(h, sizeof h, secret, 32, msg, sizeof msg);
    return psnr_get64(h);
}

uint64_t psnr_intro_random(void)
{
    uint64_t v = 0;
#ifdef _WIN32
    unsigned int a = 0, b = 0;
    if (rand_s(&a) == 0 && rand_s(&b) == 0) v = (uint64_t)a << 32 | b;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&v, sizeof v, 1, f) != 1) v = 0;
        fclose(f);
    }
#endif
    if (!v) {   /* (no random source: still a value, if a guessable one) */
        static uint64_t z;
        z += 0x9E3779B97F4A7C15ull ^ (uint64_t)time(NULL) ^ (uint64_t)clock() << 32;
        v = z;
        v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
        v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
        v ^= v >> 31;
    }
    return v;
}
