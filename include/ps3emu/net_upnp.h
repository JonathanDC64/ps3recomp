#pragma once
#include <stdint.h>
/* UPnP port mapping on the player's router (libs/network/net_upnp.c, with miniupnpc 2.3.3 in third_party/miniupnpc),
 * for a game that friends connect to: the router forwards one port, UDP and TCP with the same number, to this PC,
 * and gives its public address for an invite code.
 *
 * Every call blocks (seconds on a slow network or router); run them off the game thread. One lock serialises them.
 * Built without PS3RECOMP_UPNP, net_upnp_map returns NET_UPNP_ERR_NOT_BUILT and the rest do nothing. */
#ifdef __cplusplus
extern "C" {
#endif

#define NET_UPNP_OK                0
#define NET_UPNP_ERR_NOT_BUILT    -1   /* the runtime was built without UPnP (PS3RECOMP_UPNP=OFF) */
#define NET_UPNP_ERR_ARGS         -2   /* port 0 */
#define NET_UPNP_ERR_OFFLINE      -3   /* the session is locked offline (ps3_net_lock_offline) */
#define NET_UPNP_ERR_NO_ROUTER    -4   /* no UPnP router answered on the network */
#define NET_UPNP_ERR_NOT_ROUTER   -5   /* UPnP devices answered, but none is a usable router (IGD) */
#define NET_UPNP_ERR_NO_WAN_IP    -6   /* the router has no public address to give */
#define NET_UPNP_ERR_TAKEN        -7   /* the port is already forwarded to another device, or another port */
#define NET_UPNP_ERR_REFUSED      -8   /* the router refused the mapping (a UPnP error code; see the reason) */
#define NET_UPNP_ERR_NETWORK      -9   /* a socket, HTTP or memory error while talking to the router */
#define NET_UPNP_ERR_PRIVATE_IP  -10   /* mapped, but the router's address is private or carrier-grade NAT (double
                                        * NAT), so friends can't reach it; ext_ip_out has that address */

/* Find the router (UPnP discovery, waiting up to timeout_ms for answers to each of its up to four searches, and at
 * least 1 s each: miniupnpc's floor; 0 or less: 2000), read its public address, and forward `port` on it, UDP and TCP, to the same
 * port on this PC's LAN address (net_upnp_lan_ip). description (NULL: "ps3recomp") names the mappings in the
 * router's list. A mapping for the port that already points at this PC and port counts as done; one with our
 * description is renewed, and removed by net_upnp_unmap. A mapping to another device fails (NET_UPNP_ERR_TAKEN).
 * A 3600 s lease first (net_upnp_lease): call again within that time to renew it, so a forward nobody renews, or
 * removes after a crash, ends by itself. A router that refuses leases gets a permanent mapping. A different port
 * than the last call's: the old mappings go first. If only one protocol maps, the other is undone. NET_UPNP_OK with
 * the public address in ext_ip_out, or a NET_UPNP_ERR_* code with the reason in net_upnp_last_error. */
int net_upnp_map(uint16_t port, const char* description, int timeout_ms, char ext_ip_out[16]);

/* Remove the mappings this process added (both protocols), ignoring errors. */
void net_upnp_unmap(void);

/* net_upnp_unmap, waiting at most ms for it (it goes on a thread of its own), for a process about to end at once
 * (TerminateProcess: no atexit handlers). Returns at once if nothing is mapped. A forward the router didn't remove in
 * time ends with its lease. */
void net_upnp_unmap_bounded(int ms);

/* A short plain-English reason for the last failure ("" after a success). The calling thread's copy. */
const char* net_upnp_last_error(void);

/* This PC's address on the way to the router, from the last net_upnp_map ("" if none was found). */
const char* net_upnp_lan_ip(void);

/* The mappings' lease: 0 permanent, else seconds (3600); -1 while nothing is mapped. */
int net_upnp_lease(void);

/* A dotted IPv4 address that the internet can't reach: private (10/8, 172.16/12, 192.168/16), carrier-grade NAT
 * (100.64/10), loopback, link-local, 0/8, multicast and above (ps3_net_ip_local). 1 if so, 0 for a public address,
 * -1 if ip is not a dotted IPv4 address. */
int net_upnp_is_private_ipv4(const char* ip);

#ifdef __cplusplus
}
#endif
