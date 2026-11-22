/*
 * test_http_abort - a transaction closed from another thread while a call
 * waits on its connection: cellHttpTransactionAbortConnection,
 * cellHttpClientCloseAllConnections, cellHttpDestroyClient and cellHttpEnd
 * while another thread is in cellHttpRecvResponse or
 * cellHttpClientPollConnections, over plain TCP and over TLS. The wait must
 * end at once with an error, and the connection must not be freed under it
 * (closed there and then, the waiting call went on with a freed TLS
 * connection: a crash in mbedtls_ssl_read under
 * cellHttpResponseGetStatusCode). Also a request left alone still gets its
 * answer. A loopback server holds each request unanswered for a while;
 * nothing leaves the PC.
 *
 *   clang-cl /I include /I third_party/mbedtls/include /Fe:test_http_abort.exe libs/network/tests/test_http_abort.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c mbedtls.lib ws2_32.lib
 *   cc -std=gnu17 -I include -I third_party/mbedtls/include -o test_http_abort libs/network/tests/test_http_abort.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c libmbedtls.a -lpthread
 * (mbedtls.lib / libmbedtls.a: the .c files of third_party/mbedtls/library, compiled once)
 */
#include "../cellHttp.c"
#include "../ps3_net.c"
#include "../ps3_net_signin.c"
#include "../ps3_tls.c"
#include "tls_test_server.h"

/* The sign-in's name (np_psnr.c has the real one). */
const char* np_psnr_online_id(void) { return "PS3Player"; }

#include <assert.h>
#ifndef _WIN32
#  include <arpa/inet.h>
#  include <time.h>
#endif

/* ppu_memory.h hooks the runtime supplies; inert here. */
uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static long long now_ms(void)
{
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void nap_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* ---- the server: sign-in requests refused, the game's held, then answered ---- */

enum { ANSWER_AFTER_MS = 2500 };    /* after the test's close: the old code's wait ended only then */

static http_socket_t s_listen;
static volatile int  s_tls;         /* the server speaks TLS (tls_test_server.h) */
static volatile int  s_stop;
static volatile int  s_held;        /* a game request came and waits for its answer */
static volatile long long s_release_at;   /* when to answer it (0: not yet) */

static int c_send(http_socket_t c, ps3_tls* t, const char* p, int n)
{
    return t ? ps3_tls_send_all(t, p, (size_t)n, 3000) : (send(c, p, n, 0) == n ? 0 : -1);
}

static int c_recv(http_socket_t c, ps3_tls* t, char* p, int n, int ms)
{
    if (t) return ps3_tls_recv(t, p, (size_t)n, ms);
    return recv(c, p, n, 0);
}

static void serve_one(http_socket_t c, ps3_tls* t)
{
    char req[4096];
    int n = 0;
    while (n < (int)sizeof req - 1) {
        int k = c_recv(c, t, req + n, (int)sizeof req - 1 - n, 3000);
        if (k <= 0) return;
        n += k;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (strstr(req, " /dss/")) {   /* the sign-in: refused, so the game's request goes without a session */
        static const char no[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        c_send(c, t, no, (int)sizeof no - 1);
        return;
    }
    s_held = 1;
    while (!s_stop && !(s_release_at && now_ms() >= s_release_at)) nap_ms(10);
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    c_send(c, t, ok, (int)sizeof ok - 1);
    /* until the client closes (or a moment) */
    char sink[256];
    long long end = now_ms() + 1000;
    while (now_ms() < end && c_recv(c, t, sink, sizeof sink, 200) > 0) {}
}

#ifdef _WIN32
static DWORD WINAPI server_main(void* arg)
#else
static void* server_main(void* arg)
#endif
{
    (void)arg;
    while (!s_stop) {
        http_socket_t c = accept(s_listen, NULL, NULL);
        if (c == HTTP_INVALID_SOCKET) break;
        ps3_tls* t = NULL;
        if (s_tls && !(t = tts_accept((intptr_t)c, TTS_OURS, 3000))) {
            http_closesocket(c);
            continue;
        }
        serve_one(c, t);
        ps3_tls_close(t);
        http_closesocket(c);
    }
    return 0;
}

/* ---- the game's side ---- */

enum { SCHEME = 0x100, HOST = 0x110, PATH = 0x160, METHOD = 0x1A0, URI = 0x200, CLIENT = 0x300, TRANS = 0x304,
       SENT = 0x308, READY = 0x30C, CODE = 0x310 };

static void put_str(u32 ea, const char* s) { strcpy((char*)vm_base + ea, s); }

enum { RECV, POLL };
enum { NOTHING, ABORT, CLOSE_ALL, DESTROY_CLIENT, END };
static const char* const k_how[] = {"nothing", "abort", "close all", "destroy the client", "End"};

static volatile CellHttpTransId s_t;
static volatile CellHttpClientId s_client;
static volatile int  s_waiter_kind;
static volatile s32  s_result;
static volatile long long s_returned_at;

#ifdef _WIN32
static DWORD WINAPI waiter_main(void* arg)
#else
static void* waiter_main(void* arg)
#endif
{
    (void)arg;
    if (s_waiter_kind == RECV)
        s_result = cellHttpResponseGetStatusCode(s_t, (s32*)(uintptr_t)CODE);   /* the crash's call: reads the headers */
    else
        s_result = cellHttpClientPollConnections(s_client, (CellHttpTransId*)(uintptr_t)READY, -1);   /* no limit */
    s_returned_at = now_ms();
    return 0;
}

static int s_fails;

static void one_case(u16 port, int tls, int waiter, int how)
{
    s_tls = tls;
    s_held = 0;
    s_release_at = 0;
    s_returned_at = 0;
    vm_write32(READY, 0xFFFFFFFF);
    vm_write32(CODE, 0);
    assert(cellHttpInit(NULL, 0) == CELL_OK);
    assert(cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) == CELL_OK);
    s_client = vm_read32(CLIENT);
    vm_write32(URI + 0, SCHEME);
    vm_write32(URI + 4, HOST);
    vm_write32(URI + 8, 0);
    vm_write32(URI + 12, 0);
    vm_write32(URI + 16, PATH);
    vm_write32(URI + 20, port);
    vm_write32(URI + 24, 0);
    assert(cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, s_client, (const char*)(uintptr_t)METHOD,
                                     (const CellHttpUri*)(uintptr_t)URI) == CELL_OK);
    s_t = vm_read32(TRANS);
    assert(cellHttpSetRecvTimeOut(s_t, 8000000) == CELL_OK);   /* 8 s: the old code's wait, unanswered */
    assert(cellHttpSendRequest(s_t, NULL, 0, (u32*)(uintptr_t)SENT) == CELL_OK);
    for (int i = 0; i < 500 && !s_held; i++) nap_ms(10);
    assert(s_held);

    s_waiter_kind = waiter;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, waiter_main, NULL, 0, NULL);
    assert(th);
#else
    pthread_t th;
    assert(pthread_create(&th, NULL, waiter_main, NULL) == 0);
#endif
    nap_ms(300);   /* the waiter is in its wait */
    assert(!s_returned_at);
    const long long closed_at = now_ms();
    switch (how) {
    case ABORT:          assert(cellHttpTransactionAbortConnection(s_t) == CELL_OK); break;
    case CLOSE_ALL:      assert(cellHttpClientCloseAllConnections(s_client) == CELL_OK); break;
    case DESTROY_CLIENT: assert(cellHttpDestroyClient(s_client) == CELL_OK); break;
    case END:            assert(cellHttpEnd() == CELL_OK); break;
    default: break;
    }
    s_release_at = now_ms() + (how == NOTHING ? 100 : ANSWER_AFTER_MS);
#ifdef _WIN32
    WaitForSingleObject(th, 20000);
    CloseHandle(th);
#else
    pthread_join(th, NULL);
#endif
    const long long took = s_returned_at - closed_at;
    int ok;
    if (how == NOTHING)   /* left alone: the answer, as before */
        ok = waiter == RECV ? s_result == CELL_OK && vm_read32(CODE) == 200
                            : s_result == CELL_OK && vm_read32(READY) == s_t;
    else                  /* the wait ends at once, and not with the answer that came later */
        ok = took < 1000 && (waiter == RECV ? s_result != CELL_OK
                                            : s_result == CELL_OK && vm_read32(READY) == 0);
    printf("%-4s %-5s %-19s -> 0x%08x after %4lld ms: %s\n", tls ? "TLS" : "TCP", waiter == RECV ? "recv" : "poll",
           k_how[how], (unsigned)s_result, took, ok ? "ok" : "FAILED");
    if (!ok) s_fails++;

    if (how != END) {
        if (how != DESTROY_CLIENT) {
            cellHttpDestroyTransaction(s_t);
            cellHttpDestroyClient(s_client);
        }
        assert(cellHttpEnd() == CELL_OK);
    }
    /* the server's side of this case ends (its answer, then the client's close) before the next */
    s_release_at = now_ms();
    nap_ms(how == NOTHING ? 100 : 300);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, 65536);
    put_str(SCHEME, "http");
    put_str(HOST, "game.test");
    put_str(PATH, "/cgi-bin/addQWCData.spd");
    put_str(METHOD, "POST");

#ifdef _WIN32
    /* Winsock for the test's own server: each case's cellHttpEnd stops
     * cellHttp's (WSACleanup), which would close every socket with it */
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    assert(bind(s_listen, (struct sockaddr*)&a, sizeof a) == 0 && listen(s_listen, 8) == 0);
    assert(getsockname(s_listen, (struct sockaddr*)&a, &al) == 0);
    u16 port = ntohs(a.sin_port);
#ifdef _WIN32
    HANDLE sv = CreateThread(NULL, 0, server_main, NULL, 0, NULL);
    assert(sv);
#else
    pthread_t sv;
    assert(pthread_create(&sv, NULL, server_main, NULL) == 0);
#endif

    char key[512], ca[600], addr[64];
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(key, sizeof key, "%stest_http_abort_%lu.key", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(key, sizeof key, "/tmp/test_http_abort_%ld.key", (long)getpid());
#endif
    snprintf(ca, sizeof ca, "%s.ca.pem", key);
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
    env("PS3_NET_ONLINE", "1");
    env("PS3_NET_HOSTS", "game.test=127.0.0.1");

    for (int tls = 0; tls < 2; tls++) {
        if (tls) {   /* TLS goes to the sign-in server only: this one, whose sign-in refuses */
            tts_write_ca(ca);
            env("PS3_NET_CA_FILE", ca);
            env("PS3_NET_TLS", "dss.test");
            env("PS3_NET_SIGNIN", addr);
            env("PS3_NET_KEY_FILE", key);
        }
        for (int waiter = RECV; waiter <= POLL; waiter++)
            for (int how = NOTHING; how <= END; how++)
                one_case(port, tls, waiter, how);
    }

    s_stop = 1;
    s_release_at = now_ms();
    http_closesocket(s_listen);
    remove(ca);
    remove(key);
    printf("%s\n", s_fails ? "test_http_abort: FAILED" : "test_http_abort: all passed");
    return s_fails ? 1 : 0;
}
