#pragma once
#include <stddef.h>
#include <stdint.h>
/* TLS on the links to the online server that has a sign-in (ps3emu/ps3_net_signin.h): its /dss/v1 calls, the
 * title's HTTP requests to it, and psnr's connection to it (libs/network/ps3_tls.c, with mbedTLS 3.6 in
 * third_party/mbedtls). The server's certificate must chain to a root built in here, Let's Encrypt's four (ISRG
 * Root X1 and X2, Root YE and YR), and carry the server's name. Off unless set:
 *
 *   PS3_NET_TLS=1          the server speaks TLS; its certificate names PS3_NET_SIGNIN's host
 *   PS3_NET_TLS=<name>     the same, with <name> as the name to check (a test server reached by its address)
 *   PS3_NET_CA_FILE=path   the roots to trust instead (PEM): a test server's own
 *
 * With PS3_NET_TLS set, what can't go over TLS doesn't go: a failed handshake is a failed connection. psnr's relay
 * streams and P2P stay as they are (players' traffic, as on a PS3 and on RPCS3).
 *
 * The socket belongs to the caller, who connects it and closes it after ps3_tls_close. While TLS runs on it, it is
 * non-blocking; the calls below wait (poll) up to their timeout. One thread at a time per connection. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps3_tls ps3_tls;

#define PS3_TLS_AGAIN (-2)   /* (timeout 0) nothing yet */

/* PS3_NET_TLS is set, and not to "0". */
int ps3_tls_enabled(void);

/* The name the server's certificate must carry: 0 and the name in out[cap], or -1 if TLS is off or there is none. */
int ps3_tls_server_name(char* out, size_t cap);

/* The handshake on a connected TCP socket (a SOCKET or an fd), as a client, checking the certificate against name.
 * The connection, or NULL with the reason in why[cap] (for the log). */
ps3_tls* ps3_tls_connect(intptr_t sock, const char* name, int timeout_ms, char* why, size_t wcap);

/* All n bytes, within timeout_ms: 0, or -1 (an error, or the time ran out). */
int ps3_tls_send_all(ps3_tls* t, const void* p, size_t n, int timeout_ms);

/* Up to n bytes: how many (> 0), 0 when the server closed the connection, -1 on an error or when timeout_ms ran out
 * first; with timeout_ms 0, PS3_TLS_AGAIN when nothing has come. */
int ps3_tls_recv(ps3_tls* t, void* p, size_t n, int timeout_ms);

/* 1 if data has come that ps3_tls_recv would return without the socket being readable (it is inside TLS already). */
int ps3_tls_pending(ps3_tls* t);

/* Says goodbye to the server (no wait) and frees the connection. The socket stays open. NULL is fine. */
void ps3_tls_close(ps3_tls* t);

#ifdef __cplusplus
}
#endif
