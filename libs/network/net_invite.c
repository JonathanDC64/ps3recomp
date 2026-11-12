/*
 * net_invite.c - see ps3emu/net_invite.h.
 */
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#  define _CRT_RAND_S   /* rand_s: the system's random source */
#endif
#include "ps3emu/net_invite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static int symbol(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch == 'O') return 0;
    if (ch == 'I' || ch == 'L') return 1;
    const char* p = strchr(kAlphabet, ch);
    return ch && p ? (int)(p - kAlphabet) : -1;
}

/* the 4-bit check over the 76 bits before it (FNV-1a over the ten bytes, check nibble zero) */
static unsigned check(const uint8_t b[10])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 10; i++) h = (h ^ (i == 9 ? (uint8_t)(b[i] & 0xF0) : b[i])) * 16777619u;
    return (h ^ (h >> 4) ^ (h >> 8) ^ (h >> 16)) & 0xFu;
}

static void pack(const net_invite* in, uint8_t b[10])
{
    const uint32_t k = in->key & NET_INVITE_KEY_MASK;
    memcpy(b, in->ip, 4);
    b[4] = (uint8_t)(in->port >> 8);
    b[5] = (uint8_t)in->port;
    b[6] = (uint8_t)(k >> 20);
    b[7] = (uint8_t)(k >> 12);
    b[8] = (uint8_t)(k >> 4);
    b[9] = (uint8_t)((k & 0xF) << 4);
    b[9] = (uint8_t)(b[9] | check(b));
}

void net_invite_encode(const net_invite* in, char out[NET_INVITE_TEXT])
{
    uint8_t b[10];
    pack(in, b);
    int o = 0;
    for (int i = 0; i < 16; i++) {   /* 5 bits each, from the top */
        const int bit = i * 5;
        const unsigned v = ((unsigned)b[bit / 8] << 8 | (bit / 8 + 1 < 10 ? b[bit / 8 + 1] : 0)) >> (11 - bit % 8) & 31u;
        if (i && i % 4 == 0) out[o++] = '-';
        out[o++] = kAlphabet[v];
    }
    out[o] = 0;
}

int net_invite_decode(const char* text, net_invite* out)
{
    uint8_t b[10] = {0};
    int n = 0;
    for (const char* p = text; p && *p; p++) {
        if (*p == '-' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        const int v = symbol(*p);
        if (v < 0 || n >= 16) return NET_INVITE_NOT_A_CODE;
        const int bit = n * 5;
        const unsigned w = (unsigned)v << (11 - bit % 8);
        b[bit / 8] = (uint8_t)(b[bit / 8] | (w >> 8));
        if (bit / 8 + 1 < 10) b[bit / 8 + 1] = (uint8_t)(b[bit / 8 + 1] | (w & 0xFF));
        n++;
    }
    if (n != 16) return NET_INVITE_NOT_A_CODE;
    if ((b[9] & 0xF) != check(b)) return NET_INVITE_TYPO;
    memcpy(out->ip, b, 4);
    out->port = (uint16_t)(b[4] << 8 | b[5]);
    out->key = (uint32_t)b[6] << 20 | (uint32_t)b[7] << 12 | (uint32_t)b[8] << 4 | (uint32_t)(b[9] >> 4);
    return 0;
}

uint32_t net_invite_new_key(void)
{
    uint32_t v = 0;
#ifdef _WIN32
    unsigned int r = 0;
    if (rand_s(&r) == 0) v = r;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&v, sizeof v, 1, f) != 1) v = 0;
        fclose(f);
    }
#endif
    if (!v) v = (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)clock();   /* (no random source: still a key) */
    v &= NET_INVITE_KEY_MASK;
    return v ? v : 1;
}

void net_invite_key_blob(uint32_t key, uint8_t out[32])
{
    uint64_t z = 0x5EA41E55C0DE0000ull ^ (key & NET_INVITE_KEY_MASK);
    for (int i = 0; i < 32; i += 8) {   /* splitmix64: the same 32 bytes on every machine */
        z += 0x9E3779B97F4A7C15ull;
        uint64_t x = z;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x ^= x >> 31;
        for (int k = 0; k < 8; k++) out[i + k] = (uint8_t)(x >> (56 - 8 * k));
    }
}

void net_invite_key_hex(uint32_t key, char out[65])
{
    uint8_t b[32];
    net_invite_key_blob(key, b);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", b[i]);
}
