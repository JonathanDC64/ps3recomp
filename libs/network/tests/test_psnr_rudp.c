/*
 * test_psnr_rudp - the reliable layer over UDP (psnr/psnr_rudp.c) between two ends on a made-up link that loses,
 * reorders, copies and delays datagrams, on a clock of the test's own: streams both ways arrive whole and in order,
 * messages larger than one datagram too; keepalives hold an idle session; a dead link ends it after 20 s (data not
 * acknowledged) or 30 s (silence); a CLOSE ends it at once; other sessions' and malformed datagrams change nothing.
 * Then the introductions' packets (psnr/psnr_intro.c): each read back as it was built, requests 64 bytes and no
 * answer bigger; and the session cookie.
 *
 *   clang-cl /I include /Fe:test_psnr_rudp.exe libs/network/tests/test_psnr_rudp.c third_party/monocypher/monocypher.c
 *   cc -std=gnu17 -I include -o test_psnr_rudp libs/network/tests/test_psnr_rudp.c third_party/monocypher/monocypher.c
 */
#include "../psnr/psnr_rudp.c"
#include "../psnr/psnr_intro.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail++; } } while (0)

/* ---- the link ---- */

#define LINK_MAX 4096
static struct pkt { int to; uint64_t at; size_t len; uint8_t b[PSNR_RUDP_DGRAM]; } s_q[LINK_MAX];
static int s_qn;
static uint64_t s_now;
static uint32_t s_rng = 12345;
static int s_loss, s_dup, s_jitter;   /* percent lost, percent copied, ms of random delay */
static int s_down;                    /* everything lost */
static unsigned s_sent[2], s_bytes_on_wire[2];

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void enqueue(int to, const uint8_t* p, size_t n, uint64_t at)
{
    if (s_qn >= LINK_MAX) return;   /* (a full link drops it, as a router would) */
    struct pkt* q = &s_q[s_qn++];
    q->to = to;
    q->at = at;
    q->len = n;
    memcpy(q->b, p, n);
}

/* ctx: the end it is sent to (0 or 1) */
static void link_send(void* ctx, const uint8_t* p, size_t n)
{
    const int to = (int)(intptr_t)ctx;
    s_sent[1 - to]++;
    s_bytes_on_wire[1 - to] += (unsigned)n;
    CHECK(n <= PSNR_RUDP_DGRAM);
    if (s_down || (int)(rnd() % 100) < s_loss) return;
    const uint64_t base = s_now + 10 + (s_jitter ? rnd() % (unsigned)s_jitter : 0);
    enqueue(to, p, n, base);
    if ((int)(rnd() % 100) < s_dup) enqueue(to, p, n, base + rnd() % 40);
}

static psnr_rudp* s_end[2];

/* One step of the clock: what is due arrives, then both ends tick. */
static void step(unsigned ms)
{
    s_now += ms;
    for (int i = 0; i < s_qn;) {
        if (s_q[i].at <= s_now) {
            struct pkt p = s_q[i];
            s_q[i] = s_q[--s_qn];
            if (s_end[p.to]) psnr_rudp_input(s_end[p.to], p.b, p.len, s_now);
        } else {
            i++;
        }
    }
    for (int e = 0; e < 2; e++) if (s_end[e]) psnr_rudp_tick(s_end[e], s_now);
}

static void fresh(uint32_t session)
{
    for (int e = 0; e < 2; e++) psnr_rudp_free(s_end[e]);
    s_qn = 0;
    s_end[0] = psnr_rudp_new(session, link_send, (void*)(intptr_t)1, s_now);   /* 0 sends to 1 */
    s_end[1] = psnr_rudp_new(session, link_send, (void*)(intptr_t)0, s_now);
    s_sent[0] = s_sent[1] = s_bytes_on_wire[0] = s_bytes_on_wire[1] = 0;
}

/* Streams of n bytes both ways at once, written in pieces of up to `piece`; 1 if both arrive whole and in order
 * within `limit_ms` of the clock. */
static int transfer(size_t n, size_t piece, uint64_t limit_ms, uint64_t* took)
{
    uint8_t* src[2];
    uint8_t* dst[2];
    size_t wrote[2] = {0, 0}, got[2] = {0, 0};
    for (int e = 0; e < 2; e++) {
        src[e] = (uint8_t*)malloc(n);
        dst[e] = (uint8_t*)malloc(n);
        for (size_t i = 0; i < n; i++) src[e][i] = (uint8_t)(rnd() >> 8);
    }
    const uint64_t start = s_now;
    while ((got[0] < n || got[1] < n) && s_now - start < limit_ms) {
        for (int e = 0; e < 2; e++) {
            if (wrote[e] < n) {
                size_t k = piece < n - wrote[e] ? piece : n - wrote[e];
                if (psnr_rudp_write(s_end[e], src[e] + wrote[e], k, s_now) == 0) wrote[e] += k;
            }
            got[1 - e] += psnr_rudp_read(s_end[1 - e], dst[1 - e] + got[1 - e], n - got[1 - e]);
        }
        step(5);
    }
    if (took) *took = s_now - start;
    const int ok = got[0] == n && got[1] == n && !memcmp(src[0], dst[1], n) && !memcmp(src[1], dst[0], n);
    for (int e = 0; e < 2; e++) { free(src[e]); free(dst[e]); }
    return ok;
}

static void test_streams(void)
{
    uint64_t took = 0;
    s_loss = s_dup = s_jitter = 0;
    fresh(0x11111111);
    CHECK(transfer(200000, 7000, 60000, &took));
    printf("clean link: 2 x 200000 bytes in %llu ms of the clock, %u + %u datagrams\n", (unsigned long long)took,
           s_sent[0], s_sent[1]);

    /* 20% lost, 10% copied, up to 80 ms of jitter (so reordered) */
    s_loss = 20; s_dup = 10; s_jitter = 80;
    fresh(0x22222222);
    CHECK(transfer(100000, 3000, 120000, &took));
    printf("bad link (20%% lost, 10%% copied, reordered): 2 x 100000 bytes in %llu ms, %u + %u datagrams\n",
           (unsigned long long)took, s_sent[0], s_sent[1]);
    CHECK(psnr_rudp_dead(s_end[0], s_now) == PSNR_RUDP_ALIVE && psnr_rudp_dead(s_end[1], s_now) == PSNR_RUDP_ALIVE);
    for (int i = 0; i < 200 && !(psnr_rudp_flushed(s_end[0]) && psnr_rudp_flushed(s_end[1])); i++) step(10);
    CHECK(psnr_rudp_flushed(s_end[0]) && psnr_rudp_flushed(s_end[1]));   /* (all acknowledged, in the end) */
    CHECK(psnr_rudp_write(s_end[0], "x", 1, s_now) == 0 && !psnr_rudp_flushed(s_end[0]));

    /* single bytes, and one write of a message far larger than a datagram (a psnr frame can be 64 KB) */
    fresh(0x33333333);
    CHECK(transfer(5000, 1, 60000, NULL));
    fresh(0x33333334);
    CHECK(transfer(65538, 65538, 60000, NULL));
    s_loss = s_dup = s_jitter = 0;
}

static void test_keepalive_and_timeouts(void)
{
    s_loss = s_dup = s_jitter = 0;
    fresh(0x44444444);
    CHECK(transfer(1000, 1000, 5000, NULL));
    /* idle for 2 minutes: keepalives both ways keep it */
    const unsigned before = s_sent[0];
    for (int i = 0; i < 120000 / 50; i++) step(50);
    CHECK(psnr_rudp_dead(s_end[0], s_now) == PSNR_RUDP_ALIVE && psnr_rudp_dead(s_end[1], s_now) == PSNR_RUDP_ALIVE);
    const unsigned kept = s_sent[0] - before;
    printf("idle 120 s: %u keepalives from one end\n", kept);
    CHECK(kept >= 20 && kept <= 30);   /* (one about every 5 s) */
    CHECK(psnr_rudp_due_ms(s_end[0], s_now, 10000) <= PSNR_RUDP_KEEPALIVE_MS);

    /* the link goes: with data waiting, over after 20 s without an ack */
    s_down = 1;
    const uint64_t cut = s_now;
    CHECK(psnr_rudp_write(s_end[0], "lost", 4, s_now) == 0);
    while (psnr_rudp_dead(s_end[0], s_now) == PSNR_RUDP_ALIVE && s_now - cut < 60000) step(50);
    printf("dead link, data waiting: over after %llu ms (%d)\n", (unsigned long long)(s_now - cut),
           psnr_rudp_dead(s_end[0], s_now));
    CHECK(psnr_rudp_dead(s_end[0], s_now) == PSNR_RUDP_UNACKED && s_now - cut >= 20000 && s_now - cut < 21000);
    /* the other end had nothing to send: over after 30 s of silence (its last keepalive came up to 5 s before the
     * cut) */
    while (psnr_rudp_dead(s_end[1], s_now) == PSNR_RUDP_ALIVE && s_now - cut < 60000) step(50);
    CHECK(psnr_rudp_dead(s_end[1], s_now) == PSNR_RUDP_SILENT && s_now - cut >= 25000 && s_now - cut < 31000);
    s_down = 0;

    /* a CLOSE ends the other end at once, and the closed one takes no more */
    fresh(0x55555555);
    CHECK(transfer(100, 100, 5000, NULL));
    psnr_rudp_close(s_end[0], PSNR_RUDP_BYE);
    step(20);
    CHECK(psnr_rudp_dead(s_end[1], s_now) == PSNR_RUDP_CLOSED && psnr_rudp_dead(s_end[0], s_now) == PSNR_RUDP_CLOSED);
    CHECK(psnr_rudp_write(s_end[0], "x", 1, s_now) == -1 && psnr_rudp_write(s_end[1], "x", 1, s_now) == -1);
}

static void test_garbage(void)
{
    s_loss = s_dup = s_jitter = 0;
    fresh(0x66666666);
    CHECK(transfer(3000, 3000, 5000, NULL));
    uint8_t p[PSNR_RUDP_DGRAM + 8];
    /* another session's data, a datagram longer than any, a short one, an ack for numbers never sent */
    memset(p, 0, sizeof p);
    memcpy(p, "PSNR\x08", 5);
    psnr_put32(p + 5, 0x66666667);
    psnr_put32(p + 9, 1);
    psnr_rudp_input(s_end[1], p, 100, s_now);
    psnr_put32(p + 5, 0x66666666);
    psnr_put32(p + 9, 4);
    psnr_rudp_input(s_end[1], p, sizeof p, s_now);
    psnr_rudp_input(s_end[1], p, 12, s_now);
    psnr_put32(p + 9, 0);
    psnr_put32(p + 13, 99999);
    psnr_rudp_input(s_end[0], p, PSNR_RUDP_HDR, s_now);
    /* a CLOSE of another session */
    uint8_t c[10];
    psnr_rudp_close_packet(c, 0x66666667, 0);
    psnr_rudp_input(s_end[1], c, sizeof c, s_now);
    CHECK(psnr_rudp_readable(s_end[1]) == 0 && psnr_rudp_dead(s_end[1], s_now) == PSNR_RUDP_ALIVE);
    CHECK(psnr_rudp_session_of(c, sizeof c) == 0x66666667 && psnr_rudp_session_of("PSNR\x01....", 9) == 0);
    /* and the session goes on as before */
    CHECK(transfer(3000, 500, 5000, NULL));
    /* too much written and not sent (a peer that stopped acknowledging): refused, not grown without end */
    s_down = 1;
    static uint8_t big[PSNR_RUDP_QUEUE_MAX];
    const int first = psnr_rudp_write(s_end[0], big, sizeof big, s_now);
    CHECK(first == -1 || psnr_rudp_write(s_end[0], big, sizeof big, s_now) == -1);
    s_down = 0;
}

/* ---- the introductions' packets ---- */

static void test_packets(void)
{
    uint8_t b[128];
    psnr_intro_pkt k;
    const uint8_t lan[4] = {192, 168, 1, 10}, pub[4] = {203, 0, 113, 7}, up[4] = {198, 51, 100, 9};
    psnr_cand c[3] = { psnr_cand_of(lan, 36100), psnr_cand_of(up, 36100), psnr_cand_of(pub, 40001) };

    CHECK(psnr_intro_register(b, 3, 0xABCDE12345ull, 0x1122334455667788ull, 36100, c, 2) == 64);
    CHECK(psnr_intro_read(b, 64, &k) && k.type == PSNR_PKT_REGISTER && k.version == 3 && k.room == 0xABCDE12345ull &&
          k.owner == 0x1122334455667788ull && k.room_port == 36100 && k.n == 2 && !memcmp(k.cands[1].ip, up, 4) &&
          k.cands[0].port == 36100 && k.cands[0].family == 4);
    CHECK(!psnr_intro_read(b, 40, &k));   /* (a request must be padded) */
    CHECK(psnr_intro_unregister(b, 7, 8) == 64 && psnr_intro_read(b, 64, &k) && k.room == 7 && k.owner == 8);
    CHECK(psnr_intro_lookup(b, 3, 0xABCDE12345ull, 0xCAFEF00D, &c[0]) == 64);
    CHECK(psnr_intro_read(b, 64, &k) && k.type == PSNR_PKT_LOOKUP && k.nonce == 0xCAFEF00D && k.n == 1 &&
          !memcmp(k.cands[0].ip, lan, 4));
    size_t n = psnr_intro_registered(b, 0xABCDE12345ull, PSNR_INTRO_OK, &c[2], 20);
    CHECK(n == 22 && psnr_intro_read(b, n, &k) && k.status == 0 && k.refresh == 20 && k.seen.port == 40001);
    n = psnr_intro_found(b, 0xABCDE12345ull, 0xCAFEF00D, PSNR_INTRO_OK, PSNR_INTRO_SAME_NETWORK, &c[2], 36100, c, 3);
    CHECK(n == 50 && n <= 64);
    CHECK(psnr_intro_read(b, n, &k) && k.type == PSNR_PKT_FOUND && k.flags == PSNR_INTRO_SAME_NETWORK && k.n == 3 &&
          k.room_port == 36100 && !memcmp(k.cands[2].ip, pub, 4) && k.seen.port == 40001);
    n = psnr_intro_found(b, 5, 6, PSNR_INTRO_NO_ROOM, 0, NULL, 0, NULL, 0);
    CHECK(psnr_intro_read(b, n, &k) && k.status == PSNR_INTRO_NO_ROOM && k.n == 0);
    CHECK(psnr_intro_read(b, 18, &k) && k.status == PSNR_INTRO_NO_ROOM);   /* (a refusal may end at its status) */
    n = psnr_intro_intro(b, 0xABCDE12345ull, 0xCAFEF00D, 0, c, 2);
    CHECK(n == 33 && psnr_intro_read(b, n, &k) && k.type == PSNR_PKT_INTRO && k.n == 2 && k.nonce == 0xCAFEF00D);
    CHECK(psnr_intro_knock(b, 9, 10) == 17 && psnr_intro_read(b, 17, &k) && k.room == 9 && k.nonce == 10);
    CHECK(psnr_intro_open(b, 9, 0x0102030405060708ull) == 64 && psnr_intro_read(b, 64, &k) &&
          k.nonce64 == 0x0102030405060708ull && k.room == 9);
    CHECK(psnr_intro_cookie(b, 1, 2) == 21 && psnr_intro_read(b, 21, &k) && k.nonce64 == 1 && k.cookie == 2);
    CHECK(psnr_intro_open2(b, 1, 2) == 64 && psnr_intro_read(b, 64, &k) && k.type == PSNR_PKT_OPEN2 && k.cookie == 2);
    CHECK(psnr_intro_opened(b, 1, 77) == 17 && psnr_intro_read(b, 17, &k) && k.session == 77);
    /* too many candidates, a family that isn't 4, cut short, not ours */
    n = psnr_intro_intro(b, 1, 2, 0, c, 2);
    b[18] = 3;
    CHECK(!psnr_intro_read(b, n, &k));
    n = psnr_intro_knock(b, 9, 10);
    CHECK(!psnr_intro_read(b, n - 1, &k));
    psnr_intro_lookup(b, 3, 1, 2, &c[0]);
    b[20] = 6;
    CHECK(!psnr_intro_read(b, 64, &k));
    CHECK(!psnr_intro_read("XSNR\x10", 5, &k) && psnr_udp_type("PSNR\x92", 5) == 0x92);

    /* the cookie: the same for the same inputs, another for any other address, port, nonce or minute or secret */
    uint8_t secret[32], other[32];
    for (int i = 0; i < 32; i++) { secret[i] = (uint8_t)i; other[i] = (uint8_t)(i + 1); }
    const uint64_t ck = psnr_intro_cookie_of(secret, pub, 40001, 555, 1000);
    CHECK(ck == psnr_intro_cookie_of(secret, pub, 40001, 555, 1000));
    CHECK(ck != psnr_intro_cookie_of(secret, lan, 40001, 555, 1000));
    CHECK(ck != psnr_intro_cookie_of(secret, pub, 40002, 555, 1000));
    CHECK(ck != psnr_intro_cookie_of(secret, pub, 40001, 556, 1000));
    CHECK(ck != psnr_intro_cookie_of(secret, pub, 40001, 555, 1001));
    CHECK(ck != psnr_intro_cookie_of(other, pub, 40001, 555, 1000));
    CHECK(psnr_intro_random() != psnr_intro_random());
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    s_now = 1000000;
    test_streams();
    test_keepalive_and_timeouts();
    test_garbage();
    test_packets();
    for (int e = 0; e < 2; e++) psnr_rudp_free(s_end[e]);
    printf(s_fail ? "test_psnr_rudp: %d FAILED\n" : "test_psnr_rudp: all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
