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
 *                      name it does not list does not resolve (fail closed).
 *                      Unset: names resolve through the OS as before. */
#ifdef __cplusplus
extern "C" {
#endif

/* PS3_NET_ONLINE is on. Live. */
int ps3_net_online(void);

/* Look `host` up in a host map string. 1 and its address text in addr (cap
 * bytes) if an entry matches, 0 if none does. *bad, if given, counts the
 * malformed entries skipped (no '=', an empty name or address, or an address
 * that does not fit). */
int ps3_net_hosts_lookup(const char* map, const char* host, char* addr, size_t cap, int* bad);

/* A guest host name to an IPv4 address in network order: offline fails; with
 * PS3_NET_HOSTS set only listed names resolve; otherwise the OS resolver.
 * 0 on success, -1 if the name does not resolve. The caller has started
 * Winsock. */
int ps3_net_resolve(const char* host, uint8_t ip[4]);

#ifdef __cplusplus
}
#endif
