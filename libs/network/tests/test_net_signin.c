/*
 * test_net_signin - the network sign-in (ps3emu/ps3_net_signin.h) against a
 * loopback server that checks what a Demon's Souls Recomp server checks: the
 * challenge, the Ed25519 signature, a new key's work. Then the session on
 * cellHttp's requests, to that server only. Also SHA-256 (FIPS 180-4) and
 * Ed25519 (RFC 8032) vectors, and the reply parsing. Then all of it over
 * TLS (ps3emu/ps3_tls.h), and a server without it. Nothing leaves the PC.
 *
 *   clang-cl /I include /I third_party/mbedtls/include /Fe:test_net_signin.exe libs/network/tests/test_net_signin.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c mbedtls.lib ws2_32.lib
 *   cc -std=gnu17 -I include -I third_party/mbedtls/include -o test_net_signin libs/network/tests/test_net_signin.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c libmbedtls.a -lpthread
 * (mbedtls.lib / libmbedtls.a: the .c files of third_party/mbedtls/library, compiled once)
 */
#include "../cellHttp.c"
#include "../ps3_net.c"
#include "../ps3_net_signin.c"
#include "../ps3_tls.c"
#include "tls_test_server.h"

#include <assert.h>
#ifndef _WIN32
#  include <arpa/inet.h>
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

static const char* s_name = "Ashen-01";
const char* np_psnr_online_id(void) { return s_name; }

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void hex(const char* h, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static int sha_is(const void* data, size_t len, const char* want)
{
    uint8_t h[32], w[32];
    ps3_sha256(data, len, h);
    hex(want, w, 32);
    return !memcmp(h, w, 32);
}

/* ---- the fake server ------------------------------------------------ */

enum { NORMAL, BANNED, NOT_OURS, BUSY_JSON };

static volatile int s_mode = NORMAL;
static int      s_bits = 10;
static uint8_t  s_challenge[32];
static int      s_challenge_live;
static uint8_t  s_known[4][32];
static int      s_nknown;
static uint8_t  s_issued[32];
static volatile int s_challenges, s_sessions, s_games, s_banner_gets;
static const char* volatile s_banners = "{\"banners\":[]}";
static const char* volatile s_players = "{\"days\":30,\"players\":[]}";
static volatile int s_post_status = 200;
static const char* volatile s_post_reply = "{\"ok\":true}";
static volatile int s_stale_once, s_player_calls;
static char     s_player_post[512];
static char     s_game_request[4096];
static volatile int s_stop;
static http_socket_t s_listen;
static volatile int s_tls;          /* the server speaks TLS (tls_test_server.h) */
static volatile int s_tls_conns;    /* connections it took over TLS */
static volatile int s_tls_hellos;   /* TLS hellos a server without TLS was sent (and dropped) */
static ps3_tls* s_conn_tls;         /* the connection being served (one server thread) */

static void c_send(http_socket_t c, const char* p, int n)
{
    if (s_conn_tls) ps3_tls_send_all(s_conn_tls, p, (size_t)n, 3000);
    else send(c, p, n, 0);
}

static int c_recv(http_socket_t c, char* p, int n)
{
    return s_conn_tls ? ps3_tls_recv(s_conn_tls, p, (size_t)n, 3000) : recv(c, p, n, 0);
}

static void reply(http_socket_t c, int status, const char* body)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d X\r\nDate: Sun, 25 Oct 2026 20:00:00 GMT\r\nContent-Type: application/json\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n\r\n",
                     status, (unsigned)strlen(body));
    c_send(c, head, n);
    c_send(c, body, (int)strlen(body));
}

static int known(const uint8_t pk[32])
{
    for (int i = 0; i < s_nknown; i++)
        if (!memcmp(s_known[i], pk, 32)) return 1;
    return 0;
}

static void session_call(http_socket_t c, const char* body)
{
    char name[32], pk64[64], ver[64], ch64[64], sig64[128], msg[512];
    long long nv = 0, nonce = 0;
    uint8_t pk[32], sig[64], ch[32];
    assert(si_js_str(body, "name", name, sizeof name) && si_js_str(body, "public_key", pk64, sizeof pk64) &&
           si_js_str(body, "client_version", ver, sizeof ver) && si_js_str(body, "challenge", ch64, sizeof ch64) &&
           si_js_str(body, "signature", sig64, sizeof sig64) && si_js_int(body, "network_version", &nv) &&
           si_js_int(body, "pow_nonce", &nonce));
    assert(si_b64_decode(pk64, pk, 32, 0) == 0 && si_b64_decode(sig64, sig, 64, 0) == 0 && si_b64_decode(ch64, ch, 32, 0) == 0);
    assert(!strcmp(name, s_name) && !strcmp(ver, "0.2.0") && nv == 3);
    if (!s_challenge_live || memcmp(ch, s_challenge, 32)) {   /* a challenge works once */
        reply(c, 401, "{\"error\":\"challenge\",\"message\":\"get a new challenge\"}");
        return;
    }
    s_challenge_live = 0;
    int ml = snprintf(msg, sizeof msg, "DSS session v1\n%s\n%s\n%s\n%s\n%lld\n%lld", ch64, name, pk64, ver, nv, nonce);
    assert(crypto_ed25519_check(sig, pk, (const uint8_t*)msg, (size_t)ml) == 0);
    if (s_mode == BANNED) {
        reply(c, 403, "{\"error\":\"banned\",\"message\":\"This account is banned (case 12).\",\"case\":12}");
        return;
    }
    if (!known(pk)) {
        uint8_t in[72], h[32];
        memcpy(in, ch, 32);
        memcpy(in + 32, pk, 32);
        for (int i = 0; i < 8; i++) in[64 + i] = (uint8_t)((uint64_t)nonce >> (56 - 8 * i));
        ps3_sha256(in, sizeof in, h);
        if (si_zero_bits(h) < s_bits) {
            char r[96];
            snprintf(r, sizeof r, "{\"error\":\"work\",\"message\":\"do the work\",\"pow_bits\":%d}", s_bits);
            reply(c, 401, r);
            return;
        }
        memcpy(s_known[s_nknown++ % 4], pk, 32);
    }
    char tok[48], r[256];
    si_random_bytes(s_issued, 32);
    si_b64_encode(s_issued, 32, tok, 1);
    snprintf(r, sizeof r, "{\"token\":\"%s\",\"expires\":\"2026-10-26T20:00:00Z\",\"name\":\"%s\"}", tok, name);
    reply(c, 200, r);
}

static void serve_one(http_socket_t c)
{
    char buf[8192];
    int n = 0;
    for (;;) {
        int r = c_recv(c, buf + n, (int)sizeof buf - 1 - n);
        if (r <= 0) break;
        n += r;
        buf[n] = 0;
        if ((unsigned char)buf[0] == 0x16) break;   /* a TLS hello */
        char* end = strstr(buf, "\r\n\r\n");
        if (!end) continue;
        const char* cl = strstr(buf, "Content-Length: ");
        int want = cl ? atoi(cl + 16) : 0;
        if (n - (int)(end + 4 - buf) >= want) break;
    }
    buf[n] = 0;
    char* body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : buf + n;
    if (n > 0 && (unsigned char)buf[0] == 0x16) {   /* TLS, which this server doesn't speak: nothing to read */
        s_tls_hellos++;
    } else if (s_mode == NOT_OURS) {
        const char* page = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nContent-Length: 9\r\n\r\nnot found";
        c_send(c, page, (int)strlen(page));
    } else if (!strncmp(buf, "POST /dss/v1/challenge ", 23)) {
        s_challenges++;
        if (s_mode == BUSY_JSON) {
            reply(c, 503, "{\"error\":\"maintenance\",\"message\":\"The server is in maintenance.\"}");
        } else {
            char ch64[48], r[128];
            si_random_bytes(s_challenge, 32);
            s_challenge_live = 1;
            si_b64_encode(s_challenge, 32, ch64, 0);
            snprintf(r, sizeof r, "{\"challenge\":\"%s\",\"pow_bits\":%d,\"expires_in\":120}", ch64, s_bits);
            reply(c, 200, r);
        }
    } else if (!strncmp(buf, "POST /dss/v1/session ", 21)) {
        s_sessions++;
        session_call(c, body);
    } else if (!strncmp(buf, "GET /dss/v1/players ", 20) || !strncmp(buf, "POST /dss/v1/players/", 21)) {
        char tok[48], want[96];
        si_b64_encode(s_issued, 32, tok, 1);
        snprintf(want, sizeof want, "\r\nX-DSS-Session: %s\r\n", tok);
        if (s_stale_once || !strstr(buf, want)) {   /* a session the server no longer takes */
            s_stale_once = 0;
            reply(c, 401, "{\"error\":\"session\",\"message\":\"Sign in again.\"}");
        } else if (buf[0] == 'G') {
            reply(c, 200, s_players);
        } else {
            snprintf(s_player_post, sizeof s_player_post, "%s %s",
                     strstr(buf, "/report ") ? "report" : strstr(buf, "/erase ") ? "erase" : "block", body);
            reply(c, s_post_status, s_post_reply);
        }
        s_player_calls++;
    } else if (!strncmp(buf, "GET /dss/v1/banners ", 20)) {
        assert(!strstr(buf, "X-DSS-Session"));
        s_banner_gets++;
        reply(c, 200, s_banners);
    } else {
        snprintf(s_game_request, sizeof s_game_request, "%s", buf);   /* before the count the test waits on */
        s_games++;
        reply(c, 200, "ok");
    }
    ps3_tls_close(s_conn_tls);
    s_conn_tls = NULL;
    si_close(c);
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
        if (s_tls) {
            s_conn_tls = tts_accept((intptr_t)c, TTS_OURS, 3000);
            if (!s_conn_tls) {
                si_close(c);
                continue;
            }
            s_tls_conns++;
        }
        serve_one(c);
    }
    return 0;
}

/* ---- cellHttp's side ------------------------------------------------ */

enum { SCHEME = 0x100, HOST = 0x110, PATH = 0x160, METHOD = 0x1A0, URI = 0x200, CLIENT = 0x300, TRANS = 0x304,
       SENT = 0x308 };

static void put_str(u32 ea, const char* s) { strcpy((char*)vm_base + ea, s); }

/* A POST through cellHttp to game.test:port; 1 if it reached the server. */
static int game_request(u16 port)
{
    vm_write32(URI + 0, SCHEME);
    vm_write32(URI + 4, HOST);
    vm_write32(URI + 8, 0);
    vm_write32(URI + 12, 0);
    vm_write32(URI + 16, PATH);
    vm_write32(URI + 20, port);
    vm_write32(URI + 24, 0);
    assert(cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, vm_read32(CLIENT),
                                     (const char*)(uintptr_t)METHOD, (const CellHttpUri*)(uintptr_t)URI) == CELL_OK);
    CellHttpTransId t = vm_read32(TRANS);
    int before = s_games;
    s32 r = cellHttpSendRequest(t, NULL, 0, (u32*)(uintptr_t)SENT);
    for (int i = 0; i < 500 && s_games == before && r == CELL_OK; i++) {   /* the server thread records it */
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
    }
    cellHttpDestroyTransaction(t);
    return r == CELL_OK && s_games > before;
}

static void nap(void)
{
#ifdef _WIN32
    Sleep(10);
#else
    usleep(10000);
#endif
}

/* Waits for the players thread's next list after `seen`; its state. */
static int next_list(unsigned seen)
{
    unsigned serial = seen;
    int state = -1;
    for (int i = 0; i < 500 && serial == seen; i++) {
        nap();
        ps3_net_players(NULL, 0, &state, NULL, &serial, NULL, 0);
    }
    assert(serial != seen);
    return state;
}

/* Waits for the outcome of a block or report: *ok, and the line in out. */
static void next_result(int* ok, char* out, size_t cap)
{
    for (int i = 0; i < 500 && !ps3_net_players_result(ok, out, cap); i++) nap();
}

static long file_size(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* SHA-256: FIPS 180-4's examples, and a message across the length's block. */
    assert(sha_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    assert(sha_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    assert(sha_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    {
        char* a = (char*)malloc(1000000);
        memset(a, 'a', 1000000);
        ps3_sha256_ctx c;
        uint8_t h[32], w[32];
        ps3_sha256_init(&c);
        for (int i = 0; i < 1000000; i += 999) ps3_sha256_update(&c, a + i, i + 999 <= 1000000 ? 999 : 1000000 - i);
        ps3_sha256_final(&c, h);
        hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", w, 32);
        assert(!memcmp(h, w, 32));
        free(a);
    }

    /* Ed25519: RFC 8032 section 7.1, tests 1 and 2. */
    {
        static const struct { const char *seed, *pub, *msg, *sig; } v[] = {
            {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
             "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
             "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
            {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
             "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
             "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        };
        for (int i = 0; i < 2; i++) {
            uint8_t seed[32], sk[64], pk[32], want_pk[32], msg[1], sig[64], want_sig[64];
            size_t ml = strlen(v[i].msg) / 2;
            hex(v[i].seed, seed, 32);
            hex(v[i].pub, want_pk, 32);
            hex(v[i].msg, msg, ml);
            hex(v[i].sig, want_sig, 64);
            crypto_ed25519_key_pair(sk, pk, seed);
            assert(!memcmp(pk, want_pk, 32));
            crypto_ed25519_sign(sig, sk, msg, ml);
            assert(!memcmp(sig, want_sig, 64));
            assert(crypto_ed25519_check(sig, pk, msg, ml) == 0);
            sig[5] ^= 1;
            assert(crypto_ed25519_check(sig, pk, msg, ml) != 0);
        }
    }

    /* Base64 both ways, and text that does not hold the asked length. */
    {
        uint8_t in[32], back[32];
        char t[64];
        for (int i = 0; i < 32; i++) in[i] = (uint8_t)(i * 37 + 250);
        si_b64_encode(in, 32, t, 0);
        assert(strlen(t) == 44 && t[43] == '=' && si_b64_decode(t, back, 32, 0) == 0 && !memcmp(in, back, 32));
        si_b64_encode(in, 32, t, 1);
        assert(strlen(t) == 43 && !strchr(t, '=') && !strchr(t, '+') && !strchr(t, '/'));
        assert(si_b64_decode(t, back, 32, 1) == 0 && !memcmp(in, back, 32));
        assert(si_b64_decode(t, back, 32, 0) != 0 || !strpbrk(t, "-_"));
        assert(si_b64_decode("AAAA", back, 32, 0) != 0 && si_b64_decode("", back, 32, 1) != 0);
        si_b64_encode((const uint8_t*)"f", 1, t, 0);
        assert(!strcmp(t, "Zg=="));
    }

    /* Replies: members at the top level only, escapes, nesting, bad text. */
    {
        char s[64];
        long long n = 0;
        const char* j = "{\"message\":\"say \\\"token\\\": x\",\"nested\":{\"token\":\"inner\"},\"list\":[1,{\"a\":\"b\"},null],"
                        "\"token\":\"outer\",\"n\":-42,\"t\":true,\"u\":\"caf\\u00e9 \\ud83d\\ude00\"}";
        assert(si_js_str(j, "token", s, sizeof s) && !strcmp(s, "outer"));
        assert(si_js_str(j, "message", s, sizeof s) && !strcmp(s, "say \"token\": x"));
        assert(si_js_int(j, "n", &n) && n == -42);
        assert(si_js_str(j, "u", s, sizeof s) && !strcmp(s, "caf\xC3\xA9 \xF0\x9F\x98\x80"));
        assert(!si_js_str(j, "missing", s, sizeof s) && !si_js_str(j, "n", s, sizeof s) && !si_js_int(j, "token", &n));
        assert(!si_js_str("{\"token\":\"no end", "token", s, sizeof s));
        assert(!si_js_str("[\"token\",\"x\"]", "token", s, sizeof s));
        assert(!si_js_str("{\"a\":tru,\"token\":\"x\"}", "token", s, sizeof s));
        assert(si_js_str("{\"token\":\"abcdefghij\"}", "token", s, 5) && !strcmp(s, "abcd"));   /* cut to fit */
        assert(!si_js_member("not json", "a") && !si_js_member("", "a"));
    }

    /* Times */
    assert(si_parse_rfc3339("1970-01-01T00:00:00Z") == 0);
    assert(si_parse_rfc3339("2026-10-26T20:00:00Z") == 1793044800LL);
    assert(si_parse_rfc3339("2026-10-26T22:00:00.123+02:00") == 1793044800LL);
    assert(si_parse_rfc3339("2026-10-26 20:00:00") == -1 && si_parse_rfc3339("garbage") == -1);
    assert(si_parse_http_date("Sun, 25 Oct 2026 20:00:00 GMT") == 1793044800LL - 86400);
    assert(si_parse_http_date("Sun, 25 Foo 2026 20:00:00 GMT") == -1);

    /* The work: a nonce the check agrees with. */
    {
        uint8_t ch[32] = {1}, pk[32] = {2}, in[72], h[32];
        uint64_t nonce = 0;
        assert(si_do_work(ch, pk, 12, &nonce) == 0);
        memcpy(in, ch, 32);
        memcpy(in + 32, pk, 32);
        for (int i = 0; i < 8; i++) in[64 + i] = (uint8_t)(nonce >> (56 - 8 * i));
        ps3_sha256(in, 72, h);
        assert(si_zero_bits(h) >= 12);
    }

    /* ---- end to end on loopback ---- */
    vm_base = (uint8_t*)calloc(1, 65536);
    put_str(SCHEME, "http");
    put_str(HOST, "game.test");
    put_str(PATH, "/demons-souls-us/ss.info");
    put_str(METHOD, "POST");
    assert(cellHttpInit(NULL, 0) == CELL_OK);   /* starts Winsock */
    assert(cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) == CELL_OK);

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
    HANDLE th = CreateThread(NULL, 0, server_main, NULL, 0, NULL);
    assert(th);
#else
    pthread_t th;
    assert(pthread_create(&th, NULL, server_main, NULL) == 0);
#endif

    char key[512], addr[64], notice[256], tok[64];
    uint8_t raw[32];
#ifdef _WIN32
    char tmp[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp);
    snprintf(key, sizeof key, "%stest_net_signin_%lu.key", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(key, sizeof key, "/tmp/test_net_signin_%ld.key", (long)getpid());
#endif
    remove(key);   /* (this run's own file) */
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);

    /* Off without the knobs, or offline. */
    env("PS3_NET_ONLINE", "1");
    env("PS3_NET_HOSTS", "game.test=127.0.0.1");
    assert(!ps3_net_signin_enabled() && !ps3_net_session(tok, sizeof tok, raw));
    assert(!ps3_net_signin_banner(notice, sizeof notice) && !si_banner_started);
    assert(game_request(port) && !strstr(s_game_request, "X-DSS-Session"));
    env("PS3_NET_SIGNIN", addr);
    env("PS3_NET_KEY_FILE", key);
    env("PS3_NET_CLIENT_VERSION", "0.2.0");
    env("PS3_NET_NETWORK_VERSION", "3");
    env("PS3_NET_ONLINE", "");
    assert(!ps3_net_signin_enabled() && !ps3_net_session(tok, sizeof tok, raw) && s_challenges == 0);
    env("PS3_NET_ONLINE", "1");

    /* A new key: made, written, and it does the work at once. */
    assert(ps3_net_session(tok, sizeof tok, raw));
    assert(file_size(key) == 32 && s_challenges == 1 && s_sessions == 1 && !memcmp(raw, s_issued, 32));
    assert(si_end - time(NULL) > 86400 - 60 && si_end - time(NULL) <= 86400);   /* 24 hours by the server's clock */
    assert(ps3_net_signin_server((const uint8_t*)"\x7f\x00\x00\x01") && !ps3_net_signin_server((const uint8_t*)"\x0a\x00\x00\x01"));

    /* Kept until it is near its end: no new calls. */
    assert(ps3_net_session(tok, sizeof tok, NULL) && s_sessions == 1);

    /* The game's requests to that server carry it. */
    assert(game_request(port));
    {
        char want[96];
        snprintf(want, sizeof want, "\r\nX-DSS-Session: %s\r\n", tok);
        assert(strstr(s_game_request, want));
    }

    /* Signing in again with a known key: no work. */
    ps3_net_session_drop();
    assert(ps3_net_session(NULL, 0, raw) && s_sessions == 2 && !memcmp(raw, s_issued, 32));

    /* A server that does not know the key asks for the work, and gets it. */
    s_nknown = 0;
    ps3_net_session_drop();
    assert(ps3_net_session(NULL, 0, raw) && s_challenges == 4 && s_sessions == 4 && s_nknown == 1);
    assert(!ps3_net_signin_notice(notice, sizeof notice));

    /* Banners: each new one once, the oldest first, control characters made
     * spaces. The ask goes without the session. */
    si_banner_started = 1;   /* (no thread here: the test asks itself) */
    assert(!ps3_net_signin_banner(notice, sizeof notice));
    s_banners = "{\"banners\":[{\"id\":3,\"text\":\"Servers restart at 20:00 UTC.\"},"
                "{\"id\":4,\"text\":\"Two\\nlines\",\"ends\":\"2026-10-26T20:00:00Z\"}]}";
    assert(si_banners_fetch() && s_banner_gets == 1);
    assert(ps3_net_signin_banner(notice, sizeof notice) && !strcmp(notice, "Servers restart at 20:00 UTC."));
    assert(ps3_net_signin_banner(notice, sizeof notice) && !strcmp(notice, "Two lines"));
    assert(!ps3_net_signin_banner(notice, sizeof notice));
    s_banners = "{\"banners\":[{\"id\":4,\"text\":\"Two\\nlines\"},{\"id\":5,\"text\":\"New\"}]}";
    assert(si_banners_fetch() && s_banner_gets == 2);
    assert(ps3_net_signin_banner(notice, sizeof notice) && !strcmp(notice, "New"));
    assert(!ps3_net_signin_banner(notice, sizeof notice));
    s_banners = "{\"banners\":[]}";
    assert(si_banners_fetch() && !ps3_net_signin_banner(notice, sizeof notice));

    /* Players met: the list, a block, a report, each with the session. */
    {
        ps3_net_player pl[4];
        int state = -1, days = 0, ok = -1;
        unsigned seen = 0;
        char line[256];
        ps3_net_players(NULL, 0, &state, NULL, &seen, NULL, 0);
        assert(state == PS3_NET_PLAYERS_NONE);
        s_players = "{\"days\":30,\"players\":["
                    "{\"name\":\"Biorr\",\"fingerprint\":\"a1b2c3d4e5f60718\",\"met\":\"invasion\",\"host\":true,"
                    "\"at\":\"2026-10-26T20:00:00.25Z\",\"blocked\":false,\"reported\":false,\"reportable\":true},"
                    "{\"name\":\"Garl\",\"fingerprint\":\"0011223344556677\",\"met\":\"\",\"host\":false,"
                    "\"at\":\"2026-10-25T20:00:00Z\",\"blocked\":true,\"reported\":true,\"reportable\":false}]}";
        assert(ps3_net_players_refresh() == 0);
        assert(next_list(seen) == PS3_NET_PLAYERS_READY);
        int n = ps3_net_players(pl, 4, &state, &days, &seen, line, sizeof line);
        assert(n == 2 && days == 30 && !line[0]);
        assert(!strcmp(pl[0].name, "Biorr") && !strcmp(pl[0].fingerprint, "a1b2c3d4e5f60718") &&
               !strcmp(pl[0].met, "invasion") && pl[0].host && pl[0].at == 1793044800LL && !pl[0].blocked &&
               !pl[0].reported && pl[0].reportable);
        assert(!strcmp(pl[1].name, "Garl") && !pl[1].met[0] && !pl[1].host && pl[1].blocked && pl[1].reported &&
               !pl[1].reportable && pl[1].at == 1793044800LL - 86400);
        assert(ps3_net_players(pl, 1, NULL, NULL, NULL, NULL, 0) == 1);

        /* A block: the body, the line for the player, and the list again. */
        assert(ps3_net_player_block("Biorr", 1) == 0);
        next_result(&ok, line, sizeof line);
        assert(ok == 1 && !strcmp(line, "Blocked Biorr. You won't meet them online again."));
        assert(!strcmp(s_player_post, "block {\"name\":\"Biorr\",\"blocked\":true}"));
        assert(next_list(seen) == PS3_NET_PLAYERS_READY);
        ps3_net_players(NULL, 0, NULL, NULL, &seen, NULL, 0);

        /* Not sent: an unknown reason, a name the server's rules refuse. */
        assert(ps3_net_player_report("Biorr", "rude") == -1 && ps3_net_player_report("bad name", "other") == -1);
        assert(ps3_net_player_block("x\"y", 1) == -1);

        /* A refusal: the server's words. */
        s_post_status = 409;
        s_post_reply = "{\"error\":\"reported\",\"message\":\"You reported this player today already.\"}";
        assert(ps3_net_player_report("Biorr", "cheating") == 0);
        next_result(&ok, line, sizeof line);
        assert(ok == 0 && !strcmp(line, "You reported this player today already."));
        assert(!strcmp(s_player_post, "report {\"name\":\"Biorr\",\"reason\":\"cheating\"}"));
        assert(next_list(seen) == PS3_NET_PLAYERS_READY);
        ps3_net_players(NULL, 0, NULL, NULL, &seen, NULL, 0);
        s_post_status = 200;
        s_post_reply = "{\"ok\":true}";

        /* A session the server no longer takes: signed in again, once. */
        int sessions = s_sessions;
        s_stale_once = 1;
        assert(ps3_net_players_refresh() == 0);
        assert(next_list(seen) == PS3_NET_PLAYERS_READY && s_sessions == sessions + 1);
        ps3_net_players(NULL, 0, NULL, NULL, &seen, NULL, 0);

        /* A reply that is not a list: failed, with a line for the player. */
        s_players = "{\"error\":\"server\",\"message\":\"The server could not do that now.\"}";
        assert(ps3_net_players_refresh() == 0);
        assert(next_list(seen) == PS3_NET_PLAYERS_FAILED);
        ps3_net_players(NULL, 0, NULL, NULL, &seen, line, sizeof line);
        assert(!strcmp(line, "The server could not do that now."));
        s_players = "{\"days\":30,\"players\":[]}";
    }

    /* Over TLS (ps3emu/ps3_tls.h): the sign-in, the players calls and the
     * game's requests to that server; checked by name, then by address (the
     * test certificate has both). A server without TLS then gets no request,
     * only hellos, and the player hears it once. */
    {
        char ca[600];
        snprintf(ca, sizeof ca, "%s.ca.pem", key);
        tts_write_ca(ca);
        env("PS3_NET_CA_FILE", ca);
        env("PS3_NET_TLS", "dss.test");
        s_tls = 1;
        int sessions = s_sessions, conns = s_tls_conns;
        ps3_net_session_drop();
        assert(ps3_net_session(tok, sizeof tok, raw) && s_sessions == sessions + 1 && !memcmp(raw, s_issued, 32));
        assert(s_tls_conns == conns + 2);   /* the challenge and the session */
        assert(game_request(port) && s_tls_conns == conns + 3);
        {
            char want[96];
            snprintf(want, sizeof want, "\r\nX-DSS-Session: %s\r\n", tok);
            assert(strstr(s_game_request, want));
        }
        unsigned seen = 0;
        ps3_net_players(NULL, 0, NULL, NULL, &seen, NULL, 0);
        assert(ps3_net_players_refresh() == 0 && next_list(seen) == PS3_NET_PLAYERS_READY);
        assert(s_tls_conns == conns + 4);

        env("PS3_NET_TLS", "1");   /* the name is PS3_NET_SIGNIN's host: 127.0.0.1 */
        ps3_net_session_drop();
        assert(ps3_net_session(tok, sizeof tok, raw) && s_sessions == sessions + 2);

        s_tls = 0;
        int challenges = s_challenges, games = s_games, hellos = s_tls_hellos;
        ps3_net_session_drop();
        assert(!ps3_net_session(tok, sizeof tok, raw) && s_challenges == challenges);
        assert(ps3_net_signin_notice(notice, sizeof notice) && strstr(notice, "securely"));
        assert(!game_request(port) && s_games == games);
        for (int i = 0; i < 500 && s_tls_hellos < hellos + 2; i++) nap();
        assert(s_tls_hellos == hellos + 2);
        assert(!ps3_net_signin_notice(notice, sizeof notice));   /* (once) */

        env("PS3_NET_TLS", "");
        remove(ca);
        ps3_net_session_drop();
        assert(ps3_net_session(tok, sizeof tok, raw));   /* plain again */
    }

    /* A refusal: no session, the server's message once, then no new try for a while. */
    s_mode = BANNED;
    ps3_net_session_drop();
    assert(!ps3_net_session(tok, sizeof tok, raw));
    assert(ps3_net_signin_notice(notice, sizeof notice) && !strcmp(notice, "This account is banned (case 12)."));
    assert(!ps3_net_signin_notice(notice, sizeof notice));
    int calls = s_challenges;
    assert(!ps3_net_session(tok, sizeof tok, raw) && s_challenges == calls);
    s_games = 0;
    assert(game_request(port) && !strstr(s_game_request, "X-DSS-Session"));   /* the request goes without one */
    {   /* and the players list says why there is none */
        unsigned seen = 0;
        char line[256];
        int before = s_player_calls;
        ps3_net_players(NULL, 0, NULL, NULL, &seen, NULL, 0);
        assert(ps3_net_players_refresh() == 0 && next_list(seen) == PS3_NET_PLAYERS_FAILED);
        ps3_net_players(NULL, 0, NULL, NULL, NULL, line, sizeof line);
        assert(strstr(line, "Not signed in") && s_player_calls == before);
    }

    /* Maintenance at the challenge: its message. */
    s_mode = BUSY_JSON;
    ps3_net_session_drop();
    assert(!ps3_net_session(tok, sizeof tok, raw));
    assert(ps3_net_signin_notice(notice, sizeof notice) && strstr(notice, "maintenance"));

    /* A server with no sign-in, and none at all: quietly none. */
    s_mode = NOT_OURS;
    ps3_net_session_drop();
    assert(!ps3_net_session(tok, sizeof tok, raw) && !ps3_net_signin_notice(notice, sizeof notice));
    assert(!si_banners_fetch() && !ps3_net_signin_banner(notice, sizeof notice));
    env("PS3_NET_SIGNIN", "127.0.0.1:1");
    ps3_net_session_drop();
    assert(!ps3_net_session(tok, sizeof tok, raw) && !ps3_net_signin_notice(notice, sizeof notice));

    /* Another server's address gets no header and no sign-in. */
    s_mode = NORMAL;
    env("PS3_NET_SIGNIN", "10.255.255.1");
    ps3_net_session_drop();
    calls = s_challenges;
    assert(game_request(port) && !strstr(s_game_request, "X-DSS-Session") && s_challenges == calls);

    /* A key file that is not a key is left alone. */
    env("PS3_NET_SIGNIN", addr);
    {
        FILE* f = fopen(key, "wb");
        fwrite("short", 1, 5, f);
        fclose(f);
    }
    ps3_net_session_drop();
    assert(!ps3_net_session(tok, sizeof tok, raw) && file_size(key) == 5);
    assert(ps3_net_signin_notice(notice, sizeof notice) && strstr(notice, "key"));

    /* A name the server's rules refuse is not sent. */
    remove(key);
    s_name = "x";
    ps3_net_session_drop();
    calls = s_challenges;
    assert(!ps3_net_session(tok, sizeof tok, raw) && s_challenges == calls && file_size(key) == -1);

    /* Deleting one's data: the call goes with the session, the run is
     * offline after it, and nothing signs in again. (Last: the lock is for
     * the rest of the run.) */
    s_name = "Ashen-01";
    ps3_net_session_drop();
    assert(ps3_net_session(tok, sizeof tok, raw));
    {
        int ok = -1, kept = -1;
        char line[256];
        s_post_status = 200;
        s_post_reply = "{\"ok\":true,\"kept\":true}";
        assert(ps3_net_player_erase() == 0);
        for (int i = 0; i < 500 && !ps3_net_erase_result(&ok, &kept, line, sizeof line); i++) nap();
        assert(ok == 1 && kept == 1 && strstr(line, "deleted") && strstr(line, "name stays"));
        assert(!strcmp(s_player_post, "erase {}"));
        assert(!ps3_net_erase_result(&ok, &kept, line, sizeof line));   /* (once) */
        calls = s_challenges;
        assert(ps3_net_locked_offline() && !ps3_net_signin_enabled());
        assert(!ps3_net_session(tok, sizeof tok, raw) && s_challenges == calls);
        assert(ps3_net_player_erase() == -1 && ps3_net_players_refresh() == -1);
    }

    s_stop = 1;
    si_close(s_listen);
    remove(key);
    printf("test_net_signin: OK\n");
    return 0;
}
