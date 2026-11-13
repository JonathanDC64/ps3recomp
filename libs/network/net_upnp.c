/*
 * ps3recomp - UPnP port mapping on the player's router, with miniupnpc (third_party/miniupnpc). See
 * ps3emu/net_upnp.h.
 *
 * miniupnpc keeps no state between calls; ours (the port, which protocols we mapped, the router's control URL for
 * the removal) is under one lock, held for a whole net_upnp_map. Only addresses go in the log.
 */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <windows.h>
#  if defined(_MSC_VER) && defined(PS3RECOMP_UPNP)
#    pragma comment(lib, "ws2_32.lib")
#    pragma comment(lib, "iphlpapi.lib")   /* miniupnpc: GetAdaptersAddresses, for the discovery's interface */
#  endif
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&s_lock)
#  define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#  define UPNP_TLS __declspec(thread)
#else
#  define UPNP_TLS _Thread_local
#endif

#include "ps3emu/net_upnp.h"
#include "ps3emu/ps3_net.h"   /* ps3_net_ip_local, ps3_net_locked_offline */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>   /* nanosleep */

#ifdef PS3RECOMP_UPNP
#  ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdocumentation"   /* upstream's doc comments */
#  endif
#  include "miniupnpc.h"
#  include "upnpcommands.h"
#  include "upnperrors.h"
#  ifdef __clang__
#    pragma clang diagnostic pop
#  endif
#endif

static char s_err[192];   /* the last failure's reason (LOCK) */
static char s_lan[64];    /* this PC's address toward the router (LOCK) */
static int  s_lease = -1;
static volatile int s_mapped;      /* something of ours is mapped: read without the lock (net_upnp_unmap_bounded) */
static volatile int s_unmap_done;  /* net_upnp_unmap_bounded's thread finished */

static UPNP_TLS char t_err[sizeof(s_err)];
static UPNP_TLS char t_lan[sizeof(s_lan)];

/* ---- addresses ---- */

/* Strict dotted IPv4: four 0-255 numbers of 1-3 digits, nothing around them. 0, or -1. */
static int parse_ipv4(const char* s, uint8_t out[4])
{
    if (!s) return -1;
    for (int i = 0; i < 4; i++) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
            if (++digits > 3 || v > 255) return -1;
        }
        if (!digits) return -1;
        out[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return -1;
    }
    return *s ? -1 : 0;
}

int net_upnp_is_private_ipv4(const char* ip)
{
    uint8_t a[4];
    if (parse_ipv4(ip, a) != 0) return -1;
    return ps3_net_ip_local(a) ? 1 : 0;
}

const char* net_upnp_last_error(void)
{
    LOCK();
    memcpy(t_err, s_err, sizeof(t_err));
    UNLOCK();
    return t_err;
}

const char* net_upnp_lan_ip(void)
{
    LOCK();
    memcpy(t_lan, s_lan, sizeof(t_lan));
    UNLOCK();
    return t_lan;
}

int net_upnp_lease(void)
{
    LOCK();
    int l = s_lease;
    UNLOCK();
    return l;
}

#if defined(__GNUC__) || defined(__clang__)
#  define UPNP_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define UPNP_PRINTF(f, a)
#endif

/* The reason for the log and net_upnp_last_error; returns code. (LOCK) */
static int fail(int code, const char* fmt, ...) UPNP_PRINTF(2, 3);
static int fail(int code, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    printf("[upnp] failed: %s\n", s_err);
    return code;
}

#ifndef PS3RECOMP_UPNP

int net_upnp_map(uint16_t port, const char* description, int timeout_ms, char ext_ip_out[16])
{
    (void)port; (void)description; (void)timeout_ms; (void)ext_ip_out;
    LOCK();
    int r = fail(NET_UPNP_ERR_NOT_BUILT, "UPnP not built in");
    UNLOCK();
    return r;
}

void net_upnp_unmap(void) {}

void net_upnp_unmap_bounded(int ms) { (void)ms; }

#else   /* PS3RECOMP_UPNP */

static const char* const PROTO[2] = { "UDP", "TCP" };

static uint16_t s_port;        /* the port of the mappings below; 0 none */
static int      s_added[2];    /* UDP, TCP: ours, removed by net_upnp_unmap */
static char*    s_ctl;         /* the router's control URL and service type, for the removal */
static char     s_st[MINIUPNPC_URL_MAXSIZE];
#ifdef _WIN32
static int      s_wsa;         /* Winsock started */
#endif

/* Text from the network, for the log: printable, short. */
static const char* clean(const char* in, char* out, size_t cap)
{
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++)
        out[n++] = (*in > ' ' && *in < 127) ? *in : '?';
    out[n] = 0;
    return out;
}

/* The host part of a URL ("http://192.168.1.1:5000/desc.xml" -> "192.168.1.1"), for the log. */
static const char* url_host(const char* url, char* out, size_t cap)
{
    const char* p = url ? strstr(url, "://") : NULL;
    size_t n = 0;
    p = p ? p + 3 : (url ? url : "");
    for (; *p && *p != '/' && *p != ':' && n + 1 < cap; p++)
        out[n++] = ((*p >= '0' && *p <= '9') || *p == '.' || (*p >= 'a' && *p <= 'z') ||
                    (*p >= 'A' && *p <= 'Z') || *p == '-') ? *p : '?';
    out[n] = 0;
    return out;
}

/* A UPnP or miniupnpc error code in a few words. */
static const char* code_name(int r)
{
    switch (r) {
    case UPNPCOMMAND_HTTP_ERROR:       return "no HTTP answer";
    case UPNPCOMMAND_INVALID_RESPONSE: return "an answer it could not read";
    case UPNPCOMMAND_MEM_ALLOC_ERROR:  return "out of memory";
    case UPNPCOMMAND_UNKNOWN_ERROR:    return "unknown error";
    case 401: return "action not supported";
    case 402: return "invalid arguments";
    case 501: return "action failed";
    case 606: return "not authorized";
    case 714: return "no such entry";
    case 718: return "conflict";
    case 724: return "the same internal and external port required";
    case 725: return "only permanent leases supported";
    case 728: return "no free port mappings";
    case 729: return "conflict with other mechanisms";
    default: break;
    }
    const char* s = strupnperror(r);
    return s ? s : "unknown error";
}

/* Forward `port`/proto to lan:port. had: we mapped it on an earlier call. 0, with *owned (ours to remove later),
 * *fresh (this call made it) and *lease; or a NET_UPNP_ERR_* code with the reason set. (LOCK) */
static int map_one(const char* ctl, const char* st, const char* proto, uint16_t port, const char* lan,
                   const char* desc, int had, int* owned, int* fresh, int* lease)
{
    char eport[8], ip[40] = "", iport[16] = "", d[80] = "", en[8] = "", dur[16] = "", tmp[64];
    snprintf(eport, sizeof(eport), "%u", (unsigned)port);
    *owned = *fresh = *lease = 0;

    /* Already there? An error here (714, or a router without the call) means we just try to add it. */
    int here = 0;
    int r = UPNP_GetSpecificPortMappingEntry(ctl, st, eport, proto, NULL, ip, iport, d, en, dur);
    if (r == UPNPCOMMAND_SUCCESS && ip[0]) {
        if (strcmp(ip, lan) != 0)
            return fail(NET_UPNP_ERR_TAKEN, "port %s (%s) is already forwarded to another device (%s)",
                        eport, proto, clean(ip, tmp, sizeof(tmp)));
        if (atoi(iport) != (int)port)
            return fail(NET_UPNP_ERR_TAKEN, "port %s (%s) is already forwarded to port %s on this PC",
                        eport, proto, clean(iport, tmp, sizeof(tmp)));
        if (!had && strncmp(d, desc, sizeof(d) - 1) != 0) {
            /* Another program's, to this PC and port: it does the job; leave it alone. */
            *lease = atoi(dur);
            printf("[upnp] %s %s: already forwarded to this PC by another program; left as it is\n", proto, eport);
            return 0;
        }
        here = 1;   /* ours, from this run or an earlier one: renew it */
    }

    /* A 1-hour lease first: the caller renews it by mapping again, so a forward a crash left behind, or one no longer
     * renewed, ends by itself within the hour. A router that takes only permanent leases (725), or refuses the lease
     * for another reason that isn't about the port, gets a permanent one (net_upnp_unmap removes it). If that is
     * refused too, the first refusal is the real one, unless it was 725. */
    int l = 3600;
    r = UPNP_AddPortMapping(ctl, st, eport, eport, lan, desc, proto, NULL, "3600");
    if (r > 0 && r != 606 && r != 718 && r != 728 && r != 729) {
        int r2 = UPNP_AddPortMapping(ctl, st, eport, eport, lan, desc, proto, NULL, "0");
        if (r2 == UPNPCOMMAND_SUCCESS) { r = 0; l = 0; }
        else if (r == 725) r = r2;
    }
    if (r == UPNPCOMMAND_SUCCESS) {
        *owned = 1;
        *fresh = !here;
        *lease = l;
        printf("[upnp] %s %s -> %s:%s %s (%s)\n", proto, eport, lan, eport, here ? "renewed" : "forwarded",
               l ? "lease 3600 s" : "permanent");
        return 0;
    }
    if (here) {   /* still there from before; the renewal is what failed */
        *owned = 1;
        *lease = atoi(dur);
        printf("[upnp] %s %s: already forwarded to this PC; the renewal was refused (error %d: %s)\n",
               proto, eport, r, code_name(r));
        return 0;
    }
    if (r < 0)
        return fail(NET_UPNP_ERR_NETWORK, "could not talk to the router (%s)", code_name(r));
    return fail(NET_UPNP_ERR_REFUSED, "the router refused the port mapping (error %d: %s)", r, code_name(r));
}

static void delete_one(const char* ctl, const char* st, uint16_t port, int i)
{
    char eport[8];
    snprintf(eport, sizeof(eport), "%u", (unsigned)port);
    int r = UPNP_DeletePortMapping(ctl, st, eport, PROTO[i], NULL);
    if (r == UPNPCOMMAND_SUCCESS)
        printf("[upnp] %s %s: forward removed\n", PROTO[i], eport);
    else
        printf("[upnp] %s %s: not removed (error %d: %s)\n", PROTO[i], eport, r, code_name(r));
}

/* (LOCK) */
static void unmap_locked(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_added[i] && s_ctl) delete_one(s_ctl, s_st, s_port, i);
        s_added[i] = 0;
    }
    free(s_ctl);
    s_ctl = NULL;
    s_port = 0;
    s_lease = -1;
    s_mapped = 0;
}

void net_upnp_unmap(void)
{
    LOCK();
    unmap_locked();
    UNLOCK();
}

#ifdef _WIN32
static DWORD WINAPI unmap_thread(LPVOID p)
{
    (void)p;
    net_upnp_unmap();
    s_unmap_done = 1;
    return 0;
}
#else
static void* unmap_thread(void* p)
{
    (void)p;
    net_upnp_unmap();
    s_unmap_done = 1;
    return NULL;
}
#endif

void net_upnp_unmap_bounded(int ms)
{
    if (!s_mapped) return;
    s_unmap_done = 0;
    int started = 0;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, unmap_thread, NULL, 0, NULL);
    if (th) { CloseHandle(th); started = 1; }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, unmap_thread, NULL) == 0) { pthread_detach(th); started = 1; }
#endif
    if (!started) return;
    for (int t = 0; t < ms && !s_unmap_done; t += 10) {
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, NULL);
#endif
    }
    if (!s_unmap_done) printf("[upnp] the router didn't answer the removal within %d ms: its lease ends it\n", ms);
}

/* The router's control URL and service type, or a NET_UPNP_ERR_* code. (LOCK) */
static int find_router(int timeout_ms, struct UPNPUrls* urls, struct IGDdatas* data)
{
    char wan[64] = "", host[64];
    int err = 0;
    printf("[upnp] looking for a router (%d ms per search)\n", timeout_ms);
    struct UPNPDev* devs = upnpDiscover(timeout_ms, NULL, NULL, UPNP_LOCAL_PORT_ANY, 0, 2, &err);
    if (!devs) {
        if (err == UPNPDISCOVER_SOCKET_ERROR)
            return fail(NET_UPNP_ERR_NETWORK, "could not search the network for a router (socket error)");
        if (err == UPNPDISCOVER_MEMORY_ERROR)
            return fail(NET_UPNP_ERR_NETWORK, "out of memory searching for a router");
        return fail(NET_UPNP_ERR_NO_ROUTER, "no UPnP router found on the network (UPnP may be off in its settings)");
    }
    int n = 0;
    for (struct UPNPDev* d = devs; d; d = d->pNext) n++;
    printf("[upnp] %d UPnP device%s answered\n", n, n == 1 ? "" : "s");

    int state = UPNP_GetValidIGD(devs, urls, data, s_lan, sizeof(s_lan), wan, sizeof(wan));
    freeUPNPDevlist(devs);
    if (state < 0)
        return fail(NET_UPNP_ERR_NETWORK, "out of memory reading the router's description");
    if (state == UPNP_NO_IGD)
        return fail(NET_UPNP_ERR_NOT_ROUTER, "UPnP devices answered, but none could be read as a router");
    if (state == UPNP_UNKNOWN_DEVICE) {
        FreeUPNPUrls(urls);
        return fail(NET_UPNP_ERR_NOT_ROUTER, "UPnP devices answered, but none is a router (IGD)");
    }
    if (!s_lan[0] || !urls->controlURL || !urls->controlURL[0]) {
        FreeUPNPUrls(urls);
        return fail(NET_UPNP_ERR_NETWORK, "the router's description gave no address to send commands to");
    }
    printf("[upnp] router %s, this PC %s%s\n", url_host(urls->rootdescURL, host, sizeof(host)), s_lan,
           state == UPNP_DISCONNECTED_IGD ? " (the router says it is not connected; trying anyway)" : "");
    return 0;
}

int net_upnp_map(uint16_t port, const char* description, int timeout_ms, char ext_ip_out[16])
{
    char desc[80], ext[16] = "", tmp[64];   /* ext: miniupnpc writes up to 16 bytes */
    {
        const char* in = (description && *description) ? description : "ps3recomp";
        size_t n = 0;
        /* SOAP arguments go into XML as they are: no markup characters. */
        for (; *in && n + 1 < sizeof(desc); in++)
            desc[n++] = (*in >= ' ' && *in < 127 && !strchr("<>&\"'", *in)) ? *in : '_';
        desc[n] = 0;
    }
    if (timeout_ms <= 0) timeout_ms = 2000;

    LOCK();
    s_err[0] = 0;
    if (!port) { int r = fail(NET_UPNP_ERR_ARGS, "no port given"); UNLOCK(); return r; }
    if (ps3_net_locked_offline()) {
        int r = fail(NET_UPNP_ERR_OFFLINE, "this session is offline");
        UNLOCK();
        return r;
    }
    if (s_port && s_port != port) {
        printf("[upnp] the port changes from %u to %u: removing the old forwards\n", (unsigned)s_port, (unsigned)port);
        unmap_locked();
    }
#ifdef _WIN32
    if (!s_wsa) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
            int r = fail(NET_UPNP_ERR_NETWORK, "Winsock did not start");
            UNLOCK();
            return r;
        }
        s_wsa = 1;
    }
#endif
    s_lan[0] = 0;

    struct UPNPUrls urls;
    struct IGDdatas data;
    memset(&urls, 0, sizeof(urls));
    memset(&data, 0, sizeof(data));
    int r = find_router(timeout_ms, &urls, &data);
    if (r) { UNLOCK(); return r; }
    const char* ctl = urls.controlURL;
    const char* st = data.first.servicetype;

    /* The public address, before anything is mapped. */
    int er = UPNP_GetExternalIPAddress(ctl, st, ext);
    if (er != UPNPCOMMAND_SUCCESS)
        r = fail(er < 0 ? NET_UPNP_ERR_NETWORK : NET_UPNP_ERR_NO_WAN_IP,
                 "the router did not give its public address (error %d: %s)", er, code_name(er));
    else if (!ext[0] || !strcmp(ext, "0.0.0.0"))
        r = fail(NET_UPNP_ERR_NO_WAN_IP, "the router has no public address (not connected to the internet?)");
    else if (net_upnp_is_private_ipv4(ext) < 0)
        r = fail(NET_UPNP_ERR_NO_WAN_IP, "the router gave an address that isn't IPv4 (%s)", clean(ext, tmp, sizeof(tmp)));
    if (r) { FreeUPNPUrls(&urls); UNLOCK(); return r; }
    int priv = net_upnp_is_private_ipv4(ext) == 1;
    printf("[upnp] the router's address: %s%s\n", ext, priv ? " (not public: double NAT or carrier-grade NAT?)" : "");

    int owned[2] = { 0, 0 }, fresh[2] = { 0, 0 }, lease[2] = { 0, 0 };
    int i = 0;
    for (; i < 2; i++) {
        int had = s_port == port && s_added[i];
        r = map_one(ctl, st, PROTO[i], port, s_lan, desc, had, &owned[i], &fresh[i], &lease[i]);
        if (r) break;
    }
    if (r) {
        /* Undo what this call made, so a failure leaves the router as it was. */
        for (int j = 0; j < i; j++)
            if (fresh[j]) { delete_one(ctl, st, port, j); if (s_port == port) s_added[j] = 0; }
        FreeUPNPUrls(&urls);
        UNLOCK();
        return r;
    }

    s_port = port;
    s_lease = 0;
    for (i = 0; i < 2; i++) {
        s_added[i] = owned[i];
        if (lease[i] > 0 && (s_lease == 0 || lease[i] < s_lease)) s_lease = lease[i];
    }
    s_mapped = s_added[0] || s_added[1];
    free(s_ctl);
    s_ctl = (char*)malloc(strlen(ctl) + 1);
    if (s_ctl) memcpy(s_ctl, ctl, strlen(ctl) + 1);
    snprintf(s_st, sizeof(s_st), "%s", st);
    FreeUPNPUrls(&urls);

    if (ext_ip_out) snprintf(ext_ip_out, 16, "%s", ext);
    if (priv) {
        r = NET_UPNP_ERR_PRIVATE_IP;
        snprintf(s_err, sizeof(s_err), "port %u is forwarded, but the router's address (%s) is not public "
                 "(double NAT or carrier-grade NAT?), so friends can't reach it", (unsigned)port, ext);
        printf("[upnp] %s\n", s_err);
    } else {
        printf("[upnp] port %u open on %s\n", (unsigned)port, ext);
    }
    UNLOCK();
    return r;
}

#endif  /* PS3RECOMP_UPNP */
