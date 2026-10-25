/*
 * ps3recomp - the sign-in to a server that has one. The knobs are in
 * ps3emu/ps3_net_signin.h; the calls are the Demon's Souls Recomp server's
 * (its docs/identity.md):
 *
 *   POST /dss/v1/challenge  -> {"challenge": <base64, 32 bytes>, "pow_bits": n}
 *   POST /dss/v1/session    {"name", "public_key", "client_version",
 *                            "network_version", "challenge", "pow_nonce",
 *                            "signature"}
 *                           -> {"token": <base64url>, "expires": <RFC 3339>}
 *                              or {"error": <code>, "message": <for the player>}
 *   GET  /dss/v1/banners    -> {"banners": [{"id", "text", "ends"}]}
 *                              (no session; for the host to show, once each)
 *
 * The signature is Ed25519 (third_party/monocypher) over those fields, one
 * per line after "DSS session v1". A key the server has not seen also
 * carries a proof of work: SHA-256(challenge, public key, nonce as 8 bytes
 * big-endian) starts with pow_bits zero bits.
 */
#ifdef _WIN32
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* WSAPoll */
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <bcrypt.h>
#  include <io.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "bcrypt.lib")
   typedef SOCKET si_sock_t;
#  define SI_BAD_SOCK INVALID_SOCKET
#  define si_close closesocket
static SRWLOCK si_lock = SRWLOCK_INIT;    /* the session: held while signing in */
static SRWLOCK si_slock = SRWLOCK_INIT;   /* the server's addresses and the notice: never held long */
#  define SI_LOCK(l)   AcquireSRWLockExclusive(&(l))
#  define SI_UNLOCK(l) ReleaseSRWLockExclusive(&(l))
#else
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
#  include <poll.h>
#  include <pthread.h>
   typedef int si_sock_t;
#  define SI_BAD_SOCK (-1)
#  define si_close close
#  ifdef MSG_NOSIGNAL
#    define SI_SEND_FLAGS MSG_NOSIGNAL   /* a closed connection is an error, not SIGPIPE */
#  endif
static pthread_mutex_t si_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t si_slock = PTHREAD_MUTEX_INITIALIZER;
#  define SI_LOCK(l)   pthread_mutex_lock(&(l))
#  define SI_UNLOCK(l) pthread_mutex_unlock(&(l))
#endif

#include "ps3emu/ps3_net.h"
#include "ps3emu/ps3_net_signin.h"
#include "ps3emu/sha256.h"
#include "../../third_party/monocypher/monocypher.h"
#include "../../third_party/monocypher/monocypher-ed25519.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef SI_SEND_FLAGS
#  define SI_SEND_FLAGS 0
#endif

extern const char* np_psnr_online_id(void);   /* np_psnr.c */

#define SI_PORT        18000
#define SI_RETRY_SECS         60      /* after a failed sign-in */
#define SI_RENEW_BEFORE_SECS  600     /* sign in again this long before the session ends */
#define SI_MAX_POW_BITS       26      /* more is a server asking for minutes of work: refused */
#define SI_REPLY_MAX          16384
#define SI_CONNECT_MS         5000
#define SI_IO_MS              10000
#define SI_MAX_IPS            4

static char    si_token[64];   /* base64url of si_raw */
static uint8_t si_raw[32];
static time_t  si_renew;       /* local clock: sign in again from here */
static time_t  si_end;         /* local clock: the session ends */
static time_t  si_retry;       /* local clock: no new try before this */

static uint8_t si_ips[SI_MAX_IPS][4];   /* PS3_NET_SIGNIN's addresses (si_slock) */
static int     si_nips;
static char    si_ips_for[256];      /* the PS3_NET_SIGNIN they are for */
static char    si_notice[256];       /* (si_slock) */
static int     si_notice_new;

/* ---- small helpers -------------------------------------------------- */

static int si_net_start(void)
{
#ifdef _WIN32
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static int si_random_bytes(uint8_t* p, size_t n)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
#else
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return 0;
#endif
}

static const char SI_B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char SI_B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Base64 of in[n] into out (4 * ((n + 2) / 3) + 1 bytes): standard with
 * padding, or url-safe without. */
static void si_b64_encode(const uint8_t* in, size_t n, char* out, int url)
{
    const char* t = url ? SI_B64URL : SI_B64;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        if (i + 1 < n) out[o++] = t[v >> 6 & 63]; else if (!url) out[o++] = '=';
        if (i + 2 < n) out[o++] = t[v & 63];      else if (!url) out[o++] = '=';
    }
    out[o] = 0;
}

/* Base64 text that holds exactly `want` bytes (standard with padding, or
 * url-safe without) into out. 0 on success. */
static int si_b64_decode(const char* in, uint8_t* out, size_t want, int url)
{
    const char* t = url ? SI_B64URL : SI_B64;
    size_t len = strlen(in), full = want / 3 * 4, rest = want % 3;
    size_t need = full + (rest ? (url ? rest + 1 : 4) : 0);
    if (len != need) return -1;
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '=') {
            if (url || i < len - (rest == 1 ? 2 : 1)) return -1;
            continue;
        }
        const char* p = strchr(t, in[i]);
        if (!p || !*p) return -1;
        acc = acc << 6 | (uint32_t)(p - t);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < want) out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o == want ? 0 : -1;
}

/* ---- JSON: the top level of a reply's object ------------------------ */

static const char* si_js_ws(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int si_js_hex4(const char* p, unsigned* v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        *v = *v << 4 | (unsigned)d;
    }
    return 0;
}

/* The string whose opening quote is at p, decoded (UTF-8) into out[cap] when
 * out is given and truncated to fit. The character after the closing quote,
 * or NULL if it does not parse. */
static const char* si_js_string(const char* p, char* out, size_t cap)
{
    size_t o = 0;
    if (*p++ != '"') return NULL;
    for (;;) {
        unsigned c = (unsigned char)*p++;
        int raw = 1;   /* a byte of the text as it is (UTF-8 passes through) */
        if (!c || c < 0x20) return NULL;
        if (c == '"') break;
        if (c == '\\') {
            char e = *p++;
            raw = 0;
            switch (e) {
            case '"': case '\\': case '/': c = (unsigned char)e; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned lo;
                if (si_js_hex4(p, &c) != 0) return NULL;
                p += 4;
                if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u' && si_js_hex4(p + 2, &lo) == 0 &&
                    lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                } else if (c >= 0xD800 && c < 0xE000) {
                    c = '?';
                }
                break;
            }
            default: return NULL;
            }
        }
        if (out) {
            uint8_t u[4];
            size_t n;
            if (raw || c < 0x80) { u[0] = (uint8_t)c; n = 1; }
            else if (c < 0x800) { u[0] = (uint8_t)(0xC0 | c >> 6); u[1] = (uint8_t)(0x80 | (c & 63)); n = 2; }
            else if (c < 0x10000) { u[0] = (uint8_t)(0xE0 | c >> 12); u[1] = (uint8_t)(0x80 | (c >> 6 & 63)); u[2] = (uint8_t)(0x80 | (c & 63)); n = 3; }
            else { u[0] = (uint8_t)(0xF0 | c >> 18); u[1] = (uint8_t)(0x80 | (c >> 12 & 63)); u[2] = (uint8_t)(0x80 | (c >> 6 & 63)); u[3] = (uint8_t)(0x80 | (c & 63)); n = 4; }
            if (o + n < cap) { memcpy(out + o, u, n); o += n; }
        }
    }
    if (out && cap) out[o] = 0;
    return p;
}

/* Past one value at p, or NULL if it does not parse. */
static const char* si_js_skip(const char* p, int depth)
{
    p = si_js_ws(p);
    if (depth > 16) return NULL;
    if (*p == '"') return si_js_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        p = si_js_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                if (!(p = si_js_string(si_js_ws(p), NULL, 0))) return NULL;
                p = si_js_ws(p);
                if (*p++ != ':') return NULL;
            }
            if (!(p = si_js_skip(p, depth + 1))) return NULL;
            p = si_js_ws(p);
            if (*p == close) return p + 1;
            if (*p++ != ',') return NULL;
        }
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        while (*p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E' || (*p >= '0' && *p <= '9')) p++;
        return p;
    }
    if (!strncmp(p, "true", 4)) return p + 4;
    if (!strncmp(p, "false", 5)) return p + 5;
    if (!strncmp(p, "null", 4)) return p + 4;
    return NULL;
}

/* The value of the top-level member `key` of the object in js, or NULL. */
static const char* si_js_member(const char* js, const char* key)
{
    char name[64];
    const char* p = si_js_ws(js);
    if (*p++ != '{') return NULL;
    p = si_js_ws(p);
    if (*p == '}') return NULL;
    for (;;) {
        if (!(p = si_js_string(si_js_ws(p), name, sizeof name))) return NULL;
        p = si_js_ws(p);
        if (*p++ != ':') return NULL;
        p = si_js_ws(p);
        if (!strcmp(name, key)) return p;
        if (!(p = si_js_skip(p, 1))) return NULL;
        p = si_js_ws(p);
        if (*p++ != ',') return NULL;
    }
}

static int si_js_str(const char* js, const char* key, char* out, size_t cap)
{
    const char* v = si_js_member(js, key);
    if (!v || *v != '"') { if (cap) out[0] = 0; return 0; }
    return si_js_string(v, out, cap) != NULL;
}

static int si_js_int(const char* js, const char* key, long long* out)
{
    const char* v = si_js_member(js, key);
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) return 0;
    *out = strtoll(v, NULL, 10);
    return 1;
}

/* ---- times ---------------------------------------------------------- */

static long long si_days_from_civil(int y, int m, int d)   /* days since 1970-01-01 */
{
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* "2026-10-26T12:00:00Z" (fractions and an offset allowed): seconds since
 * 1970 UTC, or -1. */
static long long si_parse_rfc3339(const char* s)
{
    int y, mo, d, h, mi, se, n = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &se, &n) != 6 || mo < 1 || mo > 12) return -1;
    const char* p = s + n;
    if (*p == '.') do p++; while (*p >= '0' && *p <= '9');
    long long t = si_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    if (*p == 'Z' || *p == 'z') return t;
    int oh, om;
    if ((*p == '+' || *p == '-') && sscanf(p + 1, "%2d:%2d", &oh, &om) == 2)
        return *p == '+' ? t - (oh * 3600 + om * 60) : t + (oh * 3600 + om * 60);
    return -1;
}

/* An HTTP Date ("Sun, 25 Oct 2026 20:10:00 GMT"): seconds since 1970, or -1. */
static long long si_parse_http_date(const char* s)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4];
    int d, y, h, mi, se;
    if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6) return -1;
    const char* m = strstr(months, mon);
    if (!m || (m - months) % 3) return -1;
    return si_days_from_civil(y, (int)(m - months) / 3 + 1, d) * 86400 + h * 3600 + mi * 60 + se;
}

/* ---- HTTP ----------------------------------------------------------- */

/* "host[:port]" into host[cap] and *port. 0 on success. */
static int si_split_address(const char* addr, char* host, size_t cap, uint16_t* port)
{
    const char* colon = strrchr(addr, ':');
    size_t n = colon ? (size_t)(colon - addr) : strlen(addr);
    if (!n || n >= cap) return -1;
    memcpy(host, addr, n);
    host[n] = 0;
    *port = SI_PORT;
    if (colon) {
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535) return -1;
        *port = (uint16_t)p;
    }
    return 0;
}

/* host's IPv4 addresses into ips (SI_MAX_IPS at most); how many. */
static int si_resolve(const char* host, uint8_t ips[SI_MAX_IPS][4])
{
    struct addrinfo hints, *res = NULL, *ai;
    int n = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return 0;
    for (ai = res; ai && n < SI_MAX_IPS; ai = ai->ai_next)
        memcpy(ips[n++], &((struct sockaddr_in*)ai->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return n;
}

static void si_set_blocking(si_sock_t s, int on)
{
#ifdef _WIN32
    u_long nb = on ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int f = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? f & ~O_NONBLOCK : f | O_NONBLOCK);
#endif
}

static void si_set_timeouts(si_sock_t s, int ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
}

/* A connection to ip:port within SI_CONNECT_MS, or SI_BAD_SOCK. */
static si_sock_t si_dial(const uint8_t ip[4], uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    memcpy(&a.sin_addr, ip, 4);
    si_sock_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SI_BAD_SOCK) return SI_BAD_SOCK;
#ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }   /* (macOS has no MSG_NOSIGNAL) */
#endif
    si_set_blocking(s, 0);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) {
#ifdef _WIN32
        int pending = WSAGetLastError() == WSAEWOULDBLOCK;
        WSAPOLLFD pf = {s, POLLOUT, 0};
        int r = pending ? WSAPoll(&pf, 1, SI_CONNECT_MS) : -1;
#else
        int pending = errno == EINPROGRESS;
        struct pollfd pf = {s, POLLOUT, 0};
        int r = pending ? poll(&pf, 1, SI_CONNECT_MS) : -1;
#endif
        int err = 1;
        socklen_t el = sizeof err;
        if (r != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el) != 0 || err != 0) {
            si_close(s);
            return SI_BAD_SOCK;
        }
    }
    si_set_blocking(s, 1);
    si_set_timeouts(s, SI_IO_MS);
    return s;
}

/* POST path with a JSON body (may be ""), or GET it with body NULL. 0 with
 * the status, the body in out[cap] and the Date header (or "") in
 * date[dcap]; -1 if no address answered or the reply is not HTTP. */
static int si_http(const uint8_t ips[SI_MAX_IPS][4], int nips, const char* host, uint16_t port, const char* path,
                   const char* body, int* status, char* out, size_t cap, char* date, size_t dcap)
{
    si_sock_t s = SI_BAD_SOCK;
    for (int i = 0; i < nips && s == SI_BAD_SOCK; i++) s = si_dial(ips[i], port);
    if (s == SI_BAD_SOCK) return -1;

    char head[512];
    size_t blen = body ? strlen(body) : 0;
    int hl = body ? snprintf(head, sizeof head,
                             "POST %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: ps3recomp\r\n"
                             "Content-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                             path, host, (unsigned)port, (unsigned)blen)
                  : snprintf(head, sizeof head,
                             "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: ps3recomp\r\nConnection: close\r\n\r\n",
                             path, host, (unsigned)port);
    int ok = hl > 0 && hl < (int)sizeof head && send(s, head, hl, SI_SEND_FLAGS) == hl;
    for (size_t sent = 0; ok && sent < blen;) {
        int n = send(s, body + sent, (int)(blen - sent), SI_SEND_FLAGS);
        if (n <= 0) ok = 0; else sent += (size_t)n;
    }
    char* buf = ok ? (char*)malloc(SI_REPLY_MAX + 1) : NULL;
    size_t got = 0;
    while (buf) {
        if (got == SI_REPLY_MAX) { free(buf); buf = NULL; break; }   /* too big for these replies */
        int n = recv(s, buf + got, (int)(SI_REPLY_MAX - got), 0);
        if (n < 0) { free(buf); buf = NULL; break; }
        if (n == 0) break;
        got += (size_t)n;
    }
    si_close(s);
    if (!buf) return -1;
    buf[got] = 0;

    char* end = strstr(buf, "\r\n\r\n");
    if (strncmp(buf, "HTTP/1.", 7) != 0 || got < 12 || !end) { free(buf); return -1; }
    *status = atoi(buf + 9);
    *end = 0;
    const char* bodyp = end + 4;
    size_t bl = got - (size_t)(bodyp - buf);
    if (dcap) date[0] = 0;
    for (char* line = strstr(buf, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        const char* l = line + 2;
        if (!strncmp(l, "Date:", 5) || !strncmp(l, "date:", 5)) {
            l = si_js_ws(l + 5);
            size_t n = strcspn(l, "\r\n");
            if (n < dcap) { memcpy(date, l, n); date[n] = 0; }
        } else if (!strncmp(l, "Content-Length:", 15) || !strncmp(l, "content-length:", 15)) {
            long long cl = strtoll(l + 15, NULL, 10);
            if (cl >= 0 && (size_t)cl < bl) bl = (size_t)cl;
        }
    }
    if (bl >= cap) bl = cap - 1;
    memcpy(out, bodyp, bl);
    out[bl] = 0;
    free(buf);
    return 0;
}

/* ---- the key -------------------------------------------------------- */

static FILE* si_open_file(const char* path, const char* mode)
{
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, 1024) ||
        !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return fopen(path, mode);   /* not UTF-8: the ANSI code page's */
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

/* The install's key seed: read from path, or made and written there when
 * the file does not exist. 1 if made now, 0 if read, -1 on failure. The
 * file is only ever created, never written over: losing it loses the name. */
static int si_load_key(const char* path, uint8_t seed[32])
{
    FILE* f = si_open_file(path, "rb");
    if (f) {
        size_t n = fread(seed, 1, 32, f);
        int more = fgetc(f);
        fclose(f);
        if (n == 32 && more == EOF) return 0;
        crypto_wipe(seed, 32);
        printf("[signin] PS3_NET_KEY_FILE is not a key (32 bytes); left as it is\n");
        return -1;
    }
    if (si_random_bytes(seed, 32) != 0) {
        printf("[signin] no random numbers from the system: no key\n");
        return -1;
    }
#ifdef _WIN32
    f = si_open_file(path, "wbx");
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    f = fd >= 0 ? fdopen(fd, "wb") : NULL;
#endif
    int ok = f && fwrite(seed, 1, 32, f) == 32 && fflush(f) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(f)) == 0;
#else
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (f) fclose(f);
    if (!ok) {
        crypto_wipe(seed, 32);
        printf("[signin] could not write PS3_NET_KEY_FILE\n");
        return -1;
    }
    printf("[signin] made this install's key (PS3_NET_KEY_FILE)\n");
    return 1;
}

/* ---- proof of work -------------------------------------------------- */

static int si_zero_bits(const uint8_t h[32])
{
    int z = 0;
    for (int i = 0; i < 32; i++) {
        if (h[i]) {
            for (uint8_t b = h[i]; !(b & 0x80); b <<= 1) z++;
            return z;
        }
        z += 8;
    }
    return z;
}

/* A nonce for which SHA-256(challenge, pub, nonce) starts with `bits` zero
 * bits. The first 64 bytes are one block, hashed once. 0 on success. */
static int si_do_work(const uint8_t challenge[32], const uint8_t pub[32], int bits, uint64_t* nonce)
{
    ps3_sha256_ctx base;
    ps3_sha256_init(&base);
    ps3_sha256_update(&base, challenge, 32);
    ps3_sha256_update(&base, pub, 32);
    uint64_t limit = (uint64_t)1 << (bits + 5);   /* 32 times what it takes on average */
    for (uint64_t n = 0; n < limit; n++) {
        ps3_sha256_ctx c = base;
        uint8_t nb[8], h[32];
        for (int i = 0; i < 8; i++) nb[i] = (uint8_t)(n >> (56 - 8 * i));
        ps3_sha256_update(&c, nb, 8);
        ps3_sha256_final(&c, h);
        if (si_zero_bits(h) >= bits) { *nonce = n; return 0; }
    }
    return -1;
}

/* ---- the notice ----------------------------------------------------- */

void ps3_net_signin_post_notice(const char* text)
{
    SI_LOCK(si_slock);
    size_t o = 0;
    for (const char* p = text; *p && o + 1 < sizeof si_notice; p++)
        si_notice[o++] = (unsigned char)*p < 0x20 ? ' ' : *p;
    si_notice[o] = 0;
    si_notice_new = o > 0;
    SI_UNLOCK(si_slock);
    printf("[signin] for the player: %s\n", text);
}

int ps3_net_signin_notice(char* out, size_t cap)
{
    int r = 0;
    SI_LOCK(si_slock);
    if (si_notice_new && cap) {
        snprintf(out, cap, "%s", si_notice);
        si_notice_new = 0;
        r = 1;
    }
    SI_UNLOCK(si_slock);
    return r;
}

/* ---- the sign-in ---------------------------------------------------- */

int ps3_net_signin_enabled(void)
{
    const char* a = getenv("PS3_NET_SIGNIN");
    const char* k = getenv("PS3_NET_KEY_FILE");
    return ps3_net_online() && a && *a && k && *k;
}

/* The sign-in server's addresses, looked up again when PS3_NET_SIGNIN
 * changed or `fresh` asks for it. Copies them into ips; how many. */
static int si_server_ips(const char* addr, const char* host, int fresh, uint8_t ips[SI_MAX_IPS][4])
{
    SI_LOCK(si_slock);
    int n = si_nips;
    int same = !strcmp(si_ips_for, addr);
    SI_UNLOCK(si_slock);
    if (fresh || !same || !n) {
        uint8_t got[SI_MAX_IPS][4];
        n = si_net_start() == 0 ? si_resolve(host, got) : 0;
        SI_LOCK(si_slock);
        memcpy(si_ips, got, sizeof got);
        si_nips = n;
        snprintf(si_ips_for, sizeof si_ips_for, "%s", addr);
        SI_UNLOCK(si_slock);
    }
    SI_LOCK(si_slock);
    memcpy(ips, si_ips, sizeof si_ips);
    n = si_nips;
    SI_UNLOCK(si_slock);
    return n;
}

int ps3_net_signin_server(const uint8_t ip[4])
{
    char host[256];
    uint16_t port;
    uint8_t ips[SI_MAX_IPS][4];
    if (!ps3_net_signin_enabled()) return 0;
    const char* addr = getenv("PS3_NET_SIGNIN");
    if (si_split_address(addr, host, sizeof host, &port) != 0) return 0;
    int n = si_server_ips(addr, host, 0, ips);
    for (int i = 0; i < n; i++)
        if (!memcmp(ips[i], ip, 4)) return 1;
    return 0;
}

static int si_name_ok(const char* s)
{
    size_t n = strlen(s);
    if (n < 3 || n > 16) return 0;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_'))
            return 0;
    return 1;
}

static int si_version_ok(const char* s)
{
    size_t n = strlen(s);
    if (!n || n > 64) return 0;
    for (; *s; s++)
        if (!((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-' || *s == '+'))
            return 0;
    return 1;
}

/* A refusal: the server's message for the player, or a plain one. */
static void si_refused(const char* reply, int status)
{
    char code[32], msg[256];
    si_js_str(reply, "error", code, sizeof code);
    if (si_js_str(reply, "message", msg, sizeof msg) && msg[0]) {
        ps3_net_signin_post_notice(msg);
    } else if (code[0]) {
        char t[96];
        snprintf(t, sizeof t, "The online server refused the sign-in (%s).", code);
        ps3_net_signin_post_notice(t);
    }
    printf("[signin] refused: HTTP %d%s%s\n", status, code[0] ? ", " : "", code);
}

/* One sign-in, under si_lock. 0 with the session set, -1 without. */
static int si_sign_in(time_t now)
{
    const char* addr = getenv("PS3_NET_SIGNIN");
    const char* key_file = getenv("PS3_NET_KEY_FILE");
    const char* version = getenv("PS3_NET_CLIENT_VERSION");
    const char* nv_env = getenv("PS3_NET_NETWORK_VERSION");
    const char* name = np_psnr_online_id();
    int nv = nv_env && *nv_env ? atoi(nv_env) : 1;
    char host[256];
    uint16_t port;
    if (!version || !*version) version = "0.0.0";
    if (si_split_address(addr, host, sizeof host, &port) != 0) {
        printf("[signin] PS3_NET_SIGNIN=%s is not host[:port]\n", addr);
        return -1;
    }
    if (!name || !si_name_ok(name) || !si_version_ok(version)) {
        printf("[signin] no sign-in: the online name or PS3_NET_CLIENT_VERSION is not valid\n");
        return -1;
    }

    uint8_t seed[32], sk[64], pk[32], sig[64];
    int made = si_load_key(key_file, seed);
    if (made < 0) {
        ps3_net_signin_post_notice("Online play needs this install's key file, and it could not be read or made.");
        return -1;
    }
    crypto_ed25519_key_pair(sk, pk, seed);   /* (wipes seed) */
    char pk64[48];
    si_b64_encode(pk, 32, pk64, 0);

    uint8_t ips[SI_MAX_IPS][4];
    int nips = si_server_ips(addr, host, 1, ips);
    int want_work = made, result = -1;
    char* reply = (char*)malloc(SI_REPLY_MAX);
    char date[64];
    for (int attempt = 0; reply && attempt < 3; attempt++) {
        int status;
        if (si_http(ips, nips, host, port, "/dss/v1/challenge", "", &status, reply, SI_REPLY_MAX, date, sizeof date) != 0) {
            printf("[signin] %s:%u did not answer\n", host, (unsigned)port);
            break;
        }
        if (status != 200) {
            if (si_js_member(reply, "error")) si_refused(reply, status);
            else printf("[signin] %s:%u has no sign-in (HTTP %d)\n", host, (unsigned)port, status);
            break;
        }
        char ch64[64];
        uint8_t ch[32];
        long long bits = 0;
        si_js_int(reply, "pow_bits", &bits);
        if (!si_js_str(reply, "challenge", ch64, sizeof ch64) || si_b64_decode(ch64, ch, 32, 0) != 0) {
            printf("[signin] the challenge does not read\n");
            break;
        }
        uint64_t nonce = 0;
        if (want_work && bits > 0) {
            if (bits > SI_MAX_POW_BITS) {
                printf("[signin] the server asks for %lld bits of work, more than %d\n", bits, SI_MAX_POW_BITS);
                break;
            }
            if (si_do_work(ch, pk, (int)bits, &nonce) != 0) break;
        }

        char msg[512], body[1024], sig64[96];
        int ml = snprintf(msg, sizeof msg, "DSS session v1\n%s\n%s\n%s\n%s\n%d\n%llu", ch64, name, pk64, version, nv,
                          (unsigned long long)nonce);
        if (ml <= 0 || ml >= (int)sizeof msg) break;
        crypto_ed25519_sign(sig, sk, (const uint8_t*)msg, (size_t)ml);
        si_b64_encode(sig, 64, sig64, 0);
        snprintf(body, sizeof body,
                 "{\"name\":\"%s\",\"public_key\":\"%s\",\"client_version\":\"%s\",\"network_version\":%d,"
                 "\"challenge\":\"%s\",\"pow_nonce\":%llu,\"signature\":\"%s\"}",
                 name, pk64, version, nv, ch64, (unsigned long long)nonce, sig64);
        if (si_http(ips, nips, host, port, "/dss/v1/session", body, &status, reply, SI_REPLY_MAX, date, sizeof date) != 0) {
            printf("[signin] %s:%u did not answer\n", host, (unsigned)port);
            break;
        }
        if (status != 200) {
            char code[32];
            si_js_str(reply, "error", code, sizeof code);
            if (!strcmp(code, "work") && !want_work) { want_work = 1; continue; }   /* a key the server has not seen */
            if (!strcmp(code, "challenge")) continue;                              /* (it ran out meanwhile) */
            if (code[0]) si_refused(reply, status);
            else printf("[signin] %s:%u has no sign-in (HTTP %d)\n", host, (unsigned)port, status);
            break;
        }

        char tok[64], expires[64];
        uint8_t raw[32];
        if (!si_js_str(reply, "token", tok, sizeof tok) || si_b64_decode(tok, raw, 32, 1) != 0) {
            printf("[signin] the session's token does not read\n");
            break;
        }
        /* How long it lasts by the server's clock (Date), counted on ours */
        long long ends = si_js_str(reply, "expires", expires, sizeof expires) ? si_parse_rfc3339(expires) : -1;
        long long server_now = si_parse_http_date(date);
        long long life = ends < 0 ? 3600 : ends - (server_now >= 0 ? server_now : (long long)now);
        if (life < 60) life = 60;
        memcpy(si_raw, raw, 32);
        snprintf(si_token, sizeof si_token, "%s", tok);
        si_end = now + (time_t)life;
        si_renew = si_end - (time_t)(life / 2 < SI_RENEW_BEFORE_SECS ? life / 2 : SI_RENEW_BEFORE_SECS);
        crypto_wipe(raw, sizeof raw);
        crypto_wipe(tok, sizeof tok);
        printf("[signin] signed in to %s:%u as \"%s\" for %lld minutes%s\n", host, (unsigned)port, name, life / 60,
               want_work ? " (a new key)" : "");
        result = 0;
        break;
    }
    free(reply);
    crypto_wipe(sk, sizeof sk);
    return result;
}

int ps3_net_session(char* token, size_t cap, uint8_t raw[32])
{
    if (!ps3_net_signin_enabled()) return 0;
    SI_LOCK(si_lock);
    time_t now = time(NULL);
    if (!(si_token[0] && now < si_renew) && now >= si_retry && si_sign_in(now) != 0)
        si_retry = now + SI_RETRY_SECS;
    int ok = si_token[0] && now < si_end;
    if (ok && token && cap) snprintf(token, cap, "%s", si_token);
    if (ok && raw) memcpy(raw, si_raw, 32);
    SI_UNLOCK(si_lock);
    return ok;
}

void ps3_net_session_drop(void)
{
    SI_LOCK(si_lock);
    crypto_wipe(si_token, sizeof si_token);
    crypto_wipe(si_raw, sizeof si_raw);
    si_end = si_renew = si_retry = 0;
    SI_UNLOCK(si_lock);
}

/* ---- banners -------------------------------------------------------- */

/* GET /dss/v1/banners on the sign-in server, with no session:
 *   {"banners": [{"id": n, "text": "...", "ends": <RFC 3339>}]}
 * at most 4, the oldest first. A thread of their own asks every
 * SI_BANNER_SECS; each banner goes to the host once. */
#define SI_BANNER_SECS  300
#define SI_BANNER_QUEUE 4
#define SI_BANNER_SEEN  64

static char      si_banner[SI_BANNER_QUEUE][256];   /* (si_slock) fetched, not taken yet; the oldest first */
static int       si_nbanner;
static long long si_seen[SI_BANNER_SEEN];           /* (si_slock) the ids queued so far, the newest 64 */
static int       si_nseen;
static int       si_banner_started;                 /* (si_slock) the thread runs */

static int si_banner_seen(long long id)
{
    for (int i = 0; i < si_nseen && i < SI_BANNER_SEEN; i++)
        if (si_seen[i] == id) return 1;
    return 0;
}

/* One ask: the banners not seen before go into the queue (a full queue
 * drops the rest until the next ask). 1 if the server gave a list. */
static int si_banners_fetch(void)
{
    char host[256];
    uint16_t port;
    uint8_t ips[SI_MAX_IPS][4];
    int status = 0;
    if (!ps3_net_signin_enabled()) return 0;
    const char* addr = getenv("PS3_NET_SIGNIN");
    if (si_split_address(addr, host, sizeof host, &port) != 0) return 0;
    int nips = si_server_ips(addr, host, 0, ips);
    char* reply = nips > 0 ? (char*)malloc(SI_REPLY_MAX) : NULL;
    if (!reply) return 0;
    const char* p = NULL;
    if (si_http(ips, nips, host, port, "/dss/v1/banners", NULL, &status, reply, SI_REPLY_MAX, NULL, 0) == 0 &&
        status == 200)
        p = si_js_member(reply, "banners");
    if (!p || *p != '[') {   /* (none, or a server with no banners: quietly) */
        free(reply);
        return 0;
    }
    p = si_js_ws(p + 1);
    while (*p == '{') {
        long long id = 0;
        char text[256];
        int have = si_js_int(p, "id", &id) && si_js_str(p, "text", text, sizeof text) && text[0];
        if (!(p = si_js_skip(p, 1))) break;
        if (have) {
            for (char* c = text; *c; c++)
                if ((unsigned char)*c < 0x20) *c = ' ';
            SI_LOCK(si_slock);
            int fresh = !si_banner_seen(id) && si_nbanner < SI_BANNER_QUEUE;
            if (fresh) {
                snprintf(si_banner[si_nbanner++], sizeof si_banner[0], "%s", text);
                si_seen[si_nseen++ % SI_BANNER_SEEN] = id;
            }
            SI_UNLOCK(si_slock);
            if (fresh) printf("[signin] banner %lld from the server\n", id);
        }
        p = si_js_ws(p);
        if (*p != ',') break;
        p = si_js_ws(p + 1);
    }
    free(reply);
    return 1;
}

#ifdef _WIN32
static DWORD WINAPI si_banner_main(LPVOID arg)
#else
static void* si_banner_main(void* arg)
#endif
{
    (void)arg;
    for (;;) {
        si_banners_fetch();
#ifdef _WIN32
        Sleep(SI_BANNER_SECS * 1000);
#else
        sleep(SI_BANNER_SECS);
#endif
    }
    return 0;
}

int ps3_net_signin_banner(char* out, size_t cap)
{
    int start = 0, r = 0;
    if (!ps3_net_signin_enabled()) return 0;
    SI_LOCK(si_slock);
    if (!si_banner_started) start = si_banner_started = 1;
    if (si_nbanner && cap) {
        snprintf(out, cap, "%s", si_banner[0]);
        memmove(si_banner[0], si_banner[1], sizeof si_banner[0] * (size_t)(si_nbanner - 1));
        si_nbanner--;
        r = 1;
    }
    SI_UNLOCK(si_slock);
    if (start) {
#ifdef _WIN32
        HANDLE th = CreateThread(NULL, 0, si_banner_main, NULL, 0, NULL);
        if (th) CloseHandle(th);
#else
        pthread_t th;
        if (pthread_create(&th, NULL, si_banner_main, NULL) == 0) pthread_detach(th);
#endif
    }
    return r;
}
