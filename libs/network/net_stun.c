/*
 * net_stun.c - the public IPv4 address from a STUN server (net_stun.h).
 *
 * RFC 5389, the client side of one Binding request over UDP: a 20-byte
 * header (type 0x0001, length 0, the magic cookie, a 12-byte transaction id)
 * and no attributes. The answer's XOR-MAPPED-ADDRESS (0x0020; 0x8020 on some
 * old servers) holds the address the request came from, XORed with the
 * cookie; MAPPED-ADDRESS (0x0001) is the RFC 3489 form without the XOR.
 */
#ifdef _WIN32
#  define _CRT_RAND_S   /* rand_s, before stdlib.h */
#endif
#include "ps3emu/net_stun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* WSAPoll */
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
   typedef SOCKET stun_sock;
#  define STUN_BAD INVALID_SOCKET
#  define stun_close closesocket
#  define stun_poll WSAPoll
   typedef WSAPOLLFD stun_pollfd;
#else
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  include <poll.h>
#  include <fcntl.h>
   typedef int stun_sock;
#  define STUN_BAD (-1)
#  define stun_close close
#  define stun_poll poll
   typedef struct pollfd stun_pollfd;
#endif

#define STUN_MAGIC 0x2112A442u

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

size_t net_stun_request(uint8_t out[NET_STUN_REQUEST_LEN], const uint8_t txid[12])
{
    out[0] = 0x00; out[1] = 0x01;   /* Binding request */
    out[2] = 0x00; out[3] = 0x00;   /* no attributes */
    out[4] = (uint8_t)(STUN_MAGIC >> 24); out[5] = (uint8_t)(STUN_MAGIC >> 16);
    out[6] = (uint8_t)(STUN_MAGIC >> 8);  out[7] = (uint8_t)STUN_MAGIC;
    memcpy(out + 8, txid, 12);
    return NET_STUN_REQUEST_LEN;
}

int net_stun_parse(const uint8_t* p, size_t n, const uint8_t txid[12], uint8_t ip[4], uint16_t* port)
{
    if (n < 20 || rd16(p) != 0x0101 || rd32(p + 4) != STUN_MAGIC || memcmp(p + 8, txid, 12) != 0)
        return NET_STUN_ERR_BAD_ANSWER;
    const size_t len = rd16(p + 2);
    if ((len & 3) || 20 + len > n) return NET_STUN_ERR_BAD_ANSWER;
    int have = 0;   /* 2: XOR-MAPPED-ADDRESS found, 1: only MAPPED-ADDRESS */
    uint8_t got_ip[4] = {0};
    uint16_t got_port = 0;
    for (size_t at = 20; at + 4 <= 20 + len;) {
        const uint16_t type = rd16(p + at), alen = rd16(p + at + 2);
        const uint8_t* v = p + at + 4;
        if (at + 4 + alen > 20 + len) return NET_STUN_ERR_BAD_ANSWER;
        const int xored = type == 0x0020 || type == 0x8020;
        if ((xored || type == 0x0001) && alen >= 8 && v[1] == 0x01 && (xored ? 2 : 1) > have) {   /* family 1: IPv4 */
            uint16_t pt = rd16(v + 2);
            uint32_t a = rd32(v + 4);
            if (xored) {
                pt ^= (uint16_t)(STUN_MAGIC >> 16);
                a ^= STUN_MAGIC;
            }
            got_ip[0] = (uint8_t)(a >> 24); got_ip[1] = (uint8_t)(a >> 16);
            got_ip[2] = (uint8_t)(a >> 8);  got_ip[3] = (uint8_t)a;
            got_port = pt;
            have = xored ? 2 : 1;
        }
        at += 4 + (((size_t)alen + 3) & ~(size_t)3);
    }
    if (!have) return NET_STUN_ERR_BAD_ANSWER;
    memcpy(ip, got_ip, 4);
    if (port) *port = got_port;
    return NET_STUN_OK;
}

const char* net_stun_error_text(int rc)
{
    switch (rc) {
    case NET_STUN_OK:             return "ok";
    case NET_STUN_ERR_RESOLVE:    return "the STUN server's name did not resolve";
    case NET_STUN_ERR_SOCKET:     return "no network socket for the request";
    case NET_STUN_ERR_TIMEOUT:    return "the STUN server did not answer";
    case NET_STUN_ERR_BAD_ANSWER: return "the STUN server's answer had no address";
    default:                      return "unknown";
    }
}

/* 12 bytes no other request uses (they only match the answer to the request) */
static void stun_txid(uint8_t txid[12])
{
    static unsigned s_count;
#ifdef _WIN32
    for (int i = 0; i < 12; i += 4) {
        unsigned r = 0;
        if (rand_s(&r) != 0) r = (unsigned)time(NULL) * 2654435761u + (unsigned)i + ++s_count;
        memcpy(txid + i, &r, 4);
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    const int got = f && fread(txid, 1, 12, f) == 12;
    if (f) fclose(f);
    if (!got) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        for (int i = 0; i < 12; i++) txid[i] = (uint8_t)((tv.tv_usec >> (i & 7)) ^ (tv.tv_sec >> i) ^ (++s_count * 31u));
    }
#endif
}

static long long stun_now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

int net_stun_public_ipv4(const char* host, uint16_t port, int timeout_ms, char out_ip[16], uint16_t* out_port)
{
    if (!host || !*host || !out_ip) return NET_STUN_ERR_RESOLVE;
    out_ip[0] = 0;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NET_STUN_ERR_SOCKET;
#endif
    int rc = NET_STUN_ERR_RESOLVE;
    stun_sock s = STUN_BAD;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_text[8];
    snprintf(port_text, sizeof port_text, "%u", (unsigned)port);
    if (getaddrinfo(host, port_text, &hints, &res) != 0 || !res) goto done;
    rc = NET_STUN_ERR_SOCKET;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == STUN_BAD) goto done;
    {
        uint8_t txid[12], req[NET_STUN_REQUEST_LEN], buf[576];
        stun_txid(txid);
        net_stun_request(req, txid);
        const long long t0 = stun_now_ms(), end = t0 + (timeout_ms > 0 ? timeout_ms : 3000);
        long long next_send = t0;
        int rto = 500, bad = 0;
        rc = NET_STUN_ERR_TIMEOUT;
        for (;;) {
            long long now = stun_now_ms();
            if (now >= end) break;
            if (now >= next_send) {
                if (sendto(s, (const char*)req, (int)sizeof req, 0, res->ai_addr, (int)res->ai_addrlen) < 0) {
                    rc = NET_STUN_ERR_SOCKET;
                    break;
                }
                next_send = now + rto;
                rto *= 2;
            }
            long long wait = (next_send < end ? next_send : end) - now;
            if (wait < 1) wait = 1;
            stun_pollfd pf;
            memset(&pf, 0, sizeof pf);
            pf.fd = s;
            pf.events = POLLIN;
            if (stun_poll(&pf, 1, (int)wait) <= 0) continue;
            const int got = (int)recv(s, (char*)buf, (int)sizeof buf, 0);
            if (got <= 0) continue;   /* (an ICMP "port unreachable" on some systems): keep waiting */
            uint8_t ip[4];
            uint16_t pt = 0;
            if (net_stun_parse(buf, (size_t)got, txid, ip, &pt) == NET_STUN_OK) {
                snprintf(out_ip, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                if (out_port) *out_port = pt;
                rc = NET_STUN_OK;
                break;
            }
            bad = 1;   /* not ours or not usable: wait for another */
        }
        if (rc == NET_STUN_ERR_TIMEOUT && bad) rc = NET_STUN_ERR_BAD_ANSWER;
    }
done:
    if (s != STUN_BAD) stun_close(s);
    if (res) freeaddrinfo(res);
#ifdef _WIN32
    WSACleanup();
#endif
    return rc;
}
