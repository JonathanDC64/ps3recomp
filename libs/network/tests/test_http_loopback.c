/*
 * test_http_loopback - cellHttp end to end against a loopback listener.
 *
 * Every pointer the title passes is a guest address: the URI fields (path at
 * +16, the username at +8), the POST body, the header name and value, and
 * the buffer the response body lands in. Also the network gate in front of
 * it: offline nothing connects, and with PS3_NET_HOSTS set only listed
 * names do. And PS3_HTTP_HEADER's extra header. Nothing leaves the PC.
 *
 *   clang-cl /I include /Fe:test_http_loopback.exe libs/network/tests/test_http_loopback.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_http_loopback libs/network/tests/test_http_loopback.c
 */
#include "../cellHttp.c"
#include "../ps3_net.c"

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
enum { SCHEME = 0x100, HOST = 0x110, OTHER = 0x130, USER = 0x150, PATH = 0x160, METHOD = 0x1A0,
       HNAME = 0x1B0, HVALUE = 0x1C0, URI = 0x200, CLIENT = 0x300, TRANS = 0x304, SENT = 0x308,
       RECVD = 0x30C, BODY = 0x400, RBUF = 0x800 };

static void put_str(u32 ea, const char* s) { strcpy((char*)vm_base + ea, s); }

static void put_uri(u32 host_ea, u16 port)
{
    vm_write32(URI + 0,  SCHEME);
    vm_write32(URI + 4,  host_ea);
    vm_write32(URI + 8,  USER);     /* a username: the old +8 read sent it as the path */
    vm_write32(URI + 12, 0);
    vm_write32(URI + 16, PATH);
    vm_write32(URI + 20, port);
    vm_write32(URI + 24, 0);
}

static CellHttpTransId new_trans(CellHttpClientId client, u32 host_ea, u16 port)
{
    put_uri(host_ea, port);
    assert(cellHttpCreateTransaction((CellHttpTransId*)(uintptr_t)TRANS, client,
                                     (const char*)(uintptr_t)METHOD, (const CellHttpUri*)(uintptr_t)URI) == CELL_OK);
    return vm_read32(TRANS);
}

/* Read what the client sent, up to and including `body_len` body bytes. */
static int read_request(http_socket_t c, char* buf, int cap, int body_len)
{
    int n = 0;
    for (;;) {
        int r = recv(c, buf + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += r;
        buf[n] = 0;
        const char* end = strstr(buf, "\r\n\r\n");
        if (end && n - (int)(end + 4 - buf) >= body_len) break;
    }
    buf[n] = 0;
    return n;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, 65536);
    put_str(SCHEME, "http");
    put_str(HOST, "game.test");
    put_str(OTHER, "localhost");
    put_str(USER, "player");
    put_str(PATH, "/demons-souls-us/ss.info");
    put_str(METHOD, "POST");
    put_str(HNAME, "X-Test");
    put_str(HVALUE, "yes");
    memcpy(vm_base + BODY, "BODY123", 7);

    assert(cellHttpInit(NULL, 0) == CELL_OK);   /* starts Winsock */

    http_socket_t l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    assert(bind(l, (struct sockaddr*)&a, sizeof a) == 0 && listen(l, 4) == 0);
    assert(getsockname(l, (struct sockaddr*)&a, &al) == 0);
    u16 port = ntohs(a.sin_port);

    /* Ids start at 1 (titles take 0 for "none"): a second client gets 2,
     * written big-endian for the guest. */
    assert(cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) == CELL_OK);
    assert(vm_read32(CLIENT) == 1);
    assert(cellHttpCreateClient((CellHttpClientId*)(uintptr_t)CLIENT) == CELL_OK);
    CellHttpClientId client = vm_read32(CLIENT);
    assert(client == 2);

    /* Offline: no connection at all. */
    env("PS3_NET_ONLINE", "");
    env("PS3_NET_HOSTS", "");
    CellHttpTransId t = new_trans(client, HOST, port);
    assert((u32)cellHttpSendRequest(t, (const void*)(uintptr_t)BODY, 7, (u32*)(uintptr_t)SENT)
           == CELL_HTTP_ERROR_CONNECTION_FAILED);
    cellHttpDestroyTransaction(t);

    /* Online with a map: an unlisted name fails closed, though the OS knows it. */
    env("PS3_NET_ONLINE", "1");
    env("PS3_NET_HOSTS", "GAME.test=127.0.0.1");
    t = new_trans(client, OTHER, port);
    assert((u32)cellHttpSendRequest(t, NULL, 0, (u32*)(uintptr_t)SENT) == CELL_HTTP_ERROR_CONNECTION_FAILED);
    cellHttpDestroyTransaction(t);

    /* The listed name reaches the listener with the right request line, the
     * header the title added and the body from guest memory. */
    t = new_trans(client, HOST, port);
    assert(cellHttpAddRequestHeader(t, (const char*)(uintptr_t)HNAME, (const char*)(uintptr_t)HVALUE) == CELL_OK);
    assert(cellHttpSendRequest(t, (const void*)(uintptr_t)BODY, 7, (u32*)(uintptr_t)SENT) == CELL_OK);
    assert(vm_read32(SENT) == 7);
    http_socket_t c = accept(l, NULL, NULL);   /* the connect completed from the backlog */
    char req[4096];
    read_request(c, req, sizeof req, 7);
    printf("request:\n%s\n", req);
    assert(!strncmp(req, "POST /demons-souls-us/ss.info HTTP/1.1\r\n", 40));
    assert(strstr(req, "\r\nHost: game.test\r\n"));
    assert(strstr(req, "\r\nContent-Length: 7\r\n"));
    assert(strstr(req, "\r\nX-Test: yes\r\n"));
    assert(!strcmp(strstr(req, "\r\n\r\n") + 4, "BODY123"));

    /* Headers first, read on their own; then the body, which RecvResponse
     * takes from the socket into the guest buffer. */
    const char* hdr = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n";
    send(c, hdr, (int)strlen(hdr), 0);
    assert(cellHttpRecvResponse(t, NULL, 0, (u32*)(uintptr_t)RECVD) == CELL_OK);
    assert(http_trans_ok(t) && HTTP_TRANS(t)->status_code == 200);
    send(c, "0123456789", 10, 0);
    memset(vm_base + RBUF, 0, 32);
    u32 got = 0;
    while (got < 10) {
        assert(cellHttpRecvResponse(t, (void*)(uintptr_t)(RBUF + got), 10 - got, (u32*)(uintptr_t)RECVD) == CELL_OK);
        u32 n = vm_read32(RECVD);
        assert(n > 0);
        got += n;
    }
    assert(!memcmp(vm_base + RBUF, "0123456789", 10));
    cellHttpDestroyTransaction(t);
    http_closesocket(c);

    /* PS3_HTTP_HEADER adds one header to each request; a malformed one, or a
     * name the title sent itself, adds nothing. */
    {
        static const struct { const char* env; const char* want; } cases[] = {
            {"", NULL},                              /* first: the header lines without it */
            {"X-DeS-Group:  ab12 ", "\r\nX-DeS-Group: ab12\r\n"},
            {"X-Test: no", NULL},                    /* the title's X-Test: yes stays the only one */
            {"Bad Name: x", NULL},
            {"X-Two: a\r\nX-Evil: b", NULL},
            {"X-Empty:   ", NULL},
        };
        int base = -1;
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            env("PS3_HTTP_HEADER", cases[k].env);
            t = new_trans(client, HOST, port);
            assert(cellHttpAddRequestHeader(t, (const char*)(uintptr_t)HNAME, (const char*)(uintptr_t)HVALUE) == CELL_OK);
            assert(cellHttpSendRequest(t, (const void*)(uintptr_t)BODY, 7, (u32*)(uintptr_t)SENT) == CELL_OK);
            c = accept(l, NULL, NULL);
            read_request(c, req, sizeof req, 7);
            const char* end = strstr(req, "\r\n\r\n");
            assert(end);
            int lines = 0;
            for (const char* q = req; q < end; q++)
                if (q[0] == '\r' && q[1] == '\n') lines++;
            if (base < 0) base = lines;
            if (cases[k].want) {
                assert(strstr(req, cases[k].want));
                assert(lines == base + 1);
            } else {
                assert(lines == base);
            }
            assert(strstr(req, "\r\nX-Test: yes\r\n"));
            cellHttpDestroyTransaction(t);
            http_closesocket(c);
        }
        env("PS3_HTTP_HEADER", "");
    }

    env("PS3_NET_HOSTS", "");
    http_closesocket(l);
    cellHttpEnd();
    printf("test_http_loopback: all passed\n");
    return 0;
}
