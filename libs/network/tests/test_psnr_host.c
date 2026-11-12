/*
 * test_psnr_host - the in-process room server (psnr/psnr_host.c) against the
 * real client (psnr/psnr.c) on loopback: the key, rooms, pushes, the probe,
 * the relay, a reconnect and the ban; and the invite code (net_invite.c).
 *
 *   clang-cl /I include /Fe:test_psnr_host.exe libs/network/tests/test_psnr_host.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_psnr_host libs/network/tests/test_psnr_host.c -lpthread
 */
#ifdef _WIN32
#  define _CRT_RAND_S   /* net_invite.c's rand_s, before the first stdlib.h of this one file */
#endif
#include "../psnr/psnr.c"
#include "../psnr/psnr_host.c"
#include "../net_invite.c"

#include <assert.h>
#ifdef _WIN32
#  define sleep_ms(n) Sleep(n)
#else
#  include <unistd.h>
#  define sleep_ms(n) usleep((n) * 1000)
#endif

/* psnr.c's TLS calls: plain TCP here */
int ps3_tls_enabled(void) { return 0; }
int ps3_tls_server_name(char* out, size_t cap) { (void)out; (void)cap; return -1; }
ps3_tls* ps3_tls_connect(intptr_t s, const char* n, int ms, char* why, size_t cap)
{ (void)s; (void)n; (void)ms; (void)why; (void)cap; return NULL; }
int ps3_tls_send_all(ps3_tls* t, const void* p, size_t n, int ms) { (void)t; (void)p; (void)n; (void)ms; return -1; }
int ps3_tls_recv(ps3_tls* t, void* p, size_t n, int ms) { (void)t; (void)p; (void)n; (void)ms; return -1; }
void ps3_tls_close(ps3_tls* t) { (void)t; }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail++; } } while (0)

static const char kComm[] = "NPWR00000_00";

/* the next push of this type within ms, or 0 */
static int wait_push(psnr_client* c, uint8_t type, psnr_msg* out, int ms)
{
    for (int t = 0; t < ms; t += 10) {
        psnr_msg m;
        while (psnr_poll(c, &m) == 1) {
            if (m.type == type) { *out = m; return 1; }
            psnr_msg_free(&m);
        }
        sleep_ms(10);
    }
    return 0;
}

static ph_sock udp_socket(uint16_t* port)
{
    ph_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr*)&a, sizeof a);
    ph_socklen n = (ph_socklen)sizeof a;
    getsockname(s, (struct sockaddr*)&a, &n);
    *port = ntohs(a.sin_port);
    return s;
}

static void udp_send(ph_sock s, const void* p, size_t n, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    sendto(s, (const char*)p, (int)n, 0, (struct sockaddr*)&a, sizeof a);
}

static int udp_recv(ph_sock s, uint8_t* buf, int cap, int ms)
{
    ph_pollfd pf;
    pf.fd = s; pf.events = POLLIN; pf.revents = 0;
    if (PH_POLL(&pf, 1, ms) <= 0) return -1;
    return (int)recv(s, (char*)buf, cap, 0);
}

static void test_invite(void)
{
    net_invite in = { {203, 0, 113, 7}, 36100, 0x0ABCDEF }, out;
    char code[NET_INVITE_TEXT];
    net_invite_encode(&in, code);
    printf("invite code: %s\n", code);
    CHECK(strlen(code) == 19 && code[4] == '-' && code[9] == '-' && code[14] == '-');
    for (const char* p = code; *p; p++) CHECK(!strchr("ILOU", *p));
    memset(&out, 0, sizeof out);
    CHECK(net_invite_decode(code, &out) == 0);
    CHECK(!memcmp(out.ip, in.ip, 4) && out.port == in.port && out.key == in.key);
    /* lower case, no dashes, spaces around it */
    char loose[40];
    int n = 0;
    loose[n++] = ' ';
    for (const char* p = code; *p; p++) if (*p != '-') loose[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    loose[n++] = '\n';
    loose[n] = 0;
    CHECK(net_invite_decode(loose, &out) == 0 && out.key == in.key);
    /* O for 0, I and L for 1 */
    char look[NET_INVITE_TEXT];
    memcpy(look, code, sizeof look);
    for (char* p = look; *p; p++) { if (*p == '0') *p = 'O'; else if (*p == '1') *p = 'l'; }
    CHECK(net_invite_decode(look, &out) == 0 && out.port == in.port);
    /* one character changed: almost always caught */
    int caught = 0, tries = 0;
    for (int i = 0; i < 19; i++) {
        if (code[i] == '-') continue;
        for (const char* a = kAlphabet; *a; a++) {
            if (*a == code[i]) continue;
            char t[NET_INVITE_TEXT];
            memcpy(t, code, sizeof t);
            t[i] = *a;
            tries++;
            caught += net_invite_decode(t, &out) == NET_INVITE_TYPO;
        }
    }
    printf("one-character typos caught: %d of %d\n", caught, tries);
    CHECK(caught * 100 >= tries * 85);
    CHECK(net_invite_decode("7KQ2-M9XD", &out) == NET_INVITE_NOT_A_CODE);
    CHECK(net_invite_decode("7KQ2-M9XD-0RTB-4HWU", &out) == NET_INVITE_NOT_A_CODE);
    CHECK(net_invite_decode("", &out) == NET_INVITE_NOT_A_CODE);
    /* keys: 28 bits, never 0, the blob and the hex agree */
    for (int i = 0; i < 100; i++) {
        uint32_t k = net_invite_new_key();
        CHECK(k && !(k & ~NET_INVITE_KEY_MASK));
    }
    uint8_t blob[32], blob2[32];
    char hex[65];
    net_invite_key_blob(0x1234567, blob);
    net_invite_key_blob(0x1234567, blob2);
    CHECK(!memcmp(blob, blob2, 32));
    net_invite_key_blob(0x1234568, blob2);
    CHECK(memcmp(blob, blob2, 32) != 0);
    net_invite_key_hex(0x1234567, hex);
    CHECK(strlen(hex) == 64);
    char two[3] = { hex[0], hex[1], 0 };
    CHECK(strtoul(two, NULL, 16) == blob[0]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_invite();

    psnr_host_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.has_key = 1;
    net_invite_key_blob(0x0C0FFEE, cfg.key);
    cfg.bind_loopback = 1;
    char err[128] = "";
    psnr_host* h = psnr_host_start(&cfg, err, sizeof err);
    CHECK(h != NULL);
    if (!h) { printf("start: %s\n", err); return 1; }
    const uint16_t port = psnr_host_port(h);
    CHECK(port != 0);

    /* the key */
    uint8_t wrong[32];
    net_invite_key_blob(0x0BADBAD, wrong);
    uint32_t uid = 0;
    uint8_t ip[4];
    CHECK(psnr_connect("127.0.0.1", port, kComm, "Mallory", 3700, wrong, &uid, ip) == NULL);
    CHECK(psnr_connect_error() == PSNR_E_REFUSED);
    CHECK(psnr_connect("127.0.0.1", port, kComm, "Mallory", 3700, NULL, &uid, ip) == NULL);
    CHECK(psnr_connect_error() == PSNR_E_REFUSED);

    uint32_t ida = 0, idb = 0;
    psnr_client* a = psnr_connect("127.0.0.1", port, kComm, "Alpha", 3701, cfg.key, &ida, ip);
    psnr_client* b = psnr_connect("127.0.0.1", port, kComm, "Bravo", 3702, cfg.key, &idb, ip);
    CHECK(a && b && ida && idb && ida != idb);
    if (!a || !b) return 1;
    CHECK(psnr_relay_available(a) && psnr_relay_available(b));
    CHECK(ip[0] == 127);
    sleep_ms(50);
    CHECK(psnr_host_players(h) == 2);

    /* A makes a room: max u8 | flags u32 | external blob | internal blob | own data blob */
    uint8_t body[64], *p = body;
    *p++ = 4;
    p = psnr_put32(p, 0);
    p = psnr_put16(p, 2); *p++ = 'E'; *p++ = 'X';
    p = psnr_put16(p, 1); *p++ = 'I';
    p = psnr_put16(p, 1); *p++ = 'a';
    psnr_msg m;
    CHECK(psnr_call(a, PSNR_CREATE_ROOM, body, (uint32_t)(p - body), &m, 2000) == 1);
    CHECK(m.type == PSNR_ROOM_JOINED && m.len >= 14);
    const uint64_t room = psnr_get64(m.data);
    CHECK(room != 0 && psnr_get16(m.data + 8) == 1 && psnr_get16(m.data + 10) == 1);
    psnr_msg_free(&m);

    /* a search finds it */
    p = body;
    p = psnr_put16(p, 0);
    p = psnr_put16(p, 10);
    CHECK(psnr_call(b, PSNR_SEARCH_ROOMS, body, 4, &m, 2000) == 1);
    CHECK(m.type == PSNR_ROOM_LIST && psnr_get16(m.data) == 1 && psnr_get16(m.data + 2) == 1);
    CHECK(psnr_get64(m.data + 4) == room && !memcmp(m.data + 12, "Alpha", 6));
    psnr_msg_free(&m);

    /* B joins: both members in the reply; A hears of B; both are told to relay datagrams */
    p = body;
    p = psnr_put64(p, room);
    p = psnr_put16(p, 1); *p++ = 'b';
    CHECK(psnr_call(b, PSNR_JOIN_ROOM, body, (uint32_t)(p - body), &m, 2000) == 1);
    /* room u64 | me u16 | owner u16 | max u8 | flags u32 | "EX" blob | "I" blob | count u8 at 24 */
    CHECK(m.type == PSNR_ROOM_JOINED && psnr_get16(m.data + 8) == 2 && m.len > 24 && m.data[24] == 2);
    psnr_msg_free(&m);
    CHECK(wait_push(a, PSNR_MEMBER_JOINED, &m, 2000));
    CHECK(psnr_get64(m.data) == room && psnr_get16(m.data + 8) == 2 && psnr_get32(m.data + 10) == idb);
    CHECK(!memcmp(m.data + 14, "Bravo", 6));
    CHECK(psnr_get16(m.data + 14 + 16 + 4) == 3702);   /* B's address for A: its declared P2P port (no probe yet) */
    psnr_msg_free(&m);
    CHECK(wait_push(a, PSNR_ROUTE, &m, 2000));
    CHECK(psnr_get32(m.data + 8) == idb && (m.data[12] & PSNR_ROUTE_UDP_RELAY));
    psnr_msg_free(&m);
    CHECK(wait_push(b, PSNR_ROUTE, &m, 2000));
    CHECK(psnr_get32(m.data + 8) == ida && (m.data[12] & PSNR_ROUTE_UDP_RELAY));
    psnr_msg_free(&m);

    /* probes from each P2P socket, then a datagram A -> B through the host */
    uint16_t pa, pb;
    ph_sock ua = udp_socket(&pa), ub = udp_socket(&pb);
    uint8_t pkt[256], rx[256];
    psnr_probe_packet(a, pkt);
    udp_send(ua, pkt, PSNR_UDP_PROBE_LEN, port);
    int got = udp_recv(ua, rx, sizeof rx, 2000);
    uint8_t pip[4];
    uint16_t pport = 0;
    CHECK(got > 0 && psnr_probe_reply(rx, (size_t)got, pip, &pport) && pport == pa && pip[0] == 127);
    psnr_probe_packet(b, pkt);
    udp_send(ub, pkt, PSNR_UDP_PROBE_LEN, port);
    got = udp_recv(ub, rx, sizeof rx, 2000);
    CHECK(got > 0 && psnr_probe_reply(rx, (size_t)got, pip, &pport) && pport == pb);
    size_t n = psnr_relay_wrap(a, idb, "hello", 5, pkt, sizeof pkt);
    udp_send(ua, pkt, n, port);
    got = udp_recv(ub, rx, sizeof rx, 2000);
    uint32_t from = 0;
    const uint8_t* pay = NULL;
    size_t plen = 0;
    CHECK(got > 0 && psnr_relay_unwrap(rx, (size_t)got, &from, &pay, &plen));
    CHECK(from == ida && plen == 5 && !memcmp(pay, "hello", 5));
    /* a relay with a wrong token goes nowhere */
    n = psnr_relay_wrap(a, idb, "nope", 4, pkt, sizeof pkt);
    pkt[9] ^= 0x55;
    udp_send(ua, pkt, n, port);
    CHECK(udp_recv(ub, rx, sizeof rx, 300) < 0);

    /* a room message from A reaches B */
    p = body;
    p = psnr_put64(p, room);
    p = psnr_put16(p, 0);
    p = psnr_put16(p, 3); *p++ = 'm'; *p++ = 's'; *p++ = 'g';
    CHECK(psnr_call(a, PSNR_ROOM_MESSAGE, body, (uint32_t)(p - body), &m, 2000) == 1 && m.type == PSNR_OK);
    psnr_msg_free(&m);
    CHECK(wait_push(b, PSNR_ROOM_MSG, &m, 2000));
    CHECK(psnr_get64(m.data) == room && psnr_get16(m.data + 8) == 1 && !memcmp(m.data + 14, "msg", 3));
    psnr_msg_free(&m);

    /* a hosted game's own message from B reaches A, with B's user number; one too long is refused */
    p = body;
    p = psnr_put16(p, 4); *p++ = 'f'; *p++ = 'o'; *p++ = 'l'; *p++ = 'w';
    CHECK(psnr_call(b, PSNR_APP_SEND, body, (uint32_t)(p - body), &m, 2000) == 1 && m.type == PSNR_OK);
    psnr_msg_free(&m);
    CHECK(wait_push(a, PSNR_APP_MSG, &m, 2000));
    CHECK(m.len == 10 && psnr_get32(m.data) == idb && psnr_get16(m.data + 4) == 4 && !memcmp(m.data + 6, "folw", 4));
    psnr_msg_free(&m);
    {
        static uint8_t big[2 + PSNR_APP_MAX + 1];
        psnr_put16(big, PSNR_APP_MAX + 1);
        CHECK(psnr_call(b, PSNR_APP_SEND, big, sizeof big, &m, 2000) == 1 && m.type == PSNR_ERROR);
        psnr_msg_free(&m);
    }

    /* only the owner sets room data */
    p = body;
    p = psnr_put64(p, room);
    *p++ = 1;
    p = psnr_put16(p, 1); *p++ = 'x';
    CHECK(psnr_call(b, PSNR_SET_ROOM_DATA, body, (uint32_t)(p - body), &m, 2000) == 1 && m.type == PSNR_ERROR &&
          psnr_get32(m.data) == PSNR_E_NOT_OWNER);
    psnr_msg_free(&m);

    /* B leaves: A hears it */
    p = body;
    p = psnr_put64(p, room);
    CHECK(psnr_call(b, PSNR_LEAVE_ROOM, body, 8, &m, 2000) == 1 && m.type == PSNR_OK);
    psnr_msg_free(&m);
    CHECK(wait_push(a, PSNR_MEMBER_LEFT, &m, 2000));
    CHECK(psnr_get64(m.data) == room && psnr_get16(m.data + 8) == 2 && psnr_get16(m.data + 10) == 1);
    psnr_msg_free(&m);

    /* the same name from the same address is a reconnect: the old connection goes */
    psnr_client* a2 = psnr_connect("127.0.0.1", port, kComm, "ALPHA", 3701, cfg.key, &uid, ip);
    CHECK(a2 != NULL);
    int gone = 0;
    for (int t = 0; t < 200 && !gone; t++) {
        psnr_msg x;
        int r = psnr_poll(a, &x);
        if (r == 1) psnr_msg_free(&x);
        gone = r < 0;
        sleep_ms(10);
    }
    CHECK(gone);
    psnr_close(a);

    /* wrong keys: after a few the address is ignored, even with the right key */
    for (int i = 0; i < PH_BAD_KEYS; i++) psnr_connect("127.0.0.1", port, kComm, "Mallory", 3700, wrong, &uid, ip);
    CHECK(psnr_connect("127.0.0.1", port, kComm, "Charlie", 3703, cfg.key, &uid, ip) == NULL);

    psnr_close(a2);
    psnr_close(b);
    ph_close(ua);
    ph_close(ub);
    psnr_host_stop(h);
    printf(s_fail ? "test_psnr_host: %d FAILED\n" : "test_psnr_host: all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
