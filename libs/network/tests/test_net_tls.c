/*
 * test_net_tls - TLS on the links to the online server (ps3emu/ps3_tls.h)
 * against a loopback server with the test certificates (tls_test_server.h):
 * a good certificate passes, by name and by address; a wrong name, a
 * stranger's CA, a server that doesn't speak TLS and one that says nothing
 * are refused. Data both ways, in pieces bigger than a TLS record. The
 * knobs, and the built-in roots. Nothing leaves the PC.
 *
 *   clang-cl /I include /I third_party/mbedtls/include /Fe:test_net_tls.exe libs/network/tests/test_net_tls.c mbedtls.lib ws2_32.lib
 *   cc -std=gnu17 -I include -I third_party/mbedtls/include -o test_net_tls libs/network/tests/test_net_tls.c libmbedtls.a -lpthread
 * (mbedtls.lib / libmbedtls.a: the .c files of third_party/mbedtls/library, compiled once)
 */
#include "../ps3_tls.c"
#include "tls_test_server.h"

#include <assert.h>
#ifdef _WIN32
#  define test_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <unistd.h>
#  define test_close close
#endif

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
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

/* ---- the server: one connection per case ---------------------------- */

enum { ECHO, ECHO_STRANGER, BIG, PLAIN, SILENT };

#define BIG_UP   200000   /* bytes the client sends */
#define BIG_DOWN 300000   /* bytes the server sends back */

static tls_sock_t   s_listen;
static volatile int s_case = ECHO;
static volatile int s_served;
static volatile int s_server_ok;   /* the case went as the server saw it */

static unsigned char pattern(size_t i) { return (unsigned char)(i * 7 + (i >> 9)); }

static void serve(tls_sock_t c)
{
    int which = s_case;
    s_server_ok = 0;
    if (which == SILENT) {
        nap_ms(1500);
        s_server_ok = 1;
        return;
    }
    if (which == PLAIN) {
        char b[512];
        recv(c, b, sizeof b, 0);
        const char* r = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, r, (int)strlen(r), 0);
        s_server_ok = 1;
        return;
    }
    ps3_tls* t = tts_accept((intptr_t)c, which == ECHO_STRANGER ? TTS_STRANGER : TTS_OURS, 3000);
    if (!t) return;   /* (a client that refused the certificate ends it here) */
    if (which == BIG) {
        unsigned char* b = (unsigned char*)malloc(BIG_DOWN);
        size_t got = 0;
        int ok = 1;
        while (ok && got < BIG_UP) {
            int n = ps3_tls_recv(t, b + got, BIG_UP - got, 5000);
            if (n <= 0) ok = 0; else got += (size_t)n;
        }
        for (size_t i = 0; ok && i < BIG_UP; i++) ok = b[i] == pattern(i);
        for (size_t i = 0; i < BIG_DOWN; i++) b[i] = pattern(i + 1);
        if (ok) ok = ps3_tls_send_all(t, b, BIG_DOWN, 5000) == 0;
        free(b);
        s_server_ok = ok;
    } else {
        char line[64];
        size_t n = 0;
        while (n < sizeof line - 1) {
            int k = ps3_tls_recv(t, line + n, 1, 5000);
            if (k <= 0) break;
            if (line[n++] == '\n') break;
        }
        line[n] = 0;
        char reply[96];
        snprintf(reply, sizeof reply, "pong:%s", line);
        s_server_ok = ps3_tls_send_all(t, reply, strlen(reply), 5000) == 0;
    }
    ps3_tls_close(t);   /* (the goodbye: the client reads the end) */
}

#ifdef _WIN32
static DWORD WINAPI server_main(void* arg)
#else
static void* server_main(void* arg)
#endif
{
    (void)arg;
    for (;;) {
        tls_sock_t c = accept(s_listen, NULL, NULL);
#ifdef _WIN32
        if (c == INVALID_SOCKET) break;
#else
        if (c < 0) break;
#endif
        serve(c);
        nap_ms(50);   /* (let the client read before the close) */
        test_close(c);
        s_served++;
    }
    return 0;
}

static unsigned short s_port;

static tls_sock_t dial(void)
{
    tls_sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(s_port);
    assert(connect(s, (struct sockaddr*)&a, sizeof a) == 0);
    return s;
}

/* Waits until the server has finished n connections. */
static void served(int n)
{
    for (int i = 0; i < 500 && s_served < n; i++) nap_ms(10);
    assert(s_served >= n);
}

/* One echo: the handshake with name, "ping", the reply, the end. 1 if it all went through; why when it didn't. */
static int echo(const char* name, char* why, size_t wcap)
{
    int before = s_served;
    tls_sock_t s = dial();
    ps3_tls* t = ps3_tls_connect((intptr_t)s, name, 3000, why, wcap);
    int ok = 0;
    if (t) {
        char b[64] = "";
        size_t n = 0;
        ok = ps3_tls_send_all(t, "ping\n", 5, 3000) == 0;
        for (;;) {
            int k = ok ? ps3_tls_recv(t, b + n, sizeof b - 1 - n, 3000) : -1;
            if (k <= 0) { ok = ok && k == 0; break; }   /* the end must be the server's goodbye */
            n += (size_t)k;
        }
        b[n] = 0;
        ok = ok && !strcmp(b, "pong:ping\n");
        ps3_tls_close(t);
    }
    test_close(s);
    served(before + 1);
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char why[256], name[256];

    /* The built-in roots: Let's Encrypt's four, all readable. */
    {
        mbedtls_x509_crt roots;
        mbedtls_x509_crt_init(&roots);
        assert(mbedtls_x509_crt_parse(&roots, (const unsigned char*)TLS_ROOTS, sizeof TLS_ROOTS) == 0);
        const char* want[4] = {"CN=ISRG Root X1", "CN=ISRG Root X2", "CN=Root YE", "CN=Root YR"};
        int n = 0;
        for (mbedtls_x509_crt* c = &roots; c && c->raw.len; c = c->next, n++) {
            char dn[256];
            mbedtls_x509_dn_gets(dn, sizeof dn, &c->subject);
            assert(n < 4 && strstr(dn, want[n]));
        }
        assert(n == 4);
        mbedtls_x509_crt_free(&roots);
    }

    /* The knobs. */
    env("PS3_NET_TLS", "");
    assert(!ps3_tls_enabled() && ps3_tls_server_name(name, sizeof name) == -1);
    env("PS3_NET_TLS", "0");
    assert(!ps3_tls_enabled() && ps3_tls_server_name(name, sizeof name) == -1);
    env("PS3_NET_TLS", "1");
    env("PS3_NET_SIGNIN", "");
    assert(ps3_tls_enabled() && ps3_tls_server_name(name, sizeof name) == -1);   /* no server, no name */
    env("PS3_NET_SIGNIN", "play.example.org:18000");
    assert(ps3_tls_server_name(name, sizeof name) == 0 && !strcmp(name, "play.example.org"));
    env("PS3_NET_SIGNIN", "play.example.org");
    assert(ps3_tls_server_name(name, sizeof name) == 0 && !strcmp(name, "play.example.org"));
    assert(ps3_tls_server_name(name, 8) == -1);   /* (too long for the room) */
    env("PS3_NET_TLS", "dss.test");
    assert(ps3_tls_server_name(name, sizeof name) == 0 && !strcmp(name, "dss.test"));
    assert(!ps3_tls_connect(0, "", 1000, why, sizeof why) && strstr(why, "no server name"));

    /* The test CA instead of the built-in roots (read at the first connection). */
    char ca[512];
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(ca, sizeof ca, "%stest_net_tls_%lu.pem", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(ca, sizeof ca, "/tmp/test_net_tls_%ld.pem", (long)getpid());
#endif
    tts_write_ca(ca);
    env("PS3_NET_CA_FILE", ca);

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    assert(bind(s_listen, (struct sockaddr*)&a, sizeof a) == 0 && listen(s_listen, 4) == 0);
    assert(getsockname(s_listen, (struct sockaddr*)&a, &al) == 0);
    s_port = ntohs(a.sin_port);
    tts_setup();
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, server_main, NULL, 0, NULL);
    assert(th);
#else
    pthread_t th;
    assert(pthread_create(&th, NULL, server_main, NULL) == 0);
#endif

    /* A good certificate, by name and by address. */
    s_case = ECHO;
    assert(echo("dss.test", why, sizeof why) && s_server_ok);
    assert(echo("127.0.0.1", why, sizeof why) && s_server_ok);

    /* Not for this name. */
    assert(!echo("other.test", why, sizeof why) && strstr(why, "not accepted for other.test"));
    printf("  wrong name: %s\n", why);

    /* The right name, from a CA nobody here trusts. */
    s_case = ECHO_STRANGER;
    assert(!echo("dss.test", why, sizeof why) && strstr(why, "not accepted for dss.test"));
    printf("  stranger's CA: %s\n", why);

    /* A server that does not speak TLS: refused, and the request never went. */
    s_case = PLAIN;
    assert(!echo("dss.test", why, sizeof why));
    printf("  plain server: %s\n", why);

    /* A server that says nothing: given up on in time. */
    s_case = SILENT;
    {
        int before = s_served;
        tls_sock_t s = dial();
        unsigned long long t0 = tls_now_ms();
        assert(!ps3_tls_connect((intptr_t)s, "dss.test", 300, why, sizeof why) && strstr(why, "in time"));
        unsigned long long took = tls_now_ms() - t0;
        assert(took >= 250 && took < 1200);
        test_close(s);
        served(before + 1);
    }

    /* Data both ways, bigger than a record (16 KB) and than the socket's buffers. */
    s_case = BIG;
    {
        int before = s_served;
        tls_sock_t s = dial();
        ps3_tls* t = ps3_tls_connect((intptr_t)s, "dss.test", 3000, why, sizeof why);
        assert(t);
        unsigned char* b = (unsigned char*)malloc(BIG_DOWN);
        for (size_t i = 0; i < BIG_UP; i++) b[i] = pattern(i);
        assert(ps3_tls_send_all(t, b, BIG_UP, 5000) == 0);
        memset(b, 0, BIG_DOWN);
        size_t got = 0;
        int pending_seen = 0;
        for (;;) {
            int n = ps3_tls_recv(t, b + got, got < BIG_DOWN ? 1000 : 1, 5000);   /* small reads: TLS holds the rest */
            if (n <= 0) { assert(n == 0); break; }
            got += (size_t)n;
            if (got < BIG_DOWN && ps3_tls_pending(t)) pending_seen = 1;
        }
        assert(got == BIG_DOWN && pending_seen);
        for (size_t i = 0; i < BIG_DOWN; i++) assert(b[i] == pattern(i + 1));
        free(b);
        ps3_tls_close(t);
        test_close(s);
        served(before + 1);
        assert(s_server_ok);
    }

    /* Nothing waiting: timeout 0 says so at once. */
    s_case = ECHO;
    {
        int before = s_served;
        tls_sock_t s = dial();
        ps3_tls* t = ps3_tls_connect((intptr_t)s, "dss.test", 3000, why, sizeof why);
        char b[16];
        assert(t && ps3_tls_recv(t, b, sizeof b, 0) == PS3_TLS_AGAIN && !ps3_tls_pending(t));
        assert(ps3_tls_send_all(t, "x\n", 2, 3000) == 0);
        int n = 0;
        while (n == 0 || n == PS3_TLS_AGAIN) {
            n = ps3_tls_recv(t, b, sizeof b, 0);
            if (n == PS3_TLS_AGAIN) nap_ms(5);
        }
        assert(n > 0);
        ps3_tls_close(t);
        test_close(s);
        served(before + 1);
    }
    ps3_tls_close(NULL);

    test_close(s_listen);
    remove(ca);
    printf("test_net_tls: OK\n");
    return 0;
}
