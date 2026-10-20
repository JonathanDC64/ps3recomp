/*
 * ps3recomp - cellHttp HLE
 *
 * HTTP client: create connections, send requests, receive responses.
 */

#ifndef PS3RECOMP_CELL_HTTP_H
#define PS3RECOMP_CELL_HTTP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
/* The firmware's values (libhttp: 0x80710001-0x80710fff). Titles compare some
 * of them, e.g. ALREADY_INITIALIZED from cellHttpInit. */
#define CELL_HTTP_ERROR_ALREADY_INITIALIZED     0x80710001
#define CELL_HTTP_ERROR_NOT_INITIALIZED         0x80710002
#define CELL_HTTP_ERROR_NO_MEMORY               0x80710003
#define CELL_HTTP_ERROR_NO_BUFFER               0x80710004
#define CELL_HTTP_ERROR_NO_STRING               0x80710005
#define CELL_HTTP_ERROR_INSUFFICIENT            0x80710006
#define CELL_HTTP_ERROR_INVALID_URI             0x80710007
#define CELL_HTTP_ERROR_BAD_CLIENT              0x80710010
#define CELL_HTTP_ERROR_BAD_TRANS               0x80710011
#define CELL_HTTP_ERROR_NO_CONNECTION           0x80710012
/* "No Content-Length" from cellHttpResponseGetContentLength: titles compare
 * the result with it before reading the body to the end. */
#define CELL_HTTP_ERROR_NO_CONTENT_LENGTH       0x80710017
#define CELL_HTTP_ERROR_TRANS_ABORTED           0x80710024
#define CELL_HTTP_ERROR_UNKNOWN                 0x80710051
/* Network failures are a class (0x807101xx resolver ... 0x807107xx recv)
 * with the errno in the low byte; these are the classes alone. */
#define CELL_HTTP_ERROR_NET_RESOLVER            0x80710100
#define CELL_HTTP_ERROR_NET_CONNECT             0x80710500
#define CELL_HTTP_ERROR_NET_SEND                0x80710600
#define CELL_HTTP_ERROR_NET_RECV                0x80710700
/* This runtime's older names for the codes above. */
#define CELL_HTTP_ERROR_NOT_FOUND               CELL_HTTP_ERROR_BAD_TRANS
#define CELL_HTTP_ERROR_INVALID_PARAMETER       CELL_HTTP_ERROR_NO_BUFFER
#define CELL_HTTP_ERROR_CONNECTION_FAILED       CELL_HTTP_ERROR_NET_CONNECT
#define CELL_HTTP_ERROR_SEND_FAILED             CELL_HTTP_ERROR_NET_SEND
#define CELL_HTTP_ERROR_RECV_FAILED             CELL_HTTP_ERROR_NET_RECV
#define CELL_HTTP_ERROR_TIMEOUT                 (CELL_HTTP_ERROR_NET_RECV | 0x3C)   /* ETIMEDOUT */
#define CELL_HTTP_ERROR_INVALID_URL             CELL_HTTP_ERROR_INVALID_URI
#define CELL_HTTP_ERROR_ABORTED                 CELL_HTTP_ERROR_TRANS_ABORTED

/* ---------------------------------------------------------------------------
 * HTTP methods
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_METHOD_GET        "GET"
#define CELL_HTTP_METHOD_POST       "POST"
#define CELL_HTTP_METHOD_HEAD       "HEAD"
#define CELL_HTTP_METHOD_PUT        "PUT"
#define CELL_HTTP_METHOD_DELETE     "DELETE"

/* ---------------------------------------------------------------------------
 * Status codes
 * -----------------------------------------------------------------------*/
#define CELL_HTTP_STATUS_OK                 200
#define CELL_HTTP_STATUS_CREATED            201
#define CELL_HTTP_STATUS_NO_CONTENT         204
#define CELL_HTTP_STATUS_MOVED_PERMANENTLY  301
#define CELL_HTTP_STATUS_FOUND              302
#define CELL_HTTP_STATUS_NOT_MODIFIED       304
#define CELL_HTTP_STATUS_BAD_REQUEST        400
#define CELL_HTTP_STATUS_UNAUTHORIZED       401
#define CELL_HTTP_STATUS_FORBIDDEN          403
#define CELL_HTTP_STATUS_NOT_FOUND          404
#define CELL_HTTP_STATUS_INTERNAL_ERROR     500
#define CELL_HTTP_STATUS_SERVICE_UNAVAIL    503

/* ---------------------------------------------------------------------------
 * Types
 * -----------------------------------------------------------------------*/
typedef u32 CellHttpClientId;
typedef u32 CellHttpTransId;

/* Guest order; only ever a pointer here (cellHttp.c reads it by guest offset). */
typedef struct CellHttpUri {
    const char* scheme;
    const char* hostname;
    const char* username;
    const char* password;
    const char* path;
    u32         port;
    u8          reserved[4];
} CellHttpUri;

/* Max number of concurrent clients and transactions */
#define CELL_HTTP_MAX_CLIENTS       8
#define CELL_HTTP_MAX_TRANSACTIONS  32

/* Max custom request headers per transaction */
#define CELL_HTTP_MAX_CUSTOM_HEADERS 16

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellHttpInit(void* pool, u32 poolSize);
s32 cellHttpEnd(void);

s32 cellHttpCreateClient(CellHttpClientId* clientId);
s32 cellHttpDestroyClient(CellHttpClientId clientId);

s32 cellHttpCreateTransaction(CellHttpTransId* transId, CellHttpClientId clientId,
                              const char* method, const CellHttpUri* uri);
s32 cellHttpDestroyTransaction(CellHttpTransId transId);

s32 cellHttpSendRequest(CellHttpTransId transId, const void* buf, u32 size,
                        u32* sent);
s32 cellHttpRecvResponse(CellHttpTransId transId, void* buf, u32 size,
                         u32* received);

s32 cellHttpGetResponseContentLength(CellHttpTransId transId, u64* length);
/* SDK names (CellHttpHeader** headers; size_t items/poolSize/required are 32-bit guest values) */
s32 cellHttpResponseGetStatusCode(CellHttpTransId transId, s32* code);
s32 cellHttpResponseGetContentLength(CellHttpTransId transId, u64* length);
s32 cellHttpResponseGetAllHeaders(CellHttpTransId transId, void* headers, u32* items, void* pool, u32 poolSize,
                                  u32* required);
s32 cellHttpGetStatusCode(CellHttpTransId transId, s32* code);

s32 cellHttpSetResolveTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetConnectTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetSendTimeOut(CellHttpTransId transId, u32 usec);
s32 cellHttpSetRecvTimeOut(CellHttpTransId transId, u32 usec);

s32 cellHttpSetRequestContentLength(CellHttpTransId transId, u64 length);
s32 cellHttpAddRequestHeader(CellHttpTransId transId, const char* name,
                             const char* value);
s32 cellHttpAbortTransaction(CellHttpTransId transId);

/* SDK names for the client and transaction settings (b8 / size_t / system_time_t
 * are 32- and 64-bit guest values) */
s32 cellHttpClientSetKeepAlive(CellHttpClientId clientId, u32 enable);
s32 cellHttpClientSetPerHostPoolSize(CellHttpClientId clientId, u32 poolSize);
s32 cellHttpClientSetUserAgent(CellHttpClientId clientId, const char* userAgent);
s32 cellHttpClientGetUserAgent(CellHttpClientId clientId, char* userAgent, u32 size, u32* required);
s32 cellHttpClientCloseAllConnections(CellHttpClientId clientId);
s32 cellHttpClientPollConnections(CellHttpClientId clientId, CellHttpTransId* transId, s64 usec);
s32 cellHttpTransactionAbortConnection(CellHttpTransId transId);
s32 cellHttpRequestSetContentLength(CellHttpTransId transId, u64 totalSize);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_HTTP_H */
