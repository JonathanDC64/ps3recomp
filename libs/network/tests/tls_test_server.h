/*
 * tls_test_server.h - the server side of TLS for the network tests, after
 * they include ../ps3_tls.c: a test CA (TTS_CA), a server certificate it
 * signed for "dss.test" and 127.0.0.1, and one for the same names from a CA
 * nobody trusts. Made for these tests only (openssl, EC P-256, valid until
 * 2126); the keys guard nothing.
 *
 * tts_write_ca(path) writes TTS_CA for PS3_NET_CA_FILE; tts_accept() does the
 * server's handshake on an accepted socket and returns a ps3_tls the test
 * reads and writes with ps3_tls_recv / ps3_tls_send_all. Under ps3_tls.c's
 * lock like the client, so both can run at once.
 */
#ifndef TLS_TEST_SERVER_H
#define TLS_TEST_SERVER_H

#include "mbedtls/pk.h"

static const char TTS_CA[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBfzCCASSgAwIBAgIUVvyzU7ALdUbrOVOP/BXEq/Ppb9UwCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRcHMzcmVjb21wIHRlc3QgQ0EwIBcNMjYxMDI2MTY1MjMzWhgP\n"
    "MjEyNjEwMDIxNjUyMzNaMBwxGjAYBgNVBAMMEXBzM3JlY29tcCB0ZXN0IENBMFkw\n"
    "EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEJBW0/KBaY7QSTUc+cWeayQrjuuAFmhTb\n"
    "oco7EUApHj/Mql7zOEPACMKvV2Gaamt84RVOEm1mKpRQh7V+B+Sdz6NCMEAwDwYD\n"
    "VR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFITtE+PBUWJu\n"
    "2r/2pbpajocDJRDiMAoGCCqGSM49BAMCA0kAMEYCIQDdbfUU54ryLC705YptHmbD\n"
    "T6AmHJMsmVpitClPNdU/egIhAKNnc3RiGl6HPyFzYyEnujcdG/93NbeZKi4XQ1S0\n"
    "sC4U\n"
    "-----END CERTIFICATE-----\n";

static const char TTS_SERVER_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBwDCCAWWgAwIBAgIUMvWxDk6qb3iyX7k2gey9qMpQjWQwCgYIKoZIzj0EAwIw\n"
    "HDEaMBgGA1UEAwwRcHMzcmVjb21wIHRlc3QgQ0EwIBcNMjYxMDI2MTY1MjM0WhgP\n"
    "MjEyNjEwMDIxNjUyMzRaMBMxETAPBgNVBAMMCGRzcy50ZXN0MFkwEwYHKoZIzj0C\n"
    "AQYIKoZIzj0DAQcDQgAEzmzMvYsJ0AXEHVDHmWrlPJ+Xu7zEMszktbR1XFWHlt73\n"
    "n4adeqiAIwQMih3pr9olJsnpw7ioy4K1x/CR8s01waOBizCBiDAZBgNVHREEEjAQ\n"
    "gghkc3MudGVzdIcEfwAAATAJBgNVHRMEAjAAMAsGA1UdDwQEAwIHgDATBgNVHSUE\n"
    "DDAKBggrBgEFBQcDATAdBgNVHQ4EFgQUQheBar4nazj2wkAQ1w1bWYpTb/UwHwYD\n"
    "VR0jBBgwFoAUhO0T48FRYm7av/alulqOhwMlEOIwCgYIKoZIzj0EAwIDSQAwRgIh\n"
    "ALR4zeMzrWb/pwfsZ5na2g0OBjEDGv9AVRmoWD/vlAaTAiEAy82EcLyWKJ6UMnTH\n"
    "TJqDTMDXSTX95HFepn3knjd8UBM=\n"
    "-----END CERTIFICATE-----\n";

static const char TTS_SERVER_KEY[] =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEICKSLKjnTw1zIdGNe5NccqXDV3OhGVWUouiDBjksjMmToAoGCCqGSM49\n"
    "AwEHoUQDQgAEzmzMvYsJ0AXEHVDHmWrlPJ+Xu7zEMszktbR1XFWHlt73n4adeqiA\n"
    "IwQMih3pr9olJsnpw7ioy4K1x/CR8s01wQ==\n"
    "-----END EC PRIVATE KEY-----\n";

static const char TTS_STRANGER_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBwzCCAWmgAwIBAgIUPOzFt/GOVU8YnBAAkb1HC/lbOmEwCgYIKoZIzj0EAwIw\n"
    "IDEeMBwGA1UEAwwVcHMzcmVjb21wIHN0cmFuZ2VyIENBMCAXDTI2MTAyNjE2NTIz\n"
    "NFoYDzIxMjYxMDAyMTY1MjM0WjATMREwDwYDVQQDDAhkc3MudGVzdDBZMBMGByqG\n"
    "SM49AgEGCCqGSM49AwEHA0IABFA9J2Zg7EIxtrolRGtvyfiLSj4dWtHV09LqTd6E\n"
    "1G+pSxjhr4hWkHeVMJbRVPz+HqoZ4Gs7HHfHv0hPSOc/IlOjgYswgYgwGQYDVR0R\n"
    "BBIwEIIIZHNzLnRlc3SHBH8AAAEwCQYDVR0TBAIwADALBgNVHQ8EBAMCB4AwEwYD\n"
    "VR0lBAwwCgYIKwYBBQUHAwEwHQYDVR0OBBYEFA92FfILqzJOwJFBgfs/7KffZVR1\n"
    "MB8GA1UdIwQYMBaAFB4lUtiIDKuUmpYNUaUNQXTYh1hDMAoGCCqGSM49BAMCA0gA\n"
    "MEUCIEA4ckPphVtE06/e1tuP1kQ30HBxJBwyFg37Y1+WalPYAiEA/B42W4nPxP7W\n"
    "sxYR9J5EhNGM4AoCw+qkgsSdSsNKUrs=\n"
    "-----END CERTIFICATE-----\n";

static const char TTS_STRANGER_KEY[] =
    "-----BEGIN EC PRIVATE KEY-----\n"
    "MHcCAQEEIERtGXoDnEdGkEO+E8cg9pCZAoWd2HaFVjPcePCaaepFoAoGCCqGSM49\n"
    "AwEHoUQDQgAEUD0nZmDsQjG2uiVEa2/J+ItKPh1a0dXT0upN3oTUb6lLGOGviFaQ\n"
    "d5UwltFU/P4eqhngazscd8e/SE9I5z8iUw==\n"
    "-----END EC PRIVATE KEY-----\n";

enum { TTS_OURS, TTS_STRANGER };

static mbedtls_ssl_config tts_conf[2];
static mbedtls_x509_crt   tts_crt[2];
static mbedtls_pk_context tts_key[2];
static int                tts_ready;

static void tts_write_ca(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f || fwrite(TTS_CA, 1, sizeof TTS_CA - 1, f) != sizeof TTS_CA - 1) {
        printf("tls_test_server: cannot write %s\n", path);
        exit(1);
    }
    fclose(f);
}

/* The two server configurations, once (the client's setup first: the random generator, PSA). */
static void tts_setup(void)
{
    char why[200] = "";
    TLS_LOCK();
    if (!tts_ready) {
        if (tls_setup(why, sizeof why) != 0) {
            printf("tls_test_server: setup: %s\n", why);
            exit(1);
        }
        const char* certs[2] = {TTS_SERVER_CERT, TTS_STRANGER_CERT};
        const char* keys[2] = {TTS_SERVER_KEY, TTS_STRANGER_KEY};
        for (int i = 0; i < 2; i++) {
            mbedtls_x509_crt_init(&tts_crt[i]);
            mbedtls_pk_init(&tts_key[i]);
            mbedtls_ssl_config_init(&tts_conf[i]);
            int r = mbedtls_x509_crt_parse(&tts_crt[i], (const unsigned char*)certs[i], strlen(certs[i]) + 1);
            if (r == 0)
                r = mbedtls_pk_parse_key(&tts_key[i], (const unsigned char*)keys[i], strlen(keys[i]) + 1, NULL, 0,
                                         mbedtls_ctr_drbg_random, &tls_drbg);
            if (r == 0)
                r = mbedtls_ssl_config_defaults(&tts_conf[i], MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT);
            if (r == 0) {
                mbedtls_ssl_conf_rng(&tts_conf[i], mbedtls_ctr_drbg_random, &tls_drbg);
                r = mbedtls_ssl_conf_own_cert(&tts_conf[i], &tts_crt[i], &tts_key[i]);
            }
            if (r != 0) {
                tls_error_text(r, why, sizeof why);
                printf("tls_test_server: certificate %d: %s\n", i, why);
                exit(1);
            }
        }
        tts_ready = 1;
    }
    TLS_UNLOCK();
}

/* The server's handshake on sock with the given certificate, within ms: the connection, or NULL. */
static ps3_tls* tts_accept(intptr_t sock, int which, int ms)
{
    char why[256] = "";
    tts_setup();
    TLS_LOCK();
    ps3_tls* t = tls_new(sock, &tts_conf[which], NULL, why, sizeof why);
    TLS_UNLOCK();
    if (t && tls_handshake(t, NULL, ms, why, sizeof why) != 0) t = NULL;
    if (!t) printf("tls_test_server: no handshake: %s\n", why);
    return t;
}

#endif
