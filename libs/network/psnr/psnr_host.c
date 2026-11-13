/*
 * psnr_host.c - see ps3emu/psnr_host.h. The handlers follow the psnr server's
 * (internal/psnr in the Demon's Souls Recomp server: rooms.go, server.go,
 * udp.go, relay.go) message for message; the comments say where this one
 * differs. Every name here starts with ph_ so a test can include this file
 * beside psnr.c.
 */
#include "ps3emu/psnr_host.h"
#include "psnr.h"

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
#endif

#define PH_CLIENTS     12      /* 4 players, plus reconnects still closing */
#define PH_ROOMS       4
#define PH_MEMBERS     8
#define PH_TIMEOUT_MS  90000   /* as the psnr server: HELLO/HEARTBEAT keep a client */
#define PH_TX_MAX      (1u << 20)
#define PH_BAD_KEYS    5       /* wrong keys from one address before it is ignored */
#define PH_BAN_MS      60000

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
    ph_buf   rx, tx;
    int      closing;             /* drop once tx is out */
} ph_client;

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
    volatile int stop;
    int       players;            /* read by psnr_host_players */
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

/* ---- sending ---- */

static void ph_flush(ph_client* c)
{
    while (c->tx.n) {
        int k = (int)send(c->s, (const char*)c->tx.b, (int)c->tx.n, 0);
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
    if (c->s == PH_BAD || c->closing == 2) return;
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
    if (add_bad && free_slot >= 0) {
        memcpy(h->bans[free_slot].ip, ip, 4);
        h->bans[free_slot].bad = 1;
        h->bans[free_slot].until = 0;
    }
    return 0;
}

static int ph_key_ok(const psnr_host* h, const uint8_t* blob, size_t n)
{
    if (!h->cfg.has_key) return 1;
    if (!blob || n != PSNR_HOST_KEY_LEN) return 0;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(blob[i] ^ h->cfg.key[i]);
    return d == 0;
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
    if (!ph_key_ok(h, blob, bn)) {
        ph_banned(h, c->ip, 1);
        printf("[psnr_host] a HELLO with a wrong key was turned away\n");
        ph_reply_err(c, req, PSNR_E_REFUSED);
        c->closing = 1;
        return;
    }
    /* One player per name; the same name from the same address is that player reconnecting. */
    for (int i = 0; i < PH_CLIENTS; i++) {
        ph_client* o = &h->cl[i];
        if (i == ci || o->s == PH_BAD || !o->hello || !ph_name_eq(o->name, name)) continue;
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
    if (game != comm)
        printf("[psnr_host] player %u: \"%s\" (%s, in %s's rooms)%s\n", c->id, name, comm, c->comm,
               ph_loopback(c->ip) ? ", this PC" : "");
    else
        printf("[psnr_host] player %u: \"%s\" (%s)%s\n", c->id, name, comm, ph_loopback(c->ip) ? ", this PC" : "");
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
        if (i != ci && h->cl[i].s != PH_BAD && h->cl[i].hello) ph_send(&h->cl[i], PSNR_APP_MSG, &w);
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
        if (h->cl[i].s != PH_BAD && h->cl[i].hello && h->cl[i].id == id) return i;
    return -1;
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
        sendto(h->udp, (const char*)reply, (int)sizeof reply, 0, (const struct sockaddr*)from, (ph_socklen)sizeof *from);
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
        struct sockaddr_in to;
        memset(&to, 0, sizeof to);
        to.sin_family = AF_INET;
        memcpy(&to.sin_addr, h->cl[dst].udp_ip, 4);
        to.sin_port = htons(h->cl[dst].udp_port);
        sendto(h->udp, (const char*)pkt, (int)(n - 8), 0, (const struct sockaddr*)&to, (ph_socklen)sizeof to);
    }
    /* PUNCH and anything else: nothing to do */
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
    if (c->s == PH_BAD) return;
    for (int i = 0; i < PH_ROOMS; i++)
        if (h->rooms[i].used) ph_leave(h, &h->rooms[i], ci);
    if (c->hello) {
        h->players--;
        printf("[psnr_host] player %u (\"%s\") gone\n", c->id, c->name);
    }
    ph_close(c->s);
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
    int ci = -1;
    for (int i = 0; i < PH_CLIENTS && ci < 0; i++)
        if (h->cl[i].s == PH_BAD) ci = i;
    if (ci < 0 || ph_banned(h, ip, 0)) { ph_close(s); return; }
    ph_client* c = &h->cl[ci];
    memset(c, 0, sizeof *c);
    c->s = s;
    c->id = ++h->next_id;
    memcpy(c->ip, ip, 4);
    n = (ph_socklen)sizeof a;
    if (getsockname(s, (struct sockaddr*)&a, &n) == 0) memcpy(c->via, &a.sin_addr, 4);
    c->last_rx = ph_now();
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
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
    size_t off = 0;
    while (c->rx.n - off >= 3 && !c->closing) {
        const uint16_t n = psnr_get16(c->rx.b + off + 1);
        if (c->rx.n - off < 3u + n) break;
        ph_message_in(h, ci, c->rx.b[off], c->rx.b + off + 3, n);
        if (h->cl[ci].s == PH_BAD) return;   /* (dropped as a reconnect) */
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
    uint8_t pkt[2048];
    while (!h->stop) {
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
        const uint64_t now = ph_now();
        for (int i = 0; i < PH_CLIENTS; i++) {
            ph_client* c = &h->cl[i];
            if (c->s == PH_BAD) continue;
            if (c->closing == 2 || (c->closing == 1 && !c->tx.n) || now - c->last_rx > PH_TIMEOUT_MS) ph_drop(h, i);
        }
    }
    return 0;
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
    h->lis = h->udp = PH_BAD;
    for (int i = 0; i < PH_CLIENTS; i++) h->cl[i].s = PH_BAD;
    h->rng = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)h;
    {
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(cfg->bind_loopback ? INADDR_LOOPBACK : INADDR_ANY);
        /* With port 0 the TCP side picks; UDP then takes the same number (a few tries if it is busy). */
        for (int tries = 0; tries < 8 && h->udp == PH_BAD; tries++) {
            if (h->lis != PH_BAD) { ph_close(h->lis); h->lis = PH_BAD; }
            h->lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (h->lis == PH_BAD) PH_FAIL("no socket");
#ifndef _WIN32
            { int one = 1; setsockopt(h->lis, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one); }
#endif
            a.sin_port = htons(cfg->port);
            if (bind(h->lis, (struct sockaddr*)&a, sizeof a) != 0)
                PH_FAIL("port %u (TCP) is in use by another program", (unsigned)cfg->port);
            ph_socklen n = (ph_socklen)sizeof a;
            getsockname(h->lis, (struct sockaddr*)&a, &n);
            h->port = ntohs(a.sin_port);
            ph_sock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (u == PH_BAD) PH_FAIL("no socket");
            if (bind(u, (struct sockaddr*)&a, sizeof a) != 0) {
                ph_close(u);
                if (cfg->port) PH_FAIL("port %u (UDP) is in use by another program", (unsigned)cfg->port);
                continue;
            }
            h->udp = u;
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
    return h;
fail:
    if (h->lis != PH_BAD) ph_close(h->lis);
    if (h->udp != PH_BAD) ph_close(h->udp);
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

void psnr_host_stop(psnr_host* h)
{
    if (!h) return;
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
    ph_close(h->lis);
    ph_close(h->udp);
    printf("[psnr_host] stopped\n");
    free(h);
}
