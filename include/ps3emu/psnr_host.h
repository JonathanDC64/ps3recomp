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
 *     is ignored for a minute after a few.
 *   - No relay streams and no scores (ERROR replies).
 *   - Small fixed limits (a few rooms of a few players).
 *   - No player addresses in the log.
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
void       psnr_host_stop(psnr_host* h);

/* The hosted game's own messages between its players (a port's, never the title's), from any player's game connected
 * to a psnr_host (np_psnr.c's connection, PSNR_SERVER): psnr_app_send hands a blob of up to PSNR_APP_MAX bytes to the
 * server, which passes it to every other player's game there; their handler gets it with the sender's user number
 * (the HELLO's). 0 if sent; -1 with no connection, or too long. The handler runs on the title's own polling thread
 * (np_psnr_pump, from cellSysutilCheckCallback), in order. A psnr server (not a psnr_host) refuses APP_SEND. */
#define PSNR_APP_MAX 1024
int  psnr_app_send(const void* data, uint32_t len);
void psnr_app_on_message(void (*fn)(uint32_t from_user, const uint8_t* data, uint32_t len));

#ifdef __cplusplus
}
#endif

#endif /* PSNR_HOST_H */
