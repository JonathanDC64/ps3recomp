/*
 * ps3recomp - whether the title may use the network, and the host map that
 * sys_net's gethostbyname and cellHttp resolve through. See ps3emu/ps3_net.h.
 */
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netdb.h>
#  include <arpa/inet.h>
#  include <signal.h>
#endif

#include "ps3emu/ps3_net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int s_locked;

void ps3_net_lock_offline(void)
{
    if (s_locked) return;
    s_locked = 1;
    printf("[net] locked offline for the rest of this run\n");
}

int ps3_net_locked_offline(void)
{
    return s_locked;
}

int ps3_net_online(void)
{
    const char* v = getenv("PS3_NET_ONLINE");
    int on = !s_locked && v && *v && strcmp(v, "0") != 0;
#ifndef _WIN32
    /* Online, a write to a connection the other end has closed must fail
     * with EPIPE for the code that made it (sys_net's send, cellHttp, psnr,
     * the sign-in), not end the process with SIGPIPE. Once, for the whole
     * process; every network path asks this first. */
    static volatile int s_no_sigpipe;
    if (on && !s_no_sigpipe) {
        s_no_sigpipe = 1;
        signal(SIGPIPE, SIG_IGN);
    }
#endif
    return on;
}

/* ---- PS3_NET_HOSTS ---- */

/* `s` against the pattern [p, pe): case-insensitive, '*' any run (also none). */
static int glob_match(const char* p, const char* pe, const char* s)
{
    const char *star = NULL, *resume = NULL;
    while (*s) {
        if (p < pe && *p == '*') { star = p++; resume = s; continue; }
        if (p < pe && tolower((unsigned char)*p) == tolower((unsigned char)*s)) { p++; s++; continue; }
        if (!star) return 0;
        p = star + 1;
        s = ++resume;
    }
    while (p < pe && *p == '*') p++;
    return p == pe;
}

static void trim(const char** b, const char** e)
{
    while (*b < *e && isspace((unsigned char)**b)) (*b)++;
    while (*e > *b && isspace((unsigned char)(*e)[-1])) (*e)--;
}

int ps3_net_hosts_lookup(const char* map, const char* host, char* addr, size_t cap, int* bad)
{
    if (bad) *bad = 0;
    if (!map || !host) return 0;
    for (const char* b = map; *b; ) {
        const char* sep = strstr(b, "&&");
        const char* e = sep ? sep : b + strlen(b);
        const char* next = sep ? sep + 2 : e;
        trim(&b, &e);
        if (b < e) {   /* "a=1&&" leaves an empty entry: not an error */
            const char* eq = memchr(b, '=', (size_t)(e - b));
            const char *ne = eq, *ab = eq ? eq + 1 : NULL;
            if (eq) { trim(&b, &ne); trim(&ab, &e); }
            if (!eq || b == ne || ab == e || (size_t)(e - ab) >= cap) {
                if (bad) (*bad)++;
            } else if (glob_match(b, ne, host)) {
                memcpy(addr, ab, (size_t)(e - ab));
                addr[e - ab] = 0;
                return 1;
            }
        }
        b = next;
    }
    return 0;
}

static int trace_on(void)
{
    return getenv("PS3_NET_TRACE") != NULL;
}

/* A name the map refused: said once per name (every time with PS3_NET_TRACE),
 * so a title that retries in a loop doesn't flood the log. */
static void refused(const char* host)
{
    /* ponytail: 32 remembered names, racy on purpose -- the worst case is a
     * line said twice. */
    static uint32_t seen[32];
    static unsigned nseen;
    uint32_t h = 2166136261u;
    for (const char* p = host; *p; p++) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
    int known = 0;
    for (unsigned i = 0; i < nseen && i < 32; i++) known |= seen[i] == h;
    if (!known && nseen < 32) seen[nseen++] = h;
    if (!known || trace_on())
        printf("[net] '%s' is not in PS3_NET_HOSTS: not resolved\n", host);
}

/* ---- local addresses ---- */

int ps3_net_ip_local(const uint8_t ip[4])
{
    return ip[0] == 0 || ip[0] == 127 || ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xF0) == 16) ||
           (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254) ||
           (ip[0] == 100 && (ip[1] & 0xC0) == 64) || ip[0] >= 224;
}

/* The text [b, e) is an IPv4 literal (a ":port" after it is fine) on a local
 * address. A host name is not. */
static int text_local(const char* b, const char* e)
{
    char t[64];
    trim(&b, &e);
    size_t n = (size_t)(e - b);
    if (!n || n >= sizeof t) return 0;
    memcpy(t, b, n);
    t[n] = 0;
    char* colon = strchr(t, ':');
    if (colon) *colon = 0;
    struct in_addr a;
    return inet_pton(AF_INET, t, &a) == 1 && ps3_net_ip_local((const uint8_t*)&a);
}

/* PS3_NET_LAN=1, or a server this run was pointed at is on a local address:
 * a server on this PC or this network, so its addresses are the player's own
 * choice. */
static int local_setup(void)
{
    const char* lan = getenv("PS3_NET_LAN");
    if (lan && *lan && strcmp(lan, "0") != 0) return 1;
    const char* servers[2] = { getenv("PSNR_SERVER"), getenv("PS3_NET_SIGNIN") };
    for (int i = 0; i < 2; i++)
        if (servers[i] && text_local(servers[i], servers[i] + strlen(servers[i]))) return 1;
    const char* map = getenv("PS3_NET_HOSTS");
    for (const char* b = map; b && *b; ) {
        const char* sep = strstr(b, "&&");
        const char* e = sep ? sep : b + strlen(b);
        const char* eq = memchr(b, '=', (size_t)(e - b));
        if (eq && text_local(eq + 1, e)) return 1;
        b = sep ? sep + 2 : e;
    }
    return 0;
}

int ps3_net_ip_allowed(const uint8_t ip[4], const uint8_t own[4], const char* what)
{
    if (!ps3_net_ip_local(ip) || local_setup()) return 1;
    if (own && ip[0] < 224 && ps3_net_ip_local(own) && !memcmp(ip, own, 3) && ip[3] != 0 && ip[3] != 255)
        return 1;   /* a peer on our own network */
    /* Said once per address (ponytail: 32 remembered, racy on purpose). */
    static uint32_t seen[32];
    static unsigned nseen;
    uint32_t v;
    memcpy(&v, ip, 4);
    int known = 0;
    for (unsigned i = 0; i < nseen && i < 32; i++) known |= seen[i] == v;
    if (!known && nseen < 32) seen[nseen++] = v;
    if (!known || trace_on())
        printf("[net] %s: %u.%u.%u.%u is a local address, not used (PS3_NET_LAN=1 allows local addresses)\n",
               what, ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

int ps3_net_resolve(const char* host, uint8_t ip[4])
{
    char mapped[256];
    const char* name = host;
    int from_map = 0;   /* the player's own map chose the address */
    if (!host || !*host) return -1;
    if (!ps3_net_online()) {
        if (trace_on()) printf("[net] '%s': offline, not resolved\n", host);
        return -1;
    }

    const char* map = getenv("PS3_NET_HOSTS");
    if (map && *map) {
        int bad = 0;
        int hit = ps3_net_hosts_lookup(map, host, mapped, sizeof(mapped), &bad);
        static int warned;
        if (bad && !warned) {
            warned = 1;
            printf("[net] PS3_NET_HOSTS: %d malformed entr%s skipped (want name=addr&&name=addr)\n",
                   bad, bad == 1 ? "y" : "ies");
        }
        if (hit) {
            name = mapped;
            from_map = 1;
            if (trace_on()) printf("[net] '%s' -> '%s' (PS3_NET_HOSTS)\n", host, name);
        } else {
            /* An IPv4 literal is an address, not a name: nothing to look up.
             * Servers hand them out (Demon's Souls' bootstrap gives the game
             * server as http://<ip>:<port>/), so they pass unless an entry
             * remaps them. */
            struct in_addr lit;
            if (inet_pton(AF_INET, host, &lit) != 1) { refused(host); return -1; }
        }
    }

    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) {
        memcpy(ip, &a, 4);
        return from_map || ps3_net_ip_allowed(ip, NULL, host) ? 0 : -1;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(name, NULL, &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        return -1;
    }
    memcpy(ip, &((const struct sockaddr_in*)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return from_map || ps3_net_ip_allowed(ip, NULL, host) ? 0 : -1;
}
