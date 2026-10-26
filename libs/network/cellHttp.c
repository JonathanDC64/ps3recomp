/*
 * ps3recomp - cellHttp HLE implementation
 *
 * Real HTTP client using native sockets (Winsock2 on Windows, POSIX elsewhere).
 * Resolves hostnames, connects via TCP, sends HTTP/1.1 requests, and receives
 * responses with header parsing and body streaming.
 */

#include "cellHttp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "ps3emu/ps3_net.h"   /* ps3_net_online, ps3_net_resolve */
#include "ps3emu/ps3_net_signin.h"   /* the session header */
#include "ps3emu/ps3_tls.h"   /* TLS to that server */
#include "ps3emu/endian.h"    /* ps3_bswap32 */

/* ---------------------------------------------------------------------------
 * Platform socket abstraction
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET http_socket_t;
#define HTTP_INVALID_SOCKET INVALID_SOCKET
#define http_closesocket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int http_socket_t;
#define HTTP_INVALID_SOCKET (-1)
#define http_closesocket close
#endif

/* Guest-EA marshalling: the generic HLE adapter passes PPC r3.. verbatim, so
 * pointer arguments are guest effective addresses, not host pointers. Translate
 * through vm_base before dereferencing. */
extern uint8_t* vm_base;
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int s_http_initialized = 0;
#ifdef _WIN32
static int s_wsa_initialized = 0;
#endif

typedef struct {
    int                  in_use;
    u32                  resolve_timeout;
    u32                  connect_timeout;
    u32                  send_timeout;
    u32                  recv_timeout;
    int                  keep_alive;
    u32                  per_host_pool;
    char                 user_agent[256];
} HttpClientSlot;

/* Custom header key/value pair */
typedef struct {
    char name[128];
    char value[512];
} HttpCustomHeader;

/* Header receive buffer size - enough for typical HTTP response headers */
#define HTTP_HDR_BUF_SIZE 8192

typedef struct {
    int                  in_use;
    CellHttpClientId     client;
    char                 method[16];
    char                 url[1024];
    char                 hostname[256];
    char                 path[512];
    u32                  port;
    s32                  status_code;
    u64                  content_length;
    int                  content_length_known;
    u64                  request_content_length;
    int                  request_content_length_set;
    int                  aborted;

    /* Custom request headers */
    HttpCustomHeader     custom_headers[CELL_HTTP_MAX_CUSTOM_HEADERS];
    u32                  custom_header_count;

    /* Socket and recv state */
    http_socket_t        sock;
    ps3_tls*             tls;              /* TLS on sock (the sign-in server's, PS3_NET_TLS), or NULL */
    int                  headers_parsed;
    u32                  body_received;
    int                  conn_close;       /* server sent Connection: close */
    int                  eof_reached;

    /* Buffer for accumulating response header bytes */
    char                 hdr_buf[HTTP_HDR_BUF_SIZE];
    u32                  hdr_buf_len;
    /* Leftover body bytes that arrived with the header read */
    char*                body_overflow;
    u32                  body_overflow_len;

    /* Transfer-Encoding: chunked: the body is decoded as it is read (http_recv_chunked) */
    int                  chunked;
    int                  ch_state;         /* CH_SIZE ... CH_FAILED */
    u64                  ch_left;          /* bytes left in the current chunk */
    char                 ch_line[80];      /* the start of the size or trailer line being read */
    u32                  ch_line_len;      /* that line's length so far */
    char                 ch_raw[2048];     /* the body as it came, before decoding */
    u32                  ch_pos, ch_len;
} HttpTransSlot;

enum { CH_SIZE, CH_DATA, CH_DATA_END, CH_TRAILER, CH_DONE, CH_FAILED };
#define HTTP_CHUNK_LINE_MAX 4096           /* a size line with its extensions, or a trailer line */

static HttpClientSlot s_clients[CELL_HTTP_MAX_CLIENTS];
static HttpTransSlot  s_transactions[CELL_HTTP_MAX_TRANSACTIONS];

/* Client and transaction ids are the slot + 1. The firmware's are never 0,
 * and titles take 0 for "none": Demon's Souls skips its whole network service
 * when its client id is 0, and reads a 0 from cellHttpClientPollConnections as
 * "no transaction ready". */
#define HTTP_CLIENT(id) (&s_clients[(id) - 1])
#define HTTP_TRANS(id)  (&s_transactions[(id) - 1])
static int http_client_ok(CellHttpClientId id)
{
    return id - 1u < CELL_HTTP_MAX_CLIENTS && s_clients[id - 1].in_use;
}
static int http_trans_ok(CellHttpTransId id)
{
    return id - 1u < CELL_HTTP_MAX_TRANSACTIONS && s_transactions[id - 1].in_use;
}

/* The User-Agent a new client starts with. The firmware's string isn't known
 * here; this has its form, and a title can read it and replace it. */
#define HTTP_DEFAULT_USER_AGENT "PS3Application libhttp/1.0.0-000 (CellOS)"

/* ---------------------------------------------------------------------------
 * Helpers
 * -----------------------------------------------------------------------*/

/* Apply a timeout (in microseconds) to the transaction's socket. */
static void http_apply_timeout(http_socket_t sock, int send, u32 usec)
{
    if (sock == HTTP_INVALID_SOCKET)
        return;

#ifdef _WIN32
    /* Winsock SO_RCVTIMEO/SO_SNDTIMEO takes milliseconds as DWORD */
    DWORD ms = usec / 1000;
    if (ms == 0 && usec > 0)
        ms = 1;
    setsockopt(sock, SOL_SOCKET, send ? SO_SNDTIMEO : SO_RCVTIMEO,
               (const char*)&ms, sizeof(ms));
#else
    struct timeval tv;
    tv.tv_sec  = usec / 1000000;
    tv.tv_usec = usec % 1000000;
    setsockopt(sock, SOL_SOCKET, send ? SO_SNDTIMEO : SO_RCVTIMEO,
               &tv, sizeof(tv));
#endif
}

/* Close the socket in a transaction slot if open, reset state. */
static void http_close_slot_socket(HttpTransSlot* t)
{
    ps3_tls_close(t->tls);
    t->tls = NULL;
    if (t->sock != HTTP_INVALID_SOCKET) {
        http_closesocket(t->sock);
        t->sock = HTTP_INVALID_SOCKET;
    }
    t->headers_parsed = 0;
    t->body_received  = 0;
    t->hdr_buf_len    = 0;
    t->conn_close     = 0;
    t->eof_reached    = 0;
    if (t->body_overflow) {
        free(t->body_overflow);
        t->body_overflow = NULL;
    }
    t->body_overflow_len = 0;
    t->chunked = 0;
}

/* PS3_HTTP_HEADER="Name: value": one more header on every request, for a
 * server that wants something the title never sends (Demon's Souls' port
 * sends its matchmaking group this way, to a server of the player's own).
 * Used only when it is a token name (letters, digits, '-'), a colon and one
 * line of printable ASCII, and the title did not add a header of that name.
 * Writes "Name" and "value" (spaces around the value trimmed); 0 if unset or
 * not usable. */
static int http_extra_header(char* name, size_t name_cap, char* value, size_t value_cap)
{
    const char* e = getenv("PS3_HTTP_HEADER");
    if (!e || !*e) return 0;
    const char* colon = strchr(e, ':');
    if (!colon || colon == e || (size_t)(colon - e) >= name_cap) return 0;
    for (const char* q = e; q < colon; q++)
        if (!((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '-'))
            return 0;
    const char* v = colon + 1;
    while (*v == ' ') v++;
    size_t vl = strlen(v);
    while (vl && v[vl - 1] == ' ') vl--;
    if (!vl || vl >= value_cap) return 0;
    for (size_t i = 0; i < vl; i++)
        if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7E) return 0;   /* (no CR or LF: one line) */
    memcpy(name, e, (size_t)(colon - e));
    name[colon - e] = 0;
    memcpy(value, v, vl);
    value[vl] = 0;
    return 1;
}

/* A piece of the request head is one line: no control characters but tab. */
static int http_one_line(const char* s)
{
    for (; *s; s++)
        if (((unsigned char)*s < 0x20 && *s != '\t') || (unsigned char)*s == 0x7F) return 0;
    return 1;
}

/* Append to the request head in buf[cap]. Once something does not fit, *len
 * is cap or more and stays there: the caller refuses the request. */
static void http_req_add(char* buf, size_t cap, size_t* len, const char* fmt, ...)
{
    if (*len >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    *len = n < 0 ? cap : *len + (size_t)n;
}

/* A client timeout (microseconds) for the TLS calls, which wait themselves:
 * 0 (none set) is a minute. */
static int http_tls_ms(u32 usec)
{
    return usec ? (int)(usec / 1000 ? usec / 1000 : 1) : 60000;
}

/* Send all bytes on a socket (over tls when set), handling partial sends.
 * Returns 0 on success. */
static int http_send_all(http_socket_t sock, ps3_tls* tls, u32 timeout_usec, const char* data, u32 len)
{
    if (tls) return ps3_tls_send_all(tls, data, len, http_tls_ms(timeout_usec));
    u32 total = 0;
    while (total < len) {
        int n = send(sock, data + total, (int)(len - total), 0);
        if (n <= 0)
            return -1;
        total += (u32)n;
    }
    return 0;
}

/* Find "\r\n\r\n" in a buffer, return pointer to start of body or NULL. */
static const char* http_find_header_end(const char* buf, u32 len)
{
    if (len < 4)
        return NULL;
    for (u32 i = 0; i <= len - 4; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' &&
            buf[i+2] == '\r' && buf[i+3] == '\n')
            return buf + i + 4;
    }
    return NULL;
}

/* Case-insensitive strstr for header matching. */
static const char* http_strcasestr(const char* haystack, const char* needle)
{
    if (!haystack || !needle)
        return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0)
        return haystack;
    for (; *haystack; haystack++) {
        int match = 1;
        for (size_t i = 0; i < nlen; i++) {
            char a = haystack[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = 0; break; }
        }
        if (match)
            return haystack;
    }
    return NULL;
}

/* Parse the response headers that have been accumulated in hdr_buf.
 * Extracts status code, Content-Length, Connection: close.
 * Returns 0 on success, -1 on parse failure. */
static int http_parse_response_headers(HttpTransSlot* t, const char* hdr_end)
{
    u32 hdr_len = (u32)(hdr_end - t->hdr_buf);

    /* Null-terminate the header section for string ops (we have room) */
    char saved = *hdr_end;
    /* We can't write past hdr_buf if hdr_end points there, but hdr_end is
     * within hdr_buf or at the boundary. We'll work with the length instead. */

    /* Parse status line: "HTTP/1.x SSS reason\r\n" */
    int major = 0, minor = 0, status = 0;
    if (sscanf(t->hdr_buf, "HTTP/%d.%d %d", &major, &minor, &status) < 3) {
        printf("[cellHttp]   Failed to parse status line\n");
        return -1;
    }
    t->status_code = (s32)status;
    printf("[cellHttp]   Response status: %d\n", status);

    /* Temporarily null-terminate header section for searching */
    char term_buf[HTTP_HDR_BUF_SIZE];
    if (hdr_len >= HTTP_HDR_BUF_SIZE)
        hdr_len = HTTP_HDR_BUF_SIZE - 1;
    memcpy(term_buf, t->hdr_buf, hdr_len);
    term_buf[hdr_len] = '\0';

    /* Content-Length */
    t->content_length_known = 0;
    const char* cl = http_strcasestr(term_buf, "content-length:");
    if (cl) {
        cl += 15; /* skip "content-length:" */
        while (*cl == ' ') cl++;
        t->content_length = (u64)strtoull(cl, NULL, 10);
        t->content_length_known = 1;
        printf("[cellHttp]   Content-Length: %llu\n",
               (unsigned long long)t->content_length);
    }

    /* Transfer-Encoding: chunked (net/http sends any answer over 2 KiB so): the body comes in chunks, decoded as it is
     * read, and a Content-Length beside it does not count (RFC 9112 6.3). Only that header's own line is looked at. */
    t->chunked = 0;
    const char* te = http_strcasestr(term_buf, "\ntransfer-encoding:");
    if (te) {
        char v[128];
        const char* eol = strchr(te + 1, '\n');
        size_t n = eol ? (size_t)(eol - te) : strlen(te);
        if (n >= sizeof v) n = sizeof v - 1;
        memcpy(v, te, n);
        v[n] = '\0';
        if (http_strcasestr(v, "chunked")) {
            t->chunked = 1;
            t->content_length_known = 0;
            t->ch_state = CH_SIZE;
            t->ch_left = 0;
            t->ch_line_len = 0;
            t->ch_pos = t->ch_len = 0;
            printf("[cellHttp]   Transfer-Encoding: chunked\n");
        }
    }

    /* Connection: close */
    t->conn_close = 0;
    const char* conn = http_strcasestr(term_buf, "connection:");
    if (conn) {
        conn += 11;
        while (*conn == ' ') conn++;
        if (http_strcasestr(conn, "close"))
            t->conn_close = 1;
    }

    /* Store leftover body bytes */
    u32 total_in_buf = t->hdr_buf_len;
    u32 body_start   = (u32)(hdr_end - t->hdr_buf);
    u32 leftover     = total_in_buf - body_start;
    if (leftover > 0) {
        t->body_overflow = (char*)malloc(leftover);
        if (t->body_overflow) {
            memcpy(t->body_overflow, hdr_end, leftover);
            t->body_overflow_len = leftover;
        }
    }

    t->headers_parsed = 1;
    return 0;
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(void* pool, u32 poolSize)
{
    printf("[cellHttp] Init(pool=%p, poolSize=%u)\n", pool, poolSize);

    if (s_http_initialized)
        return CELL_HTTP_ERROR_ALREADY_INITIALIZED;

#ifdef _WIN32
    if (!s_wsa_initialized) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            printf("[cellHttp] WSAStartup failed\n");
            return CELL_HTTP_ERROR_UNKNOWN;
        }
        s_wsa_initialized = 1;
    }
#endif

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));

    /* Ensure all sockets are marked invalid */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        s_transactions[i].sock = HTTP_INVALID_SOCKET;
    }

    s_http_initialized = 1;
    return CELL_OK;
}

s32 cellHttpEnd(void)
{
    printf("[cellHttp] End()\n");

    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    /* Close any open sockets */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (s_transactions[i].in_use)
            http_close_slot_socket(&s_transactions[i]);
    }

    memset(s_clients, 0, sizeof(s_clients));
    memset(s_transactions, 0, sizeof(s_transactions));

    /* Re-invalidate sockets after memset */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        s_transactions[i].sock = HTTP_INVALID_SOCKET;
    }

    s_http_initialized = 0;

#ifdef _WIN32
    if (s_wsa_initialized) {
        WSACleanup();
        s_wsa_initialized = 0;
    }
#endif

    return CELL_OK;
}

s32 cellHttpCreateClient(CellHttpClientId* clientId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!clientId)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    /* clientId is a raw GUEST effective address (the generic HLE adapter passes
     * r3 verbatim); translate it before writing or we fault on a host pointer. */
    CellHttpClientId* clientId_h = GUEST_PTR(clientId, CellHttpClientId*);

    for (u32 i = 0; i < CELL_HTTP_MAX_CLIENTS; i++) {
        if (!s_clients[i].in_use) {
            s_clients[i].in_use = 1;
            s_clients[i].resolve_timeout = 30000000; /* 30s default */
            s_clients[i].connect_timeout = 30000000;
            s_clients[i].send_timeout    = 120000000;
            s_clients[i].recv_timeout    = 120000000;
            s_clients[i].keep_alive      = 1;
            s_clients[i].per_host_pool   = 1;
            snprintf(s_clients[i].user_agent, sizeof(s_clients[i].user_agent), "%s",
                     HTTP_DEFAULT_USER_AGENT);
            *clientId_h = ps3_bswap32(i + 1);   /* guest big-endian */
            printf("[cellHttp] CreateClient(id=%u)\n", i + 1);
            return CELL_OK;
        }
    }

    return CELL_HTTP_ERROR_NO_MEMORY;
}

s32 cellHttpDestroyClient(CellHttpClientId clientId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;

    /* Destroy all transactions belonging to this client */
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (s_transactions[i].in_use && s_transactions[i].client == clientId) {
            http_close_slot_socket(&s_transactions[i]);
            s_transactions[i].in_use = 0;
        }
    }

    HTTP_CLIENT(clientId)->in_use = 0;
    printf("[cellHttp] DestroyClient(id=%u)\n", clientId);
    return CELL_OK;
}

s32 cellHttpCreateTransaction(CellHttpTransId* transId, CellHttpClientId clientId,
                              const char* method, const CellHttpUri* uri)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!method || !uri || !transId)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    /* CellHttpUri is { const char* scheme, hostname, username, password,
     * path; u32 port; u8 reserved[4]; } -- five 4-byte EAs, then the port,
     * so nothing but scheme lines up if you cast. Pull each field out by
     * guest offset: the path is at +16 (+8, the username, is usually NULL,
     * which sent every request as "GET /"). */
    u32 uri_ea = (u32)(uintptr_t)uri;
    u32 ea_scheme   = vm_read32(uri_ea +  0);
    u32 ea_hostname = vm_read32(uri_ea +  4);
    u32 ea_path     = vm_read32(uri_ea + 16);
    u32 uri_port    = vm_read32(uri_ea + 20);
    const char* uri_scheme   = ea_scheme   ? (const char*)(vm_base + ea_scheme)   : NULL;
    const char* uri_hostname = ea_hostname ? (const char*)(vm_base + ea_hostname) : NULL;
    const char* uri_path     = ea_path     ? (const char*)(vm_base + ea_path)     : NULL;
    method = GUEST_PTR(method, const char*);

    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;

    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        if (!s_transactions[i].in_use) {
            HttpTransSlot* t = &s_transactions[i];
            memset(t, 0, sizeof(*t));

            t->in_use  = 1;
            t->client  = clientId;
            t->sock    = HTTP_INVALID_SOCKET;
            t->aborted = 0;
            t->custom_header_count = 0;
            t->request_content_length_set = 0;

            strncpy(t->method, method, sizeof(t->method) - 1);
            t->method[sizeof(t->method) - 1] = '\0';

            /* Store hostname, path, port separately for socket connect */
            strncpy(t->hostname,
                    uri_hostname ? uri_hostname : "unknown",
                    sizeof(t->hostname) - 1);
            t->hostname[sizeof(t->hostname) - 1] = '\0';

            strncpy(t->path,
                    uri_path ? uri_path : "/",
                    sizeof(t->path) - 1);
            t->path[sizeof(t->path) - 1] = '\0';

            t->port = uri_port ? uri_port : 80;

            /* Build URL string for logging */
            snprintf(t->url, sizeof(t->url), "%s://%s:%u%s",
                     uri_scheme   ? uri_scheme   : "http",
                     t->hostname, t->port, t->path);

            vm_write32((u32)(uintptr_t)transId, (u32)i + 1);
            printf("[cellHttp] CreateTransaction(client=%u, %s %s) -> trans=%u\n",
                   clientId, method, t->url, i + 1);
            return CELL_OK;
        }
    }

    return CELL_HTTP_ERROR_NO_MEMORY;
}

s32 cellHttpDestroyTransaction(CellHttpTransId transId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    http_close_slot_socket(HTTP_TRANS(transId));
    HTTP_TRANS(transId)->in_use = 0;
    printf("[cellHttp] DestroyTransaction(trans=%u)\n", transId);
    return CELL_OK;
}

s32 cellHttpSendRequest(CellHttpTransId transId, const void* buf, u32 size,
                        u32* sent)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);

    if (t->aborted) {
        printf("[cellHttp] SendRequest(trans=%u) - transaction aborted\n", transId);
        return CELL_HTTP_ERROR_ABORTED;
    }

    printf("[cellHttp] SendRequest(trans=%u, %s %s, bodySize=%u)\n",
           transId, t->method, t->url, size);

    /* Close any previously open socket (re-send scenario) */
    http_close_slot_socket(t);

    /* ---- Resolve hostname ---- */
    /* Offline (PS3_NET_ONLINE unset) nothing leaves the PC, and online the
     * name goes through PS3_NET_HOSTS when it is set (ps3emu/ps3_net.h). */
    if (!ps3_net_online()) {
        printf("[cellHttp]   offline: no connection to '%s'\n", t->hostname);
        if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u16)t->port);
    if (ps3_net_resolve(t->hostname, (uint8_t*)&addr.sin_addr) != 0) {
        printf("[cellHttp]   could not resolve '%s'\n", t->hostname);
        if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    /* The sign-in's session (ps3emu/ps3_net_signin.h), for its server only;
     * taken before connecting, as signing in can take a moment. */
    char session[64];
    int has_session = ps3_net_signin_server((const uint8_t*)&addr.sin_addr) &&
                      ps3_net_session(session, sizeof session, NULL);

    /* ---- Create socket and connect ---- */
    http_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == HTTP_INVALID_SOCKET) {
        printf("[cellHttp]   socket() failed\n");
        if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    /* Apply timeouts from the owning client */
    HttpClientSlot* c = HTTP_CLIENT(t->client);
    http_apply_timeout(sock, 1, c->send_timeout);
    http_apply_timeout(sock, 0, c->recv_timeout);

    if (connect(sock, (struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        printf("[cellHttp]   connect() failed to %s:%u\n", t->hostname, t->port);
        http_closesocket(sock);
        if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    printf("[cellHttp]   Connected to %s:%u\n", t->hostname, t->port);

    /* TLS to the sign-in server when PS3_NET_TLS says it speaks it
     * (ps3emu/ps3_tls.h); then nothing goes to it without. */
    ps3_tls* tls = NULL;
    if (ps3_tls_enabled() && ps3_net_signin_server((const uint8_t*)&addr.sin_addr)) {
        char name[256], why[256] = "no server name (PS3_NET_TLS, PS3_NET_SIGNIN)";
        if (ps3_tls_server_name(name, sizeof name) == 0)
            tls = ps3_tls_connect((intptr_t)sock, name, http_tls_ms(c->connect_timeout), why, sizeof why);
        if (!tls) {
            printf("[cellHttp]   no secure connection to %s:%u: %s\n", t->hostname, t->port, why);
            http_closesocket(sock);
            if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
            return CELL_HTTP_ERROR_CONNECTION_FAILED;
        }
    }

    /* ---- Format HTTP request ---- */
    char req_buf[4096];
    size_t req_len = 0;
    http_req_add(req_buf, sizeof req_buf, &req_len,
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n",
                 t->method, t->path, t->hostname);

    /* The client's User-Agent, unless the title added its own; and
     * "Connection: close" when the title turned keep-alive off (this client
     * opens a new connection per request either way). */
    int ua_added = 0;
    for (u32 i = 0; i < t->custom_header_count; i++)
        if (http_strcasestr(t->custom_headers[i].name, "user-agent") == t->custom_headers[i].name &&
            strlen(t->custom_headers[i].name) == 10)
            ua_added = 1;
    if (!ua_added && c->user_agent[0])
        http_req_add(req_buf, sizeof req_buf, &req_len,
                            "User-Agent: %s\r\n", c->user_agent);
    if (!c->keep_alive)
        http_req_add(req_buf, sizeof req_buf, &req_len,
                            "Connection: close\r\n");

    /* Content-Length header if body provided or explicitly set */
    if (size > 0) {
        http_req_add(req_buf, sizeof req_buf, &req_len,
                            "Content-Length: %u\r\n", size);
    } else if (t->request_content_length_set) {
        http_req_add(req_buf, sizeof req_buf, &req_len,
                            "Content-Length: %llu\r\n",
                            (unsigned long long)t->request_content_length);
    }

    /* Append custom headers */
    for (u32 i = 0; i < t->custom_header_count; i++) {
        http_req_add(req_buf, sizeof req_buf, &req_len,
                            "%s: %s\r\n",
                            t->custom_headers[i].name,
                            t->custom_headers[i].value);
    }

    /* PS3_HTTP_HEADER, unless the title sent that name itself */
    {
        char xn[64], xv[256];
        if (http_extra_header(xn, sizeof xn, xv, sizeof xv)) {
            int dup = 0;
            for (u32 i = 0; i < t->custom_header_count; i++)
                if (http_strcasestr(t->custom_headers[i].name, xn) == t->custom_headers[i].name &&
                    strlen(t->custom_headers[i].name) == strlen(xn))
                    dup = 1;
            if (!dup)
                http_req_add(req_buf, sizeof req_buf, &req_len,
                                    "%s: %s\r\n", xn, xv);
        }
    }

    if (has_session)
        http_req_add(req_buf, sizeof req_buf, &req_len,
                            "X-DSS-Session: %s\r\n", session);

    /* End of headers */
    http_req_add(req_buf, sizeof req_buf, &req_len,
                        "\r\n");

    int lines_ok = http_one_line(t->method) && http_one_line(t->path) && http_one_line(t->hostname);
    for (u32 i = 0; i < t->custom_header_count && lines_ok; i++)
        lines_ok = http_one_line(t->custom_headers[i].name) && http_one_line(t->custom_headers[i].value);
    if (req_len >= sizeof req_buf || !lines_ok) {
        printf("[cellHttp]   Request refused: %s\n", lines_ok ? "its headers do not fit" : "a line break in it");
        ps3_tls_close(tls);
        http_closesocket(sock);
        if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
        return CELL_HTTP_ERROR_INVALID_PARAMETER;
    }

    /* ---- Send header ---- */
    if (http_send_all(sock, tls, c->send_timeout, req_buf, (u32)req_len) != 0) {
        printf("[cellHttp]   Failed to send request headers\n");
        ps3_tls_close(tls);
        http_closesocket(sock);
        if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
        return CELL_HTTP_ERROR_SEND_FAILED;
    }

    /* ---- Send body if provided ---- */
    if (buf && size > 0) {
        if (http_send_all(sock, tls, c->send_timeout, GUEST_PTR(buf, const char*), size) != 0) {
            printf("[cellHttp]   Failed to send request body\n");
            ps3_tls_close(tls);
            http_closesocket(sock);
            if (sent) vm_write32((u32)(uintptr_t)sent, (u32)0);
            return CELL_HTTP_ERROR_SEND_FAILED;
        }
    }

    printf("[cellHttp]   Request sent (%d header bytes + %u body bytes)%s\n",
           (int)req_len, size, tls ? " over TLS" : "");

    /* Store socket for recv */
    t->sock           = sock;
    t->tls            = tls;
    t->headers_parsed = 0;
    t->body_received  = 0;
    t->hdr_buf_len    = 0;
    t->eof_reached    = 0;
    t->body_overflow  = NULL;
    t->body_overflow_len = 0;

    if (sent)
        vm_write32((u32)(uintptr_t)sent, (u32)size);

    return CELL_OK;
}

/* recv() on the transaction's connection, over TLS when it has it: bytes,
 * 0 at its end, < 0 on an error. */
static int http_recv(HttpTransSlot* t, char* p, int n)
{
    if (t->tls) return ps3_tls_recv(t->tls, p, (size_t)n, http_tls_ms(HTTP_CLIENT(t->client)->recv_timeout));
    return recv(t->sock, p, n, 0);
}

/* The chunked decoder's input: the body bytes that came with the headers, then the connection's. 1 when ch_raw has
 * some, 0 at the end of the connection, < 0 on an error. */
static int chunk_fill(HttpTransSlot* t)
{
    if (t->ch_pos < t->ch_len) return 1;
    t->ch_pos = t->ch_len = 0;
    if (t->body_overflow_len) {
        u32 n = t->body_overflow_len < sizeof t->ch_raw ? t->body_overflow_len : (u32)sizeof t->ch_raw;
        memcpy(t->ch_raw, t->body_overflow, n);
        t->body_overflow_len -= n;
        memmove(t->body_overflow, t->body_overflow + n, t->body_overflow_len);
        t->ch_len = n;
        return 1;
    }
    int n = http_recv(t, t->ch_raw, (int)sizeof t->ch_raw);
    if (n <= 0) return n;
    t->ch_len = (u32)n;
    return 1;
}

/* One line of the chunked framing (a chunk size, the end of a chunk's data, or a trailer), without its CR LF. The
 * start goes to ch_line; a longer line counts on but is cut there (only a size and its extensions, or a trailer we
 * skip). 1 when the line is complete; < 0 when the body ended or failed first, or the line runs past
 * HTTP_CHUNK_LINE_MAX. */
static int chunk_line(HttpTransSlot* t)
{
    for (;;) {
        int f = chunk_fill(t);
        if (f <= 0) return f < 0 ? f : -1;
        char c = t->ch_raw[t->ch_pos++];
        if (c == '\n') {
            u32 n = t->ch_line_len < sizeof t->ch_line ? t->ch_line_len : (u32)sizeof t->ch_line - 1;
            if (n && t->ch_line[n - 1] == '\r') n--;
            t->ch_line[n] = '\0';
            t->ch_line_len = 0;
            return 1;
        }
        if (++t->ch_line_len > HTTP_CHUNK_LINE_MAX) return -1;
        if (t->ch_line_len < sizeof t->ch_line) t->ch_line[t->ch_line_len - 1] = c;
    }
}

/* A chunked body into dst, up to size bytes: the chunks' data in order, the framing left out. Stops at the last
 * chunk and its trailers (then every call gives 0 bytes, the end, as with a Content-Length). A malformed or cut
 * framing fails the transaction's body. */
static s32 http_recv_chunked(HttpTransSlot* t, char* dst, u32 size, u32* got)
{
    u32 filled = 0;
    while (filled < size && t->ch_state != CH_DONE && t->ch_state != CH_FAILED) {
        if (t->ch_state == CH_SIZE) {
            if (chunk_line(t) <= 0) goto bad;
            const char* s = t->ch_line;
            u64 v = 0;
            int digits = 0;
            while (*s == ' ' || *s == '\t') s++;
            for (;; s++, digits++) {
                int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
                      : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
                if (d < 0) break;
                v = v * 16 + (u64)d;
                if (v > 0xFFFFFFFFull - t->body_received) goto bad;   /* (body_received is 32-bit) */
            }
            while (*s == ' ' || *s == '\t') s++;
            if (!digits || (*s && *s != ';')) goto bad;
            t->ch_left = v;
            t->ch_state = v ? CH_DATA : CH_TRAILER;
        } else if (t->ch_state == CH_DATA) {
            if (chunk_fill(t) <= 0) goto bad;
            u32 n = t->ch_len - t->ch_pos;
            if (n > t->ch_left) n = (u32)t->ch_left;
            if (n > size - filled) n = size - filled;
            memcpy(dst + filled, t->ch_raw + t->ch_pos, n);
            t->ch_pos += n;
            t->ch_left -= n;
            t->body_received += n;
            filled += n;
            if (!t->ch_left) t->ch_state = CH_DATA_END;
        } else if (t->ch_state == CH_DATA_END) {
            if (chunk_line(t) <= 0 || t->ch_line[0]) goto bad;     /* the CR LF after a chunk's data */
            t->ch_state = CH_SIZE;
        } else {                                                 /* CH_TRAILER: lines up to an empty one */
            if (chunk_line(t) <= 0) goto bad;
            if (!t->ch_line[0]) t->ch_state = CH_DONE;
        }
    }
    *got = filled;
    return CELL_OK;
bad:
    printf("[cellHttp]   chunked body: malformed or cut short after %u bytes\n", t->body_received);
    t->ch_state = CH_FAILED;
    *got = filled;
    return filled ? CELL_OK : CELL_HTTP_ERROR_RECV_FAILED;
}

s32 cellHttpRecvResponse(CellHttpTransId transId, void* buf, u32 size,
                         u32* received)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);

    if (t->aborted) {
        printf("[cellHttp] RecvResponse(trans=%u) - transaction aborted\n", transId);
        if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
        return CELL_HTTP_ERROR_ABORTED;
    }

    if (t->sock == HTTP_INVALID_SOCKET) {
        printf("[cellHttp] RecvResponse(trans=%u) - no socket (send first)\n",
               transId);
        if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
        return CELL_HTTP_ERROR_CONNECTION_FAILED;
    }

    /* ---- Parse response headers on first call ---- */
    if (!t->headers_parsed) {
        printf("[cellHttp] RecvResponse(trans=%u) - reading response headers\n",
               transId);

        /* Read until we find \r\n\r\n */
        while (t->hdr_buf_len < HTTP_HDR_BUF_SIZE - 1) {
            int n = http_recv(t, t->hdr_buf + t->hdr_buf_len,
                              (int)(HTTP_HDR_BUF_SIZE - 1 - t->hdr_buf_len));
            if (n <= 0) {
                printf("[cellHttp]   recv() failed during header read (n=%d)\n", n);
                http_close_slot_socket(t);
                if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
                return (n == 0) ? CELL_HTTP_ERROR_RECV_FAILED
                                : CELL_HTTP_ERROR_RECV_FAILED;
            }
            t->hdr_buf_len += (u32)n;

            const char* hdr_end = http_find_header_end(t->hdr_buf,
                                                       t->hdr_buf_len);
            if (hdr_end) {
                if (http_parse_response_headers(t, hdr_end) != 0) {
                    http_close_slot_socket(t);
                    if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
                    return CELL_HTTP_ERROR_RECV_FAILED;
                }
                break;
            }
        }

        if (!t->headers_parsed) {
            printf("[cellHttp]   Header buffer overflow, no \\r\\n\\r\\n found\n");
            http_close_slot_socket(t);
            if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
            return CELL_HTTP_ERROR_RECV_FAILED;
        }
    }

    /* ---- Return body data ---- */
    if (!buf || size == 0) {
        if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
        return CELL_OK;
    }

    if (t->chunked) {
        u32 got = 0;
        s32 r = t->ch_state == CH_FAILED ? CELL_HTTP_ERROR_RECV_FAILED
                                         : http_recv_chunked(t, GUEST_PTR(buf, char*), size, &got);
        if (received) vm_write32((u32)(uintptr_t)received, got);
        return r;
    }

    /* Check if we've already received all expected body bytes */
    if (t->content_length_known && t->body_received >= t->content_length) {
        if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
        return CELL_OK;
    }

    if (t->eof_reached) {
        if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
        return CELL_OK;
    }

    u32 filled = 0;

    /* First drain any leftover body bytes from the header read */
    if (t->body_overflow && t->body_overflow_len > 0) {
        u32 copy = t->body_overflow_len;
        if (copy > size)
            copy = size;
        memcpy(GUEST_PTR(buf, void*), t->body_overflow, copy);
        filled += copy;
        t->body_received += copy;

        if (copy < t->body_overflow_len) {
            /* Still have overflow remaining */
            u32 remain = t->body_overflow_len - copy;
            memmove(t->body_overflow, t->body_overflow + copy, remain);
            t->body_overflow_len = remain;
        } else {
            free(t->body_overflow);
            t->body_overflow = NULL;
            t->body_overflow_len = 0;
        }
    }

    /* Read more from socket if caller wants more */
    while (filled < size) {
        /* If content-length known, don't read past it */
        u32 want = size - filled;
        if (t->content_length_known) {
            u64 remaining = t->content_length - t->body_received;
            if (remaining == 0)
                break;
            if (want > remaining)
                want = (u32)remaining;
        }

        if (want == 0)
            break;

        /* buf is a guest address (the overflow copy above already translates
         * it); recv() into the raw value wrote to the host address of the same
         * number whenever a body outgrew the header read. */
        int n = http_recv(t, GUEST_PTR(buf, char*) + filled, (int)want);
        if (n < 0) {
            /* Error - if we already have some data, return it */
            if (filled > 0)
                break;
            printf("[cellHttp]   recv() error during body read\n");
            if (received) vm_write32((u32)(uintptr_t)received, (u32)0);
            return CELL_HTTP_ERROR_RECV_FAILED;
        }
        if (n == 0) {
            /* Connection closed by peer */
            t->eof_reached = 1;
            if (!t->content_length_known) {
                /* For Connection: close responses, EOF is normal end */
                t->content_length = t->body_received;
                t->content_length_known = 1;
            }
            break;
        }

        filled += (u32)n;
        t->body_received += (u32)n;
    }

    if (received)
        vm_write32((u32)(uintptr_t)received, (u32)filled);

    return CELL_OK;
}

/* The response getters under their SDK names (the NIDs are computed from
 * these names); the older cellHttpGetStatusCode / cellHttpGetResponseContentLength
 * below keep their names. Titles call these after cellHttpSendRequest and
 * before the first cellHttpRecvResponse, so they read the response headers
 * first. Without them the three imports took the unresolved-NID path (CELL_OK,
 * the code unset, a NULL header array) and a title gave up on every response. */
static HttpTransSlot* http_resp_slot(CellHttpTransId transId, s32* err)
{
    *err = CELL_OK;
    if (!s_http_initialized) { *err = CELL_HTTP_ERROR_NOT_INITIALIZED; return NULL; }
    if (!http_trans_ok(transId)) { *err = CELL_HTTP_ERROR_BAD_TRANS; return NULL; }
    HttpTransSlot* t = HTTP_TRANS(transId);
    if (!t->headers_parsed) {
        *err = cellHttpRecvResponse(transId, NULL, 0, NULL);   /* reads and parses the headers only */
        if (*err != CELL_OK) return NULL;
    }
    return t;
}

s32 cellHttpResponseGetStatusCode(CellHttpTransId transId, s32* code)
{
    s32 err;
    HttpTransSlot* t = http_resp_slot(transId, &err);
    if (!t) return err;
    if (!code) return CELL_HTTP_ERROR_INVALID_PARAMETER;
    vm_write32((u32)(uintptr_t)code, (u32)t->status_code);
    return CELL_OK;
}

s32 cellHttpResponseGetContentLength(CellHttpTransId transId, u64* length)
{
    s32 err;
    HttpTransSlot* t = http_resp_slot(transId, &err);
    if (!t) return err;
    if (!length) return CELL_HTTP_ERROR_INVALID_PARAMETER;
    if (!t->content_length_known) return CELL_HTTP_ERROR_NO_CONTENT_LENGTH;
    vm_write64((u32)(uintptr_t)length, (u64)t->content_length);
    return CELL_OK;
}

/* CellHttpHeader is { const char* name; const char* value; } (two guest pointers). With no pool (or one too small)
 * only *required is written; with a pool of *required bytes the array comes first, then the NUL-terminated strings,
 * and *headers / *items point at it. The header bytes are the server's: only copied, never longer than the
 * header buffer. */
s32 cellHttpResponseGetAllHeaders(CellHttpTransId transId, void* headers, u32* items, void* pool, u32 poolSize,
                                  u32* required)
{
    s32 err;
    HttpTransSlot* t = http_resp_slot(transId, &err);
    if (!t) return err;
    const char* end = http_find_header_end(t->hdr_buf, t->hdr_buf_len);
    if (!end) return CELL_HTTP_ERROR_RECV_FAILED;
    const char* p = memchr(t->hdr_buf, '\n', (size_t)(end - t->hdr_buf));   /* skip the status line */
    u32 pool_ea = (u32)(uintptr_t)pool;
    u32 count = 0, strings = 0;
    for (int pass = 0; pass < 2; pass++) {
        u32 at = pool_ea + count * 8u, k = 0;          /* pass 1: strings after the array of `count` entries */
        for (const char* l = p ? p + 1 : end; l < end;) {
            const char* nl = memchr(l, '\n', (size_t)(end - l));
            const char* le = nl ? nl : end;
            const char* lt = (le > l && le[-1] == '\r') ? le - 1 : le;
            const char* colon = memchr(l, ':', (size_t)(lt - l));
            if (colon && colon > l) {
                const char* v = colon + 1;
                while (v < lt && (*v == ' ' || *v == '\t')) v++;
                u32 nlen = (u32)(colon - l), vlen = (u32)(lt - v);
                if (pass == 0) { count++; strings += nlen + 1 + vlen + 1; }
                else {
                    memcpy(vm_base + at, l, nlen); vm_base[at + nlen] = 0;
                    vm_write32(pool_ea + k * 8u, at);
                    at += nlen + 1;
                    memcpy(vm_base + at, v, vlen); vm_base[at + vlen] = 0;
                    vm_write32(pool_ea + k * 8u + 4u, at);
                    at += vlen + 1;
                    k++;
                }
            }
            l = le + 1;
        }
        if (pass == 0) {
            u32 need = count * 8u + strings;
            if (required) vm_write32((u32)(uintptr_t)required, need);
            if (!pool_ea || poolSize < need) {
                if (items) vm_write32((u32)(uintptr_t)items, 0);
                return pool_ea ? CELL_HTTP_ERROR_NO_BUFFER : CELL_OK;
            }
        }
    }
    if (headers) vm_write32((u32)(uintptr_t)headers, pool_ea);
    if (items) vm_write32((u32)(uintptr_t)items, count);
    return CELL_OK;
}

s32 cellHttpGetResponseContentLength(CellHttpTransId transId, u64* length)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    if (!length)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    vm_write64((u32)(uintptr_t)length, (u64)HTTP_TRANS(transId)->content_length);
    return CELL_OK;
}

s32 cellHttpGetStatusCode(CellHttpTransId transId, s32* code)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    if (!code)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    vm_write32((u32)(uintptr_t)code, (u32)HTTP_TRANS(transId)->status_code);
    return CELL_OK;
}

s32 cellHttpSetResolveTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);
    HttpClientSlot* c = HTTP_CLIENT(t->client);
    c->resolve_timeout = usec;
    printf("[cellHttp] SetResolveTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetConnectTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);
    HttpClientSlot* c = HTTP_CLIENT(t->client);
    c->connect_timeout = usec;
    printf("[cellHttp] SetConnectTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetSendTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);
    HttpClientSlot* c = HTTP_CLIENT(t->client);
    c->send_timeout = usec;

    /* Apply immediately if socket is already open */
    if (t->sock != HTTP_INVALID_SOCKET)
        http_apply_timeout(t->sock, 1, usec);

    printf("[cellHttp] SetSendTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetRecvTimeOut(CellHttpTransId transId, u32 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);
    HttpClientSlot* c = HTTP_CLIENT(t->client);
    c->recv_timeout = usec;

    /* Apply immediately if socket is already open */
    if (t->sock != HTTP_INVALID_SOCKET)
        http_apply_timeout(t->sock, 0, usec);

    printf("[cellHttp] SetRecvTimeOut(trans=%u, %u us)\n", transId, usec);
    return CELL_OK;
}

s32 cellHttpSetRequestContentLength(CellHttpTransId transId, u64 length)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HTTP_TRANS(transId)->request_content_length     = length;
    HTTP_TRANS(transId)->request_content_length_set = 1;
    printf("[cellHttp] SetRequestContentLength(trans=%u, %llu)\n",
           transId, (unsigned long long)length);
    return CELL_OK;
}

s32 cellHttpAddRequestHeader(CellHttpTransId transId, const char* name,
                             const char* value)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    if (!name || !value)
        return CELL_HTTP_ERROR_INVALID_PARAMETER;

    HttpTransSlot* t = HTTP_TRANS(transId);
    if (t->custom_header_count >= CELL_HTTP_MAX_CUSTOM_HEADERS) {
        printf("[cellHttp] AddRequestHeader - too many custom headers\n");
        return CELL_HTTP_ERROR_NO_BUFFER;
    }

    HttpCustomHeader* h = &t->custom_headers[t->custom_header_count];
    strncpy(h->name, GUEST_PTR(name, const char*), sizeof(h->name) - 1);
    h->name[sizeof(h->name) - 1] = '\0';
    strncpy(h->value, GUEST_PTR(value, const char*), sizeof(h->value) - 1);
    h->value[sizeof(h->value) - 1] = '\0';
    if (!h->name[0] || strchr(h->name, ':') || !http_one_line(h->name) || !http_one_line(h->value)) {
        printf("[cellHttp] AddRequestHeader - not one header line, refused\n");
        return CELL_HTTP_ERROR_INVALID_PARAMETER;
    }
    t->custom_header_count++;

    printf("[cellHttp] AddRequestHeader(trans=%u, '%s: %s')\n",
           transId, h->name, h->value);
    return CELL_OK;
}

s32 cellHttpAbortTransaction(CellHttpTransId transId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;

    if (!http_trans_ok(transId))
        return CELL_HTTP_ERROR_BAD_TRANS;

    HttpTransSlot* t = HTTP_TRANS(transId);
    t->aborted = 1;
    http_close_slot_socket(t);
    printf("[cellHttp] AbortTransaction(trans=%u)\n", transId);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Client and transaction settings under their SDK names. Demon's Souls calls
 * all of these; unregistered they returned CELL_OK with no effect, so
 * cellHttpClientGetUserAgent left its buffer and size unwritten and
 * cellHttpClientPollConnections never named a transaction.
 * -----------------------------------------------------------------------*/

s32 cellHttpClientSetKeepAlive(CellHttpClientId clientId, u32 enable)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;
    HTTP_CLIENT(clientId)->keep_alive = (enable & 0xFF) != 0;   /* b8 */
    return CELL_OK;
}

s32 cellHttpClientSetPerHostPoolSize(CellHttpClientId clientId, u32 poolSize)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;
    HTTP_CLIENT(clientId)->per_host_pool = poolSize;   /* kept; one connection per request here */
    return CELL_OK;
}

s32 cellHttpClientSetUserAgent(CellHttpClientId clientId, const char* userAgent)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;
    if (!userAgent)
        return CELL_HTTP_ERROR_NO_BUFFER;
    HttpClientSlot* c = HTTP_CLIENT(clientId);
    snprintf(c->user_agent, sizeof(c->user_agent), "%s", GUEST_PTR(userAgent, const char*));
    printf("[cellHttp] ClientSetUserAgent(client=%u, '%s')\n", clientId, c->user_agent);
    return CELL_OK;
}

/* *required is the size with the NUL. With no buffer (or size 0) only
 * *required is written, which titles use to size the buffer. */
s32 cellHttpClientGetUserAgent(CellHttpClientId clientId, char* userAgent, u32 size, u32* required)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;
    const char* ua = HTTP_CLIENT(clientId)->user_agent;
    u32 need = (u32)strlen(ua) + 1;
    if (required)
        vm_write32((u32)(uintptr_t)required, need);
    if (!userAgent || !size)
        return required ? CELL_OK : CELL_HTTP_ERROR_NO_BUFFER;
    if (size < need)
        return CELL_HTTP_ERROR_INSUFFICIENT;
    memcpy(GUEST_PTR(userAgent, char*), ua, need);
    return CELL_OK;
}

s32 cellHttpClientCloseAllConnections(CellHttpClientId clientId)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++)
        if (s_transactions[i].in_use && s_transactions[i].client == clientId)
            http_close_slot_socket(&s_transactions[i]);
    return CELL_OK;
}

/* Names one of the client's transactions whose response can be read, waiting
 * up to usec (0: just look; negative: no limit). *transId is 0 when none is
 * ready. A transaction counts when its connection has bytes (or the end of
 * the stream) to read, or holds body bytes from an earlier read. */
s32 cellHttpClientPollConnections(CellHttpClientId clientId, CellHttpTransId* transId, s64 usec)
{
    if (!s_http_initialized)
        return CELL_HTTP_ERROR_NOT_INITIALIZED;
    if (!http_client_ok(clientId))
        return CELL_HTTP_ERROR_BAD_CLIENT;
    if (!transId)
        return CELL_HTTP_ERROR_NO_BUFFER;

    fd_set rd;
    FD_ZERO(&rd);
    int nfds = 0, waiting = 0;
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        HttpTransSlot* t = &s_transactions[i];
        if (!t->in_use || t->client != clientId || t->aborted || t->sock == HTTP_INVALID_SOCKET)
            continue;
        if (t->body_overflow_len > 0 || (t->tls && ps3_tls_pending(t->tls))) {
            vm_write32((u32)(uintptr_t)transId, i + 1);
            return CELL_OK;
        }
        FD_SET(t->sock, &rd);
        if ((int)t->sock + 1 > nfds) nfds = (int)t->sock + 1;   /* ignored by Winsock */
        waiting++;
    }
    vm_write32((u32)(uintptr_t)transId, 0);
    if (!waiting)
        return CELL_OK;

    struct timeval tv, *ptv = NULL;
    if (usec >= 0) {
        tv.tv_sec  = (long)(usec / 1000000);
        tv.tv_usec = (long)(usec % 1000000);
        ptv = &tv;
    }
    if (select(nfds, &rd, NULL, NULL, ptv) <= 0)
        return CELL_OK;
    for (u32 i = 0; i < CELL_HTTP_MAX_TRANSACTIONS; i++) {
        HttpTransSlot* t = &s_transactions[i];
        if (t->in_use && t->client == clientId && !t->aborted && t->sock != HTTP_INVALID_SOCKET &&
            FD_ISSET(t->sock, &rd)) {
            vm_write32((u32)(uintptr_t)transId, i + 1);
            break;
        }
    }
    return CELL_OK;
}

s32 cellHttpTransactionAbortConnection(CellHttpTransId transId)
{
    return cellHttpAbortTransaction(transId);
}

s32 cellHttpRequestSetContentLength(CellHttpTransId transId, u64 totalSize)
{
    return cellHttpSetRequestContentLength(transId, totalSize);
}
