/*
 * ps3recomp - cellHttpUtil HLE implementation
 *
 * URL parsing, percent-encoding, and Base64 codec. Pure C, no
 * external dependencies.
 */

#include "cellHttpUtil.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include <stddef.h>   /* offsetof */

/* ---------------------------------------------------------------------------
 * Internal helpers
 * -----------------------------------------------------------------------*/

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static const char s_hex[] = "0123456789ABCDEF";

/* Is a character "unreserved" per RFC 3986? */
static int is_unreserved(u8 c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        return 1;
    return (c == '-' || c == '_' || c == '.' || c == '~');
}

/* ---------------------------------------------------------------------------
 * URL parsing
 * -----------------------------------------------------------------------*/

/* The SDK's contract (cellHttpUtilParseUri(CellHttpUri*, const char*, void*
 * pool, u32 size, u32* required)): CellHttpUri is five guest string pointers
 * (scheme, hostname, username, password, path) and a u32 port, and the strings
 * live in the caller's pool. Titles call it twice: with uri NULL to learn the
 * pool size from *required, then with a pool of that size. Demon's Souls does
 * exactly that before every cellHttpCreateTransaction. The path keeps the query
 * (it is what goes on the request line); a fragment is dropped. */
s32 cellHttpUtilParseUri(struct CellHttpUri* uri, const char* str,
                          void* pool, u32 size, u32* required)
{
    u32 uri_ea  = (u32)(uintptr_t)uri;
    u32 pool_ea = (u32)(uintptr_t)pool;
    u32 req_ea  = (u32)(uintptr_t)required;

    if (!str)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;
    if ((!pool_ea || !uri_ea) && !req_ea)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    const char* p = GUEST_PTR(str, const char*);

    /* scheme "://" */
    const char* sep = strstr(p, "://");
    if (!sep || sep == p)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
    const char* scheme = p;
    u32 scheme_len = (u32)(sep - p);
    for (u32 i = 0; i < scheme_len; i++) {
        char c = scheme[i];
        if (!isalnum((unsigned char)c) && c != '+' && c != '-' && c != '.')
            return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
    }

    /* authority: [user[:pass]@]host[:port], up to the path, query or fragment */
    const char* auth = sep + 3;
    const char* auth_end = auth + strcspn(auth, "/?#");
    const char* user = NULL; u32 user_len = 0;
    const char* pass = NULL; u32 pass_len = 0;
    const char* at = NULL;
    for (const char* q = auth; q < auth_end; q++)
        if (*q == '@') at = q;
    const char* host = auth;
    if (at) {
        const char* colon = memchr(auth, ':', (size_t)(at - auth));
        user = auth;
        user_len = (u32)((colon ? colon : at) - auth);
        if (colon) { pass = colon + 1; pass_len = (u32)(at - pass); }
        host = at + 1;
    }
    const char* host_end = auth_end;
    const char* port_str = NULL;
    if (*host == '[') {                         /* IPv6 literal */
        const char* rb = memchr(host, ']', (size_t)(auth_end - host));
        if (!rb) return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
        host_end = rb + 1;
        if (host_end < auth_end) {
            if (*host_end != ':') return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
            port_str = host_end + 1;
        }
    } else {
        const char* colon = memchr(host, ':', (size_t)(auth_end - host));
        if (colon) { host_end = colon; port_str = colon + 1; }
    }
    u32 host_len = (u32)(host_end - host);
    if (host_len == 0)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;

    u32 port;
    if (port_str && port_str < auth_end) {
        port = 0;
        for (const char* q = port_str; q < auth_end; q++) {
            if (*q < '0' || *q > '9' || port > 65535)
                return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
            port = port * 10 + (u32)(*q - '0');
        }
        if (port > 65535)
            return (s32)CELL_HTTP_UTIL_ERROR_INVALID_URI;
    } else {
        port = (scheme_len == 5 && strncmp(scheme, "https", 5) == 0) ? 443 : 80;
    }

    /* path (with query), "/" when the URL has none */
    const char* path = auth_end;
    u32 path_len = (u32)strcspn(path, "#");
    int add_slash = (path_len == 0 || path[0] != '/');

    /* pool layout as the SDK has it: scheme, host, path, username, password */
    u32 off_scheme = 0;
    u32 off_host   = off_scheme + scheme_len + 1;
    u32 off_path   = off_host + host_len + 1;
    u32 off_user   = off_path + path_len + (u32)add_slash + 1;
    u32 off_pass   = off_user + user_len + 1;
    u32 total      = off_pass + pass_len + 1;

    if (!uri_ea || !pool_ea) {
        vm_write32(req_ea, total);
        return CELL_OK;
    }
    if (size < total)
        return (s32)CELL_HTTP_UTIL_ERROR_INSUFFICIENT;
    if (req_ea)
        vm_write32(req_ea, total);

    char* out = (char*)(vm_base + pool_ea);
    memcpy(out + off_scheme, scheme, scheme_len); out[off_scheme + scheme_len] = '\0';
    memcpy(out + off_host, host, host_len);       out[off_host + host_len] = '\0';
    char* pp = out + off_path;
    if (add_slash) *pp++ = '/';
    memcpy(pp, path, path_len);                   pp[path_len] = '\0';
    if (user_len) memcpy(out + off_user, user, user_len);
    out[off_user + user_len] = '\0';
    if (pass_len) memcpy(out + off_pass, pass, pass_len);
    out[off_pass + pass_len] = '\0';

    vm_write32(uri_ea +  0, pool_ea + off_scheme);
    vm_write32(uri_ea +  4, pool_ea + off_host);
    vm_write32(uri_ea +  8, pool_ea + off_user);
    vm_write32(uri_ea + 12, pool_ea + off_pass);
    vm_write32(uri_ea + 16, pool_ea + off_path);
    vm_write32(uri_ea + 20, port);
    return CELL_OK;
}

s32 cellHttpUtilBuildUri(char* urlBuf, u32 urlBufSize,
                          const CellHttpUtilUri* uri, u32* written)
{
    if (!urlBuf || !uri)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 port = vm_read32((u32)(uintptr_t)uri + (u32)offsetof(CellHttpUtilUri, port));
    urlBuf = GUEST_PTR(urlBuf, char*);
    uri    = GUEST_PTR(uri, const CellHttpUtilUri*);

    int n = snprintf(urlBuf, urlBufSize, "%s://%s",
                     uri->scheme[0] ? uri->scheme : "http",
                     uri->hostname);
    if (n < 0) return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    /* Append port if non-default */
    int default_port = (strcmp(uri->scheme, "https") == 0) ? 443 : 80;
    if (port && port != (u32)default_port) {
        n += snprintf(urlBuf + n, urlBufSize - n, ":%u", port);
    }

    /* Path */
    n += snprintf(urlBuf + n, urlBufSize - n, "%s",
                  uri->path[0] ? uri->path : "/");

    /* Query */
    if (uri->query[0])
        n += snprintf(urlBuf + n, urlBufSize - n, "?%s", uri->query);

    /* Fragment */
    if (uri->fragment[0])
        n += snprintf(urlBuf + n, urlBufSize - n, "#%s", uri->fragment);

    if (written)
        vm_write32((u32)(uintptr_t)written, (u32)n);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * URL encoding / decoding
 * -----------------------------------------------------------------------*/

/* Host-pointer core, shared by the guest entry point and by
 * cellHttpUtilFormUrlEncode.
 *
 * FormUrlEncode used to call cellHttpUtilEscapeUri directly, which is an HLE
 * ENTRY POINT: it translates its arguments as guest EAs. Two of the three
 * pointers it was handed were already host pointers (out, and the address of a
 * local u32 for `written`), so translating them a second time sent the escaped
 * output and its length somewhere else entirely. An entry point cannot be
 * called from inside the library it lives in. */
static s32 http_escape_uri_host(char* out, u32 outSize,
                                const u8* src, u32 srcSize, u32* written)
{
    if (!out || !src)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < srcSize; i++) {
        if (is_unreserved(src[i])) {
            if (pos >= outSize)
                return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
            out[pos++] = (char)src[i];
        } else {
            if (pos + 3 > outSize)
                return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
            out[pos++] = '%';
            out[pos++] = s_hex[(src[i] >> 4) & 0xF];
            out[pos++] = s_hex[src[i] & 0xF];
        }
    }

    if (pos < outSize)
        out[pos] = '\0';

    if (written)
        *written = pos;

    return CELL_OK;
}

s32 cellHttpUtilEscapeUri(char* out, u32 outSize,
                           const u8* src, u32 srcSize, u32* written)
{
    u32 n = 0;
    s32 rc = http_escape_uri_host(GUEST_PTR(out, char*), outSize,
                                  GUEST_PTR(src, const u8*), srcSize, &n);
    if (rc == CELL_OK && written)
        vm_write32((u32)(uintptr_t)written, n);
    return rc;
}

s32 cellHttpUtilUnescapeUri(u8* out, u32 outSize,
                             const char* src, u32* required)
{
    src = GUEST_PTR(src, const char*);
    out = GUEST_PTR(out, u8*);
    if (!src)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;
    if (!out && !required)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 srcSize = (u32)strlen(src);
    u32 pos = 0;
    for (u32 i = 0; i < srcSize; i++) {
        if (out && pos >= outSize)
            return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

        u8 decoded;
        if (src[i] == '%' && i + 2 < srcSize) {
            int hi = hex_digit(src[i + 1]);
            int lo = hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded = (u8)((hi << 4) | lo);
                i += 2;
            } else {
                decoded = (u8)src[i];
            }
        } else {
            decoded = (u8)src[i];
        }

        if (out)
            out[pos] = decoded;
        pos++;
    }

    if (required)
        vm_write32((u32)(uintptr_t)required, (u32)pos);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Form URL encoding
 * -----------------------------------------------------------------------*/

s32 cellHttpUtilFormUrlEncode(char* out, u32 outSize,
                               const char* key, const char* value, u32* written)
{
    if (!out || !key || !value)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    out   = GUEST_PTR(out, char*);
    key   = GUEST_PTR(key, const char*);
    value = GUEST_PTR(value, const char*);

    u32 key_esc_len = outSize;
    u32 pos = 0;

    /* Encode key */
    s32 rc = http_escape_uri_host(out, outSize, (const u8*)key,
                                    (u32)strlen(key), &key_esc_len);
    if (rc != CELL_OK) return rc;
    pos = key_esc_len;

    /* = */
    if (pos >= outSize)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    out[pos++] = '=';

    /* Encode value */
    u32 val_esc_len = outSize - pos;
    rc = http_escape_uri_host(out + pos, outSize - pos, (const u8*)value,
                                (u32)strlen(value), &val_esc_len);
    if (rc != CELL_OK) return rc;
    pos += val_esc_len;

    if (pos < outSize)
        out[pos] = '\0';

    if (written)
        vm_write32((u32)(uintptr_t)written, (u32)pos);

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Base64
 * -----------------------------------------------------------------------*/

static const char s_b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

s32 cellHttpUtilBase64Encode(char* out, u32 outSize,
                              const u8* data, u32 dataSize, u32* written)
{
    if (!out || !data)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 needed = ((dataSize + 2) / 3) * 4;
    if (needed >= outSize)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;

    u32 pos = 0;
    for (u32 i = 0; i < dataSize; i += 3) {
        u32 n = ((u32)data[i]) << 16;
        if (i + 1 < dataSize) n |= ((u32)data[i + 1]) << 8;
        if (i + 2 < dataSize) n |= data[i + 2];

        out[pos++] = s_b64_table[(n >> 18) & 0x3F];
        out[pos++] = s_b64_table[(n >> 12) & 0x3F];
        out[pos++] = (i + 1 < dataSize) ? s_b64_table[(n >> 6) & 0x3F] : '=';
        out[pos++] = (i + 2 < dataSize) ? s_b64_table[n & 0x3F] : '=';
    }

    out[pos] = '\0';
    if (written)
        vm_write32((u32)(uintptr_t)written, (u32)pos);

    return CELL_OK;
}

static int b64_decode_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

s32 cellHttpUtilBase64Decode(u8* out, u32 outSize,
                              const char* encoded, u32 encodedLen, u32* written)
{
    if (!out || !encoded)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_PARAM;

    u32 pos = 0;
    for (u32 i = 0; i < encodedLen; i += 4) {
        int a = (i < encodedLen) ? b64_decode_char(encoded[i]) : 0;
        int b = (i + 1 < encodedLen) ? b64_decode_char(encoded[i + 1]) : 0;
        int c = (i + 2 < encodedLen) ? b64_decode_char(encoded[i + 2]) : -1;
        int d = (i + 3 < encodedLen) ? b64_decode_char(encoded[i + 3]) : -1;

        if (a < 0 || b < 0)
            return (s32)CELL_HTTP_UTIL_ERROR_PARSE_FAILED;

        u32 triple = ((u32)a << 18) | ((u32)b << 12);
        if (c >= 0) triple |= ((u32)c << 6);
        if (d >= 0) triple |= (u32)d;

        if (pos >= outSize) return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
        out[pos++] = (u8)((triple >> 16) & 0xFF);

        if (c >= 0 && pos < outSize)
            out[pos++] = (u8)((triple >> 8) & 0xFF);

        if (d >= 0 && pos < outSize)
            out[pos++] = (u8)(triple & 0xFF);
    }

    if (written)
        vm_write32((u32)(uintptr_t)written, (u32)pos);

    return CELL_OK;
}

/* cellHttpUtilBase64Encoder(out, input, len) and cellHttpUtilBase64Decoder
 * (output, in, len), the SDK pair Demon's Souls imports. Both return the
 * number of bytes written, which the title uses as the length:
 * - the encoder writes the padded text and a NUL after it (Demon's Souls sizes
 *   the buffer 4 bytes past the text);
 * - the decoder takes a multiple of 4 and writes no NUL (the buffer is exactly
 *   len / 4 * 3 bytes). */
s32 cellHttpUtilBase64Encoder(char* out, const void* input, u32 len)
{
    if (!input || !len)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;
    if (!out)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    const u8* src = (const u8*)(vm_base + (u32)(uintptr_t)input);
    char* dst = (char*)(vm_base + (u32)(uintptr_t)out);
    u32 pos = 0;
    for (u32 i = 0; i < len; i += 3) {
        u32 n = ((u32)src[i]) << 16;
        if (i + 1 < len) n |= ((u32)src[i + 1]) << 8;
        if (i + 2 < len) n |= src[i + 2];
        dst[pos++] = s_b64_table[(n >> 18) & 0x3F];
        dst[pos++] = s_b64_table[(n >> 12) & 0x3F];
        dst[pos++] = (i + 1 < len) ? s_b64_table[(n >> 6) & 0x3F] : '=';
        dst[pos++] = (i + 2 < len) ? s_b64_table[n & 0x3F] : '=';
    }
    dst[pos] = '\0';
    return (s32)pos;
}

s32 cellHttpUtilBase64Decoder(char* output, const void* in, u32 len)
{
    if (!in)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_STRING;
    if (len & 3)
        return (s32)CELL_HTTP_UTIL_ERROR_INVALID_LENGTH;
    if (!output)
        return (s32)CELL_HTTP_UTIL_ERROR_NO_BUFFER;
    const char* src = (const char*)(vm_base + (u32)(uintptr_t)in);
    u8* dst = (u8*)(vm_base + (u32)(uintptr_t)output);
    u32 pos = 0;
    for (u32 i = 0; i < len; i += 4) {
        int v[4];
        for (int k = 0; k < 4; k++) {
            v[k] = src[i + k] == '=' ? -2 : b64_decode_char(src[i + k]);
            if (v[k] == -1 || (v[k] == -2 && (k < 2 || i + 4 < len)))
                return (s32)CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER;
        }
        if (v[2] == -2 && v[3] != -2)
            return (s32)CELL_HTTP_UTIL_ERROR_INVALID_CHARACTER;
        u32 n = ((u32)v[0] << 18) | ((u32)v[1] << 12) |
                ((u32)(v[2] < 0 ? 0 : v[2]) << 6) | (u32)(v[3] < 0 ? 0 : v[3]);
        dst[pos++] = (u8)(n >> 16);
        if (v[2] >= 0) dst[pos++] = (u8)(n >> 8);
        if (v[3] >= 0) dst[pos++] = (u8)n;
    }
    return (s32)pos;
}
