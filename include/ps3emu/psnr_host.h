/*
 * psnr_host.h - a psnr room server inside the title's own process, for a
 * game one player hosts (no outside server).
 *
 * It speaks the same protocol as the psnr server (docs/api.md), so the
 * title's own NP code (np_psnr.c, sceNpMatching2.c) needs no other path:
 * the host's game connects to it over loopback, and each guest's game
 * connects to the host's address. TCP and UDP use one port number, as on
 * the psnr server.
 *
 * Differences from the psnr server:
 *   - Every player's datagrams go through this server's UDP port (a star
 *     through the host): only the host needs an open port. ROUTE pushes say
 *     so with PSNR_ROUTE_UDP_RELAY, and the clients switch at once.
 *   - With a key, a HELLO must carry it as its 32-byte session blob, or the
 *     player is refused (PSNR_E_REFUSED). An address that sends wrong keys
 *     is ignored for a minute after a few. After 100 wrong keys from any
 *     addresses the key is spent: no one else gets in until the caller sets
 *     a new one (psnr_host_new_key). A connection must say HELLO within 10 s,
 *     and one address may have only two connections waiting to.
 *   - With max_players, a room takes at most that many members: one more
 *     JOIN_ROOM is refused as full, and the player stays connected to try
 *     again (the port tells its players when the game is full).
 *   - No relay streams and no scores (ERROR replies).
 *   - Small fixed limits (a few rooms of a few players).
 *   - No player addresses in the log.
 *   - Players who can't reach its TCP port (no port forward) can join over UDP: the endpoint (a small part of a
 *     psnr server, on its UDP port; psnr_intro.h) tells the host's room server and the friend's game each other's
 *     addresses, both punch through their routers, and the friend's session runs over a reliable layer on that
 *     path (psnr_rudp.h). Such a player is a client like the others: same HELLO, key check, bans and limits.
 *
 * One thread does all of it. psnr_host_start returns once the sockets are
 * bound; psnr_host_stop closes them and joins the thread.
 */
#ifndef PSNR_HOST_H
#define PSNR_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSNR_HOST_KEY_LEN 32
#define PSNR_HOST_SAME_GAMES 4

typedef struct {
    uint16_t port;                    /* TCP and UDP; 0 picks a free one */
    int      has_key;
    uint8_t  key[PSNR_HOST_KEY_LEN];  /* with has_key: the HELLO's session blob must be exactly this */
    int      own_key_local;           /* 1: `key` stays good from this PC (127.0.0.0/8) after a new key, for the
                                       * hosting game's own connection, which keeps the key it started with */
    int      max_players;             /* >0: at most this many members in a room, its creator counted; one more
                                       * JOIN_ROOM is refused as the room being full (PSNR_E_ROOM_FULL), though the
                                       * player stays connected. 0: the room's own size only */
    uint8_t  public_ip[4];            /* the host's address as guests reach it (0.0.0.0: unknown) */
    int      bind_loopback;           /* 1: listen on 127.0.0.1 only (tests) */
    /* Other releases of the same game that play together, as the psnr server's sameGames: a player whose HELLO
     * names `other` as its communication ID ("NPWR01249_00") uses `game`'s rooms. Unused pairs are empty. */
    struct {
        char other[13], game[13];
    } same_game[PSNR_HOST_SAME_GAMES];
} psnr_host_config;

typedef struct psnr_host psnr_host;

/* NULL on failure, with why in err. */
psnr_host* psnr_host_start(const psnr_host_config* cfg, char* err, size_t err_cap);
uint16_t   psnr_host_port(const psnr_host* h);
/* Players connected and past HELLO (the host's own game counts). */
int        psnr_host_players(psnr_host* h);
/* A new key: HELLOs need it from now on (players already in stay), and the wrong keys count from 0 again. */
void       psnr_host_new_key(psnr_host* h, const uint8_t key[PSNR_HOST_KEY_LEN]);
/* 1 once too many wrong keys came for the current key: it lets no one else in until psnr_host_new_key. */
int        psnr_host_key_spent(psnr_host* h);
/* The most members of a room from now on (as max_players; 0: the room's own size). Members already in stay, even past
 * it. */
void       psnr_host_set_max_players(psnr_host* h, int n);
/* The members of the fullest room (a hosted game has one: the host's), 0 with none. */
int        psnr_host_members(psnr_host* h);

/* ---- The introductions (seamless co-op without a port forward) ----
 *
 * With an endpoint set, the room server registers a room number with it from its UDP socket, refreshes it every 20 s
 * while hosting, and unregisters it when hosting stops. A friend's game looks the number up, the endpoint introduces
 * the two, and the host knocks on the friend's addresses while the friend opens a session (psnr_intro.h). The room
 * number and the owner token come from the system's random source; the owner token never leaves this process and
 * the endpoint. */
#define PSNR_HOST_ROOM_BITS 40

typedef struct {
    uint8_t  ip[4];            /* the endpoint (0.0.0.0: none, registering stops) */
    uint16_t port;
    uint16_t version;          /* the network version: a friend's game of another version is told so */
    uint8_t  home_ip[4];       /* this PC's address on its home network (0.0.0.0: unknown) */
    uint8_t  upnp_ip[4];       /* the internet address and port UPnP mapped to the room server (0: nothing) */
    uint16_t upnp_port;
} psnr_host_intro;

/* Starts (or changes) the registering; an endpoint of 0.0.0.0 unregisters and stops. */
void     psnr_host_set_intro(psnr_host* h, const psnr_host_intro* in);
/* The room number now (never 0). It changes on psnr_host_new_room, and when the endpoint says another owner has it. */
uint64_t psnr_host_room(psnr_host* h);
/* A new room number and owner token (New code): the old number is unregistered, so its code finds nothing. Players
 * already in stay. Returns the new number. */
uint64_t psnr_host_new_room(psnr_host* h);

enum {
    PSNR_HOST_INTRO_OFF = 0,       /* no endpoint */
    PSNR_HOST_INTRO_REGISTERING,   /* asked, no answer yet */
    PSNR_HOST_INTRO_REGISTERED,    /* the endpoint has the room: friends can join with its number */
    PSNR_HOST_INTRO_UNREACHABLE,   /* the endpoint doesn't answer (still asked, every few seconds) */
    PSNR_HOST_INTRO_LIMITED        /* the endpoint has no place for another room now (asked again in a minute) */
};
typedef struct {
    int      state;            /* PSNR_HOST_INTRO_* */
    uint64_t room;             /* as psnr_host_room */
    uint8_t  seen_ip[4];       /* this host's internet address and UDP port as the endpoint saw them (0: none yet) */
    uint16_t seen_port;
    int      stuck;            /* 1: a friend's game was introduced in the last 2 minutes and still hadn't connected
                                * 15 s later (its router or this one let nothing through) */
    int      udp_players;      /* players connected over a punched session */
} psnr_host_intro_info;
void psnr_host_intro_get(psnr_host* h, psnr_host_intro_info* out);

/* UNREGISTER for every room server still running, from the calling thread, at once (the window's close, beside the
 * UPnP unmap; the process ends right after). */
void psnr_host_unregister_all(void);

/* An IPv4 address for a host name or dotted address (blocking: a DNS lookup). 0, or -1 if there is none. */
int  psnr_host_resolve(const char* name, uint8_t ip[4]);

void       psnr_host_stop(psnr_host* h);

/* The hosted game's own messages between its players (a port's, never the title's), from any player's game connected
 * to a psnr_host (np_psnr.c's connection, PSNR_SERVER): psnr_app_send hands a blob of up to PSNR_APP_MAX bytes to the
 * server, which passes it to every other player's game there; their handler gets it with the sender's user number
 * (the HELLO's). 0 if sent; -1 with no connection, or too long. The handler runs on the title's own polling thread
 * (np_psnr_pump, from cellSysutilCheckCallback), in order. A psnr server (not a psnr_host) refuses APP_SEND. */
#define PSNR_APP_MAX 1024
int  psnr_app_send(const void* data, uint32_t len);
void psnr_app_on_message(void (*fn)(uint32_t from_user, const uint8_t* data, uint32_t len));

/* A friend's game joining (np_psnr.c, with PSNR_BACKGROUND=1 and PSNR_KEY): it connects on a thread of its own, and
 * a try that fails is made again after 10 s, then 30 s, then every 60 s, until the host's game turns the key away.
 * How it goes, for the port's window: */
enum {
    PSNR_JOIN_NONE = 0,       /* not a hosted join */
    PSNR_JOIN_LOOKING,        /* a room code: asking the endpoint for the host's addresses */
    PSNR_JOIN_REACHING,       /* connecting to the host's game */
    PSNR_JOIN_CONNECTED,
    PSNR_JOIN_NO_ROOM,        /* no game hosts with that room number (tried again) */
    PSNR_JOIN_OTHER_VERSION,  /* the host's game is another network version (no more tries) */
    PSNR_JOIN_NO_SERVER,      /* the endpoint didn't answer (tried again) */
    PSNR_JOIN_NO_WAY,         /* found, but neither the punching nor TCP got through (tried again) */
    PSNR_JOIN_UNREACHABLE,    /* a direct code: the host's address didn't answer (tried again) */
    PSNR_JOIN_REFUSED,        /* the host's game turned the key away (no more tries) */
    PSNR_JOIN_GONE,           /* no room for 2 minutes: the host stopped hosting (no more tries) */
    PSNR_JOIN_SLOW            /* the endpoint asked to slow down (tried again) */
};
int psnr_join_state(void);

#ifdef __cplusplus
}
#endif

#endif /* PSNR_HOST_H */
