/*
 * psnr_rudp.h - psnr's byte stream over UDP: a small reliable layer for a path two routers were punched open on
 * (psnr_intro.h), where TCP doesn't get through.
 *
 * A state machine with no sockets, threads or clock of its own: the caller hands it the datagrams that came for its
 * session (psnr_rudp_input), the time, and a function that sends one datagram; it hands back the stream's bytes in
 * order (psnr_rudp_read). Both ends use it: psnr_host's players over UDP, and np_psnr's session to a host.
 *
 *   0x08 DATA   "PSNR" 08 | session u32 | number u32 | ack u32 | early u32 | payload (at most 1200 bytes)
 *   0x09 CLOSE  "PSNR" 09 | session u32 | reason u8
 *
 * - number: 1, 2, 3... for a datagram with payload; 0 for one with none (an ack, or a keepalive).
 * - ack: every datagram up to that number came; early: bit i set = number ack + 2 + i came too.
 * - The receiver puts them back in order and drops copies. The sender sends one again when it isn't acknowledged
 *   within a timeout that starts at 250 ms and doubles up to 2 s; up to 32 are in flight.
 * - Nothing sent for 5 s: an empty one, so the routers keep the mapping (a loading screen).
 * - A session with data unacknowledged for 20 s, or that heard nothing for 30 s, is over (psnr_rudp_dead), as a
 *   lost TCP connection.
 *
 * Not thread-safe: one caller at a time.
 */
#ifndef PSNR_RUDP_H
#define PSNR_RUDP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSNR_RUDP_PAYLOAD   1200
#define PSNR_RUDP_HDR       21
#define PSNR_RUDP_DGRAM     (PSNR_RUDP_HDR + PSNR_RUDP_PAYLOAD)
#define PSNR_RUDP_WINDOW    32
/* (overridable for tests) */
#ifndef PSNR_RUDP_RTO_MS
#  define PSNR_RUDP_RTO_MS       250
#endif
#ifndef PSNR_RUDP_RTO_MAX_MS
#  define PSNR_RUDP_RTO_MAX_MS   2000
#endif
#ifndef PSNR_RUDP_KEEPALIVE_MS
#  define PSNR_RUDP_KEEPALIVE_MS 5000
#endif
#ifndef PSNR_RUDP_UNACKED_MS
#  define PSNR_RUDP_UNACKED_MS   20000
#endif
#ifndef PSNR_RUDP_SILENT_MS
#  define PSNR_RUDP_SILENT_MS    30000
#endif
#define PSNR_RUDP_QUEUE_MAX (1u << 20)   /* bytes written and not yet sent: more is refused */
#define PSNR_RUDP_READ_MAX  (4u << 20)   /* bytes come and not yet read: more ends the session */

/* psnr_rudp_dead's answers */
enum { PSNR_RUDP_ALIVE = 0, PSNR_RUDP_UNACKED = 1, PSNR_RUDP_SILENT = 2, PSNR_RUDP_CLOSED = 3, PSNR_RUDP_BROKEN = 4 };
/* CLOSE's reasons */
enum { PSNR_RUDP_BYE = 0, PSNR_RUDP_TIMEOUT = 1, PSNR_RUDP_REFUSED = 2 };

typedef struct psnr_rudp psnr_rudp;

/* Sends one datagram to the other end; its result is not looked at (a lost one is sent again). */
typedef void (*psnr_rudp_send_fn)(void* ctx, const uint8_t* pkt, size_t len);

psnr_rudp* psnr_rudp_new(uint32_t session, psnr_rudp_send_fn send, void* ctx, uint64_t now_ms);
void       psnr_rudp_free(psnr_rudp* r);
uint32_t   psnr_rudp_session(const psnr_rudp* r);

/* Stream bytes to send: queued, and sent as the window allows. -1 (nothing queued) when the queue is full or the
 * session is over. */
int    psnr_rudp_write(psnr_rudp* r, const void* bytes, size_t n, uint64_t now_ms);
/* Stream bytes that came, in order: up to cap of them, the number copied. */
size_t psnr_rudp_read(psnr_rudp* r, void* out, size_t cap);
size_t psnr_rudp_readable(const psnr_rudp* r);
/* 1 when everything written was sent and acknowledged (a last reply arrived: the session can close). */
int    psnr_rudp_flushed(const psnr_rudp* r);

/* A datagram that came for this session (DATA or CLOSE, with its "PSNR" header). Others are ignored. */
void psnr_rudp_input(psnr_rudp* r, const void* pkt, size_t len, uint64_t now_ms);
/* Resends, acks and keepalives that are due. Call it every few tens of milliseconds, and after an input. */
void psnr_rudp_tick(psnr_rudp* r, uint64_t now_ms);
/* The ms until the next thing tick has to do (at most max_ms). */
unsigned psnr_rudp_due_ms(const psnr_rudp* r, uint64_t now_ms, unsigned max_ms);

/* PSNR_RUDP_ALIVE, or why the session is over. */
int  psnr_rudp_dead(const psnr_rudp* r, uint64_t now_ms);
/* Over from this end: a CLOSE to the other (twice, in case one is lost). */
void psnr_rudp_close(psnr_rudp* r, uint8_t reason);

/* The session number of a DATA or CLOSE datagram; 0 if buf is neither. */
uint32_t psnr_rudp_session_of(const void* buf, size_t len);
/* A CLOSE for a session there is no state for (an old one, an unknown one). */
size_t psnr_rudp_close_packet(uint8_t out[10], uint32_t session, uint8_t reason);

#ifdef __cplusplus
}
#endif

#endif /* PSNR_RUDP_H */
