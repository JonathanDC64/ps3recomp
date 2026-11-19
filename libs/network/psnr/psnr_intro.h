/*
 * psnr_intro.h - the introductions for a game one player hosts (ps3emu/psnr_host.h): the packets between a host's
 * room server, a friend's game and the endpoint (a small part of a psnr server, on its UDP port) that tells each of
 * the two the other's addresses, so they can punch through their routers and talk directly.
 *
 * Every packet starts with "PSNR" and a type byte, as psnr's UDP packets do; numbers are big-endian. A candidate (an
 * address and port where a game might be reached) is family u8 (4) | ip [4] | port u16.
 *
 *   type  from -> to          body                                                              size
 *   0x10  host -> endpoint    REGISTER   version u16 | room u64 | owner u64 | room server port u16 |
 *                                        candidates (n u8, up to 2) | zeros                       64
 *   0x90  endpoint -> host    REGISTERED room u64 | status u8 | seen candidate | refresh seconds u8  22
 *   0x11  host -> endpoint    UNREGISTER room u64 | owner u64 | zeros                              64
 *   0x12  friend -> endpoint  LOOKUP     version u16 | room u64 | nonce u32 | candidates (n u8, 1) |
 *                                        zeros                                                     64
 *   0x92  endpoint -> friend  FOUND      room u64 | nonce u32 | status u8 | flags u8 | seen candidate |
 *                                        room server port u16 | candidates (n u8, up to 3)       <= 50
 *   0x93  endpoint -> host    INTRO      room u64 | nonce u32 | flags u8 | candidates (n u8, up to 2) <= 33
 *   0x05  host -> friend      KNOCK      room u64 | nonce u32                                      17
 *   0x06  friend -> host      OPEN       room u64 | nonce u64 | zeros                              64
 *   0x86  host -> friend      COOKIE     nonce u64 | cookie u64                                    21
 *   0x07  friend -> host      OPEN2      nonce u64 | cookie u64 | zeros                            64
 *   0x87  host -> friend      OPENED     nonce u64 | session u32                                   17
 *   0x08, 0x09 DATA and CLOSE: the session itself (psnr_rudp.h).
 *
 * Requests are padded to 64 bytes and no answer is bigger than its request. The key of the invite code never goes
 * to the endpoint: the host checks it at HELLO, whatever the path.
 */
#ifndef PSNR_INTRO_H
#define PSNR_INTRO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSNR_PKT_KNOCK = 0x05, PSNR_PKT_OPEN = 0x06, PSNR_PKT_OPEN2 = 0x07, PSNR_PKT_DATA = 0x08, PSNR_PKT_CLOSE = 0x09,
    PSNR_PKT_REGISTER = 0x10, PSNR_PKT_UNREGISTER = 0x11, PSNR_PKT_LOOKUP = 0x12,
    PSNR_PKT_COOKIE = 0x86, PSNR_PKT_OPENED = 0x87,
    PSNR_PKT_REGISTERED = 0x90, PSNR_PKT_FOUND = 0x92, PSNR_PKT_INTRO = 0x93
};

/* REGISTERED's and FOUND's status */
enum {
    PSNR_INTRO_OK = 0,
    PSNR_INTRO_NO_ROOM = 1,    /* FOUND: no room has that number */
    PSNR_INTRO_VERSION = 2,    /* FOUND: the host's game is another network version */
    PSNR_INTRO_SLOW = 3,       /* either: too many requests from this address for now */
    PSNR_INTRO_TAKEN = 4,      /* REGISTERED: the room number is another owner's */
    PSNR_INTRO_FULL = 5        /* REGISTERED: no more rooms from this address, or at all */
};
#define PSNR_INTRO_SAME_NETWORK 0x01   /* FOUND's and INTRO's flags: the two games share an internet address */

#define PSNR_INTRO_REQ_LEN 64
#define PSNR_INTRO_MAX_CANDS 3

typedef struct {
    uint8_t  family;   /* 4 */
    uint8_t  ip[4];
    uint16_t port;
} psnr_cand;

/* Requests (64 bytes each, into out) */
size_t psnr_intro_register(uint8_t out[PSNR_INTRO_REQ_LEN], uint16_t version, uint64_t room, uint64_t owner,
                           uint16_t room_port, const psnr_cand* cands, int n);
size_t psnr_intro_unregister(uint8_t out[PSNR_INTRO_REQ_LEN], uint64_t room, uint64_t owner);
size_t psnr_intro_lookup(uint8_t out[PSNR_INTRO_REQ_LEN], uint16_t version, uint64_t room, uint32_t nonce,
                         const psnr_cand* own);
size_t psnr_intro_open(uint8_t out[PSNR_INTRO_REQ_LEN], uint64_t room, uint64_t nonce);
size_t psnr_intro_open2(uint8_t out[PSNR_INTRO_REQ_LEN], uint64_t nonce, uint64_t cookie);
/* Answers (at most PSNR_INTRO_REQ_LEN bytes) */
size_t psnr_intro_registered(uint8_t* out, uint64_t room, uint8_t status, const psnr_cand* seen, uint8_t refresh_s);
size_t psnr_intro_found(uint8_t* out, uint64_t room, uint32_t nonce, uint8_t status, uint8_t flags,
                        const psnr_cand* seen, uint16_t room_port, const psnr_cand* cands, int n);
size_t psnr_intro_intro(uint8_t* out, uint64_t room, uint32_t nonce, uint8_t flags, const psnr_cand* cands, int n);
size_t psnr_intro_knock(uint8_t* out, uint64_t room, uint32_t nonce);
size_t psnr_intro_cookie(uint8_t* out, uint64_t nonce, uint64_t cookie);
size_t psnr_intro_opened(uint8_t* out, uint64_t nonce, uint32_t session);

/* One packet read back: 1 if buf is a well-formed packet of its type (the fields filled), else 0. */
typedef struct {
    uint8_t   type;
    uint16_t  version, room_port;
    uint64_t  room, owner, nonce64, cookie;
    uint32_t  nonce, session;
    uint8_t   status, flags, refresh;
    psnr_cand seen;
    int       n;
    psnr_cand cands[PSNR_INTRO_MAX_CANDS];
} psnr_intro_pkt;
int psnr_intro_read(const void* buf, size_t len, psnr_intro_pkt* out);

/* The packet's type byte if buf starts "PSNR", else -1. */
int psnr_udp_type(const void* buf, size_t len);

/* A candidate from an address and port (family 4). */
psnr_cand psnr_cand_of(const uint8_t ip[4], uint16_t port);

/* The session cookie: keyed BLAKE2b of the friend's address and port, its nonce and the minute, cut to 64 bits. The
 * host answers OPEN with it and takes a player slot only for an OPEN2 that brings it back, so datagrams from
 * made-up addresses get no slot. secret: the host's own, drawn at start. */
uint64_t psnr_intro_cookie_of(const uint8_t secret[32], const uint8_t ip[4], uint16_t port, uint64_t nonce,
                              uint64_t minute);

/* 8 bytes from the system's random source (a time-mixed value if there is none). */
uint64_t psnr_intro_random(void);

#ifdef __cplusplus
}
#endif

#endif /* PSNR_INTRO_H */
