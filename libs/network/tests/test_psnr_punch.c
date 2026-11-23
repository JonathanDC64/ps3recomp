/*
 * test_psnr_punch - the NAT matrix: a friend's game joins a hosted game with a room code (np_psnr.c's rc_try against
 * psnr/psnr_host.c) through simulated routers, with a stub endpoint, all in this process.
 *
 * The routers and the internet are a virtual network in memory (macOS has only 127.0.0.1, so no loopback aliases): the
 * room server's UDP goes through PH_SENDTO and PH_TEST_UDP_IN, the friend's P2P port is a fake sender and calls of
 * np_psnr_p2p_filter, and PSNR_TCP_TARGET says where a TCP connection really goes (the room server's real listener,
 * when the host's router forwards the port). Router types: full cone, address-restricted, port-restricted, symmetric,
 * symmetric with sequential ports, and double NAT (two port-restricted layers). Every ordered pair runs a lookup,
 * punching and a HELLO with short timers, and the result must be the one docs/162 expects. Then: a host with its port
 * open (or only its TCP port), both games on one network, and the lookup's other answers. Then the join check
 * (psnr_join_check_*, a socket of its own at the friend's address): the same matrix, the same answers, the ask and the
 * host's game's answers, a direct code, a name in use, and an end while it runs; the game's own join stays as it was.
 * Last, the background join itself: it connects, keeps the session up, dials again when the host stops, and gives up
 * when the room is gone.
 *
 *   clang-cl /I include /Fe:test_psnr_punch.exe libs/network/tests/test_psnr_punch.c
 *            third_party/monocypher/monocypher.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_psnr_punch libs/network/tests/test_psnr_punch.c
 *      third_party/monocypher/monocypher.c -lpthread
 */
#ifdef _WIN32
#  define _CRT_RAND_S   /* net_invite.c's rand_s, before the first stdlib.h of this one file */
#endif
#include <stdint.h>
#include <stddef.h>

/* ---- the seams, before the files they are in ---- */
static int vnet_tcp_target(uint8_t ip[4], uint16_t* port);
#define PSNR_TCP_TARGET(ip, port) vnet_tcp_target(ip, port)
#include "../psnr/psnr.c"
#include "../psnr/psnr_rudp.c"
#include "../psnr/psnr_intro.c"

static void   vnet_host_send(const void* buf, size_t len, const struct sockaddr_in* to);
static size_t vnet_host_pop(uint8_t* buf, size_t cap, struct sockaddr_in* from);
#define PH_SENDTO(h, buf, len, to) vnet_host_send((buf), (len), (to))
#define PH_TEST_UDP_IN(h) do {                                                       \
        uint8_t b_[2048];                                                            \
        struct sockaddr_in f_;                                                       \
        size_t n_;                                                                   \
        while ((n_ = vnet_host_pop(b_, sizeof b_, &f_)) > 0) ph_udp_in((h), b_, n_, &f_); \
    } while (0)
#define PH_KNOCK_FOR_MS  1500
#define PH_REG_RETRY_MS  200
#include "../psnr/psnr_host.c"

static void vnet_local_ip(uint8_t out[4]);
#define NP_PSNR_LOCAL_IP(to, out) vnet_local_ip(out)
/* the join check's socket: a port of its own at the friend's address */
struct psnr_udp;
static struct psnr_udp* vnet_check_open(uint16_t* port);
static int vnet_check_send(const void* buf, size_t len, const uint8_t ip[4], uint16_t port);
static int vnet_check_recv(void* buf, size_t cap, uint8_t ip[4], uint16_t* port, int ms);
#define NP_JC_UDP_OPEN(port)                     vnet_check_open(port)
#define NP_JC_UDP_SEND(u, buf, len, ip, port)    ((void)(u), vnet_check_send((buf), (len), (ip), (port)))
#define NP_JC_UDP_RECV(u, buf, cap, ip, port, ms) vnet_check_recv((buf), (cap), (ip), (port), (ms))
#define NP_JC_UDP_CLOSE(u)                       ((void)(u))
static volatile int s_checks_freed;
#define NP_JC_FREED() (s_checks_freed++)
#define NP_RC_LOOKUP_MS     300
#define NP_RC_LOOKUP_FOR_MS 1500
#define NP_RC_OPEN_MS       100
#define NP_RC_PUNCH_FOR_MS  1500
#define NP_RC_SETTLE_MS     700
#define NP_RC_GONE_MS       1500
#include "../np_psnr.c"
#include "../net_invite.c"

#ifdef _WIN32
#  define sleep_ms(n) Sleep(n)
#else
#  include <unistd.h>
#  define sleep_ms(n) usleep((n) * 1000)
#endif

/* ---- what np_psnr.c and psnr.c need from the rest of the runtime: no sign-in, no TLS ---- */
const char* np_fake_username(void) { return "PS3Player"; }
int  ps3_net_online(void) { return 1; }
void ps3_net_lock_offline(void) {}
int  ps3_net_locked_offline(void) { return 0; }
int  ps3_net_signin_enabled(void) { return 0; }
int  ps3_net_session(char* token, size_t cap, uint8_t raw[32]) { (void)token; (void)cap; (void)raw; return 0; }
void ps3_net_session_drop(void) {}
static char s_notice[256];
void ps3_net_signin_post_notice(const char* text) { snprintf(s_notice, sizeof s_notice, "%s", text); }
int ps3_tls_enabled(void) { return 0; }
int ps3_tls_server_name(char* out, size_t cap) { (void)out; (void)cap; return -1; }
ps3_tls* ps3_tls_connect(intptr_t s, const char* n, int ms, char* why, size_t cap)
{ (void)s; (void)n; (void)ms; (void)why; (void)cap; return NULL; }
int ps3_tls_send_all(ps3_tls* t, const void* p, size_t n, int ms) { (void)t; (void)p; (void)n; (void)ms; return -1; }
int ps3_tls_recv(ps3_tls* t, void* p, size_t n, int ms) { (void)t; (void)p; (void)n; (void)ms; return -1; }
void ps3_tls_close(ps3_tls* t) { (void)t; }

static int s_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); s_fail++; } } while (0)

/* ---- the virtual network ---- */

enum { NAT_FULL, NAT_ADDR, NAT_PORT, NAT_SYM, NAT_SYMSEQ, NAT_DOUBLE, NAT_TYPES };
static const char* const kNatName[NAT_TYPES] = {
    "full cone", "address-restr.", "port-restr.", "symmetric", "symmetric, seq.", "double NAT"
};

#define MAPS  64
#define PERMS 256
typedef struct router {
    int      type;                /* NAT_FULL .. NAT_SYMSEQ (a double NAT is two NAT_PORT routers) */
    uint8_t  wan[4];              /* its outside address: public, or in the LAN of the router in front */
    uint8_t  lan;                 /* its LAN: 192.168.<lan>.x */
    struct router* up;            /* the router in front of it, or NULL: on the internet */
    int      fwd_udp, fwd_tcp;    /* a port forward to fwd_ip:fwd_port, the same number outside */
    uint8_t  fwd_ip[4];
    uint16_t fwd_port;
    struct { uint8_t in_ip[4], dst_ip[4]; uint16_t in_port, dst_port, out; } map[MAPS];
    int      nmap;
    struct { uint8_t ip[4]; uint16_t out, port; } perm[PERMS];   /* sent from out to ip:port: answers may come */
    int      nperm;
    uint16_t next_port;
} router;

static router s_r[4];             /* host outer, host inner, friend outer, friend inner */
static router* s_host_r;          /* the routers right in front of each game (NULL: on the internet) */
static router* s_friend_r;
static uint8_t s_host_ip[4], s_friend_ip[4];
static uint16_t s_hport;          /* the room server's port (its real loopback listener and the virtual one) */
static const uint8_t kEndpoint[4] = {198, 51, 100, 1};
#define EP_PORT 36100
#define FRIEND_PORT 3658
#define CHECK_PORT 3700           /* the join check's socket, at the friend's address */
static int s_endpoint_off;        /* the endpoint drops everything (can't be reached) */
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;

static uint32_t vrand(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

static int in_lan(const uint8_t ip[4], uint8_t lan) { return ip[0] == 192 && ip[1] == 168 && ip[2] == lan; }
static int ip_eq(const uint8_t a[4], const uint8_t b[4]) { return !memcmp(a, b, 4); }

static uint16_t nat_out(router* r, const uint8_t sip[4], uint16_t sport, const uint8_t dip[4], uint16_t dport)
{
    uint16_t out = 0;
    if (r->fwd_udp && ip_eq(sip, r->fwd_ip) && sport == r->fwd_port) {
        out = r->fwd_port;   /* (a forwarded port keeps its number outside) */
    } else {
        const int per_dest = r->type == NAT_SYM || r->type == NAT_SYMSEQ;
        for (int i = 0; i < r->nmap && !out; i++)
            if (ip_eq(r->map[i].in_ip, sip) && r->map[i].in_port == sport &&
                (!per_dest || (ip_eq(r->map[i].dst_ip, dip) && r->map[i].dst_port == dport)))
                out = r->map[i].out;
        if (!out && r->nmap < MAPS) {
            out = r->type == NAT_SYM ? (uint16_t)(20000 + vrand() % 40000) : r->next_port++;
            memcpy(r->map[r->nmap].in_ip, sip, 4);
            memcpy(r->map[r->nmap].dst_ip, dip, 4);
            r->map[r->nmap].in_port = sport;
            r->map[r->nmap].dst_port = dport;
            r->map[r->nmap].out = out;
            r->nmap++;
        }
    }
    for (int i = 0; i < r->nperm; i++)
        if (r->perm[i].out == out && r->perm[i].port == dport && ip_eq(r->perm[i].ip, dip)) return out;
    const int k = r->nperm < PERMS ? r->nperm++ : (int)(vrand() % PERMS);
    memcpy(r->perm[k].ip, dip, 4);
    r->perm[k].out = out;
    r->perm[k].port = dport;
    return out;
}

/* A datagram from sip:sport to r's outside port dport: 1 with where it goes inside, 0 if r drops it. */
static int nat_in(router* r, const uint8_t sip[4], uint16_t sport, uint16_t dport, uint8_t iip[4], uint16_t* iport)
{
    if (r->fwd_udp && dport == r->fwd_port) {
        memcpy(iip, r->fwd_ip, 4);
        *iport = r->fwd_port;
        return 1;
    }
    int m = -1;
    for (int i = 0; i < r->nmap && m < 0; i++)
        if (r->map[i].out == dport) m = i;
    if (m < 0) return 0;
    int ok = r->type == NAT_FULL;
    for (int i = 0; i < r->nperm && !ok; i++)
        if (r->perm[i].out == dport && ip_eq(r->perm[i].ip, sip) &&
            (r->type == NAT_ADDR || r->perm[i].port == sport))
            ok = 1;
    if (!ok) return 0;
    memcpy(iip, r->map[m].in_ip, 4);
    *iport = r->map[m].in_port;
    return 1;
}

/* Deliveries, made on the network's own thread (the P2P port's thread, for the friend). */
enum { TO_ENDPOINT, TO_HOST, TO_FRIEND, TO_CHECK };
#define QLEN 2048
typedef struct { int to; uint8_t from_ip[4]; uint16_t from_port; uint16_t len; uint8_t b[1300]; } vpkt;
static vpkt s_q[QLEN];            /* to deliver */
static int s_qh, s_qt;
static vpkt s_hq[QLEN];           /* the room server's, for PH_TEST_UDP_IN */
static int s_hqh, s_hqt;
static vpkt s_cq[QLEN];           /* the join check's socket's */
static int s_cqh, s_cqt;
static ph_lock s_vlock = PH_LOCK_STATIC;
static volatile int s_net_stop;
static ph_sock s_bell = PH_BAD;   /* a datagram to the room server's real socket wakes its loop */
static int s_delivered, s_dropped;

/* From (sip, sport), whose innermost router is r, to dip:dport. Under s_vlock. */
static void route(router* r, const uint8_t sip0[4], uint16_t sport, const uint8_t dip[4], uint16_t dport,
                  const void* buf, size_t len)
{
    uint8_t sip[4];
    memcpy(sip, sip0, 4);
    while (r && !in_lan(dip, r->lan)) {   /* out through each router that doesn't have dip inside */
        sport = nat_out(r, sip, sport, dip, dport);
        memcpy(sip, r->wan, 4);
        r = r->up;
    }
    uint8_t ip[4];
    memcpy(ip, dip, 4);
    for (int hops = 0; hops < 4; hops++) {   /* in through each router whose outside address it is */
        router* in = NULL;
        for (int i = 0; i < 4 && !in; i++)
            if (s_r[i].lan && ip_eq(s_r[i].wan, ip)) in = &s_r[i];
        if (!in) break;
        uint8_t iip[4];
        uint16_t iport;
        if (!nat_in(in, sip, sport, dport, iip, &iport)) { s_dropped++; return; }
        memcpy(ip, iip, 4);
        dport = iport;
    }
    int to = -1;
    if (ip_eq(ip, kEndpoint) && dport == EP_PORT && !s_endpoint_off) to = TO_ENDPOINT;
    else if (ip_eq(ip, s_host_ip) && dport == s_hport) to = TO_HOST;
    else if (ip_eq(ip, s_friend_ip) && dport == FRIEND_PORT) to = TO_FRIEND;
    else if (ip_eq(ip, s_friend_ip) && dport == CHECK_PORT) to = TO_CHECK;
    if (to < 0 || len > sizeof s_q[0].b || (s_qt + 1) % QLEN == s_qh) { s_dropped++; return; }
    vpkt* p = &s_q[s_qt];
    p->to = to;
    memcpy(p->from_ip, sip, 4);
    p->from_port = sport;
    p->len = (uint16_t)len;
    memcpy(p->b, buf, len);
    s_qt = (s_qt + 1) % QLEN;
}

static void vnet_host_send(const void* buf, size_t len, const struct sockaddr_in* to)
{
    PH_LOCK(&s_vlock);
    route(s_host_r, s_host_ip, s_hport, (const uint8_t*)&to->sin_addr, ntohs(to->sin_port), buf, len);
    PH_UNLOCK(&s_vlock);
}

static size_t vnet_host_pop(uint8_t* buf, size_t cap, struct sockaddr_in* from)
{
    size_t n = 0;
    PH_LOCK(&s_vlock);
    if (s_hqh != s_hqt) {
        vpkt* p = &s_hq[s_hqh];
        n = p->len <= cap ? p->len : 0;
        memcpy(buf, p->b, n);
        memset(from, 0, sizeof *from);
        from->sin_family = AF_INET;
        memcpy(&from->sin_addr, p->from_ip, 4);
        from->sin_port = htons(p->from_port);
        s_hqh = (s_hqh + 1) % QLEN;
    }
    PH_UNLOCK(&s_vlock);
    return n;
}

static int vnet_friend_send(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    PH_LOCK(&s_vlock);
    route(s_friend_r, s_friend_ip, FRIEND_PORT, ip, port, buf, len);
    PH_UNLOCK(&s_vlock);
    return (int)len;
}

static void vnet_local_ip(uint8_t out[4]) { memcpy(out, s_friend_ip, 4); }

static int s_check_sock;
static struct psnr_udp* vnet_check_open(uint16_t* port)
{
    PH_LOCK(&s_vlock);
    s_cqh = s_cqt = 0;
    PH_UNLOCK(&s_vlock);
    *port = CHECK_PORT;
    return (struct psnr_udp*)&s_check_sock;
}

static int vnet_check_send(const void* buf, size_t len, const uint8_t ip[4], uint16_t port)
{
    PH_LOCK(&s_vlock);
    route(s_friend_r, s_friend_ip, CHECK_PORT, ip, port, buf, len);
    PH_UNLOCK(&s_vlock);
    return (int)len;
}

static int vnet_check_recv(void* buf, size_t cap, uint8_t ip[4], uint16_t* port, int ms)
{
    for (int t = 0; t <= ms; t++) {
        int n = 0;
        PH_LOCK(&s_vlock);
        if (s_cqh != s_cqt) {
            vpkt* p = &s_cq[s_cqh];
            n = p->len <= cap ? p->len : 0;
            memcpy(buf, p->b, n);
            memcpy(ip, p->from_ip, 4);
            *port = p->from_port;
            s_cqh = (s_cqh + 1) % QLEN;
        }
        PH_UNLOCK(&s_vlock);
        if (n) return n;
        sleep_ms(1);
    }
    return 0;
}

/* Where the friend's TCP connection really goes: the room server's listener, when the host's router forwards the
 * port, or when the two share a network and it goes to the host's home address. */
static int vnet_tcp_target(uint8_t ip[4], uint16_t* port)
{
    router* outer = s_host_r;
    while (outer && outer->up) outer = outer->up;
    const int fwd = outer && outer->fwd_tcp && ip_eq(ip, outer->wan) && *port == outer->fwd_port &&
                    outer == s_host_r;   /* (one router: its forward reaches the host) */
    const int lan = s_friend_r == s_host_r && ip_eq(ip, s_host_ip) && *port == s_hport;
    if (!fwd && !lan) return 0;
    ip[0] = 127; ip[1] = 0; ip[2] = 0; ip[3] = 1;
    *port = s_hport;
    return 1;
}

/* ---- the stub endpoint (the psnr server's intro.go, as far as these games need it) ---- */

static struct {
    uint64_t room, owner;
    uint16_t version, room_port;
    psnr_cand seen, cands[2];
    int n, used;
} s_rooms[8];

static void ep_send(const uint8_t ip[4], uint16_t port, const uint8_t* b, size_t n)
{
    route(NULL, kEndpoint, EP_PORT, ip, port, b, n);   /* (under s_vlock: called from endpoint_in) */
}

static int cand_dup(const psnr_cand* list, int n, const psnr_cand* c)
{
    for (int i = 0; i < n; i++)
        if (list[i].port == c->port && ip_eq(list[i].ip, c->ip)) return 1;
    return !c->port || !(c->ip[0] | c->ip[1] | c->ip[2] | c->ip[3]);
}

static void endpoint_in(const vpkt* p)
{
    psnr_intro_pkt in;
    if (!psnr_intro_read(p->b, p->len, &in)) return;
    const psnr_cand from = psnr_cand_of(p->from_ip, p->from_port);
    uint8_t out[64];
    PH_LOCK(&s_vlock);
    if (in.type == PSNR_PKT_REGISTER) {
        int k = -1;
        for (int i = 0; i < 8 && k < 0; i++)
            if (s_rooms[i].used && s_rooms[i].room == in.room) k = i;
        for (int i = 0; i < 8 && k < 0; i++)
            if (!s_rooms[i].used) k = i;
        if (k >= 0) {
            s_rooms[k].used = 1;
            s_rooms[k].room = in.room;
            s_rooms[k].owner = in.owner;
            s_rooms[k].version = in.version;
            s_rooms[k].room_port = in.room_port;
            s_rooms[k].seen = from;
            s_rooms[k].n = in.n < 2 ? in.n : 2;
            for (int i = 0; i < s_rooms[k].n; i++) s_rooms[k].cands[i] = in.cands[i];
            ep_send(p->from_ip, p->from_port, out, psnr_intro_registered(out, in.room, PSNR_INTRO_OK, &from, 20));
        }
    } else if (in.type == PSNR_PKT_UNREGISTER) {
        for (int i = 0; i < 8; i++)
            if (s_rooms[i].used && s_rooms[i].room == in.room && s_rooms[i].owner == in.owner) s_rooms[i].used = 0;
    } else if (in.type == PSNR_PKT_LOOKUP) {
        int k = -1;
        for (int i = 0; i < 8 && k < 0; i++)
            if (s_rooms[i].used && s_rooms[i].room == in.room) k = i;
        if (k < 0 || s_rooms[k].version != in.version) {
            ep_send(p->from_ip, p->from_port, out, psnr_intro_found(out, in.room, in.nonce,
                    k < 0 ? PSNR_INTRO_NO_ROOM : PSNR_INTRO_VERSION, 0, &from, 0, NULL, 0));
        } else {
            const uint8_t same = ip_eq(s_rooms[k].seen.ip, from.ip) ? PSNR_INTRO_SAME_NETWORK : 0;
            psnr_cand hc[3], fc[2];
            int hn = 0, fn = 0;
            hc[hn++] = s_rooms[k].seen;
            for (int i = 0; i < s_rooms[k].n; i++)
                if (!cand_dup(hc, hn, &s_rooms[k].cands[i])) hc[hn++] = s_rooms[k].cands[i];
            fc[fn++] = from;
            if (in.n && !cand_dup(fc, fn, &in.cands[0])) fc[fn++] = in.cands[0];
            ep_send(p->from_ip, p->from_port, out, psnr_intro_found(out, in.room, in.nonce, PSNR_INTRO_OK, same,
                    &from, s_rooms[k].room_port, hc, hn));
            ep_send(s_rooms[k].seen.ip, s_rooms[k].seen.port, out,
                    psnr_intro_intro(out, in.room, in.nonce, same, fc, fn));
        }
    }
    PH_UNLOCK(&s_vlock);
}

#ifdef _WIN32
static DWORD WINAPI net_main(LPVOID u)
#else
static void* net_main(void* u)
#endif
{
    (void)u;
    vpkt p;
    while (!s_net_stop) {
        int have = 0;
        PH_LOCK(&s_vlock);
        if (s_qh != s_qt) {
            p = s_q[s_qh];
            s_qh = (s_qh + 1) % QLEN;
            have = 1;
            s_delivered++;
        }
        PH_UNLOCK(&s_vlock);
        if (!have) { sleep_ms(1); continue; }
        if (p.to == TO_ENDPOINT) {
            endpoint_in(&p);
        } else if (p.to == TO_HOST) {
            PH_LOCK(&s_vlock);
            if ((s_hqt + 1) % QLEN != s_hqh) {
                s_hq[s_hqt] = p;
                s_hqt = (s_hqt + 1) % QLEN;
            }
            PH_UNLOCK(&s_vlock);
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(s_hport);
            sendto(s_bell, "x", 1, 0, (struct sockaddr*)&a, sizeof a);   /* (wakes its poll) */
        } else if (p.to == TO_CHECK) {
            PH_LOCK(&s_vlock);
            if ((s_cqt + 1) % QLEN != s_cqh) {
                s_cq[s_cqt] = p;
                s_cqt = (s_cqt + 1) % QLEN;
            }
            PH_UNLOCK(&s_vlock);
        } else {
            uint32_t n = p.len;
            uint8_t ip[4];
            uint16_t port = p.from_port;
            memcpy(ip, p.from_ip, 4);
            np_psnr_p2p_filter(p.b, &n, ip, &port);
        }
    }
    return 0;
}

/* ---- one join ---- */

static const char kComm[] = "NPWR00000_00";

static router* make_router(int at, int type, const uint8_t wan[4], uint8_t lan, router* up)
{
    router* r = &s_r[at];
    memset(r, 0, sizeof *r);
    r->type = type;
    memcpy(r->wan, wan, 4);
    r->lan = lan;
    r->up = up;
    r->next_port = (uint16_t)(40000 + 1000 * at);
    return r;
}

/* The routers in front of a game: its type, outside address, LAN; *inner the one right in front; the game's address. */
static void make_side(int first, int type, const uint8_t pub[4], uint8_t lan, uint8_t inner_lan, router** inner,
                      uint8_t ip[4], uint8_t last)
{
    if (type == NAT_DOUBLE) {
        router* o = make_router(first, NAT_PORT, pub, lan, NULL);
        const uint8_t mid[4] = {192, 168, lan, 2};
        *inner = make_router(first + 1, NAT_PORT, mid, inner_lan, o);
        ip[0] = 192; ip[1] = 168; ip[2] = inner_lan; ip[3] = last;
    } else {
        *inner = make_router(first, type, pub, lan, NULL);
        ip[0] = 192; ip[1] = 168; ip[2] = lan; ip[3] = last;
    }
}

enum { FWD_NONE, FWD_BOTH, FWD_TCP };
enum { JOIN_NO = 0, JOIN_UDP = 1, JOIN_TCP = 2 };

/* The routers for a host type and a friend type (-1: on the host's network), a room server behind them with the
 * host's port forward, registered with the endpoint; PSNR_ROOM set to its number. NULL if it didn't start. */
static psnr_host* host_up(int htype, int ftype, int fwd)
{
    static const uint8_t hpub[4] = {203, 0, 113, 5}, fpub[4] = {203, 0, 113, 9};
    PH_LOCK(&s_vlock);
    memset(s_r, 0, sizeof s_r);
    memset(s_rooms, 0, sizeof s_rooms);
    s_qh = s_qt = s_hqh = s_hqt = 0;
    make_side(0, htype, hpub, 1, 4, &s_host_r, s_host_ip, 10);
    if (ftype < 0) {   /* the same network as the host */
        s_friend_r = s_host_r;
        memcpy(s_friend_ip, s_host_ip, 4);
        s_friend_ip[3] = 20;
    } else {
        make_side(2, ftype, fpub, 2, 3, &s_friend_r, s_friend_ip, 20);
    }
    PH_UNLOCK(&s_vlock);

    psnr_host_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.bind_loopback = 1;
    cfg.has_key = 1;
    net_invite_key_blob(0x0D00D1E, cfg.key);
    char err[128] = "";
    psnr_host* h = psnr_host_start(&cfg, err, sizeof err);
    if (!h) { printf("start: %s\n", err); s_fail++; return NULL; }
    PH_LOCK(&s_vlock);
    s_hport = psnr_host_port(h);
    if (fwd != FWD_NONE) {
        s_host_r->fwd_tcp = 1;
        s_host_r->fwd_udp = fwd == FWD_BOTH;
        memcpy(s_host_r->fwd_ip, s_host_ip, 4);
        s_host_r->fwd_port = s_hport;
    }
    PH_UNLOCK(&s_vlock);
    psnr_host_intro in;
    memset(&in, 0, sizeof in);
    memcpy(in.ip, kEndpoint, 4);
    in.port = EP_PORT;
    in.version = 3;
    memcpy(in.home_ip, s_host_ip, 4);
    psnr_host_set_intro(h, &in);
    psnr_host_intro_info info;
    memset(&info, 0, sizeof info);
    for (int t = 0; t < 3000 && info.state != PSNR_HOST_INTRO_REGISTERED; t += 10) {
        psnr_host_intro_get(h, &info);
        if (info.state != PSNR_HOST_INTRO_REGISTERED) sleep_ms(10);
    }
    if (!s_endpoint_off) CHECK(info.state == PSNR_HOST_INTRO_REGISTERED);
    char v[24];
    snprintf(v, sizeof v, "%llu", (unsigned long long)info.room);
    set_env("PSNR_ROOM", v);
    return h;
}

/* One try (rc_try) with the lookup's version (the host registers 3) and key: the join, and the state it ended in. */
static int join(int htype, int ftype, int fwd, uint16_t version, const char* key_hex, int* state_out)
{
    psnr_host* h = host_up(htype, ftype, fwd);
    if (!h) return JOIN_NO;
    char v[24];
    snprintf(v, sizeof v, "%u", version);
    set_env("PSNR_VERSION", v);
    set_env("PSNR_KEY", key_hex);
    uint32_t user = 0;
    uint8_t ip[4];
    int e = 0, state = 0;
    psnr_client* c = rc_try(kComm, &user, ip, &e, &state);
    int r = JOIN_NO;
    if (c) {
        r = psnr_is_session(c) ? JOIN_UDP : JOIN_TCP;
        CHECK(state == PSNR_JOIN_CONNECTED && user != 0);
        sleep_ms(50);
        CHECK(psnr_host_players(h) == 1);   /* one HELLO, whichever opened first */
        LOCK();
        if (s_game.t.pend == c) s_game.t.pend = NULL;
        UNLOCK();
        psnr_close(c);
    }
    if (state_out) *state_out = state;
    psnr_host_stop(h);
    return r;
}

/* ---- the join check ---- */

/* The host's game, for the ask: a client of the room server on this PC that answers "ROM?" as s_hg_mode says
 * (1: "FGHT" then "ROOM", 2: "FULL", 0: nothing), on a thread of its own. */
static psnr_client* s_hg;
static volatile int s_hg_stop, s_hg_mode, s_hg_asked;

static void hg_send(const char* tag, uint32_t v)
{
    uint8_t b[2 + 8];
    psnr_put16(b, 8);
    memcpy(b + 2, tag, 4);
    b[6] = (uint8_t)v; b[7] = (uint8_t)(v >> 8); b[8] = (uint8_t)(v >> 16); b[9] = (uint8_t)(v >> 24);
    psnr_send(s_hg, PSNR_APP_SEND, b, sizeof b);
}

#ifdef _WIN32
static DWORD WINAPI hg_main(LPVOID u)
#else
static void* hg_main(void* u)
#endif
{
    (void)u;
    while (!s_hg_stop) {
        psnr_msg m;
        const int r = psnr_poll(s_hg, &m);
        if (r < 0) break;
        if (r == 0) { sleep_ms(5); continue; }
        if (m.type == PSNR_APP_MSG && m.len >= 10 && !memcmp(m.data + 6, "ROM?", 4)) {
            s_hg_asked++;
            if (s_hg_mode == 1) { hg_send("FGHT", 1); hg_send("ROOM", 7); }
            else if (s_hg_mode == 2) hg_send("FULL", 4);
        }
        psnr_msg_free(&m);
    }
    return 0;
}

static int room_or_full(const uint8_t* d, uint32_t n, void* user)
{
    (void)user;
    return n >= 4 && (!memcmp(d, "ROOM", 4) || !memcmp(d, "FULL", 4));
}

/* One join check to its end (a room code: PSNR_ROOM's number; a direct code: the address given): its status. */
static psnr_join_check_status check_run(const char* direct, uint16_t version, uint32_t key, int ask, int answer_ms,
                                        const char* name)
{
    psnr_join_check_config cfg;
    memset(&cfg, 0, sizeof cfg);
    char server[64];
    snprintf(server, sizeof server, "%s", direct ? direct : "198.51.100.1:36100");
    cfg.server = server;
    cfg.room = direct ? 0 : strtoull(getenv("PSNR_ROOM"), NULL, 10);
    cfg.version = version;
    net_invite_key_blob(key, cfg.key);
    cfg.comm_id = kComm;
    cfg.online_id = name;
    if (ask) { cfg.ask = "ROM?"; cfg.ask_len = 4; }
    cfg.answer_ms = answer_ms;
    cfg.answered = room_or_full;
    psnr_join_check_status st;
    memset(&st, 0, sizeof st);
    psnr_join_check* jc = psnr_join_check_start(&cfg);
    CHECK(jc != NULL);
    if (!jc) return st;
    for (int t = 0; t < 30000; t += 10) {
        psnr_join_check_get(jc, &st);
        if (st.done) break;
        sleep_ms(10);
    }
    CHECK(st.done);
    if (st.answers > 0) {
        uint8_t b[16];
        CHECK(psnr_join_check_answer(jc, 0, b, sizeof b) == 8);
        CHECK(!memcmp(b, ask && s_hg_mode == 2 ? "FULL" : "FGHT", 4));
    }
    sleep_ms(20);   /* (done is said just before its thread lets go) */
    const int freed = s_checks_freed;
    psnr_join_check_end(jc);
    CHECK(s_checks_freed == freed + 1);   /* (over: the end frees it at once) */
    return st;
}

/* The game's own join is untouched by a check: no connection, no try under way, its state as it was. */
static void game_join_untouched(int state_before)
{
    LOCK();
    const int none = s_client == NULL && s_game.t.pend == NULL && !s_game.t.active;
    UNLOCK();
    CHECK(none);
    CHECK(psnr_join_state() == state_before);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    s_bell = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    set_env("PS3_NET_ONLINE", "1");
    set_env("PSNR_SERVER", "198.51.100.1:36100");
    set_env("PS3_NP_ONLINE_ID", "Friend");
    set_env("PS3_NET_P2P_PORT", "3658");
    np_psnr_set_p2p_sender(vnet_friend_send);
    char key_hex[65], wrong_hex[65];
    net_invite_key_hex(0x0D00D1E, key_hex);
    net_invite_key_hex(0x0BADBAD, wrong_hex);
#ifdef _WIN32
    HANDLE nt = CreateThread(NULL, 0, net_main, NULL, 0, NULL);
#else
    pthread_t nt;
    pthread_create(&nt, NULL, net_main, NULL);
#endif

    /* docs/162 section 8: 1 joins, 0 doesn't (no port forward) */
    static const int kExpect[NAT_TYPES][NAT_TYPES] = {
        /* friend:        FULL ADDR PORT SYM SEQ DBL */
        /* host FULL */  { 1,   1,   1,   1,  1,  1 },
        /* host ADDR */  { 1,   1,   1,   1,  1,  1 },
        /* host PORT */  { 1,   1,   1,   0,  0,  1 },
        /* host SYM  */  { 1,   1,   0,   0,  0,  0 },
        /* host SEQ  */  { 1,   1,   0,   0,  0,  0 },
        /* host DBL  */  { 1,   1,   1,   0,  0,  1 },
    };
    printf("\n%-18s", "host \\ friend");
    for (int f = 0; f < NAT_TYPES; f++) printf("| %-16s", kNatName[f]);
    printf("\n");
    char table[NAT_TYPES][NAT_TYPES][8];
    for (int ht = 0; ht < NAT_TYPES; ht++) {
        for (int ft = 0; ft < NAT_TYPES; ft++) {
            const int r = join(ht, ft, FWD_NONE, 3, key_hex, NULL);
            snprintf(table[ht][ft], 8, "%s", r == JOIN_UDP ? "yes" : r == JOIN_TCP ? "TCP" : "no");
            if ((r != JOIN_NO) != kExpect[ht][ft] || r == JOIN_TCP) {
                printf("FAIL: host %s, friend %s: %s, expected %s\n", kNatName[ht], kNatName[ft], table[ht][ft],
                       kExpect[ht][ft] ? "yes" : "no");
                s_fail++;
            }
        }
    }
    printf("\nThe NAT matrix (a room code, no port forward):\n\n| Host \\ friend |");
    for (int f = 0; f < NAT_TYPES; f++) printf(" %s |", kNatName[f]);
    printf("\n|---|");
    for (int f = 0; f < NAT_TYPES; f++) printf("---|");
    printf("\n");
    for (int ht = 0; ht < NAT_TYPES; ht++) {
        printf("| %s |", kNatName[ht]);
        for (int ft = 0; ft < NAT_TYPES; ft++) printf(" %s |", table[ht][ft]);
        printf("\n");
    }

    /* a host with its port open joins every friend: over UDP, or TCP when punching can't */
    printf("\nA host behind a symmetric router with its port forwarded:\n");
    for (int ft = 0; ft < NAT_TYPES; ft++) {
        const int both = join(NAT_SYM, ft, FWD_BOTH, 3, key_hex, NULL);
        const int tcp = join(NAT_SYM, ft, FWD_TCP, 3, key_hex, NULL);
        printf("  friend %-16s TCP and UDP forwarded: %-4s TCP only: %s\n", kNatName[ft],
               both == JOIN_UDP ? "UDP" : both == JOIN_TCP ? "TCP" : "no", tcp == JOIN_UDP ? "UDP" : tcp == JOIN_TCP
               ? "TCP" : "no");
        CHECK(both != JOIN_NO && tcp != JOIN_NO);
    }

    /* both games on one network: the home addresses */
    {
        const int r = join(NAT_PORT, -1, FWD_NONE, 3, key_hex, NULL);
        printf("\nBoth on one network (port-restricted router, no hairpin): %s\n",
               r == JOIN_UDP ? "UDP" : r == JOIN_TCP ? "TCP" : "no");
        CHECK(r != JOIN_NO);
    }

    /* the other answers: another version, no such room, a wrong key, no endpoint */
    {
        int st = 0;
        CHECK(join(NAT_FULL, NAT_FULL, FWD_NONE, 4, key_hex, &st) == JOIN_NO && st == PSNR_JOIN_OTHER_VERSION);
        CHECK(join(NAT_FULL, NAT_FULL, FWD_NONE, 3, wrong_hex, &st) == JOIN_NO && st == PSNR_JOIN_REFUSED);
        set_env("PSNR_ROOM", "12345");   /* (join sets it again: the room number of a host that isn't there) */
        uint32_t user = 0;
        uint8_t ip[4];
        int e = 0;
        CHECK(rc_try(kComm, &user, ip, &e, &st) == NULL && st == PSNR_JOIN_NO_ROOM);
        s_endpoint_off = 1;
        CHECK(join(NAT_FULL, NAT_FULL, FWD_NONE, 3, key_hex, &st) == JOIN_NO && st == PSNR_JOIN_NO_SERVER);
        s_endpoint_off = 0;
        printf("\nAnother version, a wrong key, no such room, no endpoint: the states they should be\n");
    }

    /* the join check: the same matrix (its own socket, at the friend's address), with no ask */
    {
        const int before = psnr_join_state();
        int bad = 0;
        for (int ht = 0; ht < NAT_TYPES; ht++)
            for (int ft = 0; ft < NAT_TYPES; ft++) {
                psnr_host* h = host_up(ht, ft, FWD_NONE);
                if (!h) continue;
                const psnr_join_check_status st = check_run(NULL, 3, 0x0D00D1E, 0, 0, NULL);
                const int got = st.state == PSNR_JOIN_CONNECTED;
                if (got != kExpect[ht][ft] || (got && !st.udp) || (!got && st.state != PSNR_JOIN_NO_WAY)) {
                    printf("FAIL: the check, host %s, friend %s: state %d, expected %s\n", kNatName[ht], kNatName[ft],
                           st.state, kExpect[ht][ft] ? "connected" : "not");
                    s_fail++;
                    bad++;
                }
                psnr_host_stop(h);
            }
        game_join_untouched(before);
        printf("\nThe join check over the NAT matrix: %s\n", bad ? "differs" : "the same as the join");
    }

    /* the check's other answers: another version, a wrong key, no such room, no endpoint, a name in use */
    {
        const int before = psnr_join_state();
        psnr_host* h = host_up(NAT_PORT, NAT_PORT, FWD_NONE);
        psnr_join_check_status st = check_run(NULL, 4, 0x0D00D1E, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_OTHER_VERSION);
        st = check_run(NULL, 3, 0x0BADBAD, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_REFUSED && st.err == PSNR_E_REFUSED);
        set_env("PSNR_ROOM", "12345");
        st = check_run(NULL, 3, 0x0D00D1E, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_NO_ROOM);
        psnr_host_stop(h);
        s_endpoint_off = 1;
        h = host_up(NAT_PORT, NAT_PORT, FWD_NONE);
        st = check_run(NULL, 3, 0x0D00D1E, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_NO_SERVER);
        psnr_host_stop(h);
        s_endpoint_off = 0;
        /* "Friend" is already a player there, from this PC: the same name from another address */
        h = host_up(NAT_PORT, NAT_PORT, FWD_NONE);
        uint8_t key[32];
        net_invite_key_blob(0x0D00D1E, key);
        psnr_client* other = psnr_connect("127.0.0.1", s_hport, kComm, "Friend", 0, key, NULL, NULL);
        CHECK(other != NULL);
        st = check_run(NULL, 3, 0x0D00D1E, 0, 0, "Friend");
        CHECK(st.state == PSNR_JOIN_NO_WAY && st.err == PSNR_E_NAME_TAKEN);
        if (other) psnr_close(other);
        psnr_host_stop(h);
        game_join_untouched(before);
        printf("\nThe check's other answers (another version, a wrong key, no such room, no endpoint, a name in use): "
               "%s\n", s_fail ? "not all" : "the states they should be");
    }

    /* the ask: the host's game answers it (a boss fight, then its room), says it is full, or says nothing */
    {
        psnr_host* h = host_up(NAT_PORT, NAT_PORT, FWD_NONE);
        uint8_t key[32];
        net_invite_key_blob(0x0D00D1E, key);
        s_hg = psnr_connect("127.0.0.1", s_hport, kComm, "Host", 0, key, NULL, NULL);
        CHECK(s_hg != NULL);
        s_hg_stop = 0;
        s_hg_asked = 0;
#ifdef _WIN32
        HANDLE hgt = CreateThread(NULL, 0, hg_main, NULL, 0, NULL);
#else
        pthread_t hgt;
        pthread_create(&hgt, NULL, hg_main, NULL);
#endif
        s_hg_mode = 1;
        psnr_join_check_status st = check_run(NULL, 3, 0x0D00D1E, 1, 3000, NULL);
        CHECK(st.state == PSNR_JOIN_CONNECTED && st.answered && st.answers == 2);
        s_hg_mode = 2;
        st = check_run(NULL, 3, 0x0D00D1E, 1, 3000, NULL);
        CHECK(st.state == PSNR_JOIN_CONNECTED && st.answered && st.answers == 1);
        s_hg_mode = 0;
        st = check_run(NULL, 3, 0x0D00D1E, 1, 600, NULL);
        CHECK(st.state == PSNR_JOIN_CONNECTED && !st.answered && st.answers == 0);
        CHECK(s_hg_asked == 3);
        s_hg_stop = 1;
#ifdef _WIN32
        WaitForSingleObject(hgt, INFINITE);
        CloseHandle(hgt);
#else
        pthread_join(hgt, NULL);
#endif
        psnr_close(s_hg);
        s_hg = NULL;
        psnr_host_stop(h);
        printf("\nThe check's ask: a boss fight and the room, full, and no answer, as the host's game said: %s\n",
               s_hg_asked == 3 ? "yes" : "no");
    }

    /* a direct code: through the host's forwarded port (TCP), a wrong key, and no forward; then an end mid-check */
    {
        const int before = psnr_join_state();
        psnr_host* h = host_up(NAT_PORT, NAT_PORT, FWD_BOTH);
        char direct[32];
        snprintf(direct, sizeof direct, "203.0.113.5:%u", s_hport);
        psnr_join_check_status st = check_run(direct, 3, 0x0D00D1E, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_CONNECTED && !st.udp);
        st = check_run(direct, 3, 0x0BADBAD, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_REFUSED && st.err == PSNR_E_REFUSED);
        psnr_host_stop(h);
        h = host_up(NAT_PORT, NAT_PORT, FWD_NONE);
        snprintf(direct, sizeof direct, "203.0.113.5:%u", s_hport);
        st = check_run(direct, 3, 0x0D00D1E, 0, 0, NULL);
        CHECK(st.state == PSNR_JOIN_UNREACHABLE);
        /* the end while it runs (the lookup, with no endpoint): it stops and frees itself */
        s_endpoint_off = 1;
        psnr_join_check_config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.server = "198.51.100.1:36100";
        cfg.room = 99;
        cfg.version = 3;
        const int freed = s_checks_freed;
        psnr_join_check* jc = psnr_join_check_start(&cfg);
        CHECK(jc != NULL);
        sleep_ms(100);
        psnr_join_check_end(jc);
        int t = 0;
        for (; t < 2000 && s_checks_freed == freed; t += 10) sleep_ms(10);
        CHECK(s_checks_freed == freed + 1);
        s_endpoint_off = 0;
        psnr_host_stop(h);
        game_join_untouched(before);
        printf("\nThe check with a direct code (forwarded, a wrong key, no forward), and an end while it runs (freed "
               "%d ms after): %s\n", t, s_checks_freed == freed + 1 ? "as expected" : "no");
    }

    /* the background join (np_psnr_connect with a room code): connected, the session kept up while the title
     * doesn't poll, then the host stops: dialled again, no room any more, and after a while it gives up */
    {
        set_env("PSNR_RETRY_MS", "200");
        set_env("PSNR_VERSION", "3");
        set_env("PSNR_KEY", key_hex);
        psnr_host* h = host_up(NAT_PORT, NAT_PORT, FWD_NONE);
        CHECK(h != NULL);
        CHECK(np_psnr_connect(kComm) == 0);   /* (at once: the context counts as started) */
        int up = 0;
        for (int t = 0; t < 8000 && !up; t += 20) {
            np_psnr_pump();
            up = np_psnr_connected();
            if (!up) sleep_ms(20);
        }
        CHECK(up && psnr_join_state() == PSNR_JOIN_CONNECTED);
        LOCK();
        const int sess = psnr_is_session(s_client);
        UNLOCK();
        CHECK(sess);
        sleep_ms(PSNR_RUDP_KEEPALIVE_MS + 1500);   /* no pumping: the join's thread keeps it alive */
        np_psnr_pump();
        CHECK(np_psnr_connected());
        psnr_host_stop(h);
        int gone = 0, lost = 0;
        for (int t = 0; t < 15000 && !gone; t += 20) {
            np_psnr_pump();
            lost |= !np_psnr_connected();
            gone = psnr_join_state() == PSNR_JOIN_GONE && s_notice[0];   /* (the notice comes right after) */
            if (!gone) sleep_ms(20);
        }
        CHECK(lost && gone);
        CHECK(!strcmp(s_notice, "No game is hosting with this code. Ask the host for its code again."));
        printf("\nThe background join: connected over UDP, kept up, lost when the host stopped, gave up: %s\n",
               up && lost && gone ? "yes" : "no");
    }

    s_net_stop = 1;
#ifdef _WIN32
    WaitForSingleObject(nt, INFINITE);
    CloseHandle(nt);
#else
    pthread_join(nt, NULL);
#endif
    printf("\n%d datagrams delivered, %d dropped by the routers\n", s_delivered, s_dropped);
    if (s_fail) {
        printf("test_psnr_punch: %d FAILED\n", s_fail);
        return 1;
    }
    printf("test_psnr_punch: all passed\n");
    return 0;
}
