/*
 * ps3recomp - sys_net module implementation
 *
 * The PS3 BSD socket API (libnet) passed through to host sockets: Winsock2 on
 * Windows, POSIX sockets elsewhere. It is OFF by default. ppu_sysprx.cpp's
 * offline model answers every libnet import unless PS3_NET_ONLINE is set, in
 * which case ps3_net_host_register() puts these handlers in front of it.
 *
 * Every pointer argument is a GUEST address, and everything the guest reads
 * back is big-endian: pollfd events, fd_set words, option values, socklen_t.
 * sockaddr_in needs no swap -- its port and address are already in network
 * order in guest memory, which is exactly what the host struct wants.
 *
 * errno is plain BSD (35 = EWOULDBLOCK). Winsock's WSAE* codes are BSD + 10000,
 * so Windows translates by subtraction; Linux numbers differ and get a table.
 */

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET host_socket_t;
    typedef WSAPOLLFD host_pollfd;
    #define HOST_INVALID_SOCKET INVALID_SOCKET
    #define HOST_SOCKET_ERROR   SOCKET_ERROR
    #define host_closesocket    closesocket
    #define host_poll           WSAPoll
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <poll.h>
    #include <time.h>
    #include <sys/time.h>
    typedef int host_socket_t;
    typedef struct pollfd host_pollfd;
    #define HOST_INVALID_SOCKET (-1)
    #define HOST_SOCKET_ERROR   (-1)
    #define host_closesocket    close
    #define host_poll           poll
#endif

#include "sysNet.h"
#include "ps3emu/ps3_knob.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../include/ps3emu/nid.h"
#include "np_psnr.h"   /* np_psnr_p2p_port, NAT traversal on the P2P socket */
#include "ps3emu/ps3_net.h"   /* ps3_net_resolve (PS3_NET_HOSTS), ps3_net_locked_offline */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

#define EA(p) ((uint32_t)(uintptr_t)(p))

/* PS3-only socket types and options */
#define SYS_NET_SOCK_DGRAM_P2P   6
#define SYS_NET_SOCK_STREAM_P2P  10
#define SYS_NET_SO_USECRYPTO     0x1101
#define SYS_NET_SO_USESIGNATURE  0x1102
#define SYS_NET_SO_TYPE          0x1008
#define SYS_NET_SO_REUSEPORT     0x0200
#define SYS_NET_TCP_NODELAY      1
#define SYS_NET_HOST_NOT_FOUND   1

/* ---------------------------------------------------------------------------
 * State
 * -----------------------------------------------------------------------*/

typedef struct {
    host_socket_t host_fd;
    int           in_use;
    int           nonblocking;
    int           p2p;      /* SOCK_DGRAM_P2P / SOCK_STREAM_P2P */
    int           stream;   /* SOCK_STREAM or SOCK_STREAM_P2P */
    uint16_t      vport;    /* the P2P vport it was bound to */
    int           listening;
    uint16_t      lo_port;  /* a bound DGRAM_P2P socket: its loopback port (see "P2P datagram vports") */
    int           has_peer; /* a connected DGRAM_P2P socket: where send goes */
    struct sockaddr_in peer;
    uint16_t      peer_vport;
} net_socket_slot;

static net_socket_slot s_sockets[SYS_NET_MAX_SOCKETS];
static int s_net_initialized = 0;

/* Guest scratch, allocated on first use through the allocator the runtime
 * handed ps3_net_host_register. */
static unsigned int (*s_alloc)(unsigned int, unsigned int) = NULL;
static uint32_t s_errno_ea = 0, s_h_errno_ea = 0, s_hostent_ea = 0, s_ntoa_ea = 0;
static int32_t  s_errno = 0;   /* ponytail: one errno for every thread, per-thread cells if a title races on it */

/* hostent scratch layout: struct at +0, h_aliases[] at +32, h_addr_list[] at
 * +40, the address at +48, the name at +64. */
#define HOSTENT_SCRATCH 320u

static int net_trace(void);   /* PS3_NET_TRACE; defined with the send/recv paths */
static void trace(const char* op, int32_t s, int n, const struct sockaddr_in* a);
static int host_recv_flags(int32_t flags);

static uint32_t scratch(uint32_t* ea, uint32_t size)
{
    if (!*ea && s_alloc) *ea = s_alloc(size, 16);
    return *ea;
}

/* A failing call returns SYS_NET_ERROR_BASE | errno (0x80010223 for
 * EWOULDBLOCK) and also leaves errno in the cell. Titles compare the return
 * value itself: Simpsons Arcade's connect wrapper waits on 0x80010224
 * (EINPROGRESS) and 0x80010238 (EISCONN), and a plain -1 read as a failure it
 * never retried. Anything that only tests for < 0 is unaffected. */
#define SYS_NET_RET_BASE 0x80010200u
static int32_t fail(int32_t err)
{
    s_errno = err;
    if (s_errno_ea) vm_write32(s_errno_ea, (uint32_t)err);
    return (int32_t)(SYS_NET_RET_BASE | (uint32_t)err);
}

static int32_t host_fail(void)
{
#ifdef _WIN32
    int e = WSAGetLastError();
    /* WSAE* is the BSD value + 10000; a nonblocking connect says WOULDBLOCK
     * where BSD says INPROGRESS, and the caller fixes that one up. */
    if (e >= 10000 && e < 10100) return fail(e - 10000);
    return fail(SYS_NET_EINVAL);
#else
    switch (errno) {
        case EBADF:         return fail(SYS_NET_EBADF);
        case ENOMEM:        return fail(SYS_NET_ENOMEM);
        case EINVAL:        return fail(SYS_NET_EINVAL);
        case EAGAIN:        return fail(SYS_NET_EWOULDBLOCK);
        case EINPROGRESS:   return fail(SYS_NET_EINPROGRESS);
        case EALREADY:      return fail(SYS_NET_EALREADY);
        case ENOTSOCK:      return fail(SYS_NET_ENOTSOCK);
        case EMSGSIZE:      return fail(SYS_NET_EMSGSIZE);
        case EADDRINUSE:    return fail(SYS_NET_EADDRINUSE);
        case EADDRNOTAVAIL: return fail(SYS_NET_EADDRNOTAVAIL);
        case ENETUNREACH:   return fail(SYS_NET_ENETUNREACH);
        case ECONNABORTED:  return fail(SYS_NET_ECONNABORTED);
        case ECONNRESET:    return fail(SYS_NET_ECONNRESET);
        case EISCONN:       return fail(SYS_NET_EISCONN);
        case ENOTCONN:      return fail(SYS_NET_ENOTCONN);
        case ETIMEDOUT:     return fail(SYS_NET_ETIMEDOUT);
        case ECONNREFUSED:  return fail(SYS_NET_ECONNREFUSED);
        case EHOSTUNREACH:  return fail(SYS_NET_EHOSTUNREACH);
        default:            return fail(SYS_NET_EINVAL);
    }
#endif
}

static void net_startup(void)
{
    if (s_net_initialized) return;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    for (int i = 0; i < SYS_NET_MAX_SOCKETS; i++) {
        s_sockets[i].host_fd = HOST_INVALID_SOCKET;
        s_sockets[i].in_use = 0;
    }
    s_net_initialized = 1;
}

/* fd 0 is never handed out: titles use 0 as "no socket" in their own tables. */
static int alloc_slot(host_socket_t fd)
{
    for (int i = 1; i < SYS_NET_MAX_SOCKETS; i++)
        if (!s_sockets[i].in_use) {
            s_sockets[i].host_fd = fd;
            s_sockets[i].in_use = 1;
            s_sockets[i].nonblocking = 0;
            s_sockets[i].p2p = 0;
            s_sockets[i].stream = 0;
            s_sockets[i].vport = 0;
            s_sockets[i].listening = 0;
            s_sockets[i].lo_port = 0;
            s_sockets[i].has_peer = 0;
            return i;
        }
    return -1;
}

static int valid_socket(int32_t s)
{
    return s > 0 && s < SYS_NET_MAX_SOCKETS && s_sockets[s].in_use;
}

static int read_sockaddr(uint32_t ea, struct sockaddr_in* out)
{
    const uint8_t* p = GUEST_PTR(ea, const uint8_t*);
    if (!p) return -1;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, p + 2, 2);   /* network order in both */
    memcpy(&out->sin_addr, p + 4, 4);
    return 0;
}

static void write_sockaddr(uint32_t ea, uint32_t len_ea, const struct sockaddr_in* in)
{
    if (ea) {
        uint8_t* p = GUEST_PTR(ea, uint8_t*);
        memset(p, 0, 16);
        p[0] = 16;
        p[1] = SYS_NET_AF_INET;
        memcpy(p + 2, &in->sin_port, 2);
        memcpy(p + 4, &in->sin_addr, 4);
    }
    if (len_ea) vm_write32(len_ea, 16);
}

static const char* ip_str(const struct in_addr* a, char* buf)
{
    const uint8_t* b = (const uint8_t*)a;
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

static void sleep_ms(int ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* MSG_DONTWAIT on a blocking socket. Winsock has no such flag, so both
 * platforms ask poll whether the call would block instead of passing it on. */
static int would_block(int32_t s, int32_t flags, short ev)
{
    if (!(flags & SYS_NET_MSG_DONTWAIT) || s_sockets[s].nonblocking) return 0;
    host_pollfd p;
    p.fd = s_sockets[s].host_fd;
    p.events = ev;
    p.revents = 0;
    return host_poll(&p, 1, 0) == 0;
}

static int set_nonblocking(host_socket_t fd, int on)
{
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

/* ---------------------------------------------------------------------------
 * Init / shutdown
 * -----------------------------------------------------------------------*/

int32_t sys_net_initialize_network_ex(void* param)
{
    (void)param;
    net_startup();
    return CELL_OK;
}

int32_t sys_net_finalize_network(void)
{
    for (int i = 1; i < SYS_NET_MAX_SOCKETS; i++)
        if (s_sockets[i].in_use) sys_net_bnet_close(i);   /* (a bound DGRAM_P2P one also lets go of the P2P port) */
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Sockets
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_socket(int32_t domain, int32_t type, int32_t protocol)
{
    net_startup();
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);
    if (domain != SYS_NET_AF_INET) return fail(SYS_NET_EINVAL);

    int host_type;
    switch (type) {
        case SYS_NET_SOCK_STREAM:
        case SYS_NET_SOCK_STREAM_P2P: host_type = SOCK_STREAM; break;
        case SYS_NET_SOCK_DGRAM:
        /* P2P sockets live on this instance's P2P port (see bind): every
         * STREAM_P2P socket shares it, and DGRAM_P2P sockets share it by vport
         * ("P2P datagram vports" below). */
        case SYS_NET_SOCK_DGRAM_P2P:  host_type = SOCK_DGRAM;  break;
        default: return fail(SYS_NET_EINVAL);
    }

    host_socket_t fd = socket(AF_INET, host_type, (type >= SYS_NET_SOCK_DGRAM_P2P) ? 0 : protocol);
    if (fd == HOST_INVALID_SOCKET) return host_fail();

    int slot = alloc_slot(fd);
    if (slot < 0) {
        host_closesocket(fd);
        return fail(SYS_NET_ENOMEM);
    }
    s_sockets[slot].p2p = (type == SYS_NET_SOCK_DGRAM_P2P || type == SYS_NET_SOCK_STREAM_P2P);
    s_sockets[slot].stream = (host_type == SOCK_STREAM);
    printf("[sys_net] socket(%d, %d, %d) -> %d\n", domain, type, protocol, slot);
    return slot;
}

/* Streams a peer opened to us through psnr's relay (np_psnr.h): each is a
 * connected host socket that the title's P2P listener hands out from accept,
 * and that poll/select report as a pending connection. The pump thread adds;
 * the title's thread takes. */
#define RELAYED_MAX 8
static struct { host_socket_t fd; uint8_t ip[4]; uint16_t port; } s_relayed[RELAYED_MAX];
static int s_nrelayed;
#ifdef _WIN32
static SRWLOCK s_relayed_lock = SRWLOCK_INIT;
#  define RELAYED_LOCK()   AcquireSRWLockExclusive(&s_relayed_lock)
#  define RELAYED_UNLOCK() ReleaseSRWLockExclusive(&s_relayed_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_relayed_lock = PTHREAD_MUTEX_INITIALIZER;
#  define RELAYED_LOCK()   pthread_mutex_lock(&s_relayed_lock)
#  define RELAYED_UNLOCK() pthread_mutex_unlock(&s_relayed_lock)
#endif

static void relayed_stream(int64_t fd, const uint8_t ip[4], uint16_t port)
{
    RELAYED_LOCK();
    if (s_nrelayed < RELAYED_MAX) {
        s_relayed[s_nrelayed].fd = (host_socket_t)fd;
        memcpy(s_relayed[s_nrelayed].ip, ip, 4);
        s_relayed[s_nrelayed].port = port;
        s_nrelayed++;
        fd = -1;
    }
    RELAYED_UNLOCK();
    if (fd >= 0) host_closesocket((host_socket_t)fd);   /* nobody will accept that many */
}

/* A P2P stream listener with a relayed connection waiting. */
static int relayed_pending(int32_t s)
{
    int n;
    if (!s_sockets[s].p2p || !s_sockets[s].stream || !s_sockets[s].listening) return 0;
    RELAYED_LOCK();
    n = s_nrelayed;
    RELAYED_UNLOCK();
    return n > 0;
}

/* ---- P2P datagram vports ----
 *
 * A console carries every DGRAM_P2P socket over its one P2P port and tells
 * them apart by vport (sockaddr_in_p2p's sin_vport, offset 8). Demon's Souls
 * opens two for a summon; with a host socket each on the P2P port the second
 * bind failed (the port was taken) and the title left the room it had made.
 *
 * Here the P2P port is one host UDP socket per instance, opened by the first
 * DGRAM_P2P bind, and a thread that reads it. Datagrams go out with a vport
 * header, and the thread hands each one that arrives to the socket bound to its
 * destination vport. A DGRAM_P2P socket's own host fd is bound to loopback, and
 * only that thread sends to it, so poll, select, blocking reads, timeouts and
 * MSG_PEEK work on it unchanged.
 *   - On the wire: "VP01", destination vport, source vport (big-endian u16),
 *     then the payload. Both peers need this runtime. A datagram without the
 *     header goes to the first DGRAM_P2P socket bound, as before.
 *   - Loopback hand-off: the sender's IPv4 address (4 bytes) and port (2,
 *     network order), its vport (2, big-endian; 0 when it sent no header),
 *     then the payload.
 * np_psnr sends its probes and punches from the P2P port too (np_psnr.h), so
 * they leave through the same router mapping as the title's traffic, and the
 * thread passes everything that arrives through np_psnr_p2p_filter first. */
#define P2P_MAGIC     "VP01"
#define P2P_HDR       8     /* on the wire */
#define P2P_FWD       8     /* on the loopback hand-off */
#define P2P_MAX_DGRAM 65507

#ifndef SIO_UDP_CONNRESET
#  define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)   /* mstcpip.h */
#endif

static host_socket_t s_p2p_fd = HOST_INVALID_SOCKET;   /* the instance's P2P port */
static uint16_t      s_p2p_bound_port;                  /* its port, host order */
static volatile int  s_p2p_gen;                         /* bumped when it closes: its thread stops */
static int           s_p2p_dgram = -1;                  /* the first DGRAM_P2P bound: headerless datagrams */
#ifdef _WIN32
static SRWLOCK s_p2p_lock = SRWLOCK_INIT;
#  define P2P_LOCK()   AcquireSRWLockExclusive(&s_p2p_lock)
#  define P2P_UNLOCK() ReleaseSRWLockExclusive(&s_p2p_lock)
#else
static pthread_mutex_t s_p2p_lock = PTHREAD_MUTEX_INITIALIZER;
#  define P2P_LOCK()   pthread_mutex_lock(&s_p2p_lock)
#  define P2P_UNLOCK() pthread_mutex_unlock(&s_p2p_lock)
#endif

static int p2p_dgram_bound(int s)
{
    return s_sockets[s].in_use && s_sockets[s].p2p && !s_sockets[s].stream && s_sockets[s].lo_port;
}

static int p2p_send(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    struct sockaddr_in a;
    if (s_p2p_fd == HOST_INVALID_SOCKET || ps3_net_locked_offline()) return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, ip, 4);
    a.sin_port = htons(port);
    return (int)sendto(s_p2p_fd, (const char*)buf, (int)len, 0, (struct sockaddr*)&a, sizeof(a));
}

/* The socket a datagram for `vport` goes to: the one bound to it, else the
 * first bound. Callers hold P2P_LOCK. */
static int p2p_target(uint16_t vport)
{
    if (vport)
        for (int i = 1; i < SYS_NET_MAX_SOCKETS; i++)
            if (p2p_dgram_bound(i) && s_sockets[i].vport == vport) return i;
    return s_p2p_dgram;
}

static void p2p_rx(host_socket_t fd, int gen)
{
    /* Received at +P2P_FWD, so the hand-off header fits in front of the
     * payload whether or not the datagram had a vport header. */
    uint8_t* buf = (uint8_t*)malloc(P2P_FWD + P2P_MAX_DGRAM);
    if (!buf) return;
    while (s_p2p_gen == gen) {
        host_pollfd p;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (host_poll(&p, 1, 250) <= 0) continue;   /* (250 ms: how soon a closed port's thread notices) */
        if (s_p2p_gen != gen) break;
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        int n = recvfrom(fd, (char*)buf + P2P_FWD, P2P_MAX_DGRAM, 0, (struct sockaddr*)&a, &alen);
        if (n == HOST_SOCKET_ERROR || n <= 0) continue;
        uint32_t un = (uint32_t)n;
        uint16_t port = ntohs(a.sin_port);
        if (np_psnr_p2p_filter(buf + P2P_FWD, &un, (uint8_t*)&a.sin_addr, &port)) continue;   /* psnr's own */
        uint8_t* pl = buf + P2P_FWD;
        uint16_t dst = 0, src = 0;
        if (un >= P2P_HDR && !memcmp(pl, P2P_MAGIC, 4)) {
            dst = (uint16_t)(pl[4] << 8 | pl[5]);
            src = (uint16_t)(pl[6] << 8 | pl[7]);
            pl += P2P_HDR;
            un -= P2P_HDR;
        }
        uint8_t* f = pl - P2P_FWD;
        uint16_t port_be = htons(port);
        memcpy(f, &a.sin_addr, 4);
        memcpy(f + 4, &port_be, 2);
        f[6] = (uint8_t)(src >> 8);
        f[7] = (uint8_t)src;
        P2P_LOCK();
        int t = p2p_target(dst);
        uint16_t lo = t > 0 ? s_sockets[t].lo_port : 0;
        P2P_UNLOCK();
        if (net_trace()) {
            char ip[16];
            printf("[sys_net] p2p in %s:%u vport %u -> %u: %u bytes to socket %d\n",
                   ip_str(&a.sin_addr, ip), port, src, dst, un, lo ? t : -1);
        }
        if (!lo) continue;   /* no socket for it: dropped, as a console drops one for an unbound vport */
        struct sockaddr_in to;
        memset(&to, 0, sizeof(to));
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(lo);
        sendto(fd, (const char*)f, (int)(un + P2P_FWD), 0, (struct sockaddr*)&to, sizeof(to));
    }
    free(buf);
}

#ifdef _WIN32
static DWORD WINAPI p2p_rx_main(LPVOID u)
{
    int gen = (int)(intptr_t)u;
    P2P_LOCK();
    host_socket_t fd = s_p2p_gen == gen ? s_p2p_fd : HOST_INVALID_SOCKET;
    P2P_UNLOCK();
    if (fd != HOST_INVALID_SOCKET) p2p_rx(fd, gen);
    return 0;
}
#else
static void* p2p_rx_main(void* u)
{
    int gen = (int)(intptr_t)u;
    P2P_LOCK();
    host_socket_t fd = s_p2p_gen == gen ? s_p2p_fd : HOST_INVALID_SOCKET;
    P2P_UNLOCK();
    if (fd != HOST_INVALID_SOCKET) p2p_rx(fd, gen);
    return NULL;
}
#endif

/* Opens the P2P port and its thread, once. Callers hold P2P_LOCK. */
static int32_t p2p_open_port(void)
{
    if (s_p2p_fd != HOST_INVALID_SOCKET) return 0;
    host_socket_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == HOST_INVALID_SOCKET) return host_fail();
#ifdef _WIN32
    /* Without this, the ICMP "port unreachable" a peer that left sends back
     * fails the next recvfrom on the whole port (WSAECONNRESET). */
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(fd, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &got, NULL, NULL);
#endif
    /* On every interface. Titles bind the console's own IP (what cellNetCtl
     * reports), and Windows then refuses to send from that socket to a peer
     * on 127.0.0.1 (WSAEADDRNOTAVAIL) -- Simpsons Arcade's every packet failed
     * that way. The port is what peers use. */
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(np_psnr_p2p_port());
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) == HOST_SOCKET_ERROR) {
        int32_t r = host_fail();
        host_closesocket(fd);
        return r;
    }
    s_p2p_fd = fd;
    s_p2p_bound_port = np_psnr_p2p_port();
    int gen = s_p2p_gen;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, p2p_rx_main, (LPVOID)(intptr_t)gen, 0, NULL);
    if (th) CloseHandle(th);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, p2p_rx_main, (void*)(intptr_t)gen) == 0) pthread_detach(th);
#endif
    np_psnr_set_p2p_sender(p2p_send);
    return 0;
}

/* A DGRAM_P2P bind: the socket takes `vport` on the P2P port. */
static int32_t p2p_bind_dgram(int32_t s, uint16_t vport)
{
    struct sockaddr_in lo;
    socklen_t len = sizeof(lo);
    memset(&lo, 0, sizeof(lo));
    lo.sin_family = AF_INET;
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_sockets[s].host_fd, (struct sockaddr*)&lo, sizeof(lo)) == HOST_SOCKET_ERROR ||
        getsockname(s_sockets[s].host_fd, (struct sockaddr*)&lo, &len) == HOST_SOCKET_ERROR)
        return host_fail();
    P2P_LOCK();
    int32_t r = p2p_open_port();
    if (r == 0) {
        s_sockets[s].vport = vport;
        s_sockets[s].lo_port = ntohs(lo.sin_port);
        if (s_p2p_dgram < 0) s_p2p_dgram = s;
    }
    P2P_UNLOCK();
    if (r == 0) printf("[sys_net] bind(%d, 0.0.0.0:%u, p2p vport %u)\n", s, s_p2p_bound_port, vport);
    return r;
}

/* A DGRAM_P2P send: the vport header in front, out of the P2P port (or
 * psnr's relay). Returns the payload's length or a host error. */
static int32_t p2p_send_dgram(int32_t s, const void* buf, uint32_t len,
                              const struct sockaddr_in* to, uint16_t to_vport)
{
    uint8_t small[1536];
    uint8_t* f = (len + P2P_HDR <= sizeof(small)) ? small : (uint8_t*)malloc(len + P2P_HDR);
    if (!f) return fail(SYS_NET_ENOMEM);
    memcpy(f, P2P_MAGIC, 4);
    f[4] = (uint8_t)(to_vport >> 8);
    f[5] = (uint8_t)to_vport;
    f[6] = (uint8_t)(s_sockets[s].vport >> 8);
    f[7] = (uint8_t)s_sockets[s].vport;
    memcpy(f + P2P_HDR, buf, len);
    int n;
    if (np_psnr_p2p_route(f, len + P2P_HDR, (const uint8_t*)&to->sin_addr, ntohs(to->sin_port)))
        n = (int)(len + P2P_HDR);   /* went through psnr's relay */
    else
        n = sendto(s_p2p_fd, (const char*)f, (int)(len + P2P_HDR), 0, (const struct sockaddr*)to, sizeof(*to));
    if (net_trace()) trace("sendto", s, n, to);
    if (f != small) free(f);
    if (n == HOST_SOCKET_ERROR) return host_fail();
    return (int32_t)len;
}

/* A DGRAM_P2P receive: one hand-off from the thread, its header taken off.
 * `from_ea`/`fromlen_ea` may be 0 (recv). */
static int32_t p2p_recv_dgram(int32_t s, uint32_t buf_ea, uint32_t len, int32_t flags,
                              uint32_t from_ea, uint32_t fromlen_ea)
{
    uint8_t* tmp = (uint8_t*)malloc(len + P2P_FWD);
    if (!tmp) return fail(SYS_NET_ENOMEM);
    int n;
    for (;;) {
        if (would_block(s, flags, POLLIN)) { free(tmp); return fail(SYS_NET_EWOULDBLOCK); }
        struct sockaddr_in a;
        socklen_t alen = sizeof(a);
        n = recvfrom(s_sockets[s].host_fd, (char*)tmp, (int)(len + P2P_FWD), host_recv_flags(flags),
                     (struct sockaddr*)&a, &alen);
        if (n == HOST_SOCKET_ERROR) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEMSGSIZE) n = (int)(len + P2P_FWD);   /* truncated, as on a console */
            else
#endif
            { free(tmp); return host_fail(); }
        }
        /* Only the thread sends here (it sends from the P2P port); anything
         * else on loopback is dropped. */
        if (n >= P2P_FWD && a.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
            ntohs(a.sin_port) == s_p2p_bound_port)
            break;
        if (flags & SYS_NET_MSG_PEEK)   /* take the stray one off, or a peek sees it forever */
            recv(s_sockets[s].host_fd, (char*)tmp, (int)(len + P2P_FWD), 0);
    }
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    from.sin_family = AF_INET;
    memcpy(&from.sin_addr, tmp, 4);
    memcpy(&from.sin_port, tmp + 4, 2);
    uint16_t src = (uint16_t)(tmp[6] << 8 | tmp[7]);
    n -= P2P_FWD;
    memcpy(GUEST_PTR(buf_ea, char*), tmp + P2P_FWD, (size_t)n);
    free(tmp);
    if (net_trace()) trace("recvfrom", s, n, &from);
    write_sockaddr(from_ea, fromlen_ea, &from);
    if (from_ea) vm_write16(from_ea + 8, src ? src : s_sockets[s].vport);
    return n;
}

int32_t sys_net_bnet_close(int32_t s)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (p2p_dgram_bound(s)) {
        P2P_LOCK();
        s_sockets[s].lo_port = 0;
        if (s == s_p2p_dgram) {
            s_p2p_dgram = -1;
            for (int i = 1; i < SYS_NET_MAX_SOCKETS && s_p2p_dgram < 0; i++)
                if (p2p_dgram_bound(i)) s_p2p_dgram = i;
        }
        if (s_p2p_dgram < 0) {   /* the last one: let go of the P2P port */
            s_p2p_gen++;
            host_closesocket(s_p2p_fd);
            s_p2p_fd = HOST_INVALID_SOCKET;
            np_psnr_set_p2p_sender(NULL);
        }
        P2P_UNLOCK();
    }
    host_closesocket(s_sockets[s].host_fd);
    s_sockets[s].in_use = 0;
    return 0;
}

int32_t sys_net_bnet_bind(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)addrlen;
    struct sockaddr_in a;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (read_sockaddr(EA(addr), &a)) return fail(SYS_NET_EINVAL);
    if (s_sockets[s].p2p && !s_sockets[s].stream) return p2p_bind_dgram(s, vm_read16(EA(addr) + 8));
    if (s_sockets[s].p2p) {
        /* P2P sockets live on this instance's P2P port (PS3_NET_P2P_PORT,
         * default 3658), whatever port the title names: that is the port psnr
         * hands to peers, and peers send straight to it. sockaddr_in_p2p keeps
         * the vport at offset 8. On every interface, as p2p_open_port says. */
        s_sockets[s].vport = vm_read16(EA(addr) + 8);
        a.sin_port = htons(np_psnr_p2p_port());
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        /* Every P2P stream socket shares that port, the listener and the ones
         * that connect out, as on a console. A connection's source port is
         * then the P2P port, and titles check it: Simpsons Arcade drops the
         * host's game setup when it arrives from any port but the one
         * signaling reported for the host. */
        int on = 1;
        setsockopt(s_sockets[s].host_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(s_sockets[s].host_fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&on, sizeof(on));
#endif
    }
    if (bind(s_sockets[s].host_fd, (struct sockaddr*)&a, sizeof(a)) == HOST_SOCKET_ERROR) {
        /* Last resort for a P2P stream socket that still can't share the
         * port: an ephemeral one. It connects, but titles that check the
         * source port will ignore what it sends. */
        if (!(s_sockets[s].p2p && s_sockets[s].stream)) return host_fail();
        a.sin_port = 0;
        if (bind(s_sockets[s].host_fd, (struct sockaddr*)&a, sizeof(a)) == HOST_SOCKET_ERROR)
            return host_fail();
    }
    char ip[16];
    printf("[sys_net] bind(%d, %s:%u%s)\n", s, ip_str(&a.sin_addr, ip), ntohs(a.sin_port),
           s_sockets[s].p2p ? ", p2p" : "");
    return 0;
}

int32_t sys_net_bnet_listen(int32_t s, int32_t backlog)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (listen(s_sockets[s].host_fd, backlog) == HOST_SOCKET_ERROR) return host_fail();
    s_sockets[s].listening = 1;
    if (s_sockets[s].p2p) np_psnr_set_stream_sink(relayed_stream);
    printf("[sys_net] listen(%d)\n", s);
    return 0;
}

int32_t sys_net_bnet_accept(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);

    host_socket_t fd = HOST_INVALID_SOCKET;
    if (s_sockets[s].p2p && s_sockets[s].stream && !s_sockets[s].nonblocking) {
        /* A blocking accept on the P2P listener: a stream through the relay
         * never reaches the host listener, so wait for either. Simpsons
         * Arcade's joiner sits here on a thread for the host's game setup. */
        while (!relayed_pending(s)) {
            host_pollfd p;
            p.fd = s_sockets[s].host_fd;
            p.events = POLLIN;
            p.revents = 0;
            if (host_poll(&p, 1, 100) != 0) break;   /* a direct connection, or an error */
            if (!valid_socket(s)) return fail(SYS_NET_EBADF);
        }
    }
    if (relayed_pending(s)) {
        /* A peer's stream through the relay: to the title it is the peer
         * connecting from its P2P port. */
        RELAYED_LOCK();
        if (s_nrelayed) {
            fd = s_relayed[0].fd;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            memcpy(&a.sin_addr, s_relayed[0].ip, 4);
            a.sin_port = htons(s_relayed[0].port);
            memmove(s_relayed, s_relayed + 1, (size_t)(--s_nrelayed) * sizeof(s_relayed[0]));
        }
        RELAYED_UNLOCK();
        /* A directly accepted socket inherits the listener's blocking mode,
         * so this one must too. Simpsons Arcade's joiner reads its game
         * setup and then once more, expecting EWOULDBLOCK; on a blocking
         * socket that read hung until the stream closed, the joiner went
         * quiet for 4 s, and the host kicked it. */
        if (fd != HOST_INVALID_SOCKET) set_nonblocking(fd, s_sockets[s].nonblocking);
    }
    if (fd == HOST_INVALID_SOCKET) {
        fd = accept(s_sockets[s].host_fd, (struct sockaddr*)&a, &len);
        if (fd == HOST_INVALID_SOCKET) return host_fail();
    }

    int slot = alloc_slot(fd);
    if (slot < 0) {
        host_closesocket(fd);
        return fail(SYS_NET_ENOMEM);
    }
    s_sockets[slot].p2p = s_sockets[s].p2p;
    s_sockets[slot].stream = 1;
    if (net_trace()) printf("[sys_net] accept(%d) -> %d\n", s, slot);
    s_sockets[slot].vport = s_sockets[s].vport;
    write_sockaddr(EA(addr), EA(addrlen), &a);
    /* The mirror of connect: titles name a stream peer by the P2P port in
     * sin_vport, and the peer connects from its P2P port (see bind). Simpsons
     * Arcade keys the host's connection on it; left at 0, the host's game
     * setup matched no member and was dropped. */
    if (addr && s_sockets[s].p2p) vm_write16(EA(addr) + 8, ntohs(a.sin_port));
    return slot;
}

int32_t sys_net_bnet_connect(int32_t s, const sys_net_sockaddr* addr, uint32_t addrlen)
{
    (void)addrlen;
    struct sockaddr_in a;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);
    if (read_sockaddr(EA(addr), &a)) return fail(SYS_NET_EINVAL);
    if (p2p_dgram_bound(s)) {
        /* A DGRAM_P2P "connect" only sets where send goes: its host socket is
         * the loopback hand-off, which a host connect would cut off. */
        s_sockets[s].peer = a;
        s_sockets[s].peer_vport = vm_read16(EA(addr) + 8);
        s_sockets[s].has_peer = 1;
        return 0;
    }
    if (s_sockets[s].p2p && s_sockets[s].stream && vm_read16(EA(addr) + 8)) {
        /* A stream P2P endpoint is (address, vport): titles put their own
         * constant in sin_port and the peer's P2P port -- what signaling
         * reported, 3658 on a console -- in sin_vport. The peer's listener is
         * on that port here (see bind), so that is where to connect. Seen in
         * Simpsons Arcade: port 4099, vport 3659. */
        a.sin_port = htons(vm_read16(EA(addr) + 8));
    }

    char ip[16];
    printf("[sys_net] connect(%d, %s:%u)\n", s, ip_str(&a.sin_addr, ip), ntohs(a.sin_port));
    if (s_sockets[s].p2p && s_sockets[s].stream) {
        /* A peer that can't take a direct connection (behind its router):
         * the stream goes through psnr's relay, and its socket replaces
         * this one. */
        int64_t fd = np_psnr_stream_connect((const uint8_t*)&a.sin_addr, ntohs(a.sin_port));
        if (fd == -1) return fail(SYS_NET_ECONNREFUSED);
        if (fd >= 0) {
            host_closesocket(s_sockets[s].host_fd);
            s_sockets[s].host_fd = (host_socket_t)fd;
            set_nonblocking(s_sockets[s].host_fd, s_sockets[s].nonblocking);   /* the mode the title set */
            return 0;
        }
    }
    if (connect(s_sockets[s].host_fd, (struct sockaddr*)&a, sizeof(a)) == HOST_SOCKET_ERROR) {
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) return fail(SYS_NET_EINPROGRESS);
#endif
        int32_t r = host_fail();
        if (net_trace()) printf("[sys_net] connect(%d) failed, errno %d\n", s, s_errno);
        return r;
    }
    return 0;
}

int32_t sys_net_bnet_shutdown(int32_t s, int32_t how)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (shutdown(s_sockets[s].host_fd, how) == HOST_SOCKET_ERROR) return host_fail();
    return 0;
}

/* ---------------------------------------------------------------------------
 * Data transfer
 * -----------------------------------------------------------------------*/

static int host_recv_flags(int32_t flags)
{
    int f = 0;
    if (flags & SYS_NET_MSG_PEEK)    f |= MSG_PEEK;
    if (flags & SYS_NET_MSG_WAITALL) f |= MSG_WAITALL;
    return f;
}

/* PS3_NET_TRACE=1: log every packet sent and every one received (not the
 * empty polls), with its peer. How a title's own session handshake is seen. */
static int net_trace(void)
{
    static int t = -1;
    PS3_KNOB(t, getenv("PS3_NET_TRACE") ? 1 : 0);
    return t;
}

static void trace(const char* op, int32_t s, int n, const struct sockaddr_in* a)
{
#ifdef _WIN32
    int err = n < 0 ? WSAGetLastError() : 0;
#else
    int err = n < 0 ? errno : 0;
#endif
    char ip[16] = "-";
    if (a) ip_str(&a->sin_addr, ip);
    /* Wall-clock milliseconds, to line two machines' traces up. */
    unsigned hh, mm, ss, ms;
#ifdef _WIN32
    SYSTEMTIME t;
    GetLocalTime(&t);
    hh = t.wHour; mm = t.wMinute; ss = t.wSecond; ms = t.wMilliseconds;
#else
    struct timeval tv;
    struct tm tm;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    hh = (unsigned)tm.tm_hour; mm = (unsigned)tm.tm_min; ss = (unsigned)tm.tm_sec;
    ms = (unsigned)(tv.tv_usec / 1000);
#endif
    printf("[sys_net %02u:%02u:%02u.%03u] %s(%d) %s:%u -> %d (host error %d)\n", hh, mm, ss, ms,
           op, s, ip, a ? ntohs(a->sin_port) : 0, n, err);
}

int32_t sys_net_bnet_send(int32_t s, const void* buf, uint32_t len, int32_t flags)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);
    if (p2p_dgram_bound(s)) {
        if (!s_sockets[s].has_peer) return fail(SYS_NET_ENOTCONN);
        return p2p_send_dgram(s, GUEST_PTR(EA(buf), const char*), len, &s_sockets[s].peer, s_sockets[s].peer_vport);
    }
    if (would_block(s, flags, POLLOUT)) return fail(SYS_NET_EWOULDBLOCK);
    int n = send(s_sockets[s].host_fd, GUEST_PTR(EA(buf), const char*), (int)len, 0);
    if (net_trace()) trace("send", s, n, NULL);
    return n == HOST_SOCKET_ERROR ? host_fail() : n;
}

int32_t sys_net_bnet_sendto(int32_t s, const void* buf, uint32_t len, int32_t flags,
                            const sys_net_sockaddr* to, uint32_t tolen)
{
    (void)tolen;
    struct sockaddr_in a;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);
    if (!to) return sys_net_bnet_send(s, buf, len, flags);   /* connected socket */
    if (read_sockaddr(EA(to), &a)) return fail(SYS_NET_EINVAL);
    if (p2p_dgram_bound(s))
        return p2p_send_dgram(s, GUEST_PTR(EA(buf), const char*), len, &a, vm_read16(EA(to) + 8));
    if (s_sockets[s].p2p && np_psnr_p2p_route(GUEST_PTR(EA(buf), const char*), len,
                                             (const uint8_t*)&a.sin_addr, ntohs(a.sin_port)))
        return (int32_t)len;   /* went through psnr's relay */
    if (would_block(s, flags, POLLOUT)) return fail(SYS_NET_EWOULDBLOCK);
    int n = sendto(s_sockets[s].host_fd, GUEST_PTR(EA(buf), const char*), (int)len, 0,
                   (struct sockaddr*)&a, sizeof(a));
    if (net_trace()) trace("sendto", s, n, &a);
    return n == HOST_SOCKET_ERROR ? host_fail() : n;
}

int32_t sys_net_bnet_recv(int32_t s, void* buf, uint32_t len, int32_t flags)
{
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);
    if (p2p_dgram_bound(s)) return p2p_recv_dgram(s, EA(buf), len, flags, 0, 0);
    if (would_block(s, flags, POLLIN)) return fail(SYS_NET_EWOULDBLOCK);
    int n = recv(s_sockets[s].host_fd, GUEST_PTR(EA(buf), char*), (int)len, host_recv_flags(flags));
    if (net_trace() && n != HOST_SOCKET_ERROR) trace("recv", s, n, NULL);
    return n == HOST_SOCKET_ERROR ? host_fail() : n;
}

int32_t sys_net_bnet_recvfrom(int32_t s, void* buf, uint32_t len, int32_t flags,
                              sys_net_sockaddr* from, uint32_t* fromlen)
{
    struct sockaddr_in a;
    socklen_t alen;
    int n;
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (ps3_net_locked_offline()) return fail(SYS_NET_ENETDOWN);
    if (p2p_dgram_bound(s)) return p2p_recv_dgram(s, EA(buf), len, flags, EA(from), EA(fromlen));

    /* psnr's own packets (probe replies, peers' punches) are consumed here;
     * the title only ever sees its own traffic. */
    do {
        if (would_block(s, flags, POLLIN)) return fail(SYS_NET_EWOULDBLOCK);
        memset(&a, 0, sizeof(a));
        alen = sizeof(a);
        n = recvfrom(s_sockets[s].host_fd, GUEST_PTR(EA(buf), char*), (int)len,
                     host_recv_flags(flags), (struct sockaddr*)&a, &alen);
        if (n == HOST_SOCKET_ERROR) return host_fail();
        if (s_sockets[s].p2p && n > 0) {
            uint32_t un = (uint32_t)n;
            uint16_t port = ntohs(a.sin_port);
            if (np_psnr_p2p_filter(GUEST_PTR(EA(buf), char*), &un, (uint8_t*)&a.sin_addr, &port))
                continue;                     /* psnr's own packet */
            n = (int)un;                      /* possibly a peer's, unwrapped */
            a.sin_port = htons(port);
        }
        break;
    } while (1);
    if (net_trace()) trace("recvfrom", s, n, &a);
    write_sockaddr(EA(from), EA(fromlen), &a);
    if (from && s_sockets[s].p2p) vm_write16(EA(from) + 8, s_sockets[s].vport);
    return n;
}

/* ---------------------------------------------------------------------------
 * Socket options
 * -----------------------------------------------------------------------*/

/* Timeouts arrive as a PS3 timeval {s64 sec, s64 usec}. */
static int set_timeout(host_socket_t fd, int name, uint32_t val_ea, uint32_t len)
{
    uint64_t us = (len >= 16) ? vm_read64(val_ea) * 1000000ull + vm_read64(val_ea + 8)
                              : (uint64_t)vm_read32(val_ea) * 1000ull;
#ifdef _WIN32
    DWORD ms = (DWORD)(us / 1000);
    return setsockopt(fd, SOL_SOCKET, name, (const char*)&ms, sizeof(ms));
#else
    struct timeval tv = { (time_t)(us / 1000000), (suseconds_t)(us % 1000000) };
    return setsockopt(fd, SOL_SOCKET, name, &tv, sizeof(tv));
#endif
}

int32_t sys_net_bnet_setsockopt(int32_t s, int32_t level, int32_t optname,
                                const void* optval, uint32_t optlen)
{
    uint32_t v_ea = EA(optval);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    host_socket_t fd = s_sockets[s].host_fd;
    int val = (v_ea && optlen >= 4) ? (int)vm_read32(v_ea) : 0;
    int ret = 0;

    if (level == SYS_NET_SOL_SOCKET) {
        switch (optname) {
            case SYS_NET_SO_NBIO:
                s_sockets[s].nonblocking = val != 0;
                ret = set_nonblocking(fd, val);
                break;
            case SYS_NET_SO_REUSEADDR:
            case SYS_NET_SO_REUSEPORT:
                ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_KEEPALIVE:
                ret = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_BROADCAST:
                ret = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_SNDBUF:
                ret = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_RCVBUF:
                ret = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&val, sizeof(val)); break;
            case SYS_NET_SO_SNDTIMEO: ret = set_timeout(fd, SO_SNDTIMEO, v_ea, optlen); break;
            case SYS_NET_SO_RCVTIMEO: ret = set_timeout(fd, SO_RCVTIMEO, v_ea, optlen); break;
            case SYS_NET_SO_LINGER: {
                struct linger l;
                l.l_onoff  = (unsigned short)vm_read32(v_ea);
                l.l_linger = (unsigned short)vm_read32(v_ea + 4);
                ret = setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char*)&l, sizeof(l));
                break;
            }
            /* NP signaling's packet crypto/signature: meaningless off-console. */
            case SYS_NET_SO_USECRYPTO:
            case SYS_NET_SO_USESIGNATURE:
                break;
            default:
                printf("[sys_net] setsockopt(%d, SOL_SOCKET, 0x%X) ignored\n", s, (unsigned)optname);
                break;
        }
    } else if (level == SYS_NET_IPPROTO_TCP && optname == SYS_NET_TCP_NODELAY) {
        ret = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&val, sizeof(val));
    } else {
        /* ponytail: IP-level options (TTL, TOS, multicast) are accepted and
         * dropped; map them when a title's traffic depends on one. */
        printf("[sys_net] setsockopt(%d, level %d, 0x%X) ignored\n", s, level, (unsigned)optname);
    }
    return ret == HOST_SOCKET_ERROR ? host_fail() : 0;
}

int32_t sys_net_bnet_getsockopt(int32_t s, int32_t level, int32_t optname,
                                void* optval, uint32_t* optlen)
{
    uint32_t v_ea = EA(optval), len_ea = EA(optlen);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    host_socket_t fd = s_sockets[s].host_fd;
    int val = 0;
    socklen_t hl = sizeof(val);

    if (level == SYS_NET_SOL_SOCKET) {
        switch (optname) {
            case SYS_NET_SO_NBIO: val = s_sockets[s].nonblocking; break;
            case SYS_NET_SO_ERROR:
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&val, &hl) == HOST_SOCKET_ERROR)
                    return host_fail();
#ifdef _WIN32
                if (val >= 10000 && val < 10100) val -= 10000;
#else
                if (val == ECONNREFUSED) val = SYS_NET_ECONNREFUSED;
                else if (val == ETIMEDOUT) val = SYS_NET_ETIMEDOUT;
                else if (val == ECONNRESET) val = SYS_NET_ECONNRESET;
                else if (val == EHOSTUNREACH) val = SYS_NET_EHOSTUNREACH;
#endif
                break;
            case SYS_NET_SO_TYPE:
                if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char*)&val, &hl) == HOST_SOCKET_ERROR)
                    return host_fail();
                val = (val == SOCK_STREAM) ? SYS_NET_SOCK_STREAM : SYS_NET_SOCK_DGRAM;
                break;
            case SYS_NET_SO_SNDBUF:
            case SYS_NET_SO_RCVBUF:
                if (getsockopt(fd, SOL_SOCKET, optname == SYS_NET_SO_SNDBUF ? SO_SNDBUF : SO_RCVBUF,
                               (char*)&val, &hl) == HOST_SOCKET_ERROR)
                    return host_fail();
                break;
            default: break;   /* everything else reads as 0 */
        }
    }
    if (v_ea) vm_write32(v_ea, (uint32_t)val);
    if (len_ea) vm_write32(len_ea, 4);
    return 0;
}

int32_t sys_net_bnet_getsockname(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (p2p_dgram_bound(s)) {   /* the P2P port and its vport, not the loopback hand-off */
        memset(&a, 0, sizeof(a));
        a.sin_port = htons(s_p2p_bound_port);
        write_sockaddr(EA(addr), EA(addrlen), &a);
        if (EA(addr)) vm_write16(EA(addr) + 8, s_sockets[s].vport);
        return 0;
    }
    if (getsockname(s_sockets[s].host_fd, (struct sockaddr*)&a, &len) == HOST_SOCKET_ERROR)
        return host_fail();
    write_sockaddr(EA(addr), EA(addrlen), &a);
    return 0;
}

int32_t sys_net_bnet_getpeername(int32_t s, sys_net_sockaddr* addr, uint32_t* addrlen)
{
    struct sockaddr_in a;
    socklen_t len = sizeof(a);
    if (!valid_socket(s)) return fail(SYS_NET_EBADF);
    if (getpeername(s_sockets[s].host_fd, (struct sockaddr*)&a, &len) == HOST_SOCKET_ERROR)
        return host_fail();
    write_sockaddr(EA(addr), EA(addrlen), &a);
    return 0;
}

/* ---------------------------------------------------------------------------
 * poll / select
 *
 * Both go through one host poll. Guest pollfd is {s32 fd, s16 events,
 * s16 revents}; guest fd_set is 1024 bits in big-endian u32 words, fd n at
 * bit (n % 32) of word (n / 32) -- the BSD fd_mask layout.
 * -----------------------------------------------------------------------*/

#define NET_POLL_MAX 64

/* Run host poll over n guest fds. Invalid fds come back POLLNVAL without
 * reaching the host (WSAPoll rejects the whole call over one bad handle). */
static int run_poll(const int32_t* gfd, const short* gev, short* grev, int n, int timeout_ms)
{
    host_pollfd hp[NET_POLL_MAX];
    int map[NET_POLL_MAX], nh = 0, nval = 0;

    for (int i = 0; i < n; i++) {
        grev[i] = 0;
        if (!valid_socket(gfd[i])) { grev[i] = SYS_NET_POLLNVAL; nval++; continue; }
        hp[nh].fd = s_sockets[gfd[i]].host_fd;
        hp[nh].events = 0;
        if (gev[i] & SYS_NET_POLLIN)  hp[nh].events |= POLLIN;
        if (gev[i] & SYS_NET_POLLOUT) hp[nh].events |= POLLOUT;
        hp[nh].revents = 0;
        map[nh++] = i;
    }
    int relayed = 0;
    for (int i = 0; i < n; i++)
        if ((gev[i] & SYS_NET_POLLIN) && valid_socket(gfd[i]) && relayed_pending(gfd[i])) relayed++;
    if (nval || relayed) timeout_ms = 0;
    if (nh == 0) { sleep_ms(timeout_ms); return nval; }

    int r = host_poll(hp, (unsigned)nh, timeout_ms);
    if (r == HOST_SOCKET_ERROR) return host_fail();

    int ready = nval;
    for (int k = 0; k < nh; k++) {
        short rv = 0, h = hp[k].revents;
        if (h & POLLIN)   rv |= SYS_NET_POLLIN;
        if (h & POLLOUT)  rv |= SYS_NET_POLLOUT;
        if (h & POLLERR)  rv |= SYS_NET_POLLERR;
        if (h & POLLHUP)  rv |= SYS_NET_POLLHUP;
        if (h & POLLNVAL) rv |= SYS_NET_POLLNVAL;
        if ((gev[map[k]] & SYS_NET_POLLIN) && relayed_pending(gfd[map[k]])) rv |= SYS_NET_POLLIN;
        grev[map[k]] = rv;
        if (rv) ready++;
    }
    return ready;
}

int32_t sys_net_bnet_poll(sys_net_pollfd* fds, uint32_t nfds, int32_t timeout_ms)
{
    int32_t gfd[NET_POLL_MAX];
    short gev[NET_POLL_MAX], grev[NET_POLL_MAX];
    uint32_t base = EA(fds);
    if (nfds > NET_POLL_MAX) return fail(SYS_NET_EINVAL);
    if (!base && nfds) return fail(SYS_NET_EINVAL);

    for (uint32_t i = 0; i < nfds; i++) {
        gfd[i] = (int32_t)vm_read32(base + i * 8);
        gev[i] = (short)vm_read16(base + i * 8 + 4);
    }
    int r = run_poll(gfd, gev, grev, (int)nfds, timeout_ms);
    if (r < 0) return r;
    for (uint32_t i = 0; i < nfds; i++) vm_write16(base + i * 8 + 6, (uint16_t)grev[i]);
    return r;
}

int32_t sys_net_bnet_select(int32_t nfds, void* readfds, void* writefds,
                            void* exceptfds, void* timeout)
{
    uint32_t rd = EA(readfds), wr = EA(writefds), ex = EA(exceptfds), tv = EA(timeout);
    int32_t gfd[NET_POLL_MAX];
    short gev[NET_POLL_MAX], grev[NET_POLL_MAX];
    int n = 0;
    if (nfds > 1024) nfds = 1024;

    for (int32_t fd = 0; fd < nfds; fd++) {
        uint32_t word = (uint32_t)fd / 32 * 4, bit = 1u << (fd % 32);
        short ev = 0;
        if (rd && (vm_read32(rd + word) & bit)) ev |= SYS_NET_POLLIN;
        if (wr && (vm_read32(wr + word) & bit)) ev |= SYS_NET_POLLOUT;
        if (!ev) continue;
        if (n == NET_POLL_MAX) return fail(SYS_NET_EINVAL);
        gfd[n] = fd; gev[n] = ev; n++;
    }

    int timeout_ms = -1;   /* NULL timeval = wait forever */
    if (tv) {
        uint64_t us = vm_read64(tv) * 1000000ull + vm_read64(tv + 8);
        timeout_ms = (int)((us + 999) / 1000);
    }
    int r = run_poll(gfd, gev, grev, n, timeout_ms);
    if (r < 0) return r;

    /* Rewrite the sets to hold only what is ready; the except set never is. */
    for (uint32_t i = 0; i < 128; i += 4) {
        if (rd) vm_write32(rd + i, 0);
        if (wr) vm_write32(wr + i, 0);
        if (ex) vm_write32(ex + i, 0);
    }
    int count = 0;
    for (int i = 0; i < n; i++) {
        uint32_t word = (uint32_t)gfd[i] / 32 * 4, bit = 1u << (gfd[i] % 32);
        short in_ready = SYS_NET_POLLIN | SYS_NET_POLLHUP | SYS_NET_POLLERR;
        if ((gev[i] & SYS_NET_POLLIN) && (grev[i] & in_ready)) {
            vm_write32(rd + word, vm_read32(rd + word) | bit); count++;
        }
        if ((gev[i] & SYS_NET_POLLOUT) && (grev[i] & (SYS_NET_POLLOUT | SYS_NET_POLLERR))) {
            vm_write32(wr + word, vm_read32(wr + word) | bit); count++;
        }
    }
    if (net_trace()) {
        static int last = -1;
        if (count != last || count) {   /* changes and every ready answer, not every empty poll */
            printf("[sys_net] socketselect(nfds %d, %d fds asked) -> %d ready\n", nfds, n, count);
            last = count;
        }
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Addresses and DNS
 * -----------------------------------------------------------------------*/

int32_t sys_net_bnet_inet_aton(const char* cp, uint32_t* inp)
{
    struct in_addr a;
    const char* s = GUEST_PTR(EA(cp), const char*);
    if (!s || inet_pton(AF_INET, s, &a) != 1) return 0;
    if (inp) memcpy(vm_ptr8(EA(inp)), &a, 4);
    return 1;
}

uint32_t sys_net_bnet_inet_addr(const char* cp)
{
    struct in_addr a;
    const char* s = GUEST_PTR(EA(cp), const char*);
    if (!s || inet_pton(AF_INET, s, &a) != 1) return 0xFFFFFFFFu;   /* INADDR_NONE */
    return ntohl(a.s_addr);
}

uint32_t sys_net_bnet_inet_ntoa(uint32_t addr)
{
    uint32_t ea = scratch(&s_ntoa_ea, 16);
    if (!ea) return 0;
    snprintf((char*)vm_ptr8(ea), 16, "%u.%u.%u.%u",
             addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
    return ea;
}

uint32_t sys_net_bnet_gethostbyname(const char* name)
{
    const char* host = GUEST_PTR(EA(name), const char*);
    uint32_t h = scratch(&s_hostent_ea, HOSTENT_SCRATCH);
    struct in_addr addr;
    if (!host || !h) return 0;

    /* Through PS3_NET_HOSTS when it is set (ps3emu/ps3_net.h). */
    if (ps3_net_resolve(host, (uint8_t*)&addr) != 0) {
        printf("[sys_net] gethostbyname('%s') failed\n", host);
        if (scratch(&s_h_errno_ea, 4)) vm_write32(s_h_errno_ea, SYS_NET_HOST_NOT_FOUND);
        return 0;
    }

    memset(vm_ptr8(h), 0, HOSTENT_SCRATCH);
    vm_write32(h + 0,  h + 64);           /* h_name */
    vm_write32(h + 4,  h + 32);           /* h_aliases -> { NULL } */
    vm_write32(h + 8,  SYS_NET_AF_INET);  /* h_addrtype */
    vm_write32(h + 12, 4);                /* h_length */
    vm_write32(h + 16, h + 40);           /* h_addr_list -> { &addr, NULL } */
    vm_write32(h + 40, h + 48);
    memcpy(vm_ptr8(h + 48), &addr, 4);
    strncpy((char*)vm_ptr8(h + 64), host, HOSTENT_SCRATCH - 65);

    char ip[16];
    printf("[sys_net] gethostbyname('%s') -> %s\n", host, ip_str(&addr, ip));
    return h;
}

uint32_t sys_net_errno_loc(void)
{
    if (scratch(&s_errno_ea, 4)) vm_write32(s_errno_ea, (uint32_t)s_errno);
    return s_errno_ea;
}

/* ---------------------------------------------------------------------------
 * Registration: libnet export names -> ctx handlers
 * -----------------------------------------------------------------------*/

#define R32(n)  ((uint32_t)ctx->gpr[n])
#define S32(n)  ((int32_t)(uint32_t)ctx->gpr[n])
#define PTR(n)  ((void*)(uintptr_t)(uint32_t)ctx->gpr[n])
#define RET_S(v) (ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)(v))
#define RET_U(v) (ctx->gpr[3] = (uint64_t)(uint32_t)(v))

static void h_socket(ppu_context* ctx)      { RET_S(sys_net_bnet_socket(S32(3), S32(4), S32(5))); }
static void h_close(ppu_context* ctx)       { RET_S(sys_net_bnet_close(S32(3))); }
static void h_bind(ppu_context* ctx)        { RET_S(sys_net_bnet_bind(S32(3), PTR(4), R32(5))); }
static void h_listen(ppu_context* ctx)      { RET_S(sys_net_bnet_listen(S32(3), S32(4))); }
static void h_accept(ppu_context* ctx)      { RET_S(sys_net_bnet_accept(S32(3), PTR(4), PTR(5))); }
static void h_connect(ppu_context* ctx)     { RET_S(sys_net_bnet_connect(S32(3), PTR(4), R32(5))); }
static void h_shutdown(ppu_context* ctx)    { RET_S(sys_net_bnet_shutdown(S32(3), S32(4))); }
static void h_send(ppu_context* ctx)        { RET_S(sys_net_bnet_send(S32(3), PTR(4), R32(5), S32(6))); }
static void h_sendto(ppu_context* ctx)      { RET_S(sys_net_bnet_sendto(S32(3), PTR(4), R32(5), S32(6), PTR(7), R32(8))); }
static void h_recv(ppu_context* ctx)        { RET_S(sys_net_bnet_recv(S32(3), PTR(4), R32(5), S32(6))); }
static void h_recvfrom(ppu_context* ctx)    { RET_S(sys_net_bnet_recvfrom(S32(3), PTR(4), R32(5), S32(6), PTR(7), PTR(8))); }
static void h_setsockopt(ppu_context* ctx)  { RET_S(sys_net_bnet_setsockopt(S32(3), S32(4), S32(5), PTR(6), R32(7))); }
static void h_getsockopt(ppu_context* ctx)  { RET_S(sys_net_bnet_getsockopt(S32(3), S32(4), S32(5), PTR(6), PTR(7))); }
static void h_getsockname(ppu_context* ctx) { RET_S(sys_net_bnet_getsockname(S32(3), PTR(4), PTR(5))); }
static void h_getpeername(ppu_context* ctx) { RET_S(sys_net_bnet_getpeername(S32(3), PTR(4), PTR(5))); }
static void h_poll(ppu_context* ctx)        { RET_S(sys_net_bnet_poll(PTR(3), R32(4), S32(5))); }
static void h_select(ppu_context* ctx)      { RET_S(sys_net_bnet_select(S32(3), PTR(4), PTR(5), PTR(6), PTR(7))); }
static void h_inet_aton(ppu_context* ctx)   { RET_S(sys_net_bnet_inet_aton(PTR(3), PTR(4))); }
static void h_inet_addr(ppu_context* ctx)   { RET_U(sys_net_bnet_inet_addr(PTR(3))); }
static void h_inet_ntoa(ppu_context* ctx)   { RET_U(sys_net_bnet_inet_ntoa(R32(3))); }
static void h_gethostbyname(ppu_context* ctx) { RET_U(sys_net_bnet_gethostbyname(PTR(3))); }
static void h_errno_loc(ppu_context* ctx)   { RET_U(sys_net_errno_loc()); }
static void h_h_errno_loc(ppu_context* ctx) { RET_U(scratch(&s_h_errno_ea, 4)); }
static void h_init(ppu_context* ctx)        { RET_S(sys_net_initialize_network_ex(PTR(3))); }
static void h_finalize(ppu_context* ctx)    { RET_S(sys_net_finalize_network()); }
static void h_ok(ppu_context* ctx)          { RET_S(0); }

void ps3_net_host_register(unsigned int (*guest_alloc)(unsigned int size, unsigned int align))
{
    static const struct { const char* name; void (*fn)(ppu_context*); } tab[] = {
        { "socket", h_socket },           { "socketclose", h_close },
        { "bind", h_bind },               { "listen", h_listen },
        { "accept", h_accept },           { "connect", h_connect },
        { "shutdown", h_shutdown },       { "send", h_send },
        { "sendto", h_sendto },           { "recv", h_recv },
        { "recvfrom", h_recvfrom },       { "setsockopt", h_setsockopt },
        { "getsockopt", h_getsockopt },   { "getsockname", h_getsockname },
        { "getpeername", h_getpeername }, { "socketpoll", h_poll },
        { "socketselect", h_select },     { "inet_aton", h_inet_aton },
        { "inet_addr", h_inet_addr },     { "inet_ntoa", h_inet_ntoa },
        { "gethostbyname", h_gethostbyname },
        { "_sys_net_errno_loc", h_errno_loc },
        { "_sys_net_h_errno_loc", h_h_errno_loc },
        { "sys_net_initialize_network_ex", h_init },
        { "sys_net_finalize_network", h_finalize },
        { "sys_net_free_thread_context", h_ok },
        { "sys_net_abort_resolver", h_ok },
    };
    s_alloc = guest_alloc;
    net_startup();
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        ps3_hle_register_ctx(ps3_compute_nid(tab[i].name), tab[i].name, tab[i].fn);
    printf("[sys_net] PS3_NET_ONLINE: host sockets registered (%u exports)\n",
           (unsigned)(sizeof(tab) / sizeof(tab[0])));
}
