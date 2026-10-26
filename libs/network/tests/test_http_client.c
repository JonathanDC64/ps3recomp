/*
 * test_http_client - the cellHttp client and transaction calls Demon's Souls
 * makes besides send and receive, and the cellHttpUtil Base64 pair:
 * - ids start at 1 (a title reads 0 as "none");
 * - the firmware's error values (ALREADY_INITIALIZED from a second Init);
 * - ClientGetUserAgent sized with no buffer, then read; ClientSetUserAgent
 *   reaching the request, and a title's own User-Agent header winning;
 * - ClientSetKeepAlive(0) adding "Connection: close";
 * - ClientPollConnections: 0 with nothing ready, the transaction once its
 *   response arrived;
 * - RequestSetContentLength, ResponseGetStatusCode, TransactionAbortConnection
 *   and ClientCloseAllConnections;
 * - Base64Encoder / Base64Decoder returning the length they wrote;
 * - ParseUri the SDK way: size query with uri NULL, then the strings in the
 *   caller's pool behind a CellHttpUri of guest pointers.
 * Loopback only; nothing leaves the PC.
 *
 *   clang-cl /I include /I third_party/mbedtls/include /Fe:test_http_client.exe libs/network/tests/test_http_client.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c mbedtls.lib ws2_32.lib
 *   cc -std=gnu17 -I include -I third_party/mbedtls/include -o test_http_client libs/network/tests/test_http_client.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c libmbedtls.a
 */
#include "../cellHttp.c"
#include "../cellHttpUtil.c"
#include "../ps3_net.c"
#include "../ps3_net_signin.c"
#include "../ps3_tls.c"

/* The sign-in's name (np_psnr.c has the real one); the sign-in is off here. */
const char* np_psnr_online_id(void) { return "PS3Player"; }

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

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* Guest layout */
enum { SCHEME = 0x100, HOST = 0x110, PATH = 0x160, METHOD = 0x1A0, HNAME = 0x1B0, HVALUE = 0x1C0,
       URI = 0x200, CLIENT = 0x300, TRANS = 0x304, SENT = 0x308, REQ = 0x30C, OUT32 = 0x310,
       UA = 0x400, UA2 = 0x500, B64IN = 0x600, B64OUT = 0x700, B64DEC = 0x800,
       URL = 0x900, PURI = 0x980, PREQ = 0x9A0, POOL = 0xA00 };

static void put_str(u32 ea, const char* s) { strcpy((char*)vm_base + ea, s); }

static CellHttpTransId new_trans(CellHttpClientId client, u16 port)
{
    vm_write32(URI + 0,  SCHEME);
    vm_write32(URI + 4,  HOST);
    vm_write32(URI + 8,  0);
    vm_write32(URI + 12, 0);
    vm_write32(URI + 16, PATH);
    vm_write32(URI + 20, port);
    vm_write32(URI + 24, 0);
    assert(cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, client,
                                     (const char*)(uintptr_t)METHOD, (const CellHttpUri*)(uintptr_t)URI) == CELL_OK);
    return vm_read32(TRANS);
}

/* Read the request headers the client sent. */
static void read_request(http_socket_t c, char* buf, int cap)
{
    int n = 0;
    while (n < cap - 1) {
        int r = recv(c, buf + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += r;
        buf[n] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    buf[n] = 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, 65536);
    put_str(SCHEME, "http");
    put_str(HOST, "game.test");
    put_str(PATH, "/cgi-bin/login.spd");
    put_str(METHOD, "POST");

    assert(cellHttpInit(NULL, 0) == CELL_OK);
    assert((u32)cellHttpInit(NULL, 0) == 0x80710001u);   /* ALREADY_INITIALIZED */

    http_socket_t l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    assert(bind(l, (struct sockaddr*)&a, sizeof a) == 0 && listen(l, 4) == 0);
    assert(getsockname(l, (struct sockaddr*)&a, &al) == 0);
    u16 port = ntohs(a.sin_port);
    env("PS3_NET_ONLINE", "1");
    env("PS3_NET_HOSTS", "game.test=127.0.0.1");

    /* Ids are never 0; a bad one is BAD_CLIENT. */
    assert(cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) == CELL_OK);
    CellHttpClientId client = vm_read32(CLIENT);
    assert(client == 1);
    assert((u32)cellHttpClientSetKeepAlive(0, 1) == 0x80710010u);
    assert((u32)cellHttpClientSetKeepAlive(9, 1) == 0x80710010u);
    assert(cellHttpClientSetKeepAlive(client, 1) == CELL_OK);
    assert(cellHttpClientSetPerHostPoolSize(client, 4) == CELL_OK);

    /* User-Agent: the size first (with the NUL), then the string. */
    vm_write32(REQ, 0);
    assert(cellHttpClientGetUserAgent(client, NULL, 0, (u32*)(uintptr_t)REQ) == CELL_OK);
    u32 need = vm_read32(REQ);
    assert(need == strlen(HTTP_DEFAULT_USER_AGENT) + 1);
    assert((u32)cellHttpClientGetUserAgent(client, (char*)(uintptr_t)UA, need - 1, NULL) == 0x80710006u);
    assert(cellHttpClientGetUserAgent(client, (char*)(uintptr_t)UA, need, NULL) == CELL_OK);
    assert(!strcmp((char*)vm_base + UA, HTTP_DEFAULT_USER_AGENT));
    assert((u32)cellHttpClientGetUserAgent(client, NULL, 0, NULL) == 0x80710004u);   /* NO_BUFFER */
    put_str(UA2, "DemonsSouls/1.0");
    assert(cellHttpClientSetUserAgent(client, (const char*)(uintptr_t)UA2) == CELL_OK);
    assert((u32)cellHttpClientSetUserAgent(client, NULL) == 0x80710004u);

    /* Poll with nothing sent: 0. */
    vm_write32(TRANS, 0xFFFFFFFFu);
    assert(cellHttpClientPollConnections(client, (CellHttpTransId*)(uintptr_t)TRANS, 0) == CELL_OK);
    assert(vm_read32(TRANS) == 0);

    /* A request carries the client's User-Agent and the length set ahead. */
    CellHttpTransId t = new_trans(client, port);
    assert(t == 1);
    assert(cellHttpRequestSetContentLength(t, 0) == CELL_OK);
    assert(cellHttpSendRequest(t, NULL, 0, (u32*)(uintptr_t)SENT) == CELL_OK);
    http_socket_t c = accept(l, NULL, NULL);
    char req[4096];
    read_request(c, req, sizeof req);
    printf("request 1:\n%s", req);
    assert(!strncmp(req, "POST /cgi-bin/login.spd HTTP/1.1\r\n", 34));
    assert(strstr(req, "\r\nUser-Agent: DemonsSouls/1.0\r\n"));
    assert(strstr(req, "\r\nContent-Length: 0\r\n"));
    assert(!strstr(req, "Connection:"));

    /* Sent, no answer yet: the poll waits 50 ms and names nothing. Then the
     * answer arrives and the poll names this transaction. */
    vm_write32(TRANS, 0xFFFFFFFFu);
    assert(cellHttpClientPollConnections(client, (CellHttpTransId*)(uintptr_t)TRANS, 50000) == CELL_OK);
    assert(vm_read32(TRANS) == 0);
    const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    send(c, resp, (int)strlen(resp), 0);
    assert(cellHttpClientPollConnections(client, (CellHttpTransId*)(uintptr_t)TRANS, 2000000) == CELL_OK);
    assert(vm_read32(TRANS) == t);
    assert(cellHttpResponseGetStatusCode(t, (s32*)(uintptr_t)OUT32) == CELL_OK);
    assert(vm_read32(OUT32) == 200);

    /* Aborting the connection: later calls on it report TRANS_ABORTED. */
    assert(cellHttpTransactionAbortConnection(t) == CELL_OK);
    assert((u32)cellHttpRecvResponse(t, (void*)(uintptr_t)UA, 2, (u32*)(uintptr_t)OUT32) == 0x80710024u);
    assert(cellHttpDestroyTransaction(t) == CELL_OK);
    assert((u32)cellHttpDestroyTransaction(t) == 0x80710011u);   /* BAD_TRANS */
    http_closesocket(c);

    /* Keep-alive off adds "Connection: close"; a title's own User-Agent wins. */
    assert(cellHttpClientSetKeepAlive(client, 0) == CELL_OK);
    t = new_trans(client, port);
    put_str(HNAME, "user-agent");
    put_str(HVALUE, "Mine");
    assert(cellHttpAddRequestHeader(t, (const char*)(uintptr_t)HNAME, (const char*)(uintptr_t)HVALUE) == CELL_OK);
    assert(cellHttpSendRequest(t, NULL, 0, (u32*)(uintptr_t)SENT) == CELL_OK);
    c = accept(l, NULL, NULL);
    read_request(c, req, sizeof req);
    printf("request 2:\n%s", req);
    assert(strstr(req, "\r\nConnection: close\r\n"));
    assert(strstr(req, "\r\nuser-agent: Mine\r\n"));
    assert(!strstr(req, "DemonsSouls/1.0"));

    /* Closing all the client's connections leaves nothing to poll. */
    assert(cellHttpClientCloseAllConnections(client) == CELL_OK);
    vm_write32(TRANS, 0xFFFFFFFFu);
    assert(cellHttpClientPollConnections(client, (CellHttpTransId*)(uintptr_t)TRANS, 0) == CELL_OK);
    assert(vm_read32(TRANS) == 0);
    cellHttpDestroyTransaction(t);
    http_closesocket(c);

    /* Base64: the encoder returns the text length and writes a NUL after it;
     * the decoder returns the byte count. */
    memcpy(vm_base + B64IN, "\x01\x02\x03\x04\xFF", 5);
    memset(vm_base + B64OUT, 0x7F, 16);
    s32 n = cellHttpUtilBase64Encoder((char*)(uintptr_t)B64OUT, (const void*)(uintptr_t)B64IN, 5);
    printf("base64: %d \"%s\"\n", n, (char*)vm_base + B64OUT);
    assert(n == 8 && !strcmp((char*)vm_base + B64OUT, "AQIDBP8="));
    memset(vm_base + B64DEC, 0x7F, 16);
    n = cellHttpUtilBase64Decoder((char*)(uintptr_t)B64DEC, (const void*)(uintptr_t)B64OUT, 8);
    assert(n == 5 && !memcmp(vm_base + B64DEC, "\x01\x02\x03\x04\xFF", 5) && vm_base[B64DEC + 5] == 0x7F);
    put_str(B64OUT, "TWFu");
    assert(cellHttpUtilBase64Decoder((char*)(uintptr_t)B64DEC, (const void*)(uintptr_t)B64OUT, 4) == 3);
    assert(!memcmp(vm_base + B64DEC, "Man", 3));
    put_str(B64OUT, "QQ==");
    assert(cellHttpUtilBase64Decoder((char*)(uintptr_t)B64DEC, (const void*)(uintptr_t)B64OUT, 4) == 1);
    assert(vm_base[B64DEC] == 'A');
    assert((u32)cellHttpUtilBase64Decoder((char*)(uintptr_t)B64DEC, (const void*)(uintptr_t)B64OUT, 3) == 0x80711009u);
    put_str(B64OUT, "QQ!=");
    assert((u32)cellHttpUtilBase64Decoder((char*)(uintptr_t)B64DEC, (const void*)(uintptr_t)B64OUT, 4) == 0x8071100Au);
    assert((u32)cellHttpUtilBase64Encoder((char*)(uintptr_t)B64OUT, NULL, 3) == 0x80711003u);

    /* ParseUri: the size query first (Demon's Souls' order), then the parse. */
    put_str(URL, "http://u:pw@c.demons-souls.com:18000/demons-souls-us/ss.info?x=1#frag");
    vm_write32(PREQ, 0);
    assert(cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL, NULL, 0, (u32*)(uintptr_t)PREQ) == CELL_OK);
    need = vm_read32(PREQ);
    /* "http" + "c.demons-souls.com" + "/demons-souls-us/ss.info?x=1" + "u" + "pw", each NUL-ended */
    printf("parseuri: need %u\n", need);
    assert(need == 5 + 19 + 29 + 2 + 3);
    assert((u32)cellHttpUtilParseUri((struct CellHttpUri*)(uintptr_t)PURI, (const char*)(uintptr_t)URL,
                                     (void*)(uintptr_t)POOL, need - 1, NULL) == 0x80711004u);   /* INSUFFICIENT */
    memset(vm_base + POOL, 0x7F, 128);
    assert(cellHttpUtilParseUri((struct CellHttpUri*)(uintptr_t)PURI, (const char*)(uintptr_t)URL,
                                (void*)(uintptr_t)POOL, need, NULL) == CELL_OK);
#define PSTR(off) ((const char*)vm_base + vm_read32(PURI + (off)))
    assert(!strcmp(PSTR(0), "http") && !strcmp(PSTR(4), "c.demons-souls.com"));
    assert(!strcmp(PSTR(8), "u") && !strcmp(PSTR(12), "pw"));
    assert(!strcmp(PSTR(16), "/demons-souls-us/ss.info?x=1") && vm_read32(PURI + 20) == 18000);
    assert(vm_read32(PURI + 0) == POOL && vm_read32(PURI + 4) == POOL + 5 && vm_read32(PURI + 16) == POOL + 24);
    assert(vm_base[POOL + need] == 0x7F);
    /* defaults: no path is "/", no port is 80 (443 for https), empty user/password */
    put_str(URL, "https://example.test");
    assert(cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL, NULL, 0, (u32*)(uintptr_t)PREQ) == CELL_OK);
    need = vm_read32(PREQ);
    assert(cellHttpUtilParseUri((struct CellHttpUri*)(uintptr_t)PURI, (const char*)(uintptr_t)URL,
                                (void*)(uintptr_t)POOL, need, NULL) == CELL_OK);
    assert(!strcmp(PSTR(16), "/") && vm_read32(PURI + 20) == 443 && !*PSTR(8) && !*PSTR(12));
    put_str(URL, "http://example.test:80x/");
    assert((u32)cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL, NULL, 0, (u32*)(uintptr_t)PREQ) == 0x80711005u);
    put_str(URL, "example.test/x");
    assert((u32)cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL, NULL, 0, (u32*)(uintptr_t)PREQ) == 0x80711005u);
    assert((u32)cellHttpUtilParseUri(NULL, NULL, NULL, 0, (u32*)(uintptr_t)PREQ) == 0x80711003u);   /* NO_STRING */
    assert((u32)cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL, NULL, 0, NULL) == 0x80711002u); /* NO_BUFFER */
    /* the parsed CellHttpUri goes straight into CreateTransaction */
    put_str(URL, "http://game.test/x.spd");
    assert(cellHttpUtilParseUri(NULL, (const char*)(uintptr_t)URL, NULL, 0, (u32*)(uintptr_t)PREQ) == CELL_OK);
    need = vm_read32(PREQ);
    assert(cellHttpUtilParseUri((struct CellHttpUri*)(uintptr_t)PURI, (const char*)(uintptr_t)URL,
                                (void*)(uintptr_t)POOL, need, NULL) == CELL_OK);
    assert(cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, client, (const char*)(uintptr_t)METHOD,
                                     (const CellHttpUri*)(uintptr_t)PURI) == CELL_OK);
    CellHttpTransId pt = vm_read32(TRANS);
    assert(http_trans_ok(pt) && !strcmp(HTTP_TRANS(pt)->path, "/x.spd") && HTTP_TRANS(pt)->port == 80);
    cellHttpDestroyTransaction(pt);

    env("PS3_NET_HOSTS", "");
    env("PS3_NET_ONLINE", "");
    http_closesocket(l);
    cellHttpDestroyClient(client);
    cellHttpEnd();
    printf("test_http_client: all passed\n");
    return 0;
}
