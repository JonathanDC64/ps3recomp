/*
 * net_invite.h - the invite code for a game one player hosts (psnr_host.h).
 *
 * A code names the host's address and port and a key, in 16 characters,
 * shown in groups of four: "7KQ2-M9XD-0RTB-4HWE". The characters are
 * Crockford's base32 (digits and letters without I, L, O and U), so no two
 * look alike; reading one back, O counts as 0 and I and L as 1, case and
 * dashes or spaces don't matter.
 *
 * The 80 bits: IPv4 address (32) | port (16) | key (28) | check (4). The
 * check catches most typos. The key is not a password anyone picks: the
 * host's game draws a new one for each session, and the host's room server
 * turns away a HELLO without it (and, after a few wrong ones, the address).
 *
 * The code carries the host's address: it is for friends, not for posting.
 */
#ifndef PS3EMU_NET_INVITE_H
#define PS3EMU_NET_INVITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_INVITE_TEXT 20   /* "XXXX-XXXX-XXXX-XXXX" and its NUL */
#define NET_INVITE_KEY_MASK 0x0FFFFFFFu

typedef struct {
    uint8_t  ip[4];
    uint16_t port;
    uint32_t key;   /* 28 bits */
} net_invite;

void net_invite_encode(const net_invite* in, char out[NET_INVITE_TEXT]);

/* 0, or NET_INVITE_NOT_A_CODE (not 16 code characters) or NET_INVITE_TYPO (the check doesn't match). */
#define NET_INVITE_NOT_A_CODE (-1)
#define NET_INVITE_TYPO       (-2)
int net_invite_decode(const char* text, net_invite* out);

/* A new 28-bit key from the system's random source. */
uint32_t net_invite_new_key(void);

/* The 32-byte HELLO blob a key stands for (psnr_host_config.key), and the same as the 64 hex digits PSNR_KEY takes
 * (np_psnr.c). */
void net_invite_key_blob(uint32_t key, uint8_t out[32]);
void net_invite_key_hex(uint32_t key, char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_NET_INVITE_H */
