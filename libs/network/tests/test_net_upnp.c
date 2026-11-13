/*
 * test_net_upnp - UPnP port mapping (net_upnp.c) without needing a router: the address check, the argument and
 * offline gates, and one real search with a short timeout. With no UPnP router on the network (CI) the search must
 * say so; where one answers, the outcome must be a documented code with a reason, the lease 3600 s (or permanent,
 * for a router that refuses leases), a second call must give the same answer (the renewal), and whatever was mapped
 * is removed again, the second time by net_upnp_unmap_bounded. Built without -DPS3RECOMP_UPNP it checks the stub.
 *
 *   clang-cl /I include /I third_party/miniupnpc/include /DPS3RECOMP_UPNP /DMINIUPNP_STATICLIB /Fe:test_net_upnp.exe libs/network/tests/test_net_upnp.c miniupnpc.lib ws2_32.lib iphlpapi.lib
 *   cc -std=gnu17 -I include -I third_party/miniupnpc/include -DPS3RECOMP_UPNP -DMINIUPNP_STATICLIB -o test_net_upnp libs/network/tests/test_net_upnp.c libminiupnpc.a -lpthread
 *
 * miniupnpc.lib / libminiupnpc.a: the .c files of third_party/miniupnpc/src, with -I third_party/miniupnpc/include
 * -I third_party/miniupnpc and miniupnpc's own definitions (CMakeLists.txt, or the CI workflows). CMake builds it
 * as test_net_upnp with PS3RECOMP_BUILD_TESTS.
 */
#include "../net_upnp.c"
#include "../ps3_net.c"

#include <assert.h>
#include <time.h>

#define TEST_PORT 47923   /* unlikely to be in use */

#ifdef PS3RECOMP_UPNP
static int documented(int r)
{
    return r == NET_UPNP_OK || (r <= NET_UPNP_ERR_ARGS && r >= NET_UPNP_ERR_PRIVATE_IP);
}
#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Private, carrier-grade NAT and other addresses the internet can't reach. */
    assert(net_upnp_is_private_ipv4("10.0.0.1") == 1);
    assert(net_upnp_is_private_ipv4("10.255.255.255") == 1);
    assert(net_upnp_is_private_ipv4("172.15.255.255") == 0);
    assert(net_upnp_is_private_ipv4("172.16.0.0") == 1);
    assert(net_upnp_is_private_ipv4("172.31.255.255") == 1);
    assert(net_upnp_is_private_ipv4("172.32.0.0") == 0);
    assert(net_upnp_is_private_ipv4("192.168.0.1") == 1);
    assert(net_upnp_is_private_ipv4("192.169.0.1") == 0);
    assert(net_upnp_is_private_ipv4("100.63.255.255") == 0);
    assert(net_upnp_is_private_ipv4("100.64.0.1") == 1);
    assert(net_upnp_is_private_ipv4("100.127.255.255") == 1);
    assert(net_upnp_is_private_ipv4("100.128.0.0") == 0);
    assert(net_upnp_is_private_ipv4("127.0.0.1") == 1);
    assert(net_upnp_is_private_ipv4("169.254.10.1") == 1);
    assert(net_upnp_is_private_ipv4("0.0.0.0") == 1);
    assert(net_upnp_is_private_ipv4("224.0.0.1") == 1);
    assert(net_upnp_is_private_ipv4("8.8.8.8") == 0);
    assert(net_upnp_is_private_ipv4("203.0.113.9") == 0);
    assert(net_upnp_is_private_ipv4("1.2.3.4") == 0);
    /* Not dotted IPv4. */
    static const char* const bad[] = { "", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.4 ", " 1.2.3.4", "1..2.3",
                                       "a.b.c.d", "1.2.3.-4", "0001.2.3.4", "::1", "1.2.3.4x", "1.2.3." };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(net_upnp_is_private_ipv4(bad[i]) == -1);
    assert(net_upnp_is_private_ipv4(NULL) == -1);

    assert(net_upnp_lease() == -1);
    net_upnp_unmap();   /* nothing mapped: nothing to do */
    net_upnp_unmap_bounded(1000);   /* likewise, at once */

    char ext[16] = "";
#ifndef PS3RECOMP_UPNP
    assert(net_upnp_map(TEST_PORT, "ps3recomp test", 300, ext) == NET_UPNP_ERR_NOT_BUILT);
    assert(!strcmp(net_upnp_last_error(), "UPnP not built in") && !ext[0] && !*net_upnp_lan_ip());
    printf("test_net_upnp: built without UPnP; the stub answers\n");
#else
    assert(net_upnp_map(0, NULL, 300, ext) == NET_UPNP_ERR_ARGS && *net_upnp_last_error());

    /* One real search. */
    time_t t0 = time(NULL);
    int r = net_upnp_map(TEST_PORT, "ps3recomp test", 300, ext);
    long took = (long)(time(NULL) - t0);
    printf("test_net_upnp: map -> %d in about %ld s: %s\n", r, took, r ? net_upnp_last_error() : ext);
    assert(documented(r) && r != NET_UPNP_ERR_NOT_BUILT && r != NET_UPNP_ERR_OFFLINE && r != NET_UPNP_ERR_ARGS);
    assert(took < 60);
    if (r == NET_UPNP_OK || r == NET_UPNP_ERR_PRIVATE_IP) {
        /* Mapped: the address and the lease are known, the reason says why it isn't public (or nothing). */
        assert(net_upnp_is_private_ipv4(ext) == (r == NET_UPNP_ERR_PRIVATE_IP));
        assert(*net_upnp_lan_ip() && (net_upnp_lease() == 3600 || net_upnp_lease() == 0));
        printf("test_net_upnp: lease %d s (0: the router takes only permanent ones)\n", net_upnp_lease());
        assert((r == NET_UPNP_OK) == !*net_upnp_last_error());
        /* Again for the same port: it is ours already, so the same answer (a renewal). */
        char ext2[16] = "";
        assert(net_upnp_map(TEST_PORT, "ps3recomp test", 300, ext2) == r && !strcmp(ext, ext2));
        net_upnp_unmap();
        assert(net_upnp_lease() == -1);
        /* Mapped again, then removed the way a closing window does: within the time given. */
        assert(net_upnp_map(TEST_PORT, "ps3recomp test", 300, ext2) == r);
        assert(s_mapped);
        net_upnp_unmap_bounded(5000);
        assert(s_unmap_done && !s_mapped && net_upnp_lease() == -1);
    } else {
        /* Not mapped: a reason, and nothing left to remove. */
        assert(*net_upnp_last_error() && net_upnp_lease() == -1 && !ext[0]);
        if (r == NET_UPNP_ERR_NO_ROUTER)
            assert(!strncmp(net_upnp_last_error(), "no UPnP router found on the network", 35) && !*net_upnp_lan_ip());
        else
            printf("test_net_upnp: a router answered; this is its answer, not a failure of the test\n");
    }

    /* A session locked offline opens nothing. */
    ps3_net_lock_offline();
    assert(net_upnp_map(TEST_PORT, NULL, 300, ext) == NET_UPNP_ERR_OFFLINE);
    assert(!strcmp(net_upnp_last_error(), "this session is offline"));
#endif

    printf("test_net_upnp: all passed\n");
    return 0;
}
