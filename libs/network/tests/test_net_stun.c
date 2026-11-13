/*
 * test_net_stun - the STUN client (net_stun.c): the request's bytes, and
 * answers built here (XOR-MAPPED-ADDRESS, MAPPED-ADDRESS, an old server's
 * 0x8020, and the ones it must refuse). No network.
 *
 *   clang-cl /I include /Fe:test_net_stun.exe libs/network/tests/test_net_stun.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_net_stun libs/network/tests/test_net_stun.c
 *
 * `test_net_stun live <host> <port>` also asks a real server and prints its answer (by hand only: it sends
 * this machine's address to that server).
 */
#include "../net_stun.c"

#include <assert.h>

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail++; } } while (0)

static const uint8_t kTx[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

/* an answer: header (type) + attributes */
static size_t answer(uint8_t* out, uint16_t type, const uint8_t* tx, const uint8_t* attrs, size_t alen)
{
    out[0] = (uint8_t)(type >> 8); out[1] = (uint8_t)type;
    out[2] = (uint8_t)(alen >> 8); out[3] = (uint8_t)alen;
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;
    memcpy(out + 8, tx, 12);
    memcpy(out + 20, attrs, alen);
    return 20 + alen;
}

/* an address attribute for 203.0.113.7:54321, XORed or not */
static size_t addr_attr(uint8_t* out, uint16_t type, int xored)
{
    uint16_t port = 54321;
    uint32_t a = (203u << 24) | (0u << 16) | (113u << 8) | 7u;
    if (xored) { port ^= 0x2112; a ^= 0x2112A442u; }
    out[0] = (uint8_t)(type >> 8); out[1] = (uint8_t)type;
    out[2] = 0; out[3] = 8;
    out[4] = 0; out[5] = 1;   /* IPv4 */
    out[6] = (uint8_t)(port >> 8); out[7] = (uint8_t)port;
    out[8] = (uint8_t)(a >> 24); out[9] = (uint8_t)(a >> 16); out[10] = (uint8_t)(a >> 8); out[11] = (uint8_t)a;
    return 12;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t req[NET_STUN_REQUEST_LEN];
    CHECK(net_stun_request(req, kTx) == 20);
    CHECK(req[0] == 0 && req[1] == 1 && req[2] == 0 && req[3] == 0);
    CHECK(req[4] == 0x21 && req[5] == 0x12 && req[6] == 0xA4 && req[7] == 0x42 && !memcmp(req + 8, kTx, 12));

    uint8_t a[64], m[128], ip[4];
    uint16_t port = 0;
    size_t an, mn;

    /* XOR-MAPPED-ADDRESS, after an attribute it doesn't know (SOFTWARE, 5 bytes: padded to 8) */
    an = 0;
    a[an++] = 0x80; a[an++] = 0x22; a[an++] = 0; a[an++] = 5;
    memcpy(a + an, "abcde\0\0\0", 8); an += 8;
    an += addr_attr(a + an, 0x0020, 1);
    mn = answer(m, 0x0101, kTx, a, an);
    memset(ip, 0, 4);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_OK);
    CHECK(ip[0] == 203 && ip[1] == 0 && ip[2] == 113 && ip[3] == 7 && port == 54321);

    /* MAPPED-ADDRESS only (RFC 3489 servers), and an old server's XOR-MAPPED-ADDRESS (0x8020) */
    an = addr_attr(a, 0x0001, 0);
    mn = answer(m, 0x0101, kTx, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_OK && ip[0] == 203 && ip[3] == 7 && port == 54321);
    an = addr_attr(a, 0x8020, 1);
    mn = answer(m, 0x0101, kTx, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_OK && ip[2] == 113 && port == 54321);

    /* both: the XORed one wins (a NAT that rewrites addresses in packets can spoil the plain one) */
    an = addr_attr(a, 0x0001, 0);
    a[8] = 10;   /* plain one says 10.0.113.7 */
    an += addr_attr(a + an, 0x0020, 1);
    mn = answer(m, 0x0101, kTx, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_OK && ip[0] == 203);

    /* refused: another transaction, an error answer, no address, IPv6 only, truncated, a length past the end */
    uint8_t other[12];
    memcpy(other, kTx, 12);
    other[11] ^= 1;
    an = addr_attr(a, 0x0020, 1);
    mn = answer(m, 0x0101, other, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);
    mn = answer(m, 0x0111, kTx, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);
    mn = answer(m, 0x0101, kTx, a, 0);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);
    an = addr_attr(a, 0x0020, 1);
    a[5] = 2;   /* IPv6 */
    mn = answer(m, 0x0101, kTx, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);
    an = addr_attr(a, 0x0020, 1);
    mn = answer(m, 0x0101, kTx, a, an);
    CHECK(net_stun_parse(m, mn - 4, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);
    CHECK(net_stun_parse(m, 12, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);
    a[3] = 40;   /* the attribute says 40 bytes: past the message */
    mn = answer(m, 0x0101, kTx, a, an);
    CHECK(net_stun_parse(m, mn, kTx, ip, &port) == NET_STUN_ERR_BAD_ANSWER);

    /* a name that can't resolve, and nothing to ask */
    char out[16];
    CHECK(net_stun_public_ipv4("", 3478, 500, out, NULL) == NET_STUN_ERR_RESOLVE);
    CHECK(net_stun_public_ipv4("name.invalid", 3478, 500, out, NULL) == NET_STUN_ERR_RESOLVE);
    CHECK(strcmp(net_stun_error_text(NET_STUN_ERR_TIMEOUT), "unknown") != 0);

    if (argc >= 4 && !strcmp(argv[1], "live")) {
        uint16_t pub = 0;
        const int rc = net_stun_public_ipv4(argv[2], (uint16_t)atoi(argv[3]), 3000, out, &pub);
        printf("live %s:%s -> %d (%s)%s%s, port %u\n", argv[2], argv[3], rc, net_stun_error_text(rc),
               rc == NET_STUN_OK ? ": " : "", rc == NET_STUN_OK ? out : "", (unsigned)pub);
        CHECK(rc == NET_STUN_OK);
    }
    printf(s_fail ? "test_net_stun: %d FAILED\n" : "test_net_stun: all passed\n", s_fail);
    return s_fail ? 1 : 0;
}
