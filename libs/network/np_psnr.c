/*
 * ps3recomp - the connection to a psnr server. See np_psnr.h.
 */
#include "np_psnr.h"
#include "ps3emu/ps3_net.h"   /* ps3_net_online, ps3_net_locked_offline */
#include "ps3emu/ps3_net_signin.h"
#include "ps3emu/ps3_tls.h"
#include "ps3emu/psnr_host.h"   /* psnr_app_send, psnr_app_on_message */
#include "psnr/psnr_intro.h"   /* a room code's join */
#include "psnr/psnr_rudp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&s_lock)
#  define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* The fake username from sceNp.c. */
extern const char* np_fake_username(void);

#define PENDING_MAX 64

static psnr_client* s_client;
static uint32_t     s_conn;      /* which connection s_client is: one more at each dial that connects (LOCK) */
static uint32_t     s_user_id;   /* ours, from HELLO: our own member entry needs no punch */
static int          s_tried;
static char         s_comm_id[16];   /* the connection's, for a redial */
static int          s_redialing;     /* a redial thread is running (LOCK) */
static struct { uint32_t req; np_psnr_reply_fn fn; void* user; } s_pending[PENDING_MAX];
static void (*s_push_fn)(const psnr_msg*);

static void set_env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* PSN online IDs: 3-16 letters, digits, '-' and '_', starting with a letter.
 * Copies the allowed characters of `in`; 1 if the result is a valid ID. */
static int clean_online_id(const char* in, char out[17])
{
    int n = 0;
    for (; in && *in && n < 16; in++)
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_')
            out[n++] = *in;
    out[n] = 0;
    return n >= 3 && isalpha((unsigned char)out[0]);
}

static char s_id[17];
static volatile int s_id_state;   /* 0 unresolved, 1 a name applies, 2 none (offline) */
static const char*  s_flag_name;  /* --username, for the first lookup */

#ifdef _WIN32
static SRWLOCK s_id_lock = SRWLOCK_INIT;
#  define ID_LOCK()   AcquireSRWLockExclusive(&s_id_lock)
#  define ID_UNLOCK() ReleaseSRWLockExclusive(&s_id_lock)
#else
static pthread_mutex_t s_id_lock = PTHREAD_MUTEX_INITIALIZER;
#  define ID_LOCK()   pthread_mutex_lock(&s_id_lock)
#  define ID_UNLOCK() pthread_mutex_unlock(&s_id_lock)
#endif

void np_psnr_setup(const char* username, const char* server)
{
    /* Before anything reads them: the runtime's getenv cache keeps the first
     * answer it sees. */
    if (server && *server) {
        set_env("PS3_NET_ONLINE", "1");
        set_env("PSNR_SERVER", server);
    }
    /* The name itself is settled on first use (np_psnr_identity): main calls
     * this before the title's options hook applies its settings, and a
     * PS3_NP_ONLINE_ID or PS3_NET_ONLINE set there must count. */
    s_flag_name = username;
    s_id_state = 0;
}

/* The first valid one of: --username, PS3_NP_ONLINE_ID, and -- online only --
 * the OS login name. */
static void resolve_identity(void)
{
    const char* named[2] = { s_flag_name, getenv("PS3_NP_ONLINE_ID") };
    int found = 0;
    for (int i = 0; i < 2 && !found; i++) {
        if (!named[i] || !*named[i]) continue;
        found = clean_online_id(named[i], s_id);
        if (!found)
            printf("[psnr] \"%s\" is not a valid online ID (3-16 letters, digits, - or _, "
                   "starting with a letter)\n", named[i]);
    }
    if (!found) {
        if (!np_psnr_enabled()) { s_id_state = 2; return; }   /* offline: "PS3Player" */
#ifdef _WIN32
        const char* os = getenv("USERNAME");
#else
        const char* os = getenv("USER");
#endif
        if (!clean_online_id(os, s_id)) strcpy(s_id, "PS3Player");
    }
    s_id_state = 1;
    printf("[psnr] player \"%s\"%s\n", s_id, np_psnr_enabled() ? ""
           : ps3_net_online() ? " (online, no psnr server)" : " (offline)");
}

const char* np_psnr_identity(void)
{
    if (!s_id_state) {
        ID_LOCK();
        if (!s_id_state) resolve_identity();
        ID_UNLOCK();
    }
    return s_id_state == 1 ? s_id : NULL;
}

int np_psnr_enabled(void)
{
    return ps3_net_online() && getenv("PSNR_SERVER");
}

int np_psnr_signed_in(void)
{
    return ps3_net_online() && np_psnr_identity() != NULL;
}

const char* np_psnr_online_id(void)
{
    const char* id = np_psnr_identity();
    return id ? id : np_fake_username();
}

uint16_t np_psnr_p2p_port(void)
{
    const char* p = getenv("PS3_NET_P2P_PORT");
    return (uint16_t)(p ? atoi(p) : 3658);
}

int np_psnr_connected(void)
{
    return s_client != NULL;
}

uint32_t np_psnr_connection(void)
{
    LOCK();
    const uint32_t n = s_client ? s_conn : 0;
    UNLOCK();
    return n;
}

/* PSNR_SERVER as host and port (36100 when it names none). */
static void server_addr(char host[256], uint16_t* port)
{
    *port = 36100;
    snprintf(host, 256, "%s", getenv("PSNR_SERVER"));
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; *port = (uint16_t)atoi(colon + 1); }
}

/* PSNR_KEY: a hosted game's key (ps3emu/psnr_host.h), 64 hex digits, sent as HELLO's session blob when
 * there is no sign-in. 1 if set and well formed. */
static int hosted_key(uint8_t key[32])
{
    const char* k = getenv("PSNR_KEY");
    if (!k || strlen(k) != 64) return 0;
    for (int i = 0; i < 64; i++) {
        const char ch = k[i];
        const int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
        if (v < 0) return 0;
        if (i & 1) key[i / 2] = (uint8_t)(key[i / 2] | v);
        else key[i / 2] = (uint8_t)(v << 4);
    }
    return 1;
}

/* One connection to psnr. With a sign-in (ps3emu/ps3_net_signin.h) HELLO
 * carries the session; a server that says it has ended gets one new sign-in.
 * Blocks for the TCP connect, TLS and the HELLO. *err: 0, or why psnr turned
 * it away (psnr_connect_error); 0 with no client is a psnr out of reach. */
static psnr_client* dial(const char* comm_id, const char* host, uint16_t port, uint32_t* user_id,
                         uint8_t ip[4], int* signed_in_out, int* err)
{
    uint8_t session[32], key[32];
    int signed_in = ps3_net_session(NULL, 0, session);
    const int keyed = !signed_in && hosted_key(key);
    psnr_client* c = psnr_connect(host, port, comm_id, np_psnr_online_id(),
                                  np_psnr_p2p_port(), signed_in ? session : keyed ? key : NULL, user_id, ip);
    memset(key, 0, sizeof(key));
    if (!c && signed_in && psnr_connect_error() == PSNR_E_SIGN_IN) {
        ps3_net_session_drop();
        signed_in = ps3_net_session(NULL, 0, session);
        if (signed_in)
            c = psnr_connect(host, port, comm_id, np_psnr_online_id(),
                             np_psnr_p2p_port(), session, user_id, ip);
    }
    memset(session, 0, sizeof(session));
    *signed_in_out = signed_in;
    *err = c ? 0 : psnr_connect_error();
    return c;
}

static int bg_start(const char* comm_id);

int np_psnr_connect(const char* comm_id)
{
    char host[256];
    uint16_t port;
    uint32_t user_id = 0;
    uint8_t ip[4] = {0};

    if (!np_psnr_enabled()) return -1;
    if (np_psnr_background() || getenv("PSNR_ROOM")) return bg_start(comm_id);
    LOCK();
    if (s_client || s_tried) {
        int r = s_client ? 0 : -1;
        UNLOCK();
        return r;
    }
    /* ponytail: one attempt per process; retry on the next context start if a
     * server that comes up late turns out to matter. A connection that was
     * up and is lost is dialled again (redial_main). */
    s_tried = 1;
    server_addr(host, &port);
    int signed_in = 0, err = 0;
    s_client = dial(comm_id, host, port, &user_id, ip, &signed_in, &err);
    s_user_id = user_id;
    if (s_client) {
        snprintf(s_comm_id, sizeof(s_comm_id), "%s", comm_id);
        s_conn++;
    }
    UNLOCK();
    /* A HELLO with no session because the sign-in itself failed says nothing
     * more: the sign-in has given its own reason (a ban, maintenance), which
     * a second notice would replace before it is shown. */
    if ((err == PSNR_E_SIGN_IN && (signed_in || !ps3_net_signin_enabled())) || err == PSNR_E_NOT_YOUR_NAME)
        ps3_net_signin_post_notice("Co-op is not available: the matchmaking server did not accept this sign-in.");
    else if (err == PSNR_E_REFUSED && getenv("PSNR_KEY"))   /* a hosted game (psnr_host.h): the key is wrong */
        ps3_net_signin_post_notice("The host's game did not accept this invite code. Ask the host for it again.");
    else if (err == PSNR_E_REFUSED)
        ps3_net_signin_post_notice("Co-op is not available right now.");
    if (s_client)
        printf("[psnr] connected to %s:%u as \"%s\" (%s), user %u, seen from %u.%u.%u.%u, p2p port %u%s%s\n",
               host, port, np_psnr_online_id(), comm_id, user_id, ip[0], ip[1], ip[2], ip[3],
               np_psnr_p2p_port(), signed_in ? ", signed in" : "", ps3_tls_enabled() ? ", over TLS" : "");
    else if (err >= PSNR_E_SIGN_IN && err <= PSNR_E_REFUSED)
        printf("[psnr] %s:%u refused the HELLO (error %d) -- NP stays offline\n", host, port, err);
    else if (err == PSNR_E_NAME_TAKEN)
        printf("[psnr] %s:%u: another player there is already \"%s\" -- pick another "
               "with --username. NP stays offline\n", host, port, np_psnr_online_id());
    else
        printf("[psnr] could not reach %s:%u -- NP stays offline\n", host, port);
    return s_client ? 0 : -1;
}

uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len,
                         np_psnr_reply_fn fn, void* user)
{
    uint32_t req = 0;
    LOCK();
    if (s_client && !ps3_net_locked_offline()) {
        int slot = -1;
        for (int i = 0; i < PENDING_MAX; i++)
            if (!s_pending[i].req) { slot = i; break; }
        if (slot >= 0 && (req = psnr_send(s_client, type, body, len)) != 0) {
            s_pending[slot].req = req;
            s_pending[slot].fn = fn;
            s_pending[slot].user = user;
        }
    }
    UNLOCK();
    return req;
}

void np_psnr_on_push(void (*fn)(const psnr_msg*))
{
    s_push_fn = fn;
}

static void (*s_tick_fn)(void);
void np_psnr_on_tick(void (*fn)(void))
{
    s_tick_fn = fn;
}

/* ---- NAT traversal and the relay, on the title's P2P socket ---- */

#define PUNCHES      5      /* per peer */
#define PUNCH_MS     200
#define PROBE_MS     500    /* until psnr answers */
#define REPROBE_MS   20000  /* after: routers forget idle UDP mappings */
#define RELAY_AFTER  3000   /* punched, nothing heard directly: use the relay */
#define MAX_PEERS    8      /* ponytail: a room has at most 4 */
#define STREAM_TIMEOUT_MS 12000

static np_psnr_p2p_send_fn s_p2p_send;
static np_psnr_p2p_open_fn s_p2p_opener;
static np_psnr_stream_sink_fn s_stream_sink;
static int      s_probed;
static uint64_t s_next_probe;

/* Everyone Matching2 told us about. relay_stream comes from psnr's ROUTE
 * pushes; relay_udp is ours to decide: punched, never heard from directly.
 * seen_port: the port the peer's datagrams really come from, when its router
 * sends from another port than the one psnr saw (a strict router gives each
 * destination a port of its own). Learned from its punches, which carry its
 * user id; 0 while the port psnr gave is the one. The title keeps the address
 * psnr gave: datagrams from seen_port reach it as from `port`, and what it
 * sends to `port` goes to seen_port. */
static struct peer {
    uint32_t user;
    uint8_t  ip[4];
    uint16_t port, seen_port;
    int      punches_left;
    uint64_t next_punch, punched_at, heard_at;
    int      relay_stream, relay_udp;
    int      said_silent;   /* "no answer" logged */
} s_peers[MAX_PEERS];

/* Punches from members we did not know yet: Matching2 can tell us about a
 * member after its first punches arrive. np_psnr_punch looks here. */
#define EARLY_MAX    8
#define EARLY_MS     10000
static struct { uint32_t user; uint8_t ip[4]; uint16_t port; uint64_t at; } s_early[EARLY_MAX];

static uint64_t nat_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* Callers hold LOCK. */
static struct peer* peer_by_user(uint32_t user)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].user == user) return &s_peers[i];
    return NULL;
}

static struct peer* peer_at(const uint8_t ip[4], uint16_t port)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].user && s_peers[i].port == port && !memcmp(s_peers[i].ip, ip, 4))
            return &s_peers[i];
    return NULL;
}

/* The member a datagram from ip:port is from: at the port psnr gave, or at the
 * one its punches showed (seen_port). */
static struct peer* peer_from(const uint8_t ip[4], uint16_t port)
{
    struct peer* p = peer_at(ip, port);
    if (p || !port) return p;
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].user && s_peers[i].seen_port == port && !memcmp(s_peers[i].ip, ip, 4))
            return &s_peers[i];
    return NULL;
}

/* A punch from `port` of the member's own address names it: it answers from
 * there. Taken only while nothing came from it yet, so a stranger can't move
 * a member that works. Then punch that port back at once, which opens our
 * router toward it too. Callers hold LOCK. */
static int learn_port(struct peer* p, const uint8_t ip[4], uint16_t port, uint64_t now)
{
    if (!p || p->heard_at || port == p->port || memcmp(p->ip, ip, 4) != 0) return 0;
    p->seen_port = port;
    p->heard_at = now;
    p->punches_left = PUNCHES;
    p->next_punch = 0;
    printf("[psnr] user %u answers from port %u, not %u (its router changes ports): using %u\n",
           p->user, port, p->port, port);
    return 1;
}

void np_psnr_set_p2p_sender(np_psnr_p2p_send_fn fn)
{
    s_p2p_send = fn;
    s_probed = 0;
    s_next_probe = 0;
}

void np_psnr_set_p2p_opener(np_psnr_p2p_open_fn fn)
{
    s_p2p_opener = fn;
}

void np_psnr_set_stream_sink(np_psnr_stream_sink_fn fn)
{
    s_stream_sink = fn;
}

/* A room member has this address (any port: a member behind some routers
 * sends from a port psnr did not see). Under LOCK. */
static int peer_ip_known(const uint8_t ip[4])
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].user && !memcmp(s_peers[i].ip, ip, 4)) return 1;
    return 0;
}

int np_psnr_local_ip(uint8_t ip[4])
{
    LOCK();
    int have = s_client != NULL;
    if (have) psnr_local_addr(s_client, ip);
    UNLOCK();
    return have;
}

void np_psnr_peer_left(uint32_t user)
{
    if (!user) return;
    LOCK();
    struct peer* p = peer_by_user(user);
    if (p) memset(p, 0, sizeof(*p));
    UNLOCK();
}

static int rc_takes(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port);

/* Says so for the first few dropped datagrams only. */
static void dropped(const char* why, const uint8_t ip[4], uint16_t port)
{
    static int said;
    if (said >= 8) return;
    said++;
    printf("[psnr] dropped a datagram from %u.%u.%u.%u:%u: %s%s\n", ip[0], ip[1], ip[2], ip[3], port, why,
           said == 8 ? " (no more of these lines)" : "");
}

int np_psnr_p2p_filter(void* buf, uint32_t* len, uint8_t from_ip[4], uint16_t* from_port)
{
    uint8_t ip[4];
    uint16_t port;
    uint32_t from;
    const uint8_t* payload;
    size_t plen;
    int drop = 1;

    if (!psnr_is_control(buf, *len)) {
        /* With psnr, P2P traffic comes from the room's members only. */
        const int rooms = np_psnr_enabled();
        LOCK();
        struct peer* p = peer_from(from_ip, *from_port);
        if (p) {
            p->heard_at = nat_now_ms();   /* the direct path works */
            *from_port = p->port;         /* (from seen_port: the title knows the peer at psnr's port) */
        }
        const int member = p || peer_ip_known(from_ip);
        UNLOCK();
        if (rooms && !member) {
            dropped("not a member of the room", from_ip, *from_port);
            return 1;
        }
        return 0;
    }
    if (rc_takes(buf, *len, from_ip, *from_port)) return 1;   /* a room code's join, and its session */
    if (psnr_relay_unwrap(buf, *len, &from, &payload, &plen)) {
        /* The peer's datagram, through the relay: the title gets it as if
         * it came from the peer's usual address. Only psnr itself relays. */
        LOCK();
        uint8_t sip[4] = {0};
        uint16_t sport = 0;
        if (s_client) psnr_server_addr(s_client, sip, &sport);
        const int from_server = s_client && sport == *from_port && !memcmp(sip, from_ip, 4);
        struct peer* p = from_server && from ? peer_by_user(from) : NULL;   /* (user 0 is a free slot) */
        if (!from_server) {
            UNLOCK();
            dropped("a relayed one, not from the psnr server", from_ip, *from_port);
            return 1;
        }
        if (p) {
            memmove(buf, payload, plen);
            *len = (uint32_t)plen;
            memcpy(from_ip, p->ip, 4);
            *from_port = p->port;
            drop = 0;
        }
        UNLOCK();
        return drop;
    }
    if (psnr_probe_reply(buf, *len, ip, &port)) {
        if (!s_probed) {
            s_probed = 1;
            s_next_probe = nat_now_ms() + REPROBE_MS;
            printf("[psnr] P2P socket seen at %u.%u.%u.%u:%u\n", ip[0], ip[1], ip[2], ip[3], port);
        }
    } else {   /* a punch */
        uint32_t user = 0;
        const uint64_t now = nat_now_ms();
        LOCK();
        struct peer* p = peer_from(from_ip, *from_port);
        if (p) {
            p->heard_at = now;
        } else if (psnr_punch_user(buf, *len, &user) && user && user != s_user_id) {
            p = peer_by_user(user);
            if (p) {
                if (!learn_port(p, from_ip, *from_port, now) && !memcmp(p->ip, from_ip, 4))
                    dropped("a punch from another port of a member that already answered", from_ip, *from_port);
            } else {   /* a member Matching2 hasn't told us about yet: kept for np_psnr_punch */
                int k = 0;   /* its own slot, else the oldest */
                for (int i = 1; i < EARLY_MAX && s_early[k].user != user; i++)
                    if (s_early[i].user == user || s_early[i].at < s_early[k].at) k = i;
                s_early[k].user = user;
                memcpy(s_early[k].ip, from_ip, 4);
                s_early[k].port = *from_port;
                s_early[k].at = now;
            }
        }
        UNLOCK();
    }
    return 1;
}

void np_psnr_punch(uint32_t user, const uint8_t ip[4], uint16_t port)
{
    if (user == s_user_id || !port) return;
    LOCK();
    struct peer* p = peer_by_user(user);
    if (!p) p = peer_by_user(0);
    if (p) {
        int known = p->user == user && p->port == port && !memcmp(p->ip, ip, 4);
        if (!known) {
            int relay_stream = p->user == user ? p->relay_stream : 0;
            int relay_udp = p->user == user ? p->relay_udp : 0;   /* (a ROUTE push may come first) */
            memset(p, 0, sizeof(*p));
            p->user = user;
            memcpy(p->ip, ip, 4);
            p->port = port;
            p->relay_stream = relay_stream;
            p->relay_udp = relay_udp;
        }
        if (!p->punches_left) {
            p->punches_left = PUNCHES;
            p->next_punch = 0;
        }
        /* Its punches may have come before Matching2 named it. */
        const uint64_t now = nat_now_ms();
        for (int i = 0; i < EARLY_MAX; i++) {
            if (s_early[i].user != user) continue;
            if (now - s_early[i].at < EARLY_MS && !learn_port(p, s_early[i].ip, s_early[i].port, now) &&
                s_early[i].port == p->port && !memcmp(s_early[i].ip, p->ip, 4))
                p->heard_at = now;   /* (from the port psnr gave: the direct path works) */
            memset(&s_early[i], 0, sizeof(s_early[i]));
        }
    }
    UNLOCK();
}

int np_psnr_p2p_route(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    uint8_t stack[1536], *pkt = stack, sip[4];
    uint16_t sport, seen = 0;
    uint32_t to = 0;
    LOCK();
    struct peer* p = peer_at(ip, port);
    if (p && p->relay_udp) to = p->user;
    else if (p) seen = p->seen_port;
    UNLOCK();
    if (seen && s_p2p_send) {   /* the peer's router answers from another port: send there */
        s_p2p_send(buf, len, ip, seen);
        return 1;
    }
    if (!to || !s_client || !s_p2p_send) return 0;
    if (len + 17 > sizeof(stack) && !(pkt = (uint8_t*)malloc(len + 17))) return 0;
    size_t n = psnr_relay_wrap(s_client, to, buf, len, pkt, len + 17);
    psnr_server_addr(s_client, sip, &sport);
    s_p2p_send(pkt, (uint32_t)n, sip, sport);
    if (pkt != stack) free(pkt);
    return 1;
}

int64_t np_psnr_stream_connect(const uint8_t ip[4], uint16_t port)
{
    uint32_t to = 0;
    LOCK();
    struct peer* p = peer_at(ip, port);
    if (p && p->relay_stream) to = p->user;
    UNLOCK();
    if (!to || !s_client) return -2;
    int64_t fd = psnr_stream_connect(s_client, to, port, STREAM_TIMEOUT_MS);
    printf("[psnr] stream to %u.%u.%u.%u:%u through the relay: %s\n",
           ip[0], ip[1], ip[2], ip[3], port, fd >= 0 ? "open" : "failed");
    return fd;
}

/* Pushes np_psnr handles itself: ROUTE and STREAM_OFFER. 1 if it took it. */
/* APP_MSG (from user u32 | blob): a hosted game's own message, for the port's handler, never the title's */
static void (*s_app_fn)(uint32_t from_user, const uint8_t* data, uint32_t len);
static int app_push(const psnr_msg* m)
{
    if (m->type != PSNR_APP_MSG) return 0;
    if (m->len >= 6) {
        const uint32_t from = psnr_get32(m->data);
        const uint16_t n = psnr_get16(m->data + 4);
        if (6u + n <= m->len && s_app_fn) s_app_fn(from, m->data + 6, n);
    }
    return 1;
}

static int nat_push(const psnr_msg* m)
{
    if (m->type == PSNR_ROUTE && m->len >= 13) {
        uint32_t user = psnr_get32(m->data + 8);
        int relay = (m->data[12] & PSNR_ROUTE_STREAM_RELAY) != 0;
        int relay_udp = (m->data[12] & PSNR_ROUTE_UDP_RELAY) != 0;   /* a hosted game (psnr_host.h) */
        LOCK();
        struct peer* p = peer_by_user(user);
        if (!p && (p = peer_by_user(0))) {
            memset(p, 0, sizeof(*p));
            p->user = user;
        }
        if (p) {
            p->relay_stream = relay;
            if (relay_udp) p->relay_udp = 1;
        }
        UNLOCK();
        if (relay) printf("[psnr] streams to user %u go through the relay\n", user);
        if (relay_udp) printf("[psnr] datagrams to user %u go through the host\n", user);
        return 1;
    }
    if (m->type == PSNR_STREAM_OFFER && m->len >= 10) {
        uint32_t id = psnr_get32(m->data), from = psnr_get32(m->data + 4);
        uint8_t ip[4] = {0};
        uint16_t port = 0;
        LOCK();
        struct peer* p = peer_by_user(from);
        if (p) { memcpy(ip, p->ip, 4); port = p->port; }
        UNLOCK();
        int64_t fd = s_client && s_stream_sink ? psnr_stream_accept(s_client, id, 5000) : -1;
        printf("[psnr] stream from user %u through the relay: %s\n", from, fd >= 0 ? "open" : "failed");
        if (fd >= 0) s_stream_sink(fd, ip, port);
        return 1;
    }
    return 0;
}

/* From np_psnr_pump: probe psnr until it answers (and again now and then),
 * send the punches that are due, and move peers that never answered a punch
 * to the relay. */
static void nat_tick(void)
{
    uint8_t pkt[PSNR_UDP_PROBE_LEN], ip[4];
    uint16_t port;
    uint64_t now = nat_now_ms();
    if (!s_client || !s_p2p_send) return;
    if (now >= s_next_probe) {
        psnr_probe_packet(s_client, pkt);
        psnr_server_addr(s_client, ip, &port);
        s_p2p_send(pkt, PSNR_UDP_PROBE_LEN, ip, port);
        s_next_probe = now + (s_probed ? REPROBE_MS : PROBE_MS);
    }
    LOCK();
    for (int i = 0; i < MAX_PEERS; i++) {
        struct peer* p = &s_peers[i];
        if (!p->user) continue;
        if (p->punches_left && now >= p->next_punch) {
            psnr_punch_packet(s_client, pkt);
            s_p2p_send(pkt, PSNR_UDP_PUNCH_LEN, p->ip, p->seen_port ? p->seen_port : p->port);
            if (!p->punched_at) p->punched_at = now;
            p->punches_left--;
            p->next_punch = now + PUNCH_MS;
        }
        if (!p->relay_udp && !p->heard_at && p->punched_at && now - p->punched_at > RELAY_AFTER) {
            if (psnr_relay_available(s_client)) {
                p->relay_udp = 1;
                printf("[psnr] nothing direct from %u.%u.%u.%u:%u; its datagrams go through the relay\n",
                       p->ip[0], p->ip[1], p->ip[2], p->ip[3], p->port);
            } else if (!p->said_silent) {   /* (no relay: the session will end; say why, once) */
                p->said_silent = 1;
                printf("[psnr] nothing direct from user %u at %u.%u.%u.%u:%u after our punches, and no relay: "
                       "no way through between the two routers\n",
                       p->user, p->ip[0], p->ip[1], p->ip[2], p->ip[3], p->port);
            }
        }
    }
    UNLOCK();
}

/* PSNR_REDIAL (default 1; 0: one connection a run, as before): a connection
 * to psnr that was up and is lost (the server restarted, the network
 * dropped) is dialled again on a thread of its own, after 2 s, then 4, 8, 16
 * and 30, then every 60 s, until it connects, psnr turns the sign-in or the
 * name away, or the run is locked offline. Dialling blocks (TCP, TLS, the HELLO: about 21 s with
 * the network gone), so it is never done on the title's thread. The title's
 * context stays started: its requests fail with SERVER_NOT_AVAILABLE while
 * the connection is down (as they did for the rest of the run), and go
 * through again once it is back. A restarted server has forgotten the rooms;
 * the title makes and finds new ones as it does anyway. PSNR_REDIAL_MS sets
 * the first wait (tests). */
static int redial_on(void)
{
    const char* e = getenv("PSNR_REDIAL");
    return !e || atoi(e) != 0;
}

static void nap_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

#ifdef _WIN32
static DWORD WINAPI redial_main(LPVOID arg)
#else
static void* redial_main(void* arg)
#endif
{
    (void)arg;
    char host[256], comm[sizeof(s_comm_id)];
    uint16_t port;
    server_addr(host, &port);
    LOCK();
    memcpy(comm, s_comm_id, sizeof(comm));
    UNLOCK();
    const char* first = getenv("PSNR_REDIAL_MS");
    unsigned wait = first && atoi(first) > 0 ? (unsigned)atoi(first) : 2000u;
    for (int tries = 1;; tries++) {
        nap_ms(wait);
        if (ps3_net_locked_offline() || !np_psnr_enabled()) break;
        uint32_t user = 0;
        uint8_t ip[4] = {0};
        int signed_in = 0, err = 0;
        psnr_client* c = dial(comm, host, port, &user, ip, &signed_in, &err);
        if (c) {
            LOCK();
            if (!s_client) {
                s_client = c;
                s_user_id = user;
                s_conn++;
                c = NULL;
            }
            UNLOCK();
            if (c) psnr_close(c);   /* (connected meanwhile: keep that one) */
            else printf("[psnr] connected again to %s:%u as \"%s\" (%s), user %u, after %d tr%s%s\n",
                        host, port, np_psnr_online_id(), comm, user, tries, tries == 1 ? "y" : "ies",
                        signed_in ? ", signed in" : "");
            break;
        }
        /* The sign-in or the name turned away: that lasts. Anything else may
         * pass (our own old connection still holds the name until the server
         * notices it is gone; maintenance ends). */
        if (err == PSNR_E_SIGN_IN || err == PSNR_E_NOT_YOUR_NAME) {
            printf("[psnr] %s:%u refused the HELLO (error %d) -- NP stays offline\n", host, port, err);
            break;
        }
        /* A hosted game's room server (psnr_host.c) turns away a wrong key: the host's game started again, with a
         * new key, so this one never comes right. Trying on would only get this address ignored for a while (its
         * wrong keys are counted), with no word to the player. */
        if (err == PSNR_E_REFUSED && getenv("PSNR_KEY")) {
            printf("[psnr] %s:%u no longer takes this invite code (the host's game has a new one) -- NP stays "
                   "offline\n", host, port);
            ps3_net_signin_post_notice("The host's game did not accept this invite code. Ask the host for it again.");
            break;
        }
        if (tries == 1) {
            if (err) printf("[psnr] %s:%u turned the HELLO away for now (error %d); trying again now and then\n",
                            host, port, err);
            else printf("[psnr] could not reach %s:%u yet; trying again now and then\n", host, port);
        }
        wait = wait >= 30000u ? 60000u : wait >= 16000u ? 30000u : wait * 2u;
    }
    LOCK();
    s_redialing = 0;
    UNLOCK();
    return 0;
}

/* ---- A hosted join in the background (PSNR_BACKGROUND, ps3emu/psnr_host.h) ----
 *
 * A friend's game joining a game one player hosts connects on a thread of its own from the start: a context start
 * doesn't wait for it, and Matching2's local answers (server and world lists) work before it is up. A try that fails
 * is made again after 10 s, then 30 s, then every 60 s; a connection that was up and is lost, after 2 s. It stops
 * for good when the host's game turns the invite code away (the host started again, with a new code). */
static volatile int s_join_state;   /* PSNR_JOIN_*, for the port's window */
static int s_bg_running;            /* (LOCK) */
static volatile int s_bg_lost;      /* the pump lost the connection: the thread dials again soon */

int np_psnr_background(void)
{
    const char* e = getenv("PSNR_BACKGROUND");
    return e && atoi(e) != 0;
}

int psnr_join_state(void)
{
    return s_join_state;
}

static void set_join_state(int st)
{
    s_join_state = st;
}

/* The wait before try n+1 after n failed tries in a row (PSNR_RETRY_MS scales it, for tests). */
static unsigned bg_wait_ms(int failed)
{
    const char* t = getenv("PSNR_RETRY_MS");
    const unsigned first = t && atoi(t) > 0 ? (unsigned)atoi(t) : 10000u;
    return failed <= 1 ? first : failed == 2 ? first * 3u : first * 6u;
}

/* ---- A room code's join (PSNR_ROOM; psnr/psnr_intro.h) ----
 *
 * The port sets PSNR_ROOM (the room number), PSNR_KEY (as for a direct code), PSNR_SERVER (here: the endpoint) and
 * PSNR_VERSION (the network version). One try: the P2P port is held open (sysNet's opener), LOOKUP goes from it to the
 * endpoint every 500 ms for 3 s, and FOUND gives the host's candidates. Then at the same time: OPEN to each candidate
 * (and to where the host's KNOCKs come from), 5 a second for 5 s, and a TCP connection to the host's room server.
 * The first to open gets the HELLO: TCP as today, or a session over the punched path (COOKIE, OPEN2, OPENED, then
 * psnr_rudp.h). The P2P port's thread hands this the packets (rc_takes); the background join's thread runs it. */
#ifndef NP_RC_LOOKUP_MS
#  define NP_RC_LOOKUP_MS     500
#endif
#ifndef NP_RC_LOOKUP_FOR_MS
#  define NP_RC_LOOKUP_FOR_MS 3000
#endif
#ifndef NP_RC_OPEN_MS
#  define NP_RC_OPEN_MS       200
#endif
#ifndef NP_RC_PUNCH_FOR_MS
#  define NP_RC_PUNCH_FOR_MS  5000
#endif
#ifndef NP_RC_SETTLE_MS
#  define NP_RC_SETTLE_MS     2000    /* after the last OPEN: an answer, or the TCP connection, may still come */
#endif
#ifndef NP_RC_GONE_MS
#  define NP_RC_GONE_MS       120000  /* no room this long: the host stopped hosting */
#endif
#define NP_RC_SLOW_MS         5000    /* "slow down": the next try after this */
#define NP_RC_CANDS           6
/* This PC's address toward the endpoint (a test's virtual network replaces it). */
#ifndef NP_PSNR_LOCAL_IP
#  define NP_PSNR_LOCAL_IP(to, out) psnr_local_ip_toward(to, out)
#endif

/* One try's steps, shared by the game's own join (rc_try: the P2P port, under LOCK, its state for the port's window)
 * and the join check (psnr_join_check_*: a socket of its own, its own lock and state). */
typedef struct rc_join rc_join;
struct rc_join {
    struct {                         /* one try's (cleared at its start; under the join's lock) */
        int       active;            /* a try is on: FOUND, KNOCK, COOKIE and OPENED are its */
        uint64_t  room, open_nonce;  /* OPEN's nonce: the LOOKUP's in its top half (the host matches its INTRO) */
        uint32_t  nonce;
        uint8_t   ep[4];
        uint16_t  ep_port;
        int       found;
        psnr_intro_pkt fpkt;
        struct {
            uint8_t  ip[4];
            uint16_t port;
            int      opens, open2s, has_cookie;
            uint64_t cookie, next;
        } c[NP_RC_CANDS];
        int       nc;
        int       opened;
        uint32_t  sid;
        uint8_t   op_ip[4];
        uint16_t  op_port;
        psnr_client* pend;           /* the session's client while it says HELLO */
    } t;
    struct { uint8_t ip[4]; uint16_t port; } sess_to;   /* where the session's datagrams go */
    /* Where this join's datagrams go out (its UDP port), its lock, where its state shows, and its session's client once
     * past HELLO (under its lock). */
    int  (*send)(rc_join* j, const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port);
    void (*lock)(rc_join* j, int take);
    void (*state)(rc_join* j, int st);
    psnr_client* (*live)(rc_join* j);
    volatile int* stop;              /* set: the try ends at its next step (the check's Cancel); NULL for the game's */
    void* owner;
};
#define RC_LOCK(j)   (j)->lock((j), 1)
#define RC_UNLOCK(j) (j)->lock((j), 0)

static int rc_stopped(const rc_join* j)
{
    return j->stop && *j->stop;
}

/* What one try needs to know: the endpoint, the room and version, the HELLO's. */
typedef struct {
    char     host[256];
    uint16_t port;
    uint64_t room;
    uint16_t version;
    int      home;                   /* the home address goes in the LOOKUP (PSNR_ROOM_HOME=0: tests, none) */
    int      tcp;                    /* TCP races the punching (PSNR_ROOM_TCP=0: tests, the UDP way only) */
    int      plain;                  /* that TCP never over TLS (the check: whatever PS3_NET_TLS says for this game) */
    const uint8_t* key;              /* the HELLO's session blob, or NULL */
    const char* comm;
    const char* name;
    uint16_t p2p;                    /* this join's UDP port: the HELLO's, and the own candidate's */
} rc_params;

/* The game's join: on the P2P port (sysNet's), under LOCK, its state for the port's window (psnr_join_state). */
static int rc_game_send(rc_join* j, const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    (void)j;
    return s_p2p_send ? s_p2p_send(buf, len, ip, port) : -1;
}
static void rc_game_lock(rc_join* j, int take)
{
    (void)j;
    if (take) LOCK();
    else UNLOCK();
}
static void rc_game_state(rc_join* j, int st)
{
    (void)j;
    set_join_state(st);
}
static psnr_client* rc_game_live(rc_join* j)
{
    (void)j;
    return psnr_is_session(s_client) ? s_client : NULL;
}
static rc_join s_game = { .send = rc_game_send, .lock = rc_game_lock, .state = rc_game_state, .live = rc_game_live };

static int rc_room_code(void)
{
    const char* r = getenv("PSNR_ROOM");
    return r && *r;
}

/* A candidate to open, once (the join's lock). */
static void rc_add(rc_join* j, const uint8_t ip[4], uint16_t port)
{
    if (!port || !(ip[0] | ip[1] | ip[2] | ip[3])) return;
    for (int i = 0; i < j->t.nc; i++)
        if (j->t.c[i].port == port && !memcmp(j->t.c[i].ip, ip, 4)) return;
    if (j->t.nc >= NP_RC_CANDS) return;
    memset(&j->t.c[j->t.nc], 0, sizeof(j->t.c[0]));
    memcpy(j->t.c[j->t.nc].ip, ip, 4);
    j->t.c[j->t.nc].port = port;
    j->t.nc++;
}

static int rc_cand(const rc_join* j, const uint8_t ip[4], uint16_t port)
{
    for (int i = 0; i < j->t.nc; i++)
        if (j->t.c[i].port == port && !memcmp(j->t.c[i].ip, ip, 4)) return i;
    return -1;
}

/* From the join's UDP port's thread: 1 if the packet is a room code's join's or its session's (then it is taken). */
static int rc_take(rc_join* j, const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    const int t = psnr_udp_type(buf, len);
    if (t == PSNR_PKT_DATA || t == PSNR_PKT_CLOSE) {
        uint8_t sip[4];
        uint16_t sport = 0;
        RC_LOCK(j);
        psnr_client* c = j->t.pend ? j->t.pend : j->live(j);
        if (c) psnr_server_addr(c, sip, &sport);
        if (c && sport == port && !memcmp(sip, ip, 4)) psnr_session_input(c, buf, len);
        RC_UNLOCK(j);
        return 1;
    }
    if (t != PSNR_PKT_FOUND && t != PSNR_PKT_KNOCK && t != PSNR_PKT_COOKIE && t != PSNR_PKT_OPENED) return 0;
    psnr_intro_pkt in;
    if (!psnr_intro_read(buf, len, &in)) return 1;
    RC_LOCK(j);
    if (!j->t.active) { RC_UNLOCK(j); return 1; }   /* (the host's last knocks, after the join) */
    switch (t) {
    case PSNR_PKT_FOUND:
        if (port == j->t.ep_port && !memcmp(ip, j->t.ep, 4) && in.nonce == j->t.nonce && in.room == j->t.room &&
            !j->t.found) {
            j->t.fpkt = in;
            j->t.found = 1;
            if (in.status == PSNR_INTRO_OK)   /* (now: the host's KNOCKs may come right behind it) */
                for (int i = 0; i < in.n; i++) rc_add(j, in.cands[i].ip, in.cands[i].port);
        }
        break;
    case PSNR_PKT_KNOCK:   /* the host's: where its router sends from toward us, maybe a port no one gave */
        if (in.room == j->t.room && in.nonce == j->t.nonce && j->t.found && rc_cand(j, ip, port) < 0) {
            rc_add(j, ip, port);
            printf("[psnr] the host's game knocks from port %u: opening there too\n", port);
        }
        break;
    case PSNR_PKT_COOKIE:
        if (in.nonce64 == j->t.open_nonce && j->t.found) {
            int i = rc_cand(j, ip, port);
            if (i < 0) { rc_add(j, ip, port); i = rc_cand(j, ip, port); }
            if (i >= 0 && !j->t.c[i].has_cookie) {
                j->t.c[i].has_cookie = 1;
                j->t.c[i].cookie = in.cookie;
                j->t.c[i].next = 0;
            }
        }
        break;
    case PSNR_PKT_OPENED:
        if (in.nonce64 == j->t.open_nonce && in.session && !j->t.opened && rc_cand(j, ip, port) >= 0) {
            j->t.opened = 1;
            j->t.sid = in.session;
            memcpy(j->t.op_ip, ip, 4);
            j->t.op_port = port;
        }
        break;
    }
    RC_UNLOCK(j);
    return 1;
}

/* The game's: from the P2P port's thread (np_psnr_p2p_filter). */
static int rc_takes(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    return rc_take(&s_game, buf, len, ip, port);
}

static void rc_sess_send(void* ctx, const uint8_t* pkt, size_t len)
{
    rc_join* j = (rc_join*)ctx;
    j->send(j, pkt, (uint32_t)len, j->sess_to.ip, j->sess_to.port);
}

static uint32_t rc_random32(void)
{
    uint32_t v = 0;
    while (!v) v = (uint32_t)psnr_intro_random();
    return v;
}

/* Once the P2P port is open (the sender set): 1, else 0. */
static int rc_p2p_ready(void)
{
    if (!s_p2p_send && s_p2p_opener) s_p2p_opener();
    return s_p2p_send != NULL;
}

/* One try at the host's game with a room code, on j. The client (past HELLO; a session's is still j->t.pend, for the
 * caller to move under j's lock), or NULL with *err and the state to show. */
static psnr_client* rc_run(rc_join* j, const rc_params* p, uint32_t* user, uint8_t ip[4], int* err, int* state)
{
    *err = 0;
    j->state(j, PSNR_JOIN_LOOKING);
    uint8_t ep[4], home[4] = {0};
    if (psnr_resolve4(p->host, ep) != 0) {
        *state = PSNR_JOIN_NO_SERVER;
        return NULL;
    }
    NP_PSNR_LOCAL_IP(ep, home);
    if (!p->home) memset(home, 0, 4);
    const psnr_cand own = psnr_cand_of(home, p->p2p);
    RC_LOCK(j);
    memset(&j->t, 0, sizeof(j->t));
    j->t.room = p->room;
    j->t.nonce = rc_random32();
    j->t.open_nonce = (uint64_t)j->t.nonce << 32 | rc_random32();
    memcpy(j->t.ep, ep, 4);
    j->t.ep_port = p->port;
    j->t.active = 1;
    const uint64_t room = j->t.room;
    const uint32_t nonce = j->t.nonce;
    const uint64_t open_nonce = j->t.open_nonce;
    RC_UNLOCK(j);

    /* LOOKUP until FOUND */
    uint8_t pkt[PSNR_INTRO_REQ_LEN];
    const int have_home = (home[0] | home[1] | home[2] | home[3]) != 0;
    const size_t lk = psnr_intro_lookup(pkt, p->version, room, nonce, have_home ? &own : NULL);
    int found = 0;
    for (unsigned t = 0; t < NP_RC_LOOKUP_FOR_MS && !found && !rc_stopped(j); t += 20) {
        if (t % NP_RC_LOOKUP_MS == 0) j->send(j, pkt, (uint32_t)lk, ep, p->port);
        nap_ms(20);
        RC_LOCK(j);
        found = j->t.found;
        RC_UNLOCK(j);
    }
    psnr_intro_pkt f;
    RC_LOCK(j);
    f = j->t.fpkt;
    if (!found || f.status != PSNR_INTRO_OK) j->t.active = 0;
    RC_UNLOCK(j);
    if (!found) { *state = PSNR_JOIN_NO_SERVER; return NULL; }
    if (f.status != PSNR_INTRO_OK) {
        *state = f.status == PSNR_INTRO_NO_ROOM ? PSNR_JOIN_NO_ROOM : f.status == PSNR_INTRO_VERSION
                     ? PSNR_JOIN_OTHER_VERSION : PSNR_JOIN_SLOW;
        return NULL;
    }

    /* OPEN to each candidate, and TCP to the room server, at the same time */
    j->state(j, PSNR_JOIN_REACHING);
    const int same = (f.flags & PSNR_INTRO_SAME_NETWORK) != 0;
    psnr_dialer* d[3] = {NULL, NULL, NULL};
    int nd = 0;
    if (p->tcp) {
        /* TCP to the host as seen at the room server's port; to a candidate with that address (UPnP's) at its own
         * port; on the same network, to the home address too */
        uint8_t tip[3][4];
        uint16_t tport[3];
        for (int i = 0; i < f.n && nd < 3; i++) {
            const psnr_cand* c = &f.cands[i];
            if (i > 0 && memcmp(c->ip, f.cands[0].ip, 4) && !same) continue;
            const uint16_t to = i == 0 ? f.room_port : c->port;
            int dup = 0;
            for (int k = 0; k < nd; k++) dup |= tport[k] == to && !memcmp(tip[k], c->ip, 4);
            if (dup || !to || !(d[nd] = psnr_dial_start(c->ip, to))) continue;
            memcpy(tip[nd], c->ip, 4);
            tport[nd++] = to;
        }
    }
    const uint64_t begin = nat_now_ms();
    uint64_t start = begin;
    int won = 0;   /* 1: the session, 2 + k: dialer k */
    while (!won && !rc_stopped(j)) {
        const uint64_t now = nat_now_ms();
        if (now - begin >= 2u * NP_RC_PUNCH_FOR_MS + NP_RC_SETTLE_MS) break;   /* (a TCP try can hang on for 21 s) */
        int pending = 0;
        RC_LOCK(j);
        for (int i = 0; i < j->t.nc; i++) {
            if (now < j->t.c[i].next) { pending = 1; continue; }
            if (j->t.c[i].has_cookie && j->t.c[i].open2s < 8) {
                const size_t n = psnr_intro_open2(pkt, open_nonce, j->t.c[i].cookie);
                j->send(j, pkt, (uint32_t)n, j->t.c[i].ip, j->t.c[i].port);
                j->t.c[i].open2s++;
                j->t.c[i].next = now + 250;
                pending = 1;
            } else if (!j->t.c[i].has_cookie && j->t.c[i].opens < NP_RC_PUNCH_FOR_MS / NP_RC_OPEN_MS) {
                const size_t n = psnr_intro_open(pkt, room, open_nonce);
                j->send(j, pkt, (uint32_t)n, j->t.c[i].ip, j->t.c[i].port);
                j->t.c[i].opens++;
                j->t.c[i].next = now + NP_RC_OPEN_MS;
                pending = 1;
            }
        }
        if (j->t.opened) won = 1;
        RC_UNLOCK(j);
        for (int k = 0; k < nd && !won; k++) {
            if (!d[k]) continue;
            const int r = psnr_dial_poll(d[k]);
            if (r > 0) won = 2 + k;
            else if (r < 0) { psnr_dial_cancel(d[k]); d[k] = NULL; }
            else pending = 1;
        }
        if (won) break;
        if (pending) start = now;   /* (the settle time runs from the last thing sent or still pending) */
        if (now - start >= NP_RC_SETTLE_MS) break;
        nap_ms(20);
    }
    psnr_client* c = NULL;
    if (won >= 2) {
        c = p->plain ? psnr_dial_client_plain(d[won - 2]) : psnr_dial_client(d[won - 2]);
        d[won - 2] = NULL;
    }
    for (int k = 0; k < nd; k++) psnr_dial_cancel(d[k]);
    RC_LOCK(j);
    j->t.active = 0;
    if (won == 1) {
        memcpy(j->sess_to.ip, j->t.op_ip, 4);
        j->sess_to.port = j->t.op_port;
        psnr_rudp* sess = psnr_rudp_new(j->t.sid, rc_sess_send, j, nat_now_ms());
        if (sess) c = j->t.pend = psnr_session_client(sess, j->t.op_ip, j->t.op_port, home);
    }
    RC_UNLOCK(j);
    if (!c) {
        *state = PSNR_JOIN_NO_WAY;
        return NULL;
    }

    /* HELLO on the one that opened */
    const int ok = psnr_hello(c, p->comm, p->name, p->p2p, p->key, user, ip) == 0;
    if (!ok) {
        *err = psnr_connect_error();
        *state = *err == PSNR_E_REFUSED ? PSNR_JOIN_REFUSED : PSNR_JOIN_NO_WAY;
        RC_LOCK(j);
        if (j->t.pend == c) j->t.pend = NULL;
        RC_UNLOCK(j);
        psnr_close(c);
        return NULL;
    }
    printf("[psnr] reached the host's game %s\n", won == 1 ? "over the punched UDP path" : "over TCP");
    *state = PSNR_JOIN_CONNECTED;
    return c;
}

/* The room code's try from the knobs (PSNR_SERVER: the endpoint, PSNR_ROOM, PSNR_VERSION, PSNR_ROOM_HOME,
 * PSNR_ROOM_TCP); the key is the caller's. */
static void rc_params_from_env(rc_params* p)
{
    memset(p, 0, sizeof(*p));
    server_addr(p->host, &p->port);
    const char* r = getenv("PSNR_ROOM");
    p->room = r ? strtoull(r, NULL, 10) : 0;
    const char* v = getenv("PSNR_VERSION");
    p->version = (uint16_t)(v ? strtoul(v, NULL, 10) : 0);
    const char* home = getenv("PSNR_ROOM_HOME");   /* (0: tests, no home address; np_psnr.h) */
    p->home = !home || strcmp(home, "0");
    const char* tcp = getenv("PSNR_ROOM_TCP");     /* (0: tests, the UDP way only) */
    p->tcp = !tcp || strcmp(tcp, "0");
}

/* One try at the host's game with a room code, for the game's own join. The client (past HELLO; a session's is still
 * s_game.t.pend, for the caller to move to s_client under LOCK), or NULL with *err and the state to show. */
static psnr_client* rc_try(const char* comm, uint32_t* user, uint8_t ip[4], int* err, int* state)
{
    *err = 0;
    set_join_state(PSNR_JOIN_LOOKING);
    if (!rc_p2p_ready()) {
        printf("[psnr] the P2P port can't be opened: a room code can't be looked up\n");
        *state = PSNR_JOIN_NO_WAY;
        return NULL;
    }
    rc_params p;
    rc_params_from_env(&p);
    uint8_t key[32];
    const int keyed = hosted_key(key);
    p.key = keyed ? key : NULL;
    p.comm = comm;
    p.name = np_psnr_online_id();
    p.p2p = np_psnr_p2p_port();
    psnr_client* c = rc_run(&s_game, &p, user, ip, err, state);
    memset(key, 0, sizeof(key));
    return c;
}

/* One try at the host's game: a direct code's TCP connection and HELLO, or a room code's (rc_try). The client, or
 * NULL with *err (as dial()'s) and the state to show. */
static psnr_client* bg_try(const char* comm, uint32_t* user, uint8_t ip[4], int* err, int* state)
{
    char host[256];
    uint16_t port;
    int signed_in = 0;
    if (rc_room_code()) return rc_try(comm, user, ip, err, state);
    server_addr(host, &port);
    set_join_state(PSNR_JOIN_REACHING);
    psnr_client* c = dial(comm, host, port, user, ip, &signed_in, err);
    *state = c ? PSNR_JOIN_CONNECTED : *err == PSNR_E_REFUSED ? PSNR_JOIN_REFUSED : PSNR_JOIN_UNREACHABLE;
    return c;
}

/* Naps until ms have passed, the pump lost the connection, or the run went offline. */
static void bg_nap(unsigned ms)
{
    for (unsigned t = 0; t < ms && !s_bg_lost && !ps3_net_locked_offline(); t += 50) nap_ms(50);
}

#ifdef _WIN32
static DWORD WINAPI bg_main(LPVOID arg)
#else
static void* bg_main(void* arg)
#endif
{
    (void)arg;
    char comm[sizeof(s_comm_id)];
    LOCK();
    memcpy(comm, s_comm_id, sizeof(comm));
    UNLOCK();
    int failed = 0, was_up = 0;
    unsigned wait = 0;
    uint64_t no_room_since = 0;
    for (;;) {
        bg_nap(wait);
        if (ps3_net_locked_offline() || !np_psnr_enabled()) break;
        LOCK();
        const int up = s_client != NULL;
        UNLOCK();
        if (up) {   /* (connected: the pump looks after it, and says when it is lost) */
            was_up = 1;
            wait = 200;
            LOCK();   /* a session's resends and keepalives: the title doesn't poll during every load */
            if (psnr_is_session(s_client)) {
                psnr_session_tick(s_client);
                wait = 50;
            }
            UNLOCK();
            continue;
        }
        if (was_up) {   /* lost after it was up: again in 2 s, then as after a failed try */
            was_up = 0;
            s_bg_lost = 0;
            failed = 0;
            wait = 2000;
            set_join_state(PSNR_JOIN_REACHING);
            continue;
        }
        uint32_t user = 0;
        uint8_t ip[4] = {0};
        int err = 0, state = 0;
        psnr_client* c = bg_try(comm, &user, ip, &err, &state);
        if (c) {
            LOCK();
            set_join_state(PSNR_JOIN_CONNECTED);   /* (before the client: whoever sees it connected sees this) */
            if (s_game.t.pend == c) s_game.t.pend = NULL;
            if (!s_client) {
                s_client = c;
                s_user_id = user;
                s_conn++;
                c = NULL;
            }
            UNLOCK();
            if (c) psnr_close(c);
            printf("[psnr] connected to the host's game as \"%s\" (%s), user %u%s\n", np_psnr_online_id(), comm, user,
                   failed ? ", after trying again" : "");
            failed = 0;
            wait = 200;
            continue;
        }
        if (state != PSNR_JOIN_NO_ROOM) no_room_since = 0;
        else if (!no_room_since) no_room_since = nat_now_ms();
        if (no_room_since && nat_now_ms() - no_room_since >= NP_RC_GONE_MS) state = PSNR_JOIN_GONE;
        set_join_state(state);
        if (state == PSNR_JOIN_REFUSED) {   /* the host's game has a new code: this one never comes right */
            printf("[psnr] the host's game did not accept this invite code -- NP stays offline\n");
            ps3_net_signin_post_notice("The host's game did not accept this invite code. Ask the host for it again.");
            break;
        }
        if (state == PSNR_JOIN_OTHER_VERSION) {
            printf("[psnr] the host's game is another version -- NP stays offline\n");
            ps3_net_signin_post_notice("The host's game is another version. You both need the same version.");
            break;
        }
        if (state == PSNR_JOIN_GONE) {
            printf("[psnr] no game has hosted with this room code for %d s -- NP stays offline\n",
                   NP_RC_GONE_MS / 1000);
            ps3_net_signin_post_notice("No game is hosting with this code. Ask the host for its code again.");
            break;
        }
        if (state == PSNR_JOIN_SLOW) {   /* the endpoint asks for a few seconds: not a failed try */
            wait = NP_RC_SLOW_MS;
            continue;
        }
        failed++;
        wait = bg_wait_ms(failed);
        if (failed == 1 || failed % 10 == 0)
            printf("[psnr] the host's game wasn't reached (%s, try %d); trying again in %u s\n",
                   state == PSNR_JOIN_NO_ROOM ? "no room with this number" : state == PSNR_JOIN_NO_SERVER
                   ? "the endpoint doesn't answer" : state == PSNR_JOIN_NO_WAY ? "found, but nothing got through"
                   : err ? "turned away for now" : "no answer", failed, (wait + 999u) / 1000u);
    }
    LOCK();
    s_bg_running = 0;
    UNLOCK();
    return 0;
}

/* np_psnr_connect with PSNR_BACKGROUND: the thread, once a run (unless it stopped for good); 0, the context counts
 * as started. */
static int bg_start(const char* comm_id)
{
    LOCK();
    if (s_client || s_bg_running || s_tried) {
        UNLOCK();
        return 0;
    }
    s_tried = 1;
    snprintf(s_comm_id, sizeof(s_comm_id), "%s", comm_id);
    set_join_state(PSNR_JOIN_REACHING);
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, bg_main, NULL, 0, NULL);
    const int ok = th != NULL;
    if (th) CloseHandle(th);
#else
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int ok = pthread_create(&th, &at, bg_main, NULL) == 0;
    pthread_attr_destroy(&at);
#endif
    s_bg_running = ok;
    UNLOCK();
    printf("[psnr] joining the host's game in the background\n");
    return ok ? 0 : -1;
}

/* Under LOCK: a redial thread, unless one runs, PSNR_REDIAL=0, or there was
 * never a connection. 1 if one runs now. */
static int redial_start(void)
{
    if (s_bg_running) {   /* a hosted join's own thread dials again */
        s_bg_lost = 1;
        return 1;
    }
    if (s_redialing) return 1;
    if (!redial_on() || !s_comm_id[0]) return 0;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, redial_main, NULL, 0, NULL);
    if (!th) return 0;
    CloseHandle(th);
#else
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int bad = pthread_create(&th, &at, redial_main, NULL);
    pthread_attr_destroy(&at);
    if (bad) return 0;
#endif
    s_redialing = 1;
    return 1;
}

void np_psnr_pump(void)
{
    psnr_msg m;
    if (s_tick_fn) s_tick_fn();
    nat_tick();
    if (!s_client) return;

    for (;;) {
        np_psnr_reply_fn fn = NULL;
        void* user = NULL;
        LOCK();
        if (!s_client) { UNLOCK(); return; }
        /* Locked offline (ps3_net_lock_offline: developer cheats in use): the
         * session ends here, as if the connection were lost. */
        const int locked = ps3_net_locked_offline();
        int r = locked ? -1 : psnr_poll(s_client, &m);
        if (r < 0) {
            /* Gone: fail every outstanding request so no title waits forever. */
            psnr_close(s_client);
            s_client = NULL;
            s_probed = 0;   /* (the next connection probes its P2P address again) */
            s_next_probe = 0;
            const int again = !locked && redial_start();
            printf("[psnr] %s\n", locked ? "closed: the network is locked offline"
                                 : again ? "connection lost; dialling again in the background" : "connection lost");
            /* The requests of the lost connection, taken all at once: a redial
             * may connect while their handlers run, and its requests must not
             * be failed with them. */
            struct { np_psnr_reply_fn fn; void* user; } lost[PENDING_MAX];
            int nlost = 0;
            for (int i = 0; i < PENDING_MAX; i++)
                if (s_pending[i].req) {
                    lost[nlost].fn = s_pending[i].fn;
                    lost[nlost].user = s_pending[i].user;
                    nlost++;
                    s_pending[i].req = 0;
                }
            UNLOCK();
            psnr_msg dead;
            memset(&dead, 0, sizeof(dead));
            for (int k = 0; k < nlost; k++)
                if (lost[k].fn) lost[k].fn(lost[k].user, &dead);
            return;
        }
        if (r == 0) { UNLOCK(); return; }
        if (m.req) {
            for (int i = 0; i < PENDING_MAX; i++)
                if (s_pending[i].req == m.req) {
                    fn = s_pending[i].fn;
                    user = s_pending[i].user;
                    s_pending[i].req = 0;
                    break;
                }
        }
        UNLOCK();

        /* Handlers run unlocked: they send follow-up requests. */
        if (m.req && fn) fn(user, &m);
        else if (!m.req && !nat_push(&m) && !app_push(&m) && s_push_fn) s_push_fn(&m);
        psnr_msg_free(&m);
    }
}

/* ---- A hosted game's own messages (ps3emu/psnr_host.h: psnr_app_send) ---- */

int psnr_app_send(const void* data, uint32_t len)
{
    if (len > PSNR_APP_MAX || (len && !data)) return -1;
    uint8_t body[2 + PSNR_APP_MAX];
    psnr_put16(body, (uint16_t)len);
    if (len) memcpy(body + 2, data, len);
    return np_psnr_request(PSNR_APP_SEND, body, 2 + len, NULL, NULL) ? 0 : -1;
}

void psnr_app_on_message(void (*fn)(uint32_t from_user, const uint8_t* data, uint32_t len))
{
    s_app_fn = fn;
}

/* ---- The join check (ps3emu/psnr_host.h: psnr_join_check_*) ----
 *
 * The same steps as a friend's game's join, before the port restarts into co-op, so the player hears at once what is
 * wrong with a code: a room code's lookup, punching and TCP race and HELLO (rc_run, on a UDP socket of its own and its
 * own lock: the title's P2P port and connection stay as they are), or a direct code's TCP connection and HELLO. Then
 * the port's ask, and the host's game's answers. One thread runs it, and another reads its socket (rc_take, as the P2P
 * port's thread does for the game's join). The caller's end and the thread's end each drop one hold; the last frees it.
 * Tests replace its socket (NP_JC_UDP_*). */
#ifndef NP_JC_UDP_OPEN
#  define NP_JC_UDP_OPEN(port)                     psnr_udp_open(port)
#  define NP_JC_UDP_SEND(u, buf, len, ip, port)    psnr_udp_send((u), (buf), (len), (ip), (port))
#  define NP_JC_UDP_RECV(u, buf, cap, ip, port, ms) psnr_udp_recv((u), (buf), (cap), (ip), (port), (ms))
#  define NP_JC_UDP_CLOSE(u)                       psnr_udp_close(u)
#endif
#ifndef NP_JC_FREED
#  define NP_JC_FREED() ((void)0)   /* (tests: a check freed itself) */
#endif
#define NP_JC_DIAL_MS    10000   /* a direct code's TCP connection (the OS gives up after about 21 s) */
#define NP_JC_ANSWER_MS  5000    /* the host's game's answer to the ask (it answers once it stands in its world) */

#ifdef _WIN32
typedef SRWLOCK jc_lock_t;
#  define JC_LOCK_INIT(l) InitializeSRWLock(l)
#  define JC_LOCK(l)      AcquireSRWLockExclusive(l)
#  define JC_UNLOCK(l)    ReleaseSRWLockExclusive(l)
#  define JC_LOCK_FREE(l) ((void)(l))
#else
typedef pthread_mutex_t jc_lock_t;
#  define JC_LOCK_INIT(l) pthread_mutex_init(l, NULL)
#  define JC_LOCK(l)      pthread_mutex_lock(l)
#  define JC_UNLOCK(l)    pthread_mutex_unlock(l)
#  define JC_LOCK_FREE(l) pthread_mutex_destroy(l)
#endif

struct psnr_join_check {
    rc_join   j;                       /* (its lock is lk) */
    jc_lock_t lk;
    int       holds;                   /* the caller's and the thread's (lk) */
    volatile int stop, rx_stop;
    psnr_udp* udp;
    uint16_t  udp_port;
    psnr_client* client;               /* a session's client once past HELLO, for the reader (lk) */
    /* what the caller gave, copied */
    char      server[256], comm[16], name[17];
    uint64_t  room;
    uint16_t  version;
    uint8_t   key[PSNR_HOST_KEY_LEN];
    uint8_t   ask[PSNR_APP_MAX];
    uint32_t  ask_len;
    int       answer_ms;
    int     (*answered)(const uint8_t* data, uint32_t len, void* user);
    void*     user;
    /* how it goes (lk) */
    psnr_join_check_status st;
    struct { uint8_t* data; uint32_t len; } ans[PSNR_JOIN_CHECK_ANSWERS];
};

static int jc_send(rc_join* j, const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    psnr_join_check* jc = (psnr_join_check*)j->owner;
    return NP_JC_UDP_SEND(jc->udp, buf, len, ip, port);
}
static void jc_lock(rc_join* j, int take)
{
    psnr_join_check* jc = (psnr_join_check*)j->owner;
    if (take) JC_LOCK(&jc->lk);
    else JC_UNLOCK(&jc->lk);
}
static void jc_state(rc_join* j, int st)   /* (never under lk: rc_run says its state outside its lock) */
{
    psnr_join_check* jc = (psnr_join_check*)j->owner;
    JC_LOCK(&jc->lk);
    jc->st.state = st;
    JC_UNLOCK(&jc->lk);
}
static psnr_client* jc_live(rc_join* j)
{
    return ((psnr_join_check*)j->owner)->client;
}

static void jc_drop(psnr_join_check* jc)
{
    JC_LOCK(&jc->lk);
    const int last = --jc->holds == 0;
    JC_UNLOCK(&jc->lk);
    if (!last) return;
    for (int i = 0; i < PSNR_JOIN_CHECK_ANSWERS; i++) free(jc->ans[i].data);
    memset(jc->key, 0, sizeof(jc->key));
    JC_LOCK_FREE(&jc->lk);
    free(jc);
    NP_JC_FREED();
}

#ifdef _WIN32
static DWORD WINAPI jc_rx_main(LPVOID arg)
#else
static void* jc_rx_main(void* arg)
#endif
{
    psnr_join_check* jc = (psnr_join_check*)arg;
    uint8_t buf[2048], ip[4];
    uint16_t port = 0;
    while (!jc->rx_stop) {
        const int n = NP_JC_UDP_RECV(jc->udp, buf, sizeof(buf), ip, &port, 50);
        if (n > 0) rc_take(&jc->j, buf, (uint32_t)n, ip, port);
    }
    return 0;
}

/* Naps ms, or less once the caller ended the check. */
static void jc_nap(psnr_join_check* jc, unsigned ms)
{
    for (unsigned t = 0; t < ms && !jc->stop; t += 20) nap_ms(20);
}

/* A direct code: TCP to the host's room server (without TLS), then HELLO with the key. */
static psnr_client* jc_direct(psnr_join_check* jc, int* err, int* state)
{
    char host[256];
    uint16_t port = 36100;
    snprintf(host, sizeof(host), "%s", jc->server);
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }
    *err = 0;
    *state = PSNR_JOIN_UNREACHABLE;
    uint8_t ip[4];
    if (psnr_resolve4(host, ip) != 0) return NULL;
    psnr_dialer* d = psnr_dial_start(ip, port);
    if (!d) return NULL;
    int r = 0;
    for (unsigned t = 0; t < NP_JC_DIAL_MS && !jc->stop && (r = psnr_dial_poll(d)) == 0; t += 20) nap_ms(20);
    if (r <= 0) {
        psnr_dial_cancel(d);
        return NULL;
    }
    psnr_client* c = psnr_dial_client_plain(d);
    if (!c) return NULL;
    uint32_t user = 0;
    uint8_t seen[4];
    if (psnr_hello(c, jc->comm, jc->name, 0, jc->key, &user, seen) != 0) {
        *err = psnr_connect_error();
        *state = *err == PSNR_E_REFUSED ? PSNR_JOIN_REFUSED : PSNR_JOIN_UNREACHABLE;
        psnr_close(c);
        return NULL;
    }
    *state = PSNR_JOIN_CONNECTED;
    return c;
}

/* Past HELLO: the ask to the host's game, and its answers (APP_MSG: from user u32 | len u16 | blob) until the caller's
 * test says the answer is complete, the time is up, or the connection ends. */
static void jc_ask(psnr_join_check* jc, psnr_client* c)
{
    uint8_t body[2 + PSNR_APP_MAX];
    psnr_put16(body, (uint16_t)jc->ask_len);
    memcpy(body + 2, jc->ask, jc->ask_len);
    if (!psnr_send(c, PSNR_APP_SEND, body, 2 + jc->ask_len)) return;
    const uint64_t end = nat_now_ms() + (uint64_t)jc->answer_ms;
    while (!jc->stop && nat_now_ms() < end) {
        psnr_msg m;
        const int r = psnr_poll(c, &m);
        if (r < 0) break;
        if (r == 0) {
            psnr_session_tick(c);   /* (a session's resends and acks: nothing else ticks it here) */
            nap_ms(10);
            continue;
        }
        int complete = 0;
        if (m.type == PSNR_APP_MSG && m.len >= 6) {
            const uint16_t n = psnr_get16(m.data + 4);
            if (6u + n <= m.len) {
                const uint8_t* blob = m.data + 6;
                uint8_t* copy = (uint8_t*)malloc(n ? n : 1);
                if (copy) memcpy(copy, blob, n);
                JC_LOCK(&jc->lk);
                if (copy && jc->st.answers < PSNR_JOIN_CHECK_ANSWERS) {
                    jc->ans[jc->st.answers].data = copy;
                    jc->ans[jc->st.answers].len = n;
                    jc->st.answers++;
                    copy = NULL;
                }
                JC_UNLOCK(&jc->lk);
                free(copy);
                complete = jc->answered && jc->answered(blob, n, jc->user);
            }
        }
        psnr_msg_free(&m);
        if (complete) {
            JC_LOCK(&jc->lk);
            jc->st.answered = 1;
            JC_UNLOCK(&jc->lk);
            break;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI jc_main(LPVOID arg)
#else
static void* jc_main(void* arg)
#endif
{
    psnr_join_check* jc = (psnr_join_check*)arg;
    psnr_client* c = NULL;
    int err = 0, state = PSNR_JOIN_NO_WAY;
    if (jc->room) {
        jc_state(&jc->j, PSNR_JOIN_LOOKING);
        jc->udp = NP_JC_UDP_OPEN(&jc->udp_port);
        int rx = 0;
#ifdef _WIN32
        HANDLE rxt = NULL;
        if (jc->udp && (rxt = CreateThread(NULL, 0, jc_rx_main, jc, 0, NULL)) != NULL) rx = 1;
#else
        pthread_t rxt;
        if (jc->udp && pthread_create(&rxt, NULL, jc_rx_main, jc) == 0) rx = 1;
#endif
        if (rx) {
            rc_params p;
            rc_params_from_env(&p);   /* (for PSNR_ROOM_HOME and PSNR_ROOM_TCP: the rest is the caller's) */
            snprintf(p.host, sizeof(p.host), "%s", jc->server);
            p.port = 36100;
            char* colon = strrchr(p.host, ':');
            if (colon) { *colon = 0; p.port = (uint16_t)atoi(colon + 1); }
            p.room = jc->room;
            p.version = jc->version;
            p.plain = 1;
            p.key = jc->key;
            p.comm = jc->comm;
            p.name = jc->name;
            p.p2p = jc->udp_port;
            uint32_t user = 0;
            uint8_t ip[4];
            for (int tries = 0; tries < 2 && !jc->stop; tries++) {
                c = rc_run(&jc->j, &p, &user, ip, &err, &state);
                if (c || state != PSNR_JOIN_SLOW) break;
                jc_nap(jc, NP_RC_SLOW_MS);   /* (the endpoint asks for a few seconds: once more after them) */
            }
            if (c) {
                JC_LOCK(&jc->lk);
                if (jc->j.t.pend == c) jc->j.t.pend = NULL;
                jc->client = c;
                JC_UNLOCK(&jc->lk);
            }
        }
        if (c && jc->ask_len && !jc->stop) jc_ask(jc, c);
        if (c) {
            JC_LOCK(&jc->lk);
            jc->st.udp = psnr_is_session(c);
            jc->client = NULL;
            JC_UNLOCK(&jc->lk);
            psnr_close(c);   /* (a session's CLOSE goes out on the socket, so before it closes) */
        }
        if (rx) {
            jc->rx_stop = 1;
#ifdef _WIN32
            WaitForSingleObject(rxt, INFINITE);
            CloseHandle(rxt);
#else
            pthread_join(rxt, NULL);
#endif
        }
        if (jc->udp) NP_JC_UDP_CLOSE(jc->udp);
        jc->udp = NULL;
    } else {
        jc_state(&jc->j, PSNR_JOIN_REACHING);
        c = jc_direct(jc, &err, &state);
        if (c && jc->ask_len && !jc->stop) jc_ask(jc, c);
        if (c) psnr_close(c);
    }
    JC_LOCK(&jc->lk);
    jc->st.state = state;
    jc->st.err = err;
    jc->st.done = 1;
    JC_UNLOCK(&jc->lk);
    printf("[psnr] the join check: %s (state %d%s%s)\n", state == PSNR_JOIN_CONNECTED ? "reached the host's game"
           : "the host's game wasn't reached", state, err ? ", refused" : "", jc->stop ? ", cancelled" : "");
    jc_drop(jc);
    return 0;
}

psnr_join_check* psnr_join_check_start(const psnr_join_check_config* cfg)
{
    if (!cfg || !cfg->server || !*cfg->server || cfg->ask_len > PSNR_APP_MAX || (cfg->ask_len && !cfg->ask))
        return NULL;
    psnr_join_check* jc = (psnr_join_check*)calloc(1, sizeof(psnr_join_check));
    if (!jc) return NULL;
    JC_LOCK_INIT(&jc->lk);
    jc->holds = 2;
    jc->j.send = jc_send;
    jc->j.lock = jc_lock;
    jc->j.state = jc_state;
    jc->j.live = jc_live;
    jc->j.stop = &jc->stop;
    jc->j.owner = jc;
    snprintf(jc->server, sizeof(jc->server), "%s", cfg->server);
    snprintf(jc->comm, sizeof(jc->comm), "%s", cfg->comm_id && *cfg->comm_id ? cfg->comm_id : "NPWR00881_00");
    snprintf(jc->name, sizeof(jc->name), "%s", cfg->online_id && *cfg->online_id ? cfg->online_id
                                                                                   : np_psnr_online_id());
    jc->room = cfg->room;
    jc->version = cfg->version;
    memcpy(jc->key, cfg->key, sizeof(jc->key));
    if (cfg->ask_len) memcpy(jc->ask, cfg->ask, cfg->ask_len);
    jc->ask_len = cfg->ask_len;
    jc->answer_ms = cfg->answer_ms > 0 ? cfg->answer_ms : NP_JC_ANSWER_MS;
    jc->answered = cfg->answered;
    jc->user = cfg->user;
    jc->st.state = jc->room ? PSNR_JOIN_LOOKING : PSNR_JOIN_REACHING;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, jc_main, jc, 0, NULL);
    const int ok = th != NULL;
    if (th) CloseHandle(th);
#else
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int ok = pthread_create(&th, &at, jc_main, jc) == 0;
    pthread_attr_destroy(&at);
#endif
    if (!ok) {
        JC_LOCK_FREE(&jc->lk);
        free(jc);
        return NULL;
    }
    printf("[psnr] the join check: %s\n", jc->room ? "a room code" : "a direct code");
    return jc;
}

void psnr_join_check_get(psnr_join_check* jc, psnr_join_check_status* out)
{
    JC_LOCK(&jc->lk);
    *out = jc->st;
    JC_UNLOCK(&jc->lk);
}

uint32_t psnr_join_check_answer(psnr_join_check* jc, int i, uint8_t* buf, uint32_t cap)
{
    uint32_t n = 0;
    JC_LOCK(&jc->lk);
    if (i >= 0 && i < jc->st.answers && jc->ans[i].len <= cap) {
        n = jc->ans[i].len;
        if (n) memcpy(buf, jc->ans[i].data, n);
    }
    JC_UNLOCK(&jc->lk);
    return n;
}

void psnr_join_check_end(psnr_join_check* jc)
{
    if (!jc) return;
    jc->stop = 1;
    jc_drop(jc);
}
