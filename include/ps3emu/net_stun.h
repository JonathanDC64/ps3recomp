/*
 * net_stun.h - the public IPv4 address this machine's traffic leaves from,
 * asked of a STUN server (RFC 5389: one Binding request over UDP).
 *
 * For a game one player hosts (psnr_host.h): when the router can't say its
 * public address (no UPnP, or a router behind another one), the invite code
 * still gets the address friends must use. It says nothing of whether they
 * can reach it: the port still has to be forwarded.
 *
 * What the server learns: the address and port the request came from, as any
 * server does. The request carries no attributes, so nothing else is sent.
 */
#ifndef NET_STUN_H
#define NET_STUN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NET_STUN_OK              0
#define NET_STUN_ERR_RESOLVE    -1   /* the server's name has no IPv4 address */
#define NET_STUN_ERR_SOCKET     -2   /* no UDP socket, or the request could not be sent */
#define NET_STUN_ERR_TIMEOUT    -3   /* no answer in time */
#define NET_STUN_ERR_BAD_ANSWER -4   /* an answer, but not a Binding success with an IPv4 address */

#define NET_STUN_REQUEST_LEN 20

/* Asks host:port (a name or a dotted address) for this machine's public IPv4 address, waiting up to timeout_ms
 * (the request is sent again at 0.5, 1.5 and 3.5 s, as RFC 5389 does). On NET_STUN_OK, out_ip holds it dotted
 * ("203.0.113.7") and *out_port (if not NULL) the public port the request left from. Blocks: call it off the
 * game's threads. */
int net_stun_public_ipv4(const char* host, uint16_t port, int timeout_ms, char out_ip[16], uint16_t* out_port);

/* A short English reason for a NET_STUN_* result. */
const char* net_stun_error_text(int rc);

/* The pure parts, for tests. net_stun_request writes a Binding request with this transaction id (20 bytes).
 * net_stun_parse reads an answer: NET_STUN_OK with the mapped address (XOR-MAPPED-ADDRESS, else MAPPED-ADDRESS)
 * when it is a Binding success for this transaction id, else NET_STUN_ERR_BAD_ANSWER. */
size_t net_stun_request(uint8_t out[NET_STUN_REQUEST_LEN], const uint8_t txid[12]);
int    net_stun_parse(const uint8_t* p, size_t n, const uint8_t txid[12], uint8_t ip[4], uint16_t* port);

#ifdef __cplusplus
}
#endif

#endif /* NET_STUN_H */
