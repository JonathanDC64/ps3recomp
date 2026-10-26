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

/* The server's banners (GET /dss/v1/banners on the sign-in server; no
 * session needed): 1 and the next new one's text in out[cap], else 0. Each
 * banner comes once. Never blocks: the first call starts a thread that asks
 * the server every 5 minutes. For the host's UI, from its frame hook. */
int ps3_net_signin_banner(char* out, size_t cap);

/* ---- players met, blocks and reports (the server's docs/social.md) ----
 * For the host's Players page. Each call returns at once: a thread of their
 * own talks to the sign-in server, with the session. */

#define PS3_NET_PLAYERS_MAX 100

typedef struct ps3_net_player {
    char name[20];          /* the online name */
    char fingerprint[20];   /* their key's: 16 hex digits */
    char met[12];           /* "help", "invasion", "monk"; "" for one blocked and not met lately */
    int  host;              /* the meeting was in this player's world (they came in) */
    long long at;           /* seconds since 1970 UTC: the meeting, or for met "" the block; -1 unknown */
    int  blocked;           /* this player blocks them */
    int  reported;          /* this player reported them in the last day */
    int  reportable;        /* they may be reported now */
} ps3_net_player;

enum { PS3_NET_PLAYERS_NONE, PS3_NET_PLAYERS_ASKING, PS3_NET_PLAYERS_READY, PS3_NET_PLAYERS_FAILED };

/* Asks the server for the list again. 0, or -1 with no sign-in. */
int ps3_net_players_refresh(void);

/* The list as last fetched: up to max rows into out, and how many. *state is
 * a PS3_NET_PLAYERS_* (FAILED: why, for the player, in msg[mcap]), *days how
 * far back the list goes, *serial counts the answers so far (the host
 * redraws when it changes). Any pointer may be NULL. */
int ps3_net_players(ps3_net_player* out, int max, int* state, int* days, unsigned* serial, char* msg, size_t mcap);

/* Blocks (blocked 1) or unblocks a player, or reports one for "cheating",
 * "name" (an offensive name) or "other"; the list is asked again after. 0
 * when it is on its way; -1 with no sign-in, a name that is not an online
 * name, an unknown reason, or another block or report still waiting. */
int ps3_net_player_block(const char* name, int blocked);
int ps3_net_player_report(const char* name, const char* reason);

/* The last block's or report's outcome, once: 1 with *ok and a line for the
 * player in out[cap], else 0. */
int ps3_net_players_result(int* ok, char* out, size_t cap);

/* Deletes what the online server keeps about this player (the server's
 * docs/social.md, POST /dss/v1/players/erase), on the same thread as the
 * blocks and reports. The run goes offline for good first
 * (ps3_net_lock_offline), so nothing in it signs in again and starts a new
 * record; the host turns online off for the next start. 0 when queued; -1
 * with the sign-in off, or another call still waiting. */
int ps3_net_player_erase(void);

/* The erase's outcome, once: 1 with *ok, *kept (the name stays on the server
 * with a restriction on record) and a line for the player in out[cap], else
 * 0. */
int ps3_net_erase_result(int* ok, int* kept, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
