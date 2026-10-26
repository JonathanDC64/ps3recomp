#pragma once
#include <stddef.h>
#include <stdint.h>
/* Whether the title may use the network, and where its host names go
 * (libs/network/ps3_net.c).
 *
 *   PS3_NET_ONLINE=1   online: real sockets, cellHttp, a connected cellNetCtl.
 *                      Unset, empty or "0": offline.
 *   PS3_NET_HOSTS=...  the host map, in RPCS3's IP/Hosts switch format:
 *                      name=addr&&name2=addr2. Names match whole and ignore
 *                      case; '*' stands for any run of characters; the first
 *                      match wins. addr is an IPv4 literal or a host name,
 *                      which is then resolved normally. While it is set, a
 *                      name it does not list does not resolve (fail closed);
 *                      an IPv4 literal it does not list is used as is.
 *                      Unset: names resolve through the OS as before. */
#ifdef __cplusplus
extern "C" {
#endif

/* PS3_NET_ONLINE is on and ps3_net_lock_offline() was never called. Live. */
int ps3_net_online(void);

/* Offline for the rest of the process, whatever the environment says later:
 * cellNetCtl, NP (psnr and local sign-in), the sys_net host sockets and
 * cellHttp all report no network. Sticky; for sessions that must never
 * reach other players, such as one with dev cheats. Call it before the
 * title starts to keep the host sockets from being registered at all. */
void ps3_net_lock_offline(void);
int  ps3_net_locked_offline(void);

/* Look `host` up in a host map string. 1 and its address text in addr (cap
 * bytes) if an entry matches, 0 if none does. *bad, if given, counts the
 * malformed entries skipped (no '=', an empty name or address, or an address
 * that does not fit). */
int ps3_net_hosts_lookup(const char* map, const char* host, char* addr, size_t cap, int* bad);

/* A guest host name to an IPv4 address in network order: offline fails; with
 * PS3_NET_HOSTS set only listed names (and IPv4 literals) resolve; otherwise
 * the OS resolver. A name PS3_NET_HOSTS does not map that comes to a local
 * address fails too, unless ps3_net_ip_allowed allows it.
 * 0 on success, -1 if the name does not resolve. The caller has started
 * Winsock. */
int ps3_net_resolve(const char* host, uint8_t ip[4]);

/* An IPv4 address (network order) that is not on the internet: this host
 * (0/8, 127/8), private (10/8, 172.16/12, 192.168/16), link-local
 * (169.254/16), shared (100.64/10), multicast and above (224/3). */
int ps3_net_ip_local(const uint8_t ip[4]);

/* The title may reach ip: any internet address; a local one only when the
 * setup is local -- PS3_NET_LAN=1, or a server this run was pointed at
 * (PS3_NET_HOSTS's addresses, PSNR_SERVER, PS3_NET_SIGNIN) is on a local
 * address itself -- or when it is on the same /24 as own (this machine's
 * address on the way to psnr: a co-op peer on the same network; may be
 * NULL). So a server cannot aim the title at the player's own network. A
 * refusal is logged once per address, with `what`. */
int ps3_net_ip_allowed(const uint8_t ip[4], const uint8_t own[4], const char* what);

#ifdef __cplusplus
}
#endif
