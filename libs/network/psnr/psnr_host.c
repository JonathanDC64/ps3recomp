/*
 * psnr_host.c - see ps3emu/psnr_host.h. The handlers follow the psnr server's
 * (internal/psnr in the Demon's Souls Recomp server: rooms.go, server.go,
 * udp.go, relay.go) message for message; the comments say where this one
 * differs. Every name here starts with ph_ so a test can include this file
 * beside psnr.c.
 */
#include "ps3emu/psnr_host.h"
#include "psnr.h"
#include "psnr_intro.h"
#include "psnr_rudp.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* WSAPoll */
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  ifndef SIO_UDP_CONNRESET   /* (mstcpip.h's, not in every SDK's default view) */
#    define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#  endif
#  ifdef _MSC_VER
#    pragma comment(lib, "ws2_32.lib")
#  endif
   typedef SOCKET ph_sock;
#  define PH_BAD INVALID_SOCKET
#  define ph_close closesocket
#  define PH_WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)
#  define PH_POLL WSAPoll
   typedef WSAPOLLFD ph_pollfd;
   typedef int ph_socklen;
   typedef SRWLOCK ph_lock;
#  define PH_LOCK_STATIC   SRWLOCK_INIT
#  define PH_LOCK_INIT(l)  InitializeSRWLock(l)
#  define PH_LOCK(l)       AcquireSRWLockExclusive(l)
#  define PH_UNLOCK(l)     ReleaseSRWLockExclusive(l)
#  define PH_LOCK_FREE(l)  ((void)(l))
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  include <poll.h>
#  include <pthread.h>
   typedef int ph_sock;
#  define PH_BAD (-1)
#  define ph_close close
#  define PH_WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK)
#  define PH_POLL poll
   typedef struct pollfd ph_pollfd;
   typedef socklen_t ph_socklen;
   typedef pthread_mutex_t ph_lock;
#  define PH_LOCK_STATIC   PTHREAD_MUTEX_INITIALIZER
#  define PH_LOCK_INIT(l)  pthread_mutex_init(l, NULL)
#  define PH_LOCK(l)       pthread_mutex_lock(l)
#  define PH_UNLOCK(l)     pthread_mutex_unlock(l)
#  define PH_LOCK_FREE(l)  pthread_mutex_destroy(l)
#endif

/* Every datagram out of the room server's UDP socket goes through this (a test's virtual network replaces it). */
#ifndef PH_SENDTO
#  define PH_SENDTO(h, buf, len, to) \
      ((void)sendto((h)->udp, (const char*)(buf), (int)(len), 0, (const struct sockaddr*)(to), (ph_socklen)sizeof *(to)))
#endif

#define PH_CLIENTS     12      /* 4 players, plus reconnects still closing */
#define PH_ROOMS       4
#define PH_MEMBERS     8
#define PH_TIMEOUT_MS  90000   /* as the psnr server: HELLO/HEARTBEAT keep a client */
#define PH_TX_MAX      (1u << 20)
#ifndef PH_BAD_KEYS
#  define PH_BAD_KEYS  5       /* wrong keys from one address before it is ignored */
#endif
#define PH_BAN_MS      60000
/* Wrong keys, from any address, before the key lets no one else in (psnr_host_key_spent): a guess at the invite
 * code's 28-bit key gets that many tries a code, whatever the number of addresses it comes from. */
#ifndef PH_KEY_TRIES
#  define PH_KEY_TRIES 100
#endif
/* A connection that hasn't said HELLO: dropped after this long, and at most PH_EARLY_PER_IP from one address, so
 * connections that never send a key can't hold the players' places. */
#ifndef PH_HELLO_MS
#  define PH_HELLO_MS  10000
#endif
#define PH_EARLY_PER_IP 2

/* The introductions (psnr_host_set_intro). Overridable for tests. */
#define PH_INTROS        8        /* introductions remembered: for the knocks and psnr_host_intro_info's stuck */
#ifndef PH_KNOCK_MS
#  define PH_KNOCK_MS    200      /* KNOCK to each of a friend's candidates 5 times a second... */
#endif
#ifndef PH_KNOCK_FOR_MS
#  define PH_KNOCK_FOR_MS 5000    /* ...for 5 s after its INTRO */
#endif
#ifndef PH_STUCK_MS
#  define PH_STUCK_MS    15000    /* introduced and not connected this long after: stuck */
#endif
#define PH_STUCK_FOR_MS  120000   /* (said for this long after the INTRO) */
#ifndef PH_REG_RETRY_MS
#  define PH_REG_RETRY_MS 1000    /* an unanswered REGISTER goes again after this */
#endif
#define PH_REG_LOST      3        /* unanswered REGISTERs in a row: the endpoint can't be reached */
#define PH_REG_SLOW_MS   5000     /* after "slow down", and between REGISTERs while it can't be reached */
#define PH_REG_FULL_MS   60000    /* after "no place for another room" */
#define PH_SESS_CLOSE_MS 3000     /* a session's last reply (a refusal) gets this long to arrive before it closes */

/* A write to a connection the other end has closed fails (EPIPE) and doesn't end the process with SIGPIPE: the
 * server starts before the runtime's online switch ignores SIGPIPE (ps3_net_online). macOS: SO_NOSIGPIPE. */
#ifdef MSG_NOSIGNAL
#  define PH_SEND_FLAGS MSG_NOSIGNAL
#else
#  define PH_SEND_FLAGS 0
#endif

typedef struct { uint8_t* b; size_t n, cap; } ph_buf;

typedef struct {
    ph_sock  s;
    uint32_t id;
    int      hello;               /* past HELLO */
    char     comm[13], name[17];
    uint8_t  ip[4];               /* as this server sees the TCP connection */
    uint8_t  via[4];              /* the local address the client reached us at */
    uint16_t p2p_port;
    uint32_t token;
    uint8_t  udp_ip[4], local_ip[4];
    uint16_t udp_port;
    uint64_t last_rx;
    uint64_t born;                /* when it connected (the HELLO's deadline counts from here) */
    ph_buf   rx, tx;
    int      closing;             /* drop once tx is out */
    /* A player over a punched UDP path (psnr_rudp.h) has a session in place of a socket: s is PH_BAD. */
    psnr_rudp* sess;
    psnr_host* host;              /* (for the session's send function) */
    uint8_t  peer_ip[4];          /* the session's other end */
    uint16_t peer_port;
    uint64_t nonce;               /* its OPEN's */
    uint64_t closing_at;
} ph_client;

#define PH_USED(c) ((c)->s != PH_BAD || (c)->sess != NULL)

typedef struct {
    uint16_t id;
    int      c;                   /* index into clients */
    ph_buf   data;
} ph_member;

typedef struct {
    int       used;
    uint64_t  id, seq;
    char      comm[13];
    uint8_t   max;
    uint32_t  flags;
    ph_buf    ext, in;
    uint16_t  owner, next_member;
    int       n;
    ph_member m[PH_MEMBERS];
} ph_room;

struct psnr_host {
    psnr_host_config cfg;
    ph_sock   lis, udp;
    uint16_t  port;
    ph_client cl[PH_CLIENTS];     /* s == PH_BAD: free */
    ph_room   rooms[PH_ROOMS];
    uint32_t  next_id;
    uint64_t  next_seq, rng;
    struct { uint8_t ip[4]; int bad; uint64_t until; } bans[16];
    /* The key HELLOs need now (cfg.key until psnr_host_new_key), the wrong ones since it was set, and whether it is
     * spent. A new one comes from another thread through key_next and key_pending. */
    uint8_t   key_now[PSNR_HOST_KEY_LEN], key_next[PSNR_HOST_KEY_LEN];
    int       key_bad;
    atomic_int key_spent, key_pending;
    atomic_int max_players;       /* the most members of a room: cfg.max_players, then psnr_host_set_max_players's */
    volatile int stop;
    int       players;            /* read by psnr_host_players */
    /* The introductions. Set from other threads (psnr_host_set_intro, psnr_host_new_room), so under ilock. */
    ph_lock   ilock;
    psnr_host_intro intro;
    uint64_t  room, owner;        /* the room number and the owner token now */
    int       reg_state;          /* PSNR_HOST_INTRO_* */
    uint8_t   seen_ip[4];
    uint16_t  seen_port;
    int       reg_waiting, reg_unanswered, reg_kick;
    uint64_t  reg_next;
    struct {
        uint32_t  nonce;          /* the friend's LOOKUP's (0: a free entry) */
        int       n, done;
        psnr_cand c[2];
        uint64_t  t0, next;
    } intros[PH_INTROS];
    uint8_t   cookie_secret[32];
    int       udp_players;
#ifdef _WIN32
    HANDLE    th;
#else
    pthread_t th;
#endif
};

/* ---- small helpers ---- */

static uint64_t ph_now(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static void ph_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* Room ids and tokens: unguessable enough for a private game with a key (the key itself is the caller's). */
static uint64_t ph_rand(psnr_host* h)
{
    uint64_t z = (h->rng += 0x9E3779B97F4A7C15ull);
#ifdef _WIN32
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    z ^= (uint64_t)q.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    z ^= (uint64_t)ts.tv_nsec << 20;
#endif
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int ph_put(ph_buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t* nb = (uint8_t*)realloc(b->b, cap);
        if (!nb) return -1;
        b->b = nb;
        b->cap = cap;
    }
    if (n) memcpy(b->b + b->n, p, n);
    b->n += n;
    return 0;
}
static void ph_u8(ph_buf* b, uint8_t v) { ph_put(b, &v, 1); }
static void ph_u16(ph_buf* b, uint16_t v) { uint8_t t[2]; psnr_put16(t, v); ph_put(b, t, 2); }
static void ph_u32(ph_buf* b, uint32_t v) { uint8_t t[4]; psnr_put32(t, v); ph_put(b, t, 4); }
static void ph_u64(ph_buf* b, uint64_t v) { uint8_t t[8]; psnr_put64(t, v); ph_put(b, t, 8); }
static void ph_blob(ph_buf* b, const ph_buf* v) { ph_u16(b, (uint16_t)v->n); ph_put(b, v->b, v->n); }
static void ph_str(ph_buf* b, const char* s, size_t n)
{
    uint8_t t[32] = {0};
    size_t k = strlen(s);
    memcpy(t, s, k < n ? k : n);
    ph_put(b, t, n);
}
static void ph_free(ph_buf* b) { free(b->b); memset(b, 0, sizeof *b); }
static void ph_set(ph_buf* b, const uint8_t* p, size_t n) { b->n = 0; ph_put(b, p, n); }

typedef struct { const uint8_t* p; size_t n; int err; } ph_rd;
static const uint8_t* ph_take(ph_rd* r, size_t n)
{
    static const uint8_t zero[32];
    if (r->err || r->n < n) { r->err = 1; return zero; }
    const uint8_t* p = r->p;
    r->p += n;
    r->n -= n;
    return p;
}
static uint8_t  ph_r8(ph_rd* r)  { return *ph_take(r, 1); }
static uint16_t ph_r16(ph_rd* r) { return psnr_get16(ph_take(r, 2)); }
static uint32_t ph_r32(ph_rd* r) { return psnr_get32(ph_take(r, 4)); }
static uint64_t ph_r64(ph_rd* r) { return psnr_get64(ph_take(r, 8)); }
/* a blob: u16 length | bytes; points into the message */
static const uint8_t* ph_rblob(ph_rd* r, size_t* n)
{
    *n = ph_r16(r);
    return r->err ? NULL : ph_take(r, *n);
}
/* a fixed-size string field, up to its first NUL */
static void ph_rstr(ph_rd* r, char* out, size_t n)
{
    const uint8_t* p = ph_take(r, n);
    size_t k = 0;
    while (k < n && p[k]) k++;
    memcpy(out, p, k);
    out[k] = 0;
}

static int ph_loopback(const uint8_t ip[4]) { return ip[0] == 127; }

static int ph_ip_set(const uint8_t ip[4]) { return ip[0] || ip[1] || ip[2] || ip[3]; }

static struct sockaddr_in ph_addr(const uint8_t ip[4], uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, ip, 4);
    a.sin_port = htons(port);
    return a;
}

/* ---- sending ---- */

static void ph_flush(ph_client* c)
{
    while (c->tx.n) {
        int k = (int)send(c->s, (const char*)c->tx.b, (int)c->tx.n, PH_SEND_FLAGS);
        if (k <= 0) {
            if (k < 0 && PH_WOULD_BLOCK()) return;
            c->closing = 2;   /* gone */
            c->tx.n = 0;
            return;
        }
        memmove(c->tx.b, c->tx.b + k, c->tx.n - (size_t)k);
        c->tx.n -= (size_t)k;
    }
}

static void ph_send(ph_client* c, uint8_t type, const ph_buf* p)
{
    if (!PH_USED(c) || c->closing == 2) return;
    if (c->sess) {   /* the session queues it (a peer that stopped reading fills the queue) */
        ph_buf f = {0};
        uint8_t hdr[3];
        hdr[0] = type;
        psnr_put16(hdr + 1, (uint16_t)p->n);
        ph_put(&f, hdr, 3);
        ph_put(&f, p->b, p->n);
        if (psnr_rudp_write(c->sess, f.b, f.n, ph_now()) != 0) c->closing = 2;
        ph_free(&f);
        return;
    }
    if (c->tx.n + 3 + p->n > PH_TX_MAX) { c->closing = 2; return; }   /* a peer that stopped reading */
    uint8_t hdr[3];
    hdr[0] = type;
    psnr_put16(hdr + 1, (uint16_t)p->n);
    ph_put(&c->tx, hdr, 3);
    ph_put(&c->tx, p->b, p->n);
    ph_flush(c);
}

static void ph_reply_err(ph_client* c, uint32_t req, uint32_t code)
{
    ph_buf w = {0};
    ph_u32(&w, req);
    ph_u32(&w, code);
    ph_send(c, PSNR_ERROR, &w);
    ph_free(&w);
}

static void ph_reply_ok(ph_client* c, uint32_t req)
{
    ph_buf w = {0};
    ph_u32(&w, req);
    ph_send(c, PSNR_OK, &w);
    ph_free(&w);
}

/* ---- rooms ---- */

/* Where viewer should send m's traffic (the psnr server's endpoint()). Plus: the host's own game is on loopback,
 * which means nothing to a guest, so a guest is given the host's address as it reaches it (the configured public
 * address, else the address its connection came in on) and the P2P port. Clients relay everything through this
 * server anyway (PSNR_ROUTE_UDP_RELAY); the address only has to name the player. */
static void ph_endpoint(psnr_host* h, const ph_client* m, const ph_client* viewer, uint8_t ip[4], uint16_t* port)
{
    if (ph_loopback(m->ip) && !ph_loopback(viewer->ip)) {
        static const uint8_t none[4];
        memcpy(ip, memcmp(h->cfg.public_ip, none, 4) ? h->cfg.public_ip : viewer->via, 4);
        *port = m->p2p_port;
        return;
    }
    if (m->udp_port && memcmp(m->ip, viewer->ip, 4)) {
        memcpy(ip, m->udp_ip, 4);
        *port = m->udp_port;
        return;
    }
    static const uint8_t none[4];
    memcpy(ip, memcmp(m->local_ip, none, 4) ? m->local_ip : m->ip, 4);
    *port = m->p2p_port;
}

static void ph_put_member(psnr_host* h, ph_buf* w, const ph_room* r, const ph_member* m, const ph_client* viewer)
{
    const ph_client* c = &h->cl[m->c];
    uint8_t ip[4];
    uint16_t port;
    ph_u16(w, m->id);
    ph_u32(w, c->id);
    ph_str(w, c->name, 16);
    ph_endpoint(h, c, viewer, ip, &port);
    ph_put(w, ip, 4);
    ph_u16(w, port);
    ph_u8(w, m->id == r->owner ? 1 : 0);
    ph_blob(w, &m->data);
}

static ph_member* ph_member_of(ph_room* r, int ci)
{
    for (int i = 0; i < r->n; i++)
        if (r->m[i].c == ci) return &r->m[i];
    return NULL;
}

static void ph_joined_reply(psnr_host* h, ph_buf* w, uint32_t req, const ph_room* r, const ph_member* me)
{
    ph_u32(w, req);
    ph_u64(w, r->id);
    ph_u16(w, me->id);
    ph_u16(w, r->owner);
    ph_u8(w, r->max);
    ph_u32(w, r->flags);
    ph_blob(w, &r->ext);
    ph_blob(w, &r->in);
    ph_u8(w, (uint8_t)r->n);
    for (int i = 0; i < r->n; i++) ph_put_member(h, w, r, &r->m[i], &h->cl[me->c]);
}

/* ROUTE: room u64 | user u32 | flags u8. Every player's datagrams go through this server. */
static void ph_route(psnr_host* h, const ph_room* r, int about, int to)
{
    ph_buf w = {0};
    ph_u64(&w, r->id);
    ph_u32(&w, h->cl[about].id);
    ph_u8(&w, PSNR_ROUTE_UDP_RELAY);
    ph_send(&h->cl[to], PSNR_ROUTE, &w);
    ph_free(&w);
}

static ph_member* ph_add_member(ph_room* r, int ci, const uint8_t* data, size_t n)
{
    if (r->n >= PH_MEMBERS) return NULL;
    ph_member* m = &r->m[r->n++];
    memset(m, 0, sizeof *m);
    m->id = ++r->next_member;
    m->c = ci;
    ph_set(&m->data, data, n);
    return m;
}

static void ph_room_free(ph_room* r)
{
    for (int i = 0; i < r->n; i++) ph_free(&r->m[i].data);
    ph_free(&r->ext);
    ph_free(&r->in);
    memset(r, 0, sizeof *r);
}

/* ci leaves r: MEMBER_LEFT (room u64 | member u16 | owner u16) to those who stay; the owner leaving hands the room
 * to the longest-standing member; the last one out closes it. */
static void ph_leave(psnr_host* h, ph_room* r, int ci)
{
    int at = -1;
    for (int i = 0; i < r->n; i++)
        if (r->m[i].c == ci) at = i;
    if (at < 0) return;
    const uint16_t gone = r->m[at].id;
    ph_free(&r->m[at].data);
    memmove(&r->m[at], &r->m[at + 1], sizeof(ph_member) * (size_t)(r->n - at - 1));
    r->n--;
    if (!r->n) { ph_room_free(r); return; }
    if (r->owner == gone) r->owner = r->m[0].id;
    ph_buf w = {0};
    ph_u64(&w, r->id);
    ph_u16(&w, gone);
    ph_u16(&w, r->owner);
    for (int i = 0; i < r->n; i++) ph_send(&h->cl[r->m[i].c], PSNR_MEMBER_LEFT, &w);
    ph_free(&w);
}

static ph_room* ph_room_by_id(psnr_host* h, uint64_t id)
{
    for (int i = 0; i < PH_ROOMS; i++)
        if (h->rooms[i].used && h->rooms[i].id == id) return &h->rooms[i];
    return NULL;
}

/* ---- the messages ---- */

static int ph_banned(psnr_host* h, const uint8_t ip[4], int add_bad)
{
    const uint64_t now = ph_now();
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (h->bans[i].bad && !memcmp(h->bans[i].ip, ip, 4)) {
            if (h->bans[i].until && now >= h->bans[i].until) h->bans[i].bad = 0, h->bans[i].until = 0;
            if (add_bad && ++h->bans[i].bad >= PH_BAD_KEYS) h->bans[i].until = now + PH_BAN_MS;
            return h->bans[i].until && now < h->bans[i].until;
        }
        if (!h->bans[i].bad && free_slot < 0) free_slot = i;
    }
    if (add_bad && free_slot < 0) {   /* full: the entry not ignored now with the fewest wrong keys makes way */
        for (int i = 0; i < 16; i++) {
            const int ignored = h->bans[i].until && now < h->bans[i].until;
            if (!ignored && (free_slot < 0 || h->bans[i].bad < h->bans[free_slot].bad)) free_slot = i;
        }
    }
    if (add_bad && free_slot >= 0) {
        memcpy(h->bans[free_slot].ip, ip, 4);
        h->bans[free_slot].bad = 1;
        h->bans[free_slot].until = 0;
    }
    return 0;
}

static int ph_key_eq(const uint8_t* a, const uint8_t* b)
{
    uint8_t d = 0;
    for (size_t i = 0; i < PSNR_HOST_KEY_LEN; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* A HELLO's key: the one set now, unless it is spent. With own_key_local the key the server started with stays good
 * from this PC, for the host's own game (it keeps the key it started with). */
static int ph_key_ok(psnr_host* h, const uint8_t* blob, size_t n, const uint8_t ip[4])
{
    if (!h->cfg.has_key) return 1;
    if (!blob || n != PSNR_HOST_KEY_LEN) return 0;
    if (h->cfg.own_key_local && ph_loopback(ip) && ph_key_eq(blob, h->cfg.key)) return 1;
    if (atomic_load(&h->key_spent)) return 0;
    return ph_key_eq(blob, h->key_now);
}

/* A new key from psnr_host_new_key, at the top of each round of the server's loop: the wrong keys count again. */
static void ph_take_key(psnr_host* h)
{
    if (!atomic_load(&h->key_pending)) return;
    memcpy(h->key_now, h->key_next, PSNR_HOST_KEY_LEN);
    h->key_bad = 0;
    atomic_store(&h->key_spent, 0);
    atomic_store(&h->key_pending, 0);
    printf("[psnr_host] a new key: HELLOs need it from now on\n");
}

static int ph_name_eq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

static void ph_drop(psnr_host* h, int ci);

static void ph_hello(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    char comm[13], name[17];
    ph_rstr(r, comm, 12);
    ph_rstr(r, name, 16);
    const uint16_t port = ph_r16(r);
    const uint8_t* blob = NULL;
    size_t bn = 0;
    if (!r->err && r->n) blob = ph_rblob(r, &bn);
    if (r->err || !comm[0]) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    if (!ph_key_ok(h, blob, bn, c->ip)) {
        ph_banned(h, c->ip, 1);
        if (!atomic_load(&h->key_spent)) {
            if (++h->key_bad >= PH_KEY_TRIES) {
                atomic_store(&h->key_spent, 1);
                printf("[psnr_host] %d wrong keys: this key lets no one else in until there is a new one\n",
                       PH_KEY_TRIES);
            } else if (h->key_bad <= 5) {   /* (a few lines, not one per guess) */
                printf("[psnr_host] a HELLO with a wrong key was turned away\n");
            }
        }
        ph_reply_err(c, req, PSNR_E_REFUSED);
        c->closing = 1;
        return;
    }
    /* One player per name; the same name from the same address is that player reconnecting. */
    for (int i = 0; i < PH_CLIENTS; i++) {
        ph_client* o = &h->cl[i];
        if (i == ci || !PH_USED(o) || !o->hello || !ph_name_eq(o->name, name)) continue;
        if (memcmp(o->ip, c->ip, 4)) { ph_reply_err(c, req, PSNR_E_NAME_TAKEN); return; }
        printf("[psnr_host] \"%s\" reconnected\n", name);
        ph_drop(h, i);
    }
    /* another release of the same game: its rooms are that game's (the psnr server does this at HELLO too) */
    const char* game = comm;
    for (int i = 0; i < PSNR_HOST_SAME_GAMES; i++)
        if (h->cfg.same_game[i].other[0] && !strncmp(h->cfg.same_game[i].other, comm, 12)) {
            game = h->cfg.same_game[i].game;
            break;
        }
    snprintf(c->comm, sizeof c->comm, "%.12s", game);
    memcpy(c->name, name, sizeof name);
    c->p2p_port = port;
    c->token = (uint32_t)ph_rand(h) | 1u;
    c->hello = 1;
    h->players++;
    if (c->sess) h->udp_players++;
    if (!ph_loopback(c->ip)) {   /* a friend that was introduced got in (either way): not stuck */
        PH_LOCK(&h->ilock);
        for (int i = 0; i < PH_INTROS; i++)
            for (int k = 0; k < h->intros[i].n; k++)
                if (!memcmp(h->intros[i].c[k].ip, c->ip, 4)) h->intros[i].done = 1;
        PH_UNLOCK(&h->ilock);
    }
    if (game != comm)
        printf("[psnr_host] player %u: \"%s\" (%s, in %s's rooms)%s\n", c->id, name, comm, c->comm,
               ph_loopback(c->ip) ? ", this PC" : "");
    else
        printf("[psnr_host] player %u: \"%s\" (%s)%s\n", c->id, name, comm,
               ph_loopback(c->ip) ? ", this PC" : c->sess ? ", over UDP" : "");
    ph_buf w = {0};
    ph_u32(&w, req);
    ph_u32(&w, c->id);
    ph_put(&w, c->ip, 4);
    ph_u32(&w, c->token);
    ph_u8(&w, 1);   /* the relay is available */
    ph_send(c, PSNR_HELLO_ACK, &w);
    ph_free(&w);
}

static void ph_create(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    const uint8_t max = ph_r8(r);
    const uint32_t flags = ph_r32(r);
    size_t ne, ni, nm;
    const uint8_t* ext = ph_rblob(r, &ne);
    const uint8_t* in = ph_rblob(r, &ni);
    const uint8_t* mine = ph_rblob(r, &nm);
    if (r->err || !max) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    ph_room* room = NULL;
    for (int i = 0; i < PH_ROOMS && !room; i++)
        if (!h->rooms[i].used) room = &h->rooms[i];
    if (!room) { ph_reply_err(c, req, PSNR_E_LIMIT); return; }
    memset(room, 0, sizeof *room);
    room->used = 1;
    do room->id = ph_rand(h) >> 1; while (!room->id || ph_room_by_id(h, room->id) != room);
    room->seq = ++h->next_seq;
    memcpy(room->comm, c->comm, sizeof room->comm);
    room->max = max > PH_MEMBERS ? PH_MEMBERS : max;
    room->flags = flags;
    ph_set(&room->ext, ext, ne);
    ph_set(&room->in, in, ni);
    ph_member* me = ph_add_member(room, ci, mine, nm);
    room->owner = me->id;
    printf("[psnr_host] room created by \"%s\" (max %u)\n", c->name, room->max);
    ph_buf w = {0};
    ph_joined_reply(h, &w, req, room, me);
    ph_send(c, PSNR_ROOM_JOINED, &w);
    ph_free(&w);
}

/* ROOM_LIST: req | total u16 | count u16 | room u64 | owner [16] | cur u8 | max u8 | flags u32 | external blob */
static void ph_search(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    const uint16_t start = ph_r16(r), max = ph_r16(r);
    if (r->err) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    ph_room* open[PH_ROOMS];
    int n = 0;
    for (uint64_t seq = 1; seq <= h->next_seq; seq++)   /* oldest first */
        for (int i = 0; i < PH_ROOMS; i++) {
            ph_room* rm = &h->rooms[i];
            if (rm->used && rm->seq == seq && !strcmp(rm->comm, c->comm) && rm->n < rm->max) open[n++] = rm;
        }
    const int from = start < n ? start : n, count = n - from < max ? n - from : max;
    ph_buf w = {0};
    ph_u32(&w, req);
    ph_u16(&w, (uint16_t)n);
    ph_u16(&w, (uint16_t)count);
    for (int k = from; k < from + count; k++) {
        const ph_room* rm = open[k];
        const char* owner = "";
        for (int i = 0; i < rm->n; i++)
            if (rm->m[i].id == rm->owner) owner = h->cl[rm->m[i].c].name;
        ph_u64(&w, rm->id);
        ph_str(&w, owner, 16);
        ph_u8(&w, (uint8_t)rm->n);
        ph_u8(&w, rm->max);
        ph_u32(&w, rm->flags);
        ph_blob(&w, &rm->ext);
    }
    ph_send(c, PSNR_ROOM_LIST, &w);
    ph_free(&w);
}

static void ph_join(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    const uint64_t id = ph_r64(r);
    size_t nm;
    const uint8_t* mine = ph_rblob(r, &nm);
    ph_room* room = r->err ? NULL : ph_room_by_id(h, id);
    if (r->err) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    if (!room || strcmp(room->comm, c->comm)) { ph_reply_err(c, req, PSNR_E_NOT_FOUND); return; }
    ph_buf w = {0};
    ph_member* me = ph_member_of(room, ci);
    if (me) {
        ph_joined_reply(h, &w, req, room, me);
        ph_send(c, PSNR_ROOM_JOINED, &w);
        ph_free(&w);
        return;
    }
    /* (max_players: the host's own limit, below the room's size; the player stays connected, and may try again) */
    const int most = atomic_load(&h->max_players);
    if (most > 0 && room->n >= most) {
        printf("[psnr_host] \"%s\" can't join yet: the game is full (the host takes %d)\n", c->name, most);
        ph_reply_err(c, req, PSNR_E_ROOM_FULL);
        return;
    }
    if (room->n >= room->max || !(me = ph_add_member(room, ci, mine, nm))) {
        ph_reply_err(c, req, PSNR_E_ROOM_FULL);
        return;
    }
    printf("[psnr_host] \"%s\" joined as member %u\n", c->name, me->id);
    ph_joined_reply(h, &w, req, room, me);
    ph_send(c, PSNR_ROOM_JOINED, &w);
    for (int i = 0; i < room->n; i++) {
        if (&room->m[i] == me) continue;
        ph_buf p = {0};
        ph_u64(&p, room->id);
        ph_put_member(h, &p, room, me, &h->cl[room->m[i].c]);
        ph_send(&h->cl[room->m[i].c], PSNR_MEMBER_JOINED, &p);
        ph_free(&p);
    }
    for (int i = 0; i < room->n; i++) {
        if (&room->m[i] == me) continue;
        ph_route(h, room, room->m[i].c, ci);
        ph_route(h, room, ci, room->m[i].c);
    }
    ph_free(&w);
}

static void ph_leave_req(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_room* room = ph_room_by_id(h, ph_r64(r));
    if (r->err || !room || !ph_member_of(room, ci)) { ph_reply_err(&h->cl[ci], req, PSNR_E_NOT_IN_ROOM); return; }
    ph_reply_ok(&h->cl[ci], req);
    ph_leave(h, room, ci);
}

/* SET_ROOM_DATA: room u64 | which u8 (0 external, 1 internal, 2 flags) | blob; owner only; ROOM_DATA to the rest */
static void ph_set_data(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    ph_room* room = ph_room_by_id(h, ph_r64(r));
    const uint8_t which = ph_r8(r);
    size_t n;
    const uint8_t* data = ph_rblob(r, &n);
    if (r->err || which > 2 || (which == 2 && n != 4)) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    ph_member* me = room ? ph_member_of(room, ci) : NULL;
    if (!me) { ph_reply_err(c, req, PSNR_E_NOT_IN_ROOM); return; }
    if (me->id != room->owner) { ph_reply_err(c, req, PSNR_E_NOT_OWNER); return; }
    if (which == 0) ph_set(&room->ext, data, n);
    else if (which == 1) ph_set(&room->in, data, n);
    else room->flags = psnr_get32(data);
    ph_reply_ok(c, req);
    ph_buf w = {0};
    ph_u64(&w, room->id);
    ph_u8(&w, which);
    ph_u16(&w, (uint16_t)n);
    ph_put(&w, data, n);
    for (int i = 0; i < room->n; i++)
        if (room->m[i].c != ci) ph_send(&h->cl[room->m[i].c], PSNR_ROOM_DATA, &w);
    ph_free(&w);
}

/* ROOM_MESSAGE: room u64 | to u16 (0 = everyone else) | blob -> ROOM_MSG: room | from u16 | to u16 | blob */
static void ph_message(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    ph_room* room = ph_room_by_id(h, ph_r64(r));
    const uint16_t to = ph_r16(r);
    size_t n;
    const uint8_t* data = ph_rblob(r, &n);
    if (r->err) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    ph_member* me = room ? ph_member_of(room, ci) : NULL;
    if (!me) { ph_reply_err(c, req, PSNR_E_NOT_IN_ROOM); return; }
    ph_reply_ok(c, req);
    ph_buf w = {0};
    ph_u64(&w, room->id);
    ph_u16(&w, me->id);
    ph_u16(&w, to);
    ph_u16(&w, (uint16_t)n);
    ph_put(&w, data, n);
    for (int i = 0; i < room->n; i++)
        if (&room->m[i] != me && (!to || room->m[i].id == to)) ph_send(&h->cl[room->m[i].c], PSNR_ROOM_MSG, &w);
    ph_free(&w);
}

/* APP_SEND: blob (at most PSNR_APP_MAX bytes) -> APP_MSG: from user u32 | blob, to every other player past HELLO.
 * The hosted game's own messages (psnr_app_send), with no room needed: everyone on this server is one group. */
static void ph_app(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    size_t n;
    const uint8_t* data = ph_rblob(r, &n);
    if (r->err || n > PSNR_APP_MAX) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    ph_reply_ok(c, req);
    ph_buf w = {0};
    ph_u32(&w, c->id);
    ph_u16(&w, (uint16_t)n);
    ph_put(&w, data, n);
    for (int i = 0; i < PH_CLIENTS; i++)
        if (i != ci && PH_USED(&h->cl[i]) && h->cl[i].hello) ph_send(&h->cl[i], PSNR_APP_MSG, &w);
    ph_free(&w);
}

/* KICK_MEMBER: room u64 | member u16; owner only; the target gets KICKED (room u64), the rest MEMBER_LEFT */
static void ph_kick(psnr_host* h, int ci, uint32_t req, ph_rd* r)
{
    ph_client* c = &h->cl[ci];
    ph_room* room = ph_room_by_id(h, ph_r64(r));
    const uint16_t target = ph_r16(r);
    if (r->err) { ph_reply_err(c, req, PSNR_E_BAD_REQUEST); return; }
    ph_member* me = room ? ph_member_of(room, ci) : NULL;
    if (!me) { ph_reply_err(c, req, PSNR_E_NOT_IN_ROOM); return; }
    if (me->id != room->owner) { ph_reply_err(c, req, PSNR_E_NOT_OWNER); return; }
    for (int i = 0; i < room->n; i++) {
        if (room->m[i].id != target || room->m[i].c == ci) continue;
        const int tc = room->m[i].c;
        ph_reply_ok(c, req);
        ph_buf w = {0};
        ph_u64(&w, room->id);
        ph_send(&h->cl[tc], PSNR_KICKED, &w);
        ph_free(&w);
        ph_leave(h, room, tc);
        return;
    }
    ph_reply_err(c, req, PSNR_E_NOT_FOUND);
}

static void ph_message_in(psnr_host* h, int ci, uint8_t type, const uint8_t* p, size_t n)
{
    ph_client* c = &h->cl[ci];
    ph_rd r = { p, n, 0 };
    const uint32_t req = ph_r32(&r);
    if (r.err) return;
    if (type == PSNR_STREAM_CONNECT || type == PSNR_STREAM_ACCEPT) {   /* no relay streams here */
        ph_reply_err(c, req, PSNR_E_NO_RELAY);
        c->closing = 1;
        return;
    }
    if (type != PSNR_HELLO && !c->hello) { ph_reply_err(c, req, PSNR_E_NO_HELLO); return; }
    switch (type) {
    case PSNR_HELLO:        ph_hello(h, ci, req, &r); break;
    case PSNR_HEARTBEAT:    break;
    case PSNR_CREATE_ROOM:  ph_create(h, ci, req, &r); break;
    case PSNR_SEARCH_ROOMS: ph_search(h, ci, req, &r); break;
    case PSNR_JOIN_ROOM:    ph_join(h, ci, req, &r); break;
    case PSNR_LEAVE_ROOM:   ph_leave_req(h, ci, req, &r); break;
    case PSNR_SET_ROOM_DATA: ph_set_data(h, ci, req, &r); break;
    case PSNR_ROOM_MESSAGE: ph_message(h, ci, req, &r); break;
    case PSNR_KICK_MEMBER:  ph_kick(h, ci, req, &r); break;
    case PSNR_APP_SEND:     ph_app(h, ci, req, &r); break;
    default:              ph_reply_err(c, req, PSNR_E_BAD_REQUEST); break;   /* scores: not here */
    }
}

/* ---- UDP: probes and the relay (udp.go, relay.go) ---- */

static int ph_shares_room(psnr_host* h, int a, int b)
{
    for (int i = 0; i < PH_ROOMS; i++) {
        if (!h->rooms[i].used) continue;
        if (ph_member_of(&h->rooms[i], a) && ph_member_of(&h->rooms[i], b)) return 1;
    }
    return 0;
}

static int ph_client_by_id(psnr_host* h, uint32_t id)
{
    for (int i = 0; i < PH_CLIENTS; i++)
        if (PH_USED(&h->cl[i]) && h->cl[i].hello && h->cl[i].id == id) return i;
    return -1;
}

/* ---- The introductions and the sessions over UDP (psnr_intro.h, psnr_rudp.h) ---- */

static void ph_frames(psnr_host* h, int ci);

/* A free client slot for a player from ip (TCP or UDP), or -1: none free, two from that address still to say HELLO,
 * or the address is ignored for wrong keys. */
static int ph_slot(psnr_host* h, const uint8_t ip[4])
{
    int ci = -1, early = 0;
    for (int i = 0; i < PH_CLIENTS; i++) {
        if (!PH_USED(&h->cl[i])) { if (ci < 0) ci = i; }
        else if (!h->cl[i].hello && !h->cl[i].closing && !memcmp(h->cl[i].ip, ip, 4)) early++;   /* still to say it */
    }
    if (ci < 0 || early >= PH_EARLY_PER_IP || ph_banned(h, ip, 0)) return -1;
    return ci;
}

static void ph_sess_send(void* ctx, const uint8_t* pkt, size_t len)
{
    ph_client* c = (ph_client*)ctx;
    const struct sockaddr_in to = ph_addr(c->peer_ip, c->peer_port);
    PH_SENDTO(c->host, pkt, len, &to);
}

static void ph_sess_read(psnr_host* h, int ci)
{
    ph_client* c = &h->cl[ci];
    uint8_t tmp[4096];
    size_t k;
    while ((k = psnr_rudp_read(c->sess, tmp, sizeof tmp)) > 0)
        if (ph_put(&c->rx, tmp, k)) { c->closing = 2; return; }
    ph_frames(h, ci);
}

static void ph_new_room(psnr_host* h)
{
    h->room = psnr_intro_random() & ((1ull << PSNR_HOST_ROOM_BITS) - 1);
    if (!h->room) h->room = 1;
    h->owner = psnr_intro_random() | 1u;
}

/* Callers hold ilock. */
static int ph_endpoint_set(const psnr_host* h) { return ph_ip_set(h->intro.ip) && h->intro.port; }

static void ph_unregister(psnr_host* h)
{
    if (!ph_endpoint_set(h)) return;
    uint8_t pkt[PSNR_INTRO_REQ_LEN];
    const size_t len = psnr_intro_unregister(pkt, h->room, h->owner);
    const struct sockaddr_in to = ph_addr(h->intro.ip, h->intro.port);
    PH_SENDTO(h, pkt, len, &to);   /* (twice: it has no answer, and one may be lost) */
    PH_SENDTO(h, pkt, len, &to);
}

static void ph_register(psnr_host* h)
{
    psnr_cand c[2];
    int n = 0;
    if (ph_ip_set(h->intro.home_ip)) c[n++] = psnr_cand_of(h->intro.home_ip, h->port);
    if (ph_ip_set(h->intro.upnp_ip) && h->intro.upnp_port) c[n++] = psnr_cand_of(h->intro.upnp_ip, h->intro.upnp_port);
    uint8_t pkt[PSNR_INTRO_REQ_LEN];
    const size_t len = psnr_intro_register(pkt, h->intro.version, h->room, h->owner, h->port, c, n);
    const struct sockaddr_in to = ph_addr(h->intro.ip, h->intro.port);
    PH_SENDTO(h, pkt, len, &to);
}

/* Each round of the loop: REGISTER when due, and the knocks. */
static void ph_intro_tick(psnr_host* h, uint64_t now)
{
    PH_LOCK(&h->ilock);
    if (ph_endpoint_set(h) && (h->reg_kick || now >= h->reg_next)) {
        if (!h->reg_kick && h->reg_waiting && ++h->reg_unanswered >= PH_REG_LOST &&
            h->reg_state != PSNR_HOST_INTRO_UNREACHABLE) {
            h->reg_state = PSNR_HOST_INTRO_UNREACHABLE;
            printf("[psnr_host] the endpoint doesn't answer: friends need the direct code (asked again every %d s)\n",
                   PH_REG_SLOW_MS / 1000);
        }
        h->reg_kick = 0;
        h->reg_waiting = 1;
        h->reg_next = now + (h->reg_unanswered >= PH_REG_LOST ? PH_REG_SLOW_MS : PH_REG_RETRY_MS);
        ph_register(h);
    }
    for (int i = 0; i < PH_INTROS; i++) {
        if (!h->intros[i].nonce || h->intros[i].done || now - h->intros[i].t0 >= PH_KNOCK_FOR_MS ||
            now < h->intros[i].next)
            continue;
        h->intros[i].next = now + PH_KNOCK_MS;
        uint8_t pkt[32];
        const size_t len = psnr_intro_knock(pkt, h->room, h->intros[i].nonce);
        for (int k = 0; k < h->intros[i].n; k++) {
            const struct sockaddr_in to = ph_addr(h->intros[i].c[k].ip, h->intros[i].c[k].port);
            PH_SENDTO(h, pkt, len, &to);
        }
    }
    PH_UNLOCK(&h->ilock);
}

/* What the endpoint says: REGISTERED and INTRO. Callers hold ilock. */
static void ph_endpoint_in(psnr_host* h, const psnr_intro_pkt* in, uint64_t now)
{
    if (in->room != h->room) return;   /* (about a number this host has left) */
    if (in->type == PSNR_PKT_REGISTERED) {
        h->reg_waiting = 0;
        h->reg_unanswered = 0;   /* (whatever it says, it answers) */
        if (h->reg_state == PSNR_HOST_INTRO_UNREACHABLE) h->reg_state = PSNR_HOST_INTRO_REGISTERING;
        switch (in->status) {
        case PSNR_INTRO_OK: {
            if (h->reg_state != PSNR_HOST_INTRO_REGISTERED)
                printf("[psnr_host] the endpoint has this room: friends can join with its room code\n");
            h->reg_state = PSNR_HOST_INTRO_REGISTERED;
            memcpy(h->seen_ip, in->seen.ip, 4);
            h->seen_port = in->seen.port;
            const unsigned every = in->refresh >= 5 && in->refresh <= 60 ? in->refresh : 20;
            h->reg_next = now + every * 1000u;
            break;
        }
        case PSNR_INTRO_SLOW:   /* try again in a few seconds (not an error) */
            h->reg_next = now + PH_REG_SLOW_MS;
            break;
        case PSNR_INTRO_TAKEN:   /* another owner's number: draw another */
            ph_new_room(h);
            memset(h->intros, 0, sizeof h->intros);
            h->reg_state = PSNR_HOST_INTRO_REGISTERING;
            h->reg_kick = 1;
            printf("[psnr_host] the room number was another host's: a new one\n");
            break;
        case PSNR_INTRO_FULL:
            if (h->reg_state != PSNR_HOST_INTRO_LIMITED)
                printf("[psnr_host] the endpoint has no place for another room now (asked again in a minute)\n");
            h->reg_state = PSNR_HOST_INTRO_LIMITED;
            h->reg_next = now + PH_REG_FULL_MS;
            break;
        default:
            break;
        }
        return;
    }
    if (in->type != PSNR_PKT_INTRO || !in->nonce) return;
    int at = -1;
    for (int i = 0; i < PH_INTROS && at < 0; i++)
        if (h->intros[i].nonce == in->nonce) at = i;
    const int again = at >= 0;
    if (!again) {   /* a free entry, else the oldest */
        at = 0;
        for (int i = 0; i < PH_INTROS; i++) {
            if (!h->intros[i].nonce) { at = i; break; }
            if (h->intros[i].t0 < h->intros[at].t0) at = i;
        }
        memset(&h->intros[at], 0, sizeof h->intros[at]);
        h->intros[at].nonce = in->nonce;
        h->intros[at].t0 = h->intros[at].next = now;
    }
    h->intros[at].n = 0;
    for (int k = 0; k < in->n && h->intros[at].n < 2; k++)
        if (in->cands[k].family == 4 && ph_ip_set(in->cands[k].ip) && in->cands[k].port)
            h->intros[at].c[h->intros[at].n++] = in->cands[k];
    if (!again) printf("[psnr_host] a friend's game looked this room up: knocking\n");
}

/* OPEN, OPEN2 (the session's opening), DATA and CLOSE (the session), and the endpoint's packets. */
static void ph_intro_in(psnr_host* h, const uint8_t* pkt, size_t n, const struct sockaddr_in* from)
{
    const uint64_t now = ph_now();
    uint8_t ip[4];
    memcpy(ip, &from->sin_addr, 4);
    const uint16_t port = ntohs(from->sin_port);
    const int type = psnr_udp_type(pkt, n);

    if (type == PSNR_PKT_DATA || type == PSNR_PKT_CLOSE) {
        const uint32_t sid = psnr_rudp_session_of(pkt, n);
        for (int i = 0; i < PH_CLIENTS; i++) {
            ph_client* c = &h->cl[i];
            if (!c->sess || psnr_rudp_session(c->sess) != sid || memcmp(c->peer_ip, ip, 4)) continue;
            c->peer_port = port;   /* (the friend's router may have moved it) */
            c->last_rx = now;
            psnr_rudp_input(c->sess, pkt, n, now);
            psnr_rudp_tick(c->sess, now);
            ph_sess_read(h, i);
            return;
        }
        if (type == PSNR_PKT_DATA && sid) {   /* a session this host doesn't have (it restarted): over */
            uint8_t bye[10];
            const size_t len = psnr_rudp_close_packet(bye, sid, PSNR_RUDP_BYE);
            PH_SENDTO(h, bye, len, from);
        }
        return;
    }

    psnr_intro_pkt in;
    if (!psnr_intro_read(pkt, n, &in)) return;
    PH_LOCK(&h->ilock);
    if (in.type == PSNR_PKT_REGISTERED || in.type == PSNR_PKT_INTRO) {
        if (ph_endpoint_set(h) && !memcmp(ip, h->intro.ip, 4) && port == h->intro.port) ph_endpoint_in(h, &in, now);
        PH_UNLOCK(&h->ilock);
        return;
    }
    if (in.type == PSNR_PKT_OPEN) {   /* COOKIE, at its source: no state until OPEN2 brings the cookie back */
        if (in.room == h->room) {
            uint8_t out[32];
            const uint64_t cookie = psnr_intro_cookie_of(h->cookie_secret, ip, port, in.nonce64, now / 60000u);
            const size_t len = psnr_intro_cookie(out, in.nonce64, cookie);
            PH_SENDTO(h, out, len, from);
        }
        PH_UNLOCK(&h->ilock);
        return;
    }
    if (in.type != PSNR_PKT_OPEN2) { PH_UNLOCK(&h->ilock); return; }
    const uint64_t minute = now / 60000u;
    if (in.cookie != psnr_intro_cookie_of(h->cookie_secret, ip, port, in.nonce64, minute) &&
        (!minute || in.cookie != psnr_intro_cookie_of(h->cookie_secret, ip, port, in.nonce64, minute - 1))) {
        PH_UNLOCK(&h->ilock);
        return;
    }
    for (int i = 0; i < PH_INTROS; i++)   /* (the nonce's top half is the LOOKUP's) */
        if (h->intros[i].nonce && h->intros[i].nonce == (uint32_t)(in.nonce64 >> 32)) h->intros[i].done = 1;
    PH_UNLOCK(&h->ilock);
    uint8_t out[32];
    for (int i = 0; i < PH_CLIENTS; i++) {   /* OPENED was lost: the same again */
        ph_client* c = &h->cl[i];
        if (c->sess && c->nonce == in.nonce64 && !memcmp(c->peer_ip, ip, 4)) {
            c->peer_port = port;
            const size_t len = psnr_intro_opened(out, in.nonce64, psnr_rudp_session(c->sess));
            PH_SENDTO(h, out, len, from);
            return;
        }
    }
    const int ci = ph_slot(h, ip);
    if (ci < 0) return;   /* (no answer: the friend's TCP try may still get in, or its retry later) */
    uint32_t sid = 0;
    while (!sid) {
        sid = (uint32_t)ph_rand(h);
        for (int i = 0; i < PH_CLIENTS; i++)
            if (h->cl[i].sess && psnr_rudp_session(h->cl[i].sess) == sid) sid = 0;
    }
    ph_client* c = &h->cl[ci];
    memset(c, 0, sizeof *c);
    c->s = PH_BAD;
    c->host = h;
    c->sess = psnr_rudp_new(sid, ph_sess_send, c, now);
    if (!c->sess) return;
    c->id = ++h->next_id;
    memcpy(c->ip, ip, 4);
    memcpy(c->peer_ip, ip, 4);
    c->peer_port = port;
    c->nonce = in.nonce64;
    PH_LOCK(&h->ilock);   /* (the address a guest names this host by: see ph_endpoint) */
    memcpy(c->via, ph_ip_set(h->intro.home_ip) ? h->intro.home_ip : h->seen_ip, 4);
    PH_UNLOCK(&h->ilock);
    c->last_rx = c->born = now;
    const size_t len = psnr_intro_opened(out, in.nonce64, sid);
    PH_SENDTO(h, out, len, from);
    printf("[psnr_host] a friend's game opened a session over UDP\n");
}

static void ph_udp_in(psnr_host* h, uint8_t* pkt, size_t n, const struct sockaddr_in* from)
{
    if (n < 5 || memcmp(pkt, "PSNR", 4)) return;
    if (pkt[4] == 0x01 && n >= 17) {   /* PROBE: user u32 | token u32 | local_ip [4] */
        const int ci = ph_client_by_id(h, psnr_get32(pkt + 5));
        if (ci < 0 || h->cl[ci].token != psnr_get32(pkt + 9)) return;
        ph_client* c = &h->cl[ci];
        memcpy(c->udp_ip, &from->sin_addr, 4);
        c->udp_port = ntohs(from->sin_port);
        memcpy(c->local_ip, pkt + 13, 4);
        uint8_t reply[11];
        memcpy(reply, "PSNR", 4);
        reply[4] = 0x81;
        memcpy(reply + 5, &from->sin_addr, 4);
        psnr_put16(reply + 9, c->udp_port);
        PH_SENDTO(h, reply, sizeof reply, from);
        return;
    }
    if (pkt[4] == 0x03 && n >= 17) {   /* RELAY: from u32 | token u32 | to u32 | payload -> "PSNR" 03 | from | payload */
        const uint32_t fid = psnr_get32(pkt + 5), tid = psnr_get32(pkt + 13);
        const int src = ph_client_by_id(h, fid), dst = ph_client_by_id(h, tid);
        if (src < 0 || dst < 0 || h->cl[src].token != psnr_get32(pkt + 9) || !h->cl[dst].udp_port ||
            !ph_shares_room(h, src, dst))
            return;
        memmove(pkt + 9, pkt + 17, n - 17);
        psnr_put32(pkt + 5, fid);
        const struct sockaddr_in to = ph_addr(h->cl[dst].udp_ip, h->cl[dst].udp_port);
        PH_SENDTO(h, pkt, n - 8, &to);
        return;
    }
    ph_intro_in(h, pkt, n, from);   /* the introductions and the sessions; PUNCH and anything else: nothing to do */
}

/* ---- connections ---- */

static void ph_nonblock(ph_sock s)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static void ph_drop(psnr_host* h, int ci)
{
    ph_client* c = &h->cl[ci];
    if (!PH_USED(c)) return;
    for (int i = 0; i < PH_ROOMS; i++)
        if (h->rooms[i].used) ph_leave(h, &h->rooms[i], ci);
    if (c->hello) {
        h->players--;
        if (c->sess) h->udp_players--;
        printf("[psnr_host] player %u (\"%s\") gone\n", c->id, c->name);
    }
    if (c->sess) {
        psnr_rudp_close(c->sess, PSNR_RUDP_BYE);
        psnr_rudp_free(c->sess);
    } else {
        ph_close(c->s);
    }
    ph_free(&c->rx);
    ph_free(&c->tx);
    memset(c, 0, sizeof *c);
    c->s = PH_BAD;
}

static void ph_accept(psnr_host* h)
{
    struct sockaddr_in a;
    ph_socklen n = (ph_socklen)sizeof a;
    ph_sock s = accept(h->lis, (struct sockaddr*)&a, &n);
    if (s == PH_BAD) return;
    uint8_t ip[4];
    memcpy(ip, &a.sin_addr, 4);
    const int ci = ph_slot(h, ip);
    if (ci < 0) { ph_close(s); return; }
    ph_client* c = &h->cl[ci];
    memset(c, 0, sizeof *c);
    c->s = s;
    c->id = ++h->next_id;
    memcpy(c->ip, ip, 4);
    n = (ph_socklen)sizeof a;
    if (getsockname(s, (struct sockaddr*)&a, &n) == 0) memcpy(c->via, &a.sin_addr, 4);
    c->last_rx = c->born = ph_now();
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    ph_nonblock(s);
}

/* Reads what arrived and handles each whole frame: u8 type | u16 length | payload. */
static void ph_read(psnr_host* h, int ci)
{
    ph_client* c = &h->cl[ci];
    uint8_t tmp[4096];
    for (;;) {
        int k = (int)recv(c->s, (char*)tmp, (int)sizeof tmp, 0);
        if (k == 0 || (k < 0 && !PH_WOULD_BLOCK())) { c->closing = 2; return; }
        if (k < 0) break;
        if (ph_put(&c->rx, tmp, (size_t)k)) { c->closing = 2; return; }
        c->last_rx = ph_now();
    }
    ph_frames(h, ci);
}

/* Handles each whole frame in a client's rx. */
static void ph_frames(psnr_host* h, int ci)
{
    ph_client* c = &h->cl[ci];
    size_t off = 0;
    while (c->rx.n - off >= 3 && !c->closing) {
        const uint16_t n = psnr_get16(c->rx.b + off + 1);
        if (c->rx.n - off < 3u + n) break;
        ph_message_in(h, ci, c->rx.b[off], c->rx.b + off + 3, n);
        if (!PH_USED(&h->cl[ci])) return;   /* (dropped as a reconnect) */
        off += 3u + n;
    }
    memmove(c->rx.b, c->rx.b + off, c->rx.n - off);
    c->rx.n -= off;
}

#ifdef _WIN32
static DWORD WINAPI ph_main(LPVOID arg)
#else
static void* ph_main(void* arg)
#endif
{
    psnr_host* h = (psnr_host*)arg;
    ph_pollfd fds[2 + PH_CLIENTS];
    int who[2 + PH_CLIENTS];
    uint8_t pkt[2048];   /* (>= PSNR_RUDP_DGRAM) */
    while (!h->stop) {
        ph_take_key(h);
        int n = 0;
        fds[n].fd = h->lis; fds[n].events = POLLIN; fds[n].revents = 0; who[n++] = -1;
        fds[n].fd = h->udp; fds[n].events = POLLIN; fds[n].revents = 0; who[n++] = -2;
        for (int i = 0; i < PH_CLIENTS; i++) {
            if (h->cl[i].s == PH_BAD) continue;
            fds[n].fd = h->cl[i].s;
            fds[n].events = (short)(POLLIN | (h->cl[i].tx.n ? POLLOUT : 0));
            fds[n].revents = 0;
            who[n++] = i;
        }
        if (PH_POLL(fds, (unsigned)n, 100) < 0) continue;
        for (int k = 0; k < n; k++) {
            if (!fds[k].revents) continue;
            if (who[k] == -1) { ph_accept(h); continue; }
            if (who[k] == -2) {
                for (int m = 0; m < 64; m++) {   /* drain the queue */
                    struct sockaddr_in from;
                    ph_socklen fl = (ph_socklen)sizeof from;
                    int got = (int)recvfrom(h->udp, (char*)pkt, (int)sizeof pkt, 0, (struct sockaddr*)&from, &fl);
                    if (got < 0) break;   /* nothing more (or an ICMP report on Windows: next round) */
                    ph_udp_in(h, pkt, (size_t)got, &from);
                }
                continue;
            }
            const int ci = who[k];
            if (h->cl[ci].s == PH_BAD) continue;
            if (fds[k].revents & POLLOUT) ph_flush(&h->cl[ci]);
            if (fds[k].revents & (POLLIN | POLLERR | POLLHUP)) ph_read(h, ci);
        }
#ifdef PH_TEST_UDP_IN
        PH_TEST_UDP_IN(h);   /* (a test's virtual network: its datagrams for this host) */
#endif
        const uint64_t now = ph_now();
        ph_intro_tick(h, now);
        for (int i = 0; i < PH_CLIENTS; i++) {
            ph_client* c = &h->cl[i];
            if (!PH_USED(c)) continue;
            int sess_done = 0;   /* a session's closing: once its last reply was acknowledged, or after a while */
            if (c->sess) {
                psnr_rudp_tick(c->sess, now);
                const int dead = psnr_rudp_dead(c->sess, now);
                if (dead != PSNR_RUDP_ALIVE) {
                    if (c->hello) printf("[psnr_host] player %u's session is over (%d)\n", c->id, dead);
                    c->closing = 2;
                }
                if (c->closing == 1 && !c->closing_at) c->closing_at = now;
                sess_done = c->closing == 1 && (psnr_rudp_flushed(c->sess) || now - c->closing_at > PH_SESS_CLOSE_MS);
            }
            if (c->closing == 2 || (c->closing == 1 && (c->sess ? sess_done : !c->tx.n)) ||
                now - c->last_rx > PH_TIMEOUT_MS || (!c->hello && now - c->born > PH_HELLO_MS))
                ph_drop(h, i);
        }
    }
    return 0;
}

/* The room servers running, for psnr_host_unregister_all. */
static ph_lock    s_live_lock = PH_LOCK_STATIC;
static psnr_host* s_live[4];

static void ph_live_add(psnr_host* h)
{
    PH_LOCK(&s_live_lock);
    for (int i = 0; i < 4; i++)
        if (!s_live[i]) { s_live[i] = h; break; }
    PH_UNLOCK(&s_live_lock);
}

static void ph_live_remove(psnr_host* h)
{
    PH_LOCK(&s_live_lock);
    for (int i = 0; i < 4; i++)
        if (s_live[i] == h) s_live[i] = NULL;
    PH_UNLOCK(&s_live_lock);
}

psnr_host* psnr_host_start(const psnr_host_config* cfg, char* err, size_t err_cap)
{
#define PH_FAIL(...) do { if (err && err_cap) snprintf(err, err_cap, __VA_ARGS__); goto fail; } while (0)
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    psnr_host* h = (psnr_host*)calloc(1, sizeof *h);
    if (!h) return NULL;
    h->cfg = *cfg;
    memcpy(h->key_now, cfg->key, PSNR_HOST_KEY_LEN);
    atomic_store(&h->max_players, cfg->max_players > 0 ? cfg->max_players : 0);
    atomic_init(&h->key_spent, 0);
    atomic_init(&h->key_pending, 0);
    h->lis = h->udp = PH_BAD;
    for (int i = 0; i < PH_CLIENTS; i++) h->cl[i].s = PH_BAD;
    h->rng = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)h;
    PH_LOCK_INIT(&h->ilock);
    ph_new_room(h);
    for (int i = 0; i < 4; i++) psnr_put64(h->cookie_secret + 8 * i, psnr_intro_random());
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(cfg->bind_loopback ? INADDR_LOOPBACK : INADDR_ANY);
        /* With port 0, a number free for both: the TCP side picks one, then the UDP side does, then numbers drawn
         * from the dynamic range. (Windows hands its picks out in order, and a whole block of them can be reserved
         * for the other protocol: on a CI runner, eight TCP picks in a row were all refused for UDP.) */
        for (int tries = 0; tries < (cfg->port ? 1 : 64) && h->udp == PH_BAD; tries++) {
            ph_sock t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            ph_sock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (t == PH_BAD || u == PH_BAD) {
                if (t != PH_BAD) ph_close(t);
                if (u != PH_BAD) ph_close(u);
                PH_FAIL("no socket");
            }
#ifndef _WIN32
            { int one = 1; setsockopt(t, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
#endif
            const int udp_first = !cfg->port && tries == 1;
            const ph_sock first = udp_first ? u : t, second = udp_first ? t : u;
            uint16_t want = cfg->port;
            if (!want && tries >= 2) want = (uint16_t)(49152 + ph_rand(h) % 16384);
            a.sin_port = htons(want);
            ph_socklen n = (ph_socklen)sizeof a;
            if (bind(first, (struct sockaddr*)&a, sizeof a) != 0 || getsockname(first, (struct sockaddr*)&a, &n) != 0) {
                ph_close(t);
                ph_close(u);
                if (cfg->port) PH_FAIL("port %u (TCP) is in use by another program", (unsigned)cfg->port);
                continue;
            }
            if (bind(second, (struct sockaddr*)&a, sizeof a) != 0) {
                ph_close(t);
                ph_close(u);
                if (cfg->port) PH_FAIL("port %u (UDP) is in use by another program", (unsigned)cfg->port);
                continue;
            }
            h->lis = t;
            h->udp = u;
            h->port = ntohs(a.sin_port);
        }
        if (h->udp == PH_BAD) PH_FAIL("no free port for both TCP and UDP");
        if (listen(h->lis, 8) != 0) PH_FAIL("listen failed");
        ph_nonblock(h->lis);
        ph_nonblock(h->udp);
#ifdef _WIN32
        {   /* a datagram to a closed port must not make later receives fail (WSAECONNRESET) */
            BOOL off = FALSE;
            DWORD got = 0;
            WSAIoctl(h->udp, SIO_UDP_CONNRESET, &off, sizeof off, NULL, 0, &got, NULL, NULL);
        }
#endif
    }
#ifdef _WIN32
    h->th = CreateThread(NULL, 0, ph_main, h, 0, NULL);
    if (!h->th) PH_FAIL("no thread");
#else
    if (pthread_create(&h->th, NULL, ph_main, h) != 0) PH_FAIL("no thread");
#endif
    printf("[psnr_host] rooms answered on port %u (TCP and UDP)%s\n", (unsigned)h->port,
           cfg->has_key ? ", with a key" : "");
    ph_live_add(h);
    return h;
fail:
    if (h->lis != PH_BAD) ph_close(h->lis);
    if (h->udp != PH_BAD) ph_close(h->udp);
    PH_LOCK_FREE(&h->ilock);
    free(h);
    return NULL;
#undef PH_FAIL
}

uint16_t psnr_host_port(const psnr_host* h)
{
    return h ? h->port : 0;
}

int psnr_host_players(psnr_host* h)
{
    return h ? h->players : 0;
}

void psnr_host_new_key(psnr_host* h, const uint8_t key[PSNR_HOST_KEY_LEN])
{
    if (!h || !key) return;
    for (int i = 0; i < 200 && atomic_load(&h->key_pending); i++) ph_sleep_ms(10);   /* (one before it, still due) */
    memcpy(h->key_next, key, PSNR_HOST_KEY_LEN);
    atomic_store(&h->key_pending, 1);
    /* the server's loop takes it within a round (100 ms at most): then key_spent reads as the new key's */
    for (int i = 0; i < 200 && atomic_load(&h->key_pending); i++) ph_sleep_ms(10);
}

int psnr_host_key_spent(psnr_host* h)
{
    return h ? atomic_load(&h->key_spent) : 0;
}

void psnr_host_set_max_players(psnr_host* h, int n)
{
    if (h) atomic_store(&h->max_players, n > 0 ? n : 0);
}

int psnr_host_members(psnr_host* h)
{
    int most = 0;
    if (h)   /* (the server's thread changes the rooms: a count read across a change is one off for a moment) */
        for (int i = 0; i < PH_ROOMS; i++)
            if (h->rooms[i].used && h->rooms[i].n > most) most = h->rooms[i].n;
    return most;
}

void psnr_host_stop(psnr_host* h)
{
    if (!h) return;
    ph_live_remove(h);
    h->stop = 1;
#ifdef _WIN32
    WaitForSingleObject(h->th, INFINITE);
    CloseHandle(h->th);
#else
    pthread_join(h->th, NULL);
#endif
    for (int i = 0; i < PH_CLIENTS; i++) ph_drop(h, i);
    for (int i = 0; i < PH_ROOMS; i++)
        if (h->rooms[i].used) ph_room_free(&h->rooms[i]);
    PH_LOCK(&h->ilock);
    ph_unregister(h);
    PH_UNLOCK(&h->ilock);
    ph_close(h->lis);
    ph_close(h->udp);
    PH_LOCK_FREE(&h->ilock);
    printf("[psnr_host] stopped\n");
    free(h);
}

/* ---- The introductions' API ---- */

void psnr_host_set_intro(psnr_host* h, const psnr_host_intro* in)
{
    if (!h || !in) return;
    PH_LOCK(&h->ilock);
    const int moved = memcmp(h->intro.ip, in->ip, 4) || h->intro.port != in->port;
    if (moved) ph_unregister(h);   /* (from the endpoint it had) */
    h->intro = *in;
    if (!ph_endpoint_set(h)) {
        h->reg_state = PSNR_HOST_INTRO_OFF;
    } else if (moved || h->reg_state == PSNR_HOST_INTRO_OFF) {
        h->reg_state = PSNR_HOST_INTRO_REGISTERING;
        h->reg_unanswered = h->reg_waiting = 0;
        memset(h->seen_ip, 0, 4);
        h->seen_port = 0;
    }
    h->reg_kick = ph_endpoint_set(h);   /* (now: also when only the candidates changed) */
    PH_UNLOCK(&h->ilock);
}

uint64_t psnr_host_room(psnr_host* h)
{
    if (!h) return 0;
    PH_LOCK(&h->ilock);
    const uint64_t r = h->room;
    PH_UNLOCK(&h->ilock);
    return r;
}

uint64_t psnr_host_new_room(psnr_host* h)
{
    if (!h) return 0;
    PH_LOCK(&h->ilock);
    ph_unregister(h);
    ph_new_room(h);
    memset(h->intros, 0, sizeof h->intros);
    if (ph_endpoint_set(h)) {
        h->reg_state = PSNR_HOST_INTRO_REGISTERING;
        h->reg_unanswered = h->reg_waiting = 0;
        h->reg_kick = 1;
    }
    const uint64_t r = h->room;
    PH_UNLOCK(&h->ilock);
    return r;
}

void psnr_host_intro_get(psnr_host* h, psnr_host_intro_info* out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!h) return;
    const uint64_t now = ph_now();
    PH_LOCK(&h->ilock);
    out->state = h->reg_state;
    out->room = h->room;
    memcpy(out->seen_ip, h->seen_ip, 4);
    out->seen_port = h->seen_port;
    for (int i = 0; i < PH_INTROS; i++)
        if (h->intros[i].nonce && !h->intros[i].done && now - h->intros[i].t0 >= PH_STUCK_MS &&
            now - h->intros[i].t0 < PH_STUCK_FOR_MS)
            out->stuck = 1;
    out->udp_players = h->udp_players;
    PH_UNLOCK(&h->ilock);
}

void psnr_host_unregister_all(void)
{
    PH_LOCK(&s_live_lock);
    for (int i = 0; i < 4; i++) {
        psnr_host* h = s_live[i];
        if (!h) continue;
        PH_LOCK(&h->ilock);
        ph_unregister(h);
        memset(h->intro.ip, 0, 4);   /* (and no REGISTER after it) */
        h->reg_state = PSNR_HOST_INTRO_OFF;
        PH_UNLOCK(&h->ilock);
    }
    PH_UNLOCK(&s_live_lock);
}

int psnr_host_resolve(const char* name, uint8_t ip[4])
{
    return psnr_resolve4(name, ip);
}
