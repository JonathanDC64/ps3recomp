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
#endif

#include "ps3emu/ps3_net.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ps3_net_online(void)
{
    const char* v = getenv("PS3_NET_ONLINE");
    return v && *v && strcmp(v, "0") != 0;
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

int ps3_net_resolve(const char* host, uint8_t ip[4])
{
    char mapped[256];
    const char* name = host;
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
        if (!hit) { refused(host); return -1; }
        name = mapped;
        if (trace_on()) printf("[net] '%s' -> '%s' (PS3_NET_HOSTS)\n", host, name);
    }

    struct in_addr a;
    if (inet_pton(AF_INET, name, &a) == 1) {
        memcpy(ip, &a, 4);
        return 0;
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
    return 0;
}
