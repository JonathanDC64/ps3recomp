/*
 * ps3recomp - TLS on the links to the online server (the knobs and calls are in ps3emu/ps3_tls.h), with mbedTLS 3.6
 * (third_party/mbedtls: its default configuration, unmodified).
 *
 * That configuration has no threading of its own, so every call into mbedTLS happens under one lock here
 * (tls_lock): the shared configuration, its random generator, PSA's key store. The sockets are non-blocking while
 * TLS runs on them, so no mbedTLS call waits on the network; the waits (poll) happen outside the lock.
 */
#ifdef _WIN32
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600   /* WSAPoll, GetTickCount64 */
#  endif
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "bcrypt.lib")   /* mbedTLS's entropy: BCryptGenRandom */
   typedef SOCKET tls_sock_t;
#  define TLS_WOULD_BLOCK() (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINTR)
static SRWLOCK tls_lock = SRWLOCK_INIT;
#  define TLS_LOCK()   AcquireSRWLockExclusive(&tls_lock)
#  define TLS_UNLOCK() ReleaseSRWLockExclusive(&tls_lock)
#else
#  include <sys/socket.h>
#  include <fcntl.h>
#  include <errno.h>
#  include <poll.h>
#  include <pthread.h>
#  include <time.h>
   typedef int tls_sock_t;
#  define TLS_WOULD_BLOCK() (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
#  define TLS_LOCK()   pthread_mutex_lock(&tls_lock)
#  define TLS_UNLOCK() pthread_mutex_unlock(&tls_lock)
#endif

#include "ps3emu/ps3_tls.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"   /* (its error codes) */
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MSG_NOSIGNAL
#  define TLS_SEND_FLAGS MSG_NOSIGNAL   /* a closed connection is an error, not SIGPIPE */
#else
#  define TLS_SEND_FLAGS 0
#endif

/* Let's Encrypt's roots (https://letsencrypt.org/certificates/, 2026-07): X1 and X2 sign today's certificates, and
 * YE and YR (made 2025-09) the next ones. Building them in, not the system's store, means only Let's Encrypt's
 * certificates pass, on every system alike. */
static const char TLS_ROOTS[] =
    /* ISRG Root X1 (RSA 4096, 2015-2035) */
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n"
    /* ISRG Root X2 (ECDSA P-384, 2020-2040) */
    "-----BEGIN CERTIFICATE-----\n"
    "MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
    "CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
    "R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
    "MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
    "ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
    "EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
    "+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
    "ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
    "AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
    "zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
    "tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
    "/q4AaOeMSQ+2b1tbFfLn\n"
    "-----END CERTIFICATE-----\n"
    /* Root YE (ECDSA P-384, 2025-2045) */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIB2TCCAWCgAwIBAgIRAKQCa6LvbHwg1AR+XmWmk4AwCgYIKoZIzj0EAwMwLjEL\n"
    "MAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWUUwHhcN\n"
    "MjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzENMAsG\n"
    "A1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZRTB2MBAGByqGSM49AgEGBSuBBAAi\n"
    "A2IABDwS/6vhrcVqcbBo+wgdI3fwn9x7DNJJOY/lTOti0vkwuRN87RhEhTH17E7X\n"
    "yFjWsPYhIPt/wzOqxTd2b+4ZJNy9ID04YywF9U5zasDVyGSNErVNtz8uSGh5izW8\n"
    "7j77GaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0O\n"
    "BBYEFKPIJlqOoUzQNWP8myPIOq5W809WMAoGCCqGSM49BAMDA2cAMGQCMHhMr8N9\n"
    "LdL1VQKs9BdV81r76eXRB6mtjuNjzk6/lBsPNToWLTDzGYgtQKO1jl63uAIwGV7m\n"
    "onyF377c+MM1oqVNs17sgu7F9YKZwgLmVbeOMDbKAXHtKMDLbiGllCcs8f47\n"
    "-----END CERTIFICATE-----\n"
    /* Root YR (RSA 4096, 2025-2045) */
    "-----BEGIN CERTIFICATE-----\n"
    "MIIFKTCCAxGgAwIBAgIRAOxGNJNgz0sP+KmC2Tqpyj0wDQYJKoZIhvcNAQELBQAw\n"
    "LjELMAkGA1UEBhMCVVMxDTALBgNVBAoTBElTUkcxEDAOBgNVBAMTB1Jvb3QgWVIw\n"
    "HhcNMjUwOTAzMDAwMDAwWhcNNDUwOTAyMjM1OTU5WjAuMQswCQYDVQQGEwJVUzEN\n"
    "MAsGA1UEChMESVNSRzEQMA4GA1UEAxMHUm9vdCBZUjCCAiIwDQYJKoZIhvcNAQEB\n"
    "BQADggIPADCCAgoCggIBANvGJnN78CTJdWL3+eGfsLN5TrNBJs+VH9hRXqRbwxu9\n"
    "sGNiB0BD1fcOxbSUQCJIM1xE13Db+5Cw1w0s0EBYsvuIP/6joF0w8cuImbgR1OGg\n"
    "YbSQ4OpzI+DG8SGuTlcE873OCS+kh3srlo6vl43M5OJg4Aeo1sfHp6kTJDoIiFBN\n"
    "JAY+OKfX/FUvYKuhjT+no49lmqmupSBI5PkBQiqrEGtWU5uxU/cQWHGu8jSjFBzn\n"
    "ZqvbNPLMXMLFxCb3WTfrJBXXjqvWG+v4bjzxjjeAtOlU7qarRDvNOyAuQYLln904\n"
    "M+faKx8hnLCpJ15ZqaEgcNlY+9MMWcC5yvL2A2j3l9+2buggZX+dOE91zYmIdawT\n"
    "vSZuVvlbRrAlLxIB6pwMBjneXCjYQ8+3BCCjssbSNpZU3hTcBDdhfAlEDlYr6pEa\n"
    "tnMdmDT5BqnKC92bd0EhM1fbLHioLccLCuievT8ZkPhZrq7Mii7gNXAcUEAR8+lz\n"
    "Yal+9zTg7C5DALyVOeG/CqfRAMn1KSHCR0NSA6P8tn/mGRlnCct5rtVCLnVySVpU\n"
    "6H1qGg3DgTOuskf8eahTMiYbI5ezPJmO5ertalskQ1utp74+eDy92PI4ftHKTbq9\n"
    "IWhH4YZKh3WnJEIt+oQvlYZbY8tpEroKrFB6PFGzrJIDRyts4HqvuH52RFj2zv/B\n"
    "AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud\n"
    "DgQWBBTe51tg0CJtQCh9Pw0B/qS1UrRRlDANBgkqhkiG9w0BAQsFAAOCAgEAWHnf\n"
    "713Bdkq7t5yN2dNIgQakUb94X9WuyhMEHHkgx4oDpSUlnG0w4g94MoqaEUE31ZjR\n"
    "LU7L5LD1g9ujFHTQu8AD215AHMVQFbm6j8hQxdXHAzDajFNQnOlDJrLjzIx176oy\n"
    "AjvUtejZx2NNmdb5fd0WGVGsCdoAJ3N8ozo7ajE8t6vfxStZb4BQ9WYJGHUDrv2N\n"
    "i5tJF6CNiPnlzs3BUfECRbE4JSk+jvy8+VoGiFE8qsH/j78x2fjgQhAQFV7P7Zxy\n"
    "dBTZ1wEkNpZNW2qnaK1SKBLa+xf6E06YRIq5uaI+HWH8SY1y5VbRgzq40EKg3yxP\n"
    "06fz+uYAUIFJoLNfhwRCc3Q6pQVuMX3yAjHAes4gk4moGcLQ5p7HAh39yeylZc1J\n"
    "41sx/jKwLIkPE6Rr1Nf4pxdsxf9SA4yOEiAkDgq04DVxn8hgYFdUtBCuiuVC2heA\n"
    "EiqVEa+8QZjuw8Gj0EbHXcRd1nInvGqRS1o9Is7YBdQN57X1AYveGBNNqjICSb7c\n"
    "awuw1EawTDrs13VUlJVEsbQ0/O/1aaV73mCdOQ8azqL2KTv1Ewu1xbquE2S+kdQU\n"
    "To9TUwat3wUA6cwXh1EfpS/3fJ0aGah5hdpRyoCLDlsSn8tkrjMfFFX0viC+GxHc\n"
    "sI1ANRYvqSFC2X1VRZfDg+wD6E21BccmifG4yWc=\n"
    "-----END CERTIFICATE-----\n"
    ;

struct ps3_tls {
    mbedtls_ssl_context ssl;
    tls_sock_t          s;
};

static int                      tls_ready;   /* (tls_lock) 1 set up, -1 it failed */
static mbedtls_ssl_config       tls_conf;
static mbedtls_x509_crt         tls_ca;
static mbedtls_entropy_context  tls_entropy;
static mbedtls_ctr_drbg_context tls_drbg;

/* ---- small helpers -------------------------------------------------- */

static unsigned long long tls_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
#endif
}

static void tls_nonblocking(tls_sock_t s)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#  ifdef SO_NOSIGPIPE
    { int one = 1; setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }   /* (macOS has no MSG_NOSIGNAL) */
#  endif
#endif
}

/* Wait until s can be read (or written): 1, or 0 when ms ran out first. */
static int tls_wait(tls_sock_t s, int write, int ms)
{
#ifdef _WIN32
    WSAPOLLFD pf = {s, (SHORT)(write ? POLLOUT : POLLIN), 0};
    return WSAPoll(&pf, 1, ms) > 0;
#else
    struct pollfd pf = {s, (short)(write ? POLLOUT : POLLIN), 0};
    return poll(&pf, 1, ms) > 0;
#endif
}

static void tls_error_text(int err, char* out, size_t cap)
{
    char e[160];
    mbedtls_strerror(err, e, sizeof e);
    snprintf(out, cap, "%s (-0x%04X)", e, (unsigned)-err);
}

/* The shared setup, once (under tls_lock): PSA, the roots, the random generator, the client configuration. */
static int tls_setup(char* why, size_t cap)
{
    if (tls_ready) return tls_ready > 0 ? 0 : -1;
    tls_ready = -1;

    psa_status_t ps = psa_crypto_init();   /* (TLS 1.3 needs it) */
    if (ps != PSA_SUCCESS) {
        snprintf(why, cap, "psa_crypto_init: %d", (int)ps);
        return -1;
    }
    mbedtls_x509_crt_init(&tls_ca);
    const char* ca_file = getenv("PS3_NET_CA_FILE");
    int r = ca_file && *ca_file ? mbedtls_x509_crt_parse_file(&tls_ca, ca_file)
                                : mbedtls_x509_crt_parse(&tls_ca, (const unsigned char*)TLS_ROOTS, sizeof TLS_ROOTS);
    if (r != 0) {   /* (> 0: that many certificates could not be read) */
        if (r < 0) tls_error_text(r, why, cap);
        else snprintf(why, cap, "%d certificates could not be read", r);
        if (ca_file && *ca_file) printf("[tls] PS3_NET_CA_FILE=%s: %s\n", ca_file, why);
        return -1;
    }
    mbedtls_entropy_init(&tls_entropy);
    mbedtls_ctr_drbg_init(&tls_drbg);
    static const unsigned char pers[] = "ps3recomp tls";
    r = mbedtls_ctr_drbg_seed(&tls_drbg, mbedtls_entropy_func, &tls_entropy, pers, sizeof pers - 1);
    if (r != 0) {
        tls_error_text(r, why, cap);
        return -1;
    }
    mbedtls_ssl_config_init(&tls_conf);
    r = mbedtls_ssl_config_defaults(&tls_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        tls_error_text(r, why, cap);
        return -1;
    }
    mbedtls_ssl_conf_authmode(&tls_conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&tls_conf, &tls_ca, NULL);
    mbedtls_ssl_conf_rng(&tls_conf, mbedtls_ctr_drbg_random, &tls_drbg);
    mbedtls_ssl_conf_min_tls_version(&tls_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    tls_ready = 1;
    return 0;
}

/* ---- the socket under mbedTLS (called with tls_lock held; never waits) ---- */

static int tls_bio_send(void* ctx, const unsigned char* p, size_t n)
{
    tls_sock_t s = ((ps3_tls*)ctx)->s;
    int k = send(s, (const char*)p, (int)(n > 0x40000000 ? 0x40000000 : n), TLS_SEND_FLAGS);
    if (k >= 0) return k;
    return TLS_WOULD_BLOCK() ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_bio_recv(void* ctx, unsigned char* p, size_t n)
{
    tls_sock_t s = ((ps3_tls*)ctx)->s;
    int k = recv(s, (char*)p, (int)(n > 0x40000000 ? 0x40000000 : n), 0);
    if (k >= 0) return k;   /* (0: the connection ended) */
    return TLS_WOULD_BLOCK() ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ---- the calls -------------------------------------------------------- */

int ps3_tls_enabled(void)
{
    const char* v = getenv("PS3_NET_TLS");
    return v && *v && strcmp(v, "0") != 0;
}

int ps3_tls_server_name(char* out, size_t cap)
{
    const char* v = getenv("PS3_NET_TLS");
    if (!v || !*v || !strcmp(v, "0") || !cap) return -1;
    if (strcmp(v, "1") != 0) {   /* the name itself */
        if (strlen(v) >= cap) return -1;
        strcpy(out, v);
        return 0;
    }
    const char* a = getenv("PS3_NET_SIGNIN");   /* host[:port] */
    if (!a || !*a) return -1;
    const char* colon = strrchr(a, ':');
    size_t n = colon ? (size_t)(colon - a) : strlen(a);
    if (!n || n >= cap) return -1;
    memcpy(out, a, n);
    out[n] = 0;
    return 0;
}

/* A connection on sock with conf (under tls_lock): NULL with why on an error. */
static ps3_tls* tls_new(intptr_t sock, const mbedtls_ssl_config* conf, const char* name, char* why, size_t wcap)
{
    ps3_tls* t = (ps3_tls*)calloc(1, sizeof *t);
    if (!t) {
        snprintf(why, wcap, "out of memory");
        return NULL;
    }
    t->s = (tls_sock_t)sock;
    tls_nonblocking(t->s);
    mbedtls_ssl_init(&t->ssl);
    int r = mbedtls_ssl_setup(&t->ssl, conf);
    if (r == 0 && name) r = mbedtls_ssl_set_hostname(&t->ssl, name);   /* SNI, and the name to check */
    if (r != 0) {
        tls_error_text(r, why, wcap);
        mbedtls_ssl_free(&t->ssl);
        free(t);
        return NULL;
    }
    mbedtls_ssl_set_bio(&t->ssl, t, tls_bio_send, tls_bio_recv, NULL);
    return t;
}

/* The handshake, within timeout_ms: 0, or -1 with why (t is then freed). */
static int tls_handshake(ps3_tls* t, const char* name, int timeout_ms, char* why, size_t wcap)
{
    unsigned long long end = tls_now_ms() + (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        TLS_LOCK();
        int r = mbedtls_ssl_handshake(&t->ssl);
        uint32_t flags = r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ? mbedtls_ssl_get_verify_result(&t->ssl) : 0;
        if (r != 0 && r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (flags) {
                char info[256];
                mbedtls_x509_crt_verify_info(info, sizeof info, "", flags);
                info[strcspn(info, "\n")] = 0;   /* the first reason */
                snprintf(why, wcap, "the certificate is not accepted for %s: %s", name ? name : "?", info);
            } else {
                tls_error_text(r, why, wcap);
            }
            mbedtls_ssl_free(&t->ssl);
        }
        TLS_UNLOCK();
        if (r == 0) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            unsigned long long now = tls_now_ms();
            if (now < end && tls_wait(t->s, r == MBEDTLS_ERR_SSL_WANT_WRITE, (int)(end - now))) continue;
            snprintf(why, wcap, "the server did not finish the handshake in time");
            TLS_LOCK();
            mbedtls_ssl_free(&t->ssl);
            TLS_UNLOCK();
        }
        free(t);
        return -1;
    }
}

ps3_tls* ps3_tls_connect(intptr_t sock, const char* name, int timeout_ms, char* why, size_t wcap)
{
    char tmp[200];
    if (!why || !wcap) { why = tmp; wcap = sizeof tmp; }
    why[0] = 0;
    if (!name || !*name) {
        snprintf(why, wcap, "no server name to check the certificate against");
        return NULL;
    }
    TLS_LOCK();
    ps3_tls* t = tls_setup(why, wcap) == 0 ? tls_new(sock, &tls_conf, name, why, wcap) : NULL;
    TLS_UNLOCK();
    if (!t) {
        if (!why[0]) snprintf(why, wcap, "TLS could not be set up");
        return NULL;
    }
    return tls_handshake(t, name, timeout_ms, why, wcap) == 0 ? t : NULL;
}

int ps3_tls_send_all(ps3_tls* t, const void* p, size_t n, int timeout_ms)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned long long end = tls_now_ms() + (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
    while (n) {
        TLS_LOCK();
        int r = mbedtls_ssl_write(&t->ssl, b, n);
        TLS_UNLOCK();
        if (r > 0) {
            b += r;
            n -= (size_t)r;
            continue;
        }
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;
        unsigned long long now = tls_now_ms();
        if (now >= end || !tls_wait(t->s, r == MBEDTLS_ERR_SSL_WANT_WRITE, (int)(end - now))) return -1;
    }
    return 0;
}

int ps3_tls_recv(ps3_tls* t, void* p, size_t n, int timeout_ms)
{
    unsigned long long end = tls_now_ms() + (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        TLS_LOCK();
        int r = mbedtls_ssl_read(&t->ssl, (unsigned char*)p, n);
        TLS_UNLOCK();
        if (r > 0) return r;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;   /* (TLS 1.3: not kept) */
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) return -1;   /* (an end without the
                                                                     server's goodbye is an error: cut short) */
        if (timeout_ms == 0) return PS3_TLS_AGAIN;
        unsigned long long now = tls_now_ms();
        if (now >= end || !tls_wait(t->s, r == MBEDTLS_ERR_SSL_WANT_WRITE, (int)(end - now))) return -1;
    }
}

int ps3_tls_pending(ps3_tls* t)
{
    TLS_LOCK();
    int r = mbedtls_ssl_get_bytes_avail(&t->ssl) > 0 || mbedtls_ssl_check_pending(&t->ssl);
    TLS_UNLOCK();
    return r;
}

void ps3_tls_close(ps3_tls* t)
{
    if (!t) return;
    TLS_LOCK();
    mbedtls_ssl_close_notify(&t->ssl);   /* (one try: the socket is non-blocking) */
    mbedtls_ssl_free(&t->ssl);
    TLS_UNLOCK();
    free(t);
}
