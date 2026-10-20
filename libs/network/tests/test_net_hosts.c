/*
 * test_net_hosts - PS3_NET_HOSTS, the host map sys_net and cellHttp resolve
 * through, and the online gate in front of it. Only loopback names and
 * literals are resolved: nothing leaves the PC.
 *
 *   clang-cl /I include /Fe:test_net_hosts.exe libs/network/tests/test_net_hosts.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_net_hosts libs/network/tests/test_net_hosts.c
 */
#include "../ps3_net.c"

#include <assert.h>

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* The address the map gives `host`, or NULL if it gives none. */
static const char* look(const char* map, const char* host)
{
    static char addr[64];
    return ps3_net_hosts_lookup(map, host, addr, sizeof(addr), NULL) ? addr : NULL;
}

static int bad_entries(const char* map)
{
    char addr[16];
    int bad = -1;
    ps3_net_hosts_lookup(map, "x", addr, sizeof(addr), &bad);
    return bad;
}

/* ps3_net_resolve as a dotted string, or "" if it fails. */
static const char* resolve(const char* host)
{
    static char out[16];
    uint8_t ip[4];
    if (ps3_net_resolve(host, ip) != 0) return "";
    snprintf(out, sizeof(out), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return out;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const char* map = "c.example.com=10.0.0.1&&*.example.com=10.0.0.2&&*=10.0.0.3";

    /* Whole-name, case-insensitive; the first match wins, so the specific
     * entry ahead of the wildcards takes its name. */
    assert(!strcmp(look(map, "c.example.com"), "10.0.0.1"));
    assert(!strcmp(look(map, "C.EXAMPLE.Com"), "10.0.0.1"));
    assert(!strcmp(look(map, "x.example.com"), "10.0.0.2"));
    assert(!strcmp(look(map, "a.b.example.com"), "10.0.0.2"));   /* '*' spans dots */
    assert(!strcmp(look(map, "example.com"), "10.0.0.3"));       /* the '.' is literal */
    assert(look("a.example.com=1.2.3.4", "a.example.com.evil") == NULL);
    assert(look("a.example.com=1.2.3.4", "xa.example.com") == NULL);
    assert(!strcmp(look("a*c=1.1.1.1", "abbbc"), "1.1.1.1"));
    assert(!strcmp(look("a*c=1.1.1.1", "ac"), "1.1.1.1"));
    assert(look("a*c=1.1.1.1", "acb") == NULL);
    assert(!strcmp(look("*b*=1.1.1.1", "abc"), "1.1.1.1"));
    /* An address may be a host name, and spaces around the parts go. */
    assert(!strcmp(look(" game = localhost && other=1.2.3.4", "game"), "localhost"));

    /* Malformed entries are skipped (and counted); the rest still apply. */
    const char* messy = "noequals&&=1.2.3.4&&empty=&&ok.test=5.6.7.8&&&&";
    assert(bad_entries(messy) == 3);
    assert(!strcmp(look(messy, "ok.test"), "5.6.7.8"));
    assert(look(messy, "noequals") == NULL && look(messy, "empty") == NULL);
    assert(bad_entries("fits=123.123.123.123") == 0);        /* 15 chars and the NUL */
    assert(bad_entries("toolong=a123456789012345") == 1);    /* 16 don't fit */
    assert(look("", "a") == NULL && look(NULL, "a") == NULL);

    /* Offline nothing resolves, not even a literal; "0" is offline too. */
    env("PS3_NET_HOSTS", "");
    env("PS3_NET_ONLINE", "");
    assert(!ps3_net_online() && !*resolve("127.0.0.1"));
    env("PS3_NET_ONLINE", "0");
    assert(!ps3_net_online() && !*resolve("127.0.0.1"));

    /* Online with no map: the OS resolver, as before. */
    env("PS3_NET_ONLINE", "1");
    assert(ps3_net_online());
    assert(!strcmp(resolve("127.0.0.1"), "127.0.0.1"));
    assert(!strcmp(resolve("localhost"), "127.0.0.1"));

    /* With a map: listed names go where it says (a literal, or a name that is
     * then resolved), and every other name fails closed -- even one the OS
     * knows. An IPv4 literal is an address: it passes, unless an entry remaps it. */
    env("PS3_NET_HOSTS", "game.test=127.0.0.2&&*.alias.test=localhost&&10.9.8.7=127.0.0.3");
    assert(!strcmp(resolve("game.test"), "127.0.0.2"));
    assert(!strcmp(resolve("GAME.TEST"), "127.0.0.2"));
    assert(!strcmp(resolve("a.alias.test"), "127.0.0.1"));
    assert(!*resolve("localhost"));
    assert(!strcmp(resolve("127.0.0.1"), "127.0.0.1"));
    assert(!strcmp(resolve("10.9.8.7"), "127.0.0.3"));
    assert(!*resolve("127.0.0.1.example"));
    assert(!*resolve("other.test"));
    assert(!*resolve("other.test"));   /* said once: see the log */
    env("PS3_NET_HOSTS", "");

    /* ps3_net_lock_offline: offline from then on, whatever the environment
     * says, and calling it again changes nothing. */
    assert(ps3_net_online() && !ps3_net_locked_offline());
    ps3_net_lock_offline();
    assert(!ps3_net_online() && ps3_net_locked_offline() && !*resolve("127.0.0.1"));
    env("PS3_NET_ONLINE", "1");
    ps3_net_lock_offline();
    assert(!ps3_net_online() && !*resolve("localhost"));

    printf("test_net_hosts: all passed\n");
    return 0;
}
