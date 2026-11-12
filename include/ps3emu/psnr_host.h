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

typedef struct {
    uint16_t port;                    /* TCP and UDP; 0 picks a free one */
    int      has_key;
    uint8_t  key[PSNR_HOST_KEY_LEN];  /* with has_key: the HELLO's session blob must be exactly this */
    uint8_t  public_ip[4];            /* the host's address as guests reach it (0.0.0.0: unknown) */
    int      bind_loopback;           /* 1: listen on 127.0.0.1 only (tests) */
} psnr_host_config;

typedef struct psnr_host psnr_host;

/* NULL on failure, with why in err. */
psnr_host* psnr_host_start(const psnr_host_config* cfg, char* err, size_t err_cap);
uint16_t   psnr_host_port(const psnr_host* h);
/* Players connected and past HELLO (the host's own game counts). */
int        psnr_host_players(psnr_host* h);
void       psnr_host_stop(psnr_host* h);

#ifdef __cplusplus
}
#endif

#endif /* PSNR_HOST_H */
