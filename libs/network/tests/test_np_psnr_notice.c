/*
 * test_np_psnr_notice - what the player is told when psnr refuses the HELLO
 * (ps3_net_signin_notice): that co-op is not available, unless the sign-in
 * itself did not work. Then the sign-in has given its own reason (a ban,
 * maintenance) or, with its server out of reach, stays quiet, and a second
 * notice from co-op would only replace the first before it is shown. Also:
 * a run locked offline (developer cheats) ends its open session. A loopback
 * psnr that answers each HELLO with an error or a welcome. Nothing leaves
 * the PC.
 *
 *   clang-cl /I include /Fe:test_np_psnr_notice.exe libs/network/tests/test_np_psnr_notice.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_psnr_notice libs/network/tests/test_np_psnr_notice.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c -lpthread
 */
#include "../psnr/psnr.c"
#include "../np_psnr.c"
#include "../ps3_net.c"
#include "../ps3_net_signin.c"

#include <assert.h>
#ifndef _WIN32
#  include <arpa/inet.h>
#  include <pthread.h>
#endif

const char* np_fake_username(void) { return "PS3Player"; }

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* The loopback psnr: one HELLO a connection, answered with s_error. */
static sock_t s_listen = BAD_SOCK;
static uint32_t s_error;
static int s_hello_len;   /* the last HELLO's body: 30 bytes, 64 with a session */
static volatile int s_hangups;   /* connections the client has closed */

#ifdef _WIN32
static DWORD WINAPI psnr_main(LPVOID arg)
#else
static void* psnr_main(void* arg)
#endif
{
    (void)arg;
    for (;;) {
        sock_t c = accept(s_listen, NULL, NULL);
        if (c == BAD_SOCK) break;
        uint8_t in[256];
        int have = 0, k;
        while ((have < 3 || have < 3 + psnr_get16(in + 1)) &&
               (k = recv(c, (char*)in + have, (int)sizeof in - have, 0)) > 0)
            have += k;
        if (have >= 7) {
            uint8_t out[15];
            s_hello_len = psnr_get16(in + 1) - 4;
            memcpy(out + 3, in + 3, 4);   /* the request id */
            if (s_error) {
                out[0] = PSNR_ERROR;
                psnr_put16(out + 1, 8);
                psnr_put32(out + 7, s_error);
                send(c, (const char*)out, 11, 0);
            } else {   /* welcome: user 7, seen from 127.0.0.1 */
                out[0] = PSNR_HELLO_ACK;
                psnr_put16(out + 1, 12);
                psnr_put32(out + 7, 7);
                memcpy(out + 11, "\x7f\x00\x00\x01", 4);
                send(c, (const char*)out, 15, 0);
            }
            while (recv(c, (char*)in, sizeof in, 0) > 0) {}   /* until the client hangs up */
            s_hangups++;
        }
        sock_close(c);
    }
    return 0;
}

static uint16_t loopback_port(sock_t s)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    assert(bind(s, (struct sockaddr*)&a, sizeof a) == 0);
    assert(getsockname(s, (struct sockaddr*)&a, &al) == 0);
    return ntohs(a.sin_port);
}

/* A new attempt, as the next process would make. */
static int hello(uint32_t error)
{
    s_error = error;
    s_hello_len = 0;
    s_tried = 0;
    return np_psnr_connect("NPWR00881_00");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    char addr[64], key[512], notice[256];

    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    uint16_t port = loopback_port(s_listen);
    assert(listen(s_listen, 8) == 0);
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, psnr_main, NULL, 0, NULL);
    assert(th);
#else
    pthread_t th;
    assert(pthread_create(&th, NULL, psnr_main, NULL) == 0);
#endif
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
    env("PS3_NET_ONLINE", "1");
    env("PSNR_SERVER", addr);
    env("PS3_NP_ONLINE_ID", "knight");
    env("PS3_NET_SIGNIN", "");
    env("PS3_NET_KEY_FILE", "");
    np_psnr_setup(NULL, NULL);

    /* No sign-in set up, and psnr wants one: co-op says so. */
    assert(hello(PSNR_E_SIGN_IN) == -1 && psnr_connect_error() == PSNR_E_SIGN_IN && s_hello_len == 30);
    assert(ps3_net_signin_notice(notice, sizeof notice) && strstr(notice, "Co-op is not available:"));
    assert(!ps3_net_signin_notice(notice, sizeof notice));

    /* psnr turns everyone away. */
    assert(hello(PSNR_E_REFUSED) == -1 && psnr_connect_error() == PSNR_E_REFUSED);
    assert(ps3_net_signin_notice(notice, sizeof notice) && !strcmp(notice, "Co-op is not available right now."));

    /* A sign-in that did not work (here its server is out of reach): the
     * HELLO goes without a session, and its refusal adds no notice. */
    sock_t gone = socket(AF_INET, SOCK_STREAM, 0);
    snprintf(addr, sizeof addr, "127.0.0.1:%u", loopback_port(gone));
    sock_close(gone);   /* (nothing listens there now) */
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(key, sizeof key, "%stest_np_psnr_notice_%lu.key", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(key, sizeof key, "/tmp/test_np_psnr_notice_%ld.key", (long)getpid());
#endif
    remove(key);   /* (this run's own file) */
    env("PS3_NET_SIGNIN", addr);
    env("PS3_NET_KEY_FILE", key);
    assert(ps3_net_signin_enabled());
    assert(hello(PSNR_E_SIGN_IN) == -1 && psnr_connect_error() == PSNR_E_SIGN_IN && s_hello_len == 30);
    assert(!ps3_net_signin_notice(notice, sizeof notice));
    remove(key);

    /* Locked offline (developer cheats): an open session ends at the next
     * pump, and no new one starts. Last: the lock lasts the whole run. */
    env("PS3_NET_SIGNIN", "");
    env("PS3_NET_KEY_FILE", "");
    assert(hello(0) == 0 && np_psnr_connected());
    int hangups = s_hangups;
    np_psnr_pump();
    assert(np_psnr_connected());   /* (not locked: still open) */
    ps3_net_lock_offline();
    np_psnr_pump();
    assert(!np_psnr_connected());
    for (int i = 0; i < 500 && s_hangups == hangups; i++) {   /* the server sees it close */
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
    assert(s_hangups == hangups + 1);
    assert(hello(0) == -1);

    printf("test_np_psnr_notice: all passed\n");
    return 0;
}
