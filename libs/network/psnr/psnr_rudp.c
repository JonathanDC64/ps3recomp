/*
 * psnr_rudp.c - see psnr_rudp.h.
 */
#include "psnr_rudp.h"
#include "psnr.h"   /* psnr_put / psnr_get */

#include <stdlib.h>
#include <string.h>

#define W PSNR_RUDP_WINDOW

typedef struct {
    uint32_t num;        /* 0: free (or acknowledged) */
    uint16_t len;
    unsigned rto;
    uint64_t sent_at, first_at;
    uint8_t  data[PSNR_RUDP_PAYLOAD];
} out_seg;

typedef struct {
    uint32_t num;        /* 0: none */
    uint16_t len;
    uint8_t  data[PSNR_RUDP_PAYLOAD];
} in_seg;

struct psnr_rudp {
    uint32_t session;
    psnr_rudp_send_fn send;
    void*    ctx;
    int      state;      /* PSNR_RUDP_ALIVE, _CLOSED or _BROKEN */
    /* sending */
    uint8_t* q;          /* written, not in a datagram yet */
    size_t   q_n, q_cap;
    out_seg  out[W];     /* in flight: numbers una..next-1, each at number % W */
    uint32_t next, una;  /* the next number to give; the oldest not acknowledged */
    uint64_t last_tx;
    /* receiving */
    uint32_t got;        /* every number up to this one came */
    in_seg   early[W];   /* got+2 .. got+W, at number % W */
    uint8_t* in;         /* in order, not read yet: in[in_off .. in_n) */
    size_t   in_n, in_off, in_cap;
    int      ack_due;
    uint64_t last_rx;
};

psnr_rudp* psnr_rudp_new(uint32_t session, psnr_rudp_send_fn send, void* ctx, uint64_t now_ms)
{
    psnr_rudp* r = (psnr_rudp*)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->session = session;
    r->send = send;
    r->ctx = ctx;
    r->next = r->una = 1;
    r->last_tx = r->last_rx = now_ms;
    return r;
}

void psnr_rudp_free(psnr_rudp* r)
{
    if (!r) return;
    free(r->q);
    free(r->in);
    free(r);
}

uint32_t psnr_rudp_session(const psnr_rudp* r)
{
    return r->session;
}

static uint32_t early_map(const psnr_rudp* r)
{
    uint32_t m = 0;
    for (uint32_t i = 0; i < 31; i++) {
        const uint32_t n = r->got + 2 + i;
        if (r->early[n % W].num == n) m |= 1u << i;
    }
    return m;
}

static void send_dgram(psnr_rudp* r, uint32_t num, const uint8_t* payload, uint16_t len, uint64_t now)
{
    uint8_t pkt[PSNR_RUDP_DGRAM];
    memcpy(pkt, "PSNR", 4);
    pkt[4] = 0x08;
    psnr_put32(pkt + 5, r->session);
    psnr_put32(pkt + 9, num);
    psnr_put32(pkt + 13, r->got);
    psnr_put32(pkt + 17, early_map(r));
    if (len) memcpy(pkt + PSNR_RUDP_HDR, payload, len);
    r->send(r->ctx, pkt, PSNR_RUDP_HDR + (size_t)len);
    r->last_tx = now;
    r->ack_due = 0;   /* (every datagram carries the acks) */
}

/* Queued bytes into datagrams, as far as the window goes. */
static void pump_out(psnr_rudp* r, uint64_t now)
{
    size_t off = 0;
    while (r->q_n - off > 0 && r->next - r->una < W) {
        out_seg* s = &r->out[r->next % W];
        const size_t len = r->q_n - off < PSNR_RUDP_PAYLOAD ? r->q_n - off : PSNR_RUDP_PAYLOAD;
        memcpy(s->data, r->q + off, len);
        s->len = (uint16_t)len;
        s->num = r->next++;
        s->sent_at = s->first_at = now;
        s->rto = PSNR_RUDP_RTO_MS;
        off += len;
        send_dgram(r, s->num, s->data, s->len, now);
    }
    if (off) {
        memmove(r->q, r->q + off, r->q_n - off);
        r->q_n -= off;
    }
}

int psnr_rudp_write(psnr_rudp* r, const void* bytes, size_t n, uint64_t now_ms)
{
    if (r->state != PSNR_RUDP_ALIVE || r->q_n + n > PSNR_RUDP_QUEUE_MAX) return -1;
    if (r->q_n + n > r->q_cap) {
        size_t cap = r->q_cap ? r->q_cap : 4096;
        while (cap < r->q_n + n) cap *= 2;
        uint8_t* q = (uint8_t*)realloc(r->q, cap);
        if (!q) return -1;
        r->q = q;
        r->q_cap = cap;
    }
    if (n) memcpy(r->q + r->q_n, bytes, n);
    r->q_n += n;
    pump_out(r, now_ms);
    return 0;
}

size_t psnr_rudp_readable(const psnr_rudp* r)
{
    return r->in_n - r->in_off;
}

size_t psnr_rudp_read(psnr_rudp* r, void* out, size_t cap)
{
    size_t n = r->in_n - r->in_off;
    if (n > cap) n = cap;
    if (n) memcpy(out, r->in + r->in_off, n);
    r->in_off += n;
    if (r->in_off == r->in_n) r->in_off = r->in_n = 0;
    return n;
}

/* Bytes that came in order, for psnr_rudp_read. 0, or -1 when too many wait to be read. */
static int deliver(psnr_rudp* r, const uint8_t* p, size_t n)
{
    if (r->in_n - r->in_off + n > PSNR_RUDP_READ_MAX) return -1;
    if (r->in_off && r->in_n + n > r->in_cap) {   /* make room at the front first */
        memmove(r->in, r->in + r->in_off, r->in_n - r->in_off);
        r->in_n -= r->in_off;
        r->in_off = 0;
    }
    if (r->in_n + n > r->in_cap) {
        size_t cap = r->in_cap ? r->in_cap : 4096;
        while (cap < r->in_n + n) cap *= 2;
        uint8_t* in = (uint8_t*)realloc(r->in, cap);
        if (!in) return -1;
        r->in = in;
        r->in_cap = cap;
    }
    memcpy(r->in + r->in_n, p, n);
    r->in_n += n;
    return 0;
}

static void take_acks(psnr_rudp* r, uint32_t ack, uint32_t map)
{
    /* (an ack for a number never sent is someone else's, or garbage: nothing in it counts) */
    if (ack >= r->next || ack + 1 < r->una) return;
    for (uint32_t n = r->una; n <= ack; n++)
        if (r->out[n % W].num == n) r->out[n % W].num = 0;
    if (ack + 1 > r->una) r->una = ack + 1;
    for (uint32_t i = 0; i < 31; i++) {
        const uint32_t n = ack + 2 + i;
        if ((map >> i & 1) && n < r->next && r->out[n % W].num == n) r->out[n % W].num = 0;
    }
}

void psnr_rudp_input(psnr_rudp* r, const void* buf, size_t len, uint64_t now_ms)
{
    const uint8_t* p = (const uint8_t*)buf;
    if (r->state != PSNR_RUDP_ALIVE || len < 9 || memcmp(p, "PSNR", 4) || psnr_get32(p + 5) != r->session) return;
    if (p[4] == 0x09) {   /* CLOSE */
        r->state = PSNR_RUDP_CLOSED;
        return;
    }
    if (p[4] != 0x08 || len < PSNR_RUDP_HDR || len > PSNR_RUDP_DGRAM) return;
    r->last_rx = now_ms;
    take_acks(r, psnr_get32(p + 13), psnr_get32(p + 17));
    const uint32_t num = psnr_get32(p + 9);
    const uint16_t n = (uint16_t)(len - PSNR_RUDP_HDR);
    if (num) {
        r->ack_due = 1;   /* (a copy too: our last ack may have been lost) */
        if (num == r->got + 1) {
            if (deliver(r, p + PSNR_RUDP_HDR, n)) { r->state = PSNR_RUDP_BROKEN; return; }
            r->got++;
            for (in_seg* e = &r->early[(r->got + 1) % W]; e->num == r->got + 1; e = &r->early[(r->got + 1) % W]) {
                if (deliver(r, e->data, e->len)) { r->state = PSNR_RUDP_BROKEN; return; }
                e->num = 0;
                r->got++;
            }
        } else if (num > r->got + 1 && num - r->got <= W) {   /* early: kept until the ones before it come */
            in_seg* e = &r->early[num % W];
            if (e->num != num) {
                e->num = num;
                e->len = n;
                memcpy(e->data, p + PSNR_RUDP_HDR, n);
            }
        }
        /* (num <= got: a copy, dropped; further ahead than the window: dropped, it comes again) */
    }
    pump_out(r, now_ms);   /* (acks may have freed the window) */
}

void psnr_rudp_tick(psnr_rudp* r, uint64_t now_ms)
{
    if (r->state != PSNR_RUDP_ALIVE) return;
    pump_out(r, now_ms);
    for (uint32_t n = r->una; n < r->next; n++) {
        out_seg* s = &r->out[n % W];
        if (s->num != n || now_ms - s->sent_at < s->rto) continue;
        send_dgram(r, s->num, s->data, s->len, now_ms);
        s->sent_at = now_ms;
        s->rto = s->rto * 2 > PSNR_RUDP_RTO_MAX_MS ? PSNR_RUDP_RTO_MAX_MS : s->rto * 2;
    }
    if (r->ack_due || now_ms - r->last_tx >= PSNR_RUDP_KEEPALIVE_MS) send_dgram(r, 0, NULL, 0, now_ms);
}

unsigned psnr_rudp_due_ms(const psnr_rudp* r, uint64_t now_ms, unsigned max_ms)
{
    if (r->state != PSNR_RUDP_ALIVE) return max_ms;
    if (r->ack_due || (r->q_n && r->next - r->una < W)) return 0;
    uint64_t due = r->last_tx + PSNR_RUDP_KEEPALIVE_MS;
    for (uint32_t n = r->una; n < r->next; n++) {
        const out_seg* s = &r->out[n % W];
        if (s->num == n && s->sent_at + s->rto < due) due = s->sent_at + s->rto;
    }
    if (due <= now_ms) return 0;
    return due - now_ms < max_ms ? (unsigned)(due - now_ms) : max_ms;
}

int psnr_rudp_dead(const psnr_rudp* r, uint64_t now_ms)
{
    if (r->state != PSNR_RUDP_ALIVE) return r->state;
    for (uint32_t n = r->una; n < r->next; n++) {
        const out_seg* s = &r->out[n % W];
        if (s->num == n && now_ms - s->first_at >= PSNR_RUDP_UNACKED_MS) return PSNR_RUDP_UNACKED;
    }
    if (now_ms - r->last_rx >= PSNR_RUDP_SILENT_MS) return PSNR_RUDP_SILENT;
    return PSNR_RUDP_ALIVE;
}

size_t psnr_rudp_close_packet(uint8_t out[10], uint32_t session, uint8_t reason)
{
    memcpy(out, "PSNR", 4);
    out[4] = 0x09;
    psnr_put32(out + 5, session);
    out[9] = reason;
    return 10;
}

void psnr_rudp_close(psnr_rudp* r, uint8_t reason)
{
    if (r->state != PSNR_RUDP_ALIVE) return;
    uint8_t pkt[10];
    psnr_rudp_close_packet(pkt, r->session, reason);
    r->send(r->ctx, pkt, sizeof pkt);
    r->send(r->ctx, pkt, sizeof pkt);
    r->state = PSNR_RUDP_CLOSED;
}

uint32_t psnr_rudp_session_of(const void* buf, size_t len)
{
    const uint8_t* p = (const uint8_t*)buf;
    if (len < 9 || memcmp(p, "PSNR", 4) || (p[4] != 0x08 && p[4] != 0x09)) return 0;
    return psnr_get32(p + 5);
}
