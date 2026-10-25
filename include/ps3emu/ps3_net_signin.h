#pragma once
#include <stddef.h>
#include <stdint.h>
/* The sign-in to a server that has one (libs/network/ps3_net_signin.c): the
 * Demon's Souls Recomp server's /dss/v1 calls (its docs/identity.md). The
 * install's Ed25519 key signs a challenge; the session token that comes back
 * goes on the title's HTTP requests to that server (X-DSS-Session) and in
 * psnr's HELLO. Off unless both of these are set:
 *
 *   PS3_NET_SIGNIN=host[:port]   the server's address; port 18000 if left out
 *   PS3_NET_KEY_FILE=path        the install's key, made at the first sign-in
 *
 * and, for the server's version gate:
 *
 *   PS3_NET_CLIENT_VERSION=x.y.z the build's version (default 0.0.0)
 *   PS3_NET_NETWORK_VERSION=n    which builds play together (default 1)
 *
 * The name is the player's online ID (np_psnr_online_id). A server that does
 * not answer, or has no sign-in, leaves the session empty and the title's
 * requests as they were. */
#ifdef __cplusplus
extern "C" {
#endif

/* PS3_NET_SIGNIN and PS3_NET_KEY_FILE are set and the network is on. */
int ps3_net_signin_enabled(void);

/* The session: 1 and its token (base64url, NUL-terminated in token[cap]; cap
 * 64 is enough) and its 32 bytes in raw (either may be NULL), or 0 for none.
 * Signs in when there is none or it ends within minutes, at most once a
 * minute after a failure. Blocks while it signs in (two requests; a new key's
 * work first, about a second). Thread-safe. */
int ps3_net_session(char* token, size_t cap, uint8_t raw[32]);

/* The server no longer takes the session: the next ps3_net_session signs in
 * again, without waiting out a failure's minute. */
void ps3_net_session_drop(void);

/* 1 if ip (IPv4, network order) is the sign-in server's: the session header
 * goes only there. */
int ps3_net_signin_server(const uint8_t ip[4]);

/* A message for the player (a refused sign-in, a refused co-op connection):
 * 1 and the text in out[cap] once per message, else 0. For the host's UI. */
int ps3_net_signin_notice(char* out, size_t cap);

/* Queue a message for ps3_net_signin_notice (for psnr's refusals). */
void ps3_net_signin_post_notice(const char* text);

#ifdef __cplusplus
}
#endif
