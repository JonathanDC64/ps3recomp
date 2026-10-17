/*
 * SPU-to-SPU lock-line coherence: a write by one SPU against a line another
 * SPU has reserved (runtime/spu/spu_channels.c, runtime/spu/spu_dma.h).
 *
 * test_spu_coherency.c covers the PPU as the writer. This covers the other
 * writer, which is the one a SPURS title actually spends its time on: five SPU
 * kernel threads sharing one management line, every one of them reserving it
 * with GETLLAR and committing with PUTLLC. If a commit does not break the
 * peers' reservations, a peer's pending PUTLLC compares its snapshot against a
 * line the committer has already replaced, finds what it expects, and commits
 * over the top. Both SPUs believe they won the same compare-and-swap. Nothing
 * in the tree reports it: the queue is simply wrong afterwards.
 *
 * So the assertions are on the peer, not the writer: after a PUTLLC commit, a
 * PUTLLUC and a plain DMA PUT into a reserved line, the peer holds SPU_EVENT_LR
 * and no reservation, and its own PUTLLC then FAILS and writes nothing. The
 * cases that must stay quiet are here for the same reason they are in the PPU
 * test -- an over-eager notify is a livelock, not a safe approximation.
 *
 * The last test is the event acknowledge. An SPU acknowledging the events it
 * has read is a read-modify-write of the same word every one of those raises
 * writes, so an edge landing between the read and the write is dropped and the
 * SPU goes back to sleep holding a reservation it has already lost. Both sides
 * are atomic, and the test races them to show no raise is lost.
 *
 * Then the runtime as a writer: spu_coh_rmw32, which updates the SPURS task
 * bitsets an SPU also commits to, raced against PUTLLCs on the same word.
 *
 * spu_channels.c is compiled in rather than linked, because spu_mfc_atomic()
 * is file-static: the same reason test_spu_mfc_slots.c does it.
 *
 * Build:
 *   clang -std=gnu17 -I include -I runtime/spu -I runtime/platform \
 *         -o /tmp/test_spu_lockline_peer \
 *         runtime/spu/tests/test_spu_lockline_peer.c \
 *         runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c \
 *         runtime/spu/spu_coherency.c runtime/spu/spu_workload.c \
 *         runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
 *         runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c \
 *         runtime/spu/spurs_policy_blob_weak.c runtime/spu/spu_interp.c \
 *         runtime/platform/win32_compat.c -lpthread -lm
 *
 * Exit code: 0 if all passed, 1 if any failed. Final line prints a summary.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest memory. The DMA and atomic paths index straight off this, so it has to
 * cover every EA the test uses; a PUT below 0x10000 is rejected as malformed
 * (the null-destination guard), so the lines live well above that. */
static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

/* The unit under test. */
#include "../spu_channels.c"

/* ---------------------------------------------------------------------------
 * Harness
 * -----------------------------------------------------------------------*/
static int g_pass, g_fail;
static const char* g_current = "(none)";

#define TEST(name) g_current = (name)

#define CHECK(cond) do {                                          \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n",             \
                g_current, __FILE__, __LINE__, #cond);            \
    }                                                             \
} while (0)

#define CHECK_EQ_U32(actual, expected) do {                       \
    uint32_t _a = (uint32_t)(actual), _e = (uint32_t)(expected);  \
    if (_a == _e) { g_pass++; }                                   \
    else {                                                        \
        g_fail++;                                                 \
        fprintf(stderr, "FAIL: %s\n  at %s:%d: %s\n"              \
                        "  actual   0x%08X\n  expected 0x%08X\n", \
                g_current, __FILE__, __LINE__, #actual, _a, _e);  \
    }                                                             \
} while (0)

/* Three 128-byte lines, and a local-store address per context to stage from.
 * LINE_D..LINE_D+0x180 is the multi-line span the wide PUT covers. */
#define LINE_A   0x00020000u
#define LINE_B   0x00020180u
#define LINE_D   0x00020400u

#define LSA_A    0x00001000u
#define LSA_B    0x00002000u

/* 256 KB of local store each: not on the stack. */
static spu_context g_a, g_b;

/* ---------------------------------------------------------------------------
 * Driving the real channel path
 *
 * Every command goes through spu_wrch, so what runs is spu_mfc_atomic() and
 * mfc_do_transfer() themselves rather than a copy of their reservation rules.
 * -----------------------------------------------------------------------*/
static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void spu_atomic(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Cmd, cmd);
}

static void spu_put(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Size, size);
    wrch(ctx, MFC_TagID, 0);
    wrch(ctx, MFC_Cmd, MFC_PUT_CMD);
}

/* Fill a context's staging buffer with a recognisable pattern, so "the write
 * landed" and "the write landed from the SPU that should have won" are
 * different assertions. */
static void stage(spu_context* ctx, uint32_t lsa, uint8_t tag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        ctx->ls[(lsa + i) & SPU_LS_MASK] = (uint8_t)(tag + (i & 0x0F));
}

static int mem_is(uint32_t ea, uint8_t tag, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (g_guest_mem[ea + i] != (uint8_t)(tag + (i & 0x0F))) return 0;
    return 1;
}

/* A context is reset rather than re-created: spu_coherency.c's table of
 * reserving contexts keys on the address and is never emptied, so reusing the
 * two keeps the test inside the eight slots it holds. */
static void reset_ctx(spu_context* ctx, uint32_t spu_id)
{
    spu_context_init(ctx, 0);
    ctx->spu_id = spu_id;
}

static void reset_pair(void)
{
    reset_ctx(&g_a, 0);
    reset_ctx(&g_b, 1);
    memset(g_guest_mem + LINE_A, 0, 0x600);
}

/* ===========================================================================
 * A peer's PUTLLC commit
 * ===========================================================================*/
static void test_putllc_breaks_peer(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    /* Both SPUs take the same line, exactly as five SPURS kernels do. */
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_b.resv_valid == 1);

    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);

    TEST("the committing SPU's PUTLLC succeeds");
    CHECK_EQ_U32(g_b.atomic_stat, 0);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("...and raises SPU_EVENT_LR on the peer holding the line");
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...and drops the peer's reservation");
    CHECK(g_a.resv_valid == 0);

    /* The committer consumed its reservation; hardware does not report that as
     * a loss, so it takes no event of its own. */
    TEST("...and does not raise a self-LR on the committer");
    CHECK_EQ_U32(g_b.event_status & SPU_EVENT_LR, 0);
    CHECK(g_b.resv_valid == 0);

    /* The whole point of the event: the peer's own commit has to lose. */
    TEST("the peer's PUTLLC then fails and writes nothing");
    stage(&g_a, LSA_A, 0xA0, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));
}

/* A PUTLLC that writes back the bytes its GETLLAR read: the HighGraphics ring
 * consumer in Demon's Souls issues one per command. It succeeds while the
 * reservation is held, and fails after any store into the line -- including
 * one that put the same bytes back, as hardware fails it, and a raw store
 * that clears no reservation (the lifted PPU's stvx and dcbz). */
static void test_noop_putllc(void)
{
    reset_pair();
    memset(g_guest_mem + LINE_A, 0x5A, 128);

    TEST("a no-op PUTLLC with nothing in between succeeds");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 0);
    CHECK(g_a.resv_valid == 0);
    CHECK_EQ_U32(g_guest_mem[LINE_A + 7], 0x5A);

    TEST("a no-op PUTLLC after a peer's commit fails and writes nothing");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_b.atomic_stat, 0);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("a no-op PUTLLC after a peer's plain PUT fails");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    stage(&g_b, LSA_B, 0xC0, 16);
    spu_put(&g_b, LSA_B, LINE_A + 32, 16);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A + 32, 0xC0, 16));

    TEST("a no-op PUTLLC after a peer put the same bytes back fails");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    memcpy(&g_b.ls[LSA_B], &g_guest_mem[LINE_A], 128);
    spu_put(&g_b, LSA_B, LINE_A, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);

    /* The Demon's Souls freeze: the HighGraphics ring producer's command
     * landed this way, the consumer's no-op PUTLLC succeeded on its stale
     * snapshot, and the entry was never read. */
    TEST("a no-op PUTLLC after a store that clears no reservation fails");
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    memset(&g_guest_mem[LINE_A + 48], 0xD0, 16);   /* a lifted stvx: a raw memcpy */
    CHECK(g_a.resv_valid == 1);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK_EQ_U32(g_guest_mem[LINE_A + 63], 0xD0);   /* the PUTLLC wrote nothing back */
}

/* A PUTLLC that fails its own compare stored nothing, so there is nothing to
 * report -- the peer keeps its reservation. */
static void test_failed_putllc_is_quiet(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
    g_guest_mem[LINE_A + 8] = 0x5A;          /* the line moves under B */
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a PUTLLC that fails its compare raises nothing");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_b.atomic_stat, 1);
    CHECK_EQ_U32(g_a.event_status, 0);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_spu_lr_raise == raised);
    CHECK_EQ_U32(g_guest_mem[LINE_A + 8], 0x5A);   /* nothing was committed */
}

/* ===========================================================================
 * A peer's PUTLLUC
 * ===========================================================================*/
static void test_putlluc_breaks_peer(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a peer's PUTLLUC raises SPU_EVENT_LR and drops the reservation");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLUC_CMD);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("...so the peer's PUTLLC fails");
    stage(&g_a, LSA_A, 0xA0, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    /* Unlike PUTLLC, a PUTLLUC is unconditional: it invalidates every
     * reservation on the line including the issuer's own, and the issuer is
     * told, because it never made the compare that would have said so. */
    reset_pair();
    raised = g_spu_lr_raise;
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);

    TEST("a PUTLLUC over the issuer's own reservation reports it lost");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLUC_CMD);
    CHECK_EQ_U32(g_b.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_b.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);
}

/* ===========================================================================
 * A peer's plain DMA PUT
 * ===========================================================================*/
static void test_plain_put_breaks_peer(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a peer's plain PUT into the reserved line raises SPU_EVENT_LR");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_put(&g_b, LSA_B, LINE_A, 128);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...and the transfer itself still lands");
    CHECK(mem_is(LINE_A, 0xB0, 128));

    TEST("...so the peer's PUTLLC fails");
    stage(&g_a, LSA_A, 0xA0, 128);
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_PUTLLC_CMD);
    CHECK_EQ_U32(g_a.atomic_stat, 1);
    CHECK(mem_is(LINE_A, 0xB0, 128));

    /* A PUT smaller than a line still covers it: the reservation is per line,
     * not per byte. */
    reset_pair();
    raised = g_spu_lr_raise;
    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a 16-byte PUT into the tail of the reserved line raises it too");
    stage(&g_b, LSA_B, 0xC0, 16);
    spu_put(&g_b, LSA_B, LINE_A + 112, 16);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);
}

static void test_plain_put_elsewhere_is_quiet(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);

    TEST("a PUT into a different line leaves the reservation alone");
    stage(&g_b, LSA_B, 0xB0, 128);
    spu_put(&g_b, LSA_B, LINE_B, 128);
    CHECK_EQ_U32(g_a.event_status, 0);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_spu_lr_raise == raised);
    CHECK(mem_is(LINE_B, 0xB0, 128));

    TEST("...and a PUT ending one byte short of it does too");
    stage(&g_b, LSA_B, 0xC0, 128);
    spu_put(&g_b, LSA_B, LINE_A - 128, 128);
    CHECK_EQ_U32(g_a.event_status, 0);
    CHECK(g_a.resv_valid == 1);
    CHECK(g_spu_lr_raise == raised);
}

/* A bulk PUT covers many lines at once, and the reserved one can be anywhere
 * in the span -- including the last, which is the case a scan that stops early
 * gets wrong. */
static void test_wide_put_finds_the_reserved_line(void)
{
    reset_pair();
    unsigned long raised = g_spu_lr_raise;

    spu_atomic(&g_a, LSA_A, LINE_D + 0x180, MFC_GETLLAR_CMD);   /* the 4th line */

    TEST("a PUT spanning four lines raises once, for the reserved one");
    stage(&g_b, LSA_B, 0xB0, 0x200);
    spu_put(&g_b, LSA_B, LINE_D, 0x200);
    CHECK_EQ_U32(g_a.event_status & SPU_EVENT_LR, SPU_EVENT_LR);
    CHECK(g_a.resv_valid == 0);
    CHECK(g_spu_lr_raise == raised + 1);

    TEST("...and the whole span landed");
    CHECK(mem_is(LINE_D, 0xB0, 0x200));
}

/* ===========================================================================
 * The event acknowledge against a concurrent raise
 *
 * The acknowledge does not take the lock-line lock: it and every raise are
 * atomic read-modify-writes of event_status, so neither can overwrite the
 * other. Raced for, then, rather than built: one thread acknowledges the tag
 * event as fast as it can while the main thread raises SPU_EVENT_LR on the
 * same context, the way a peer's commit does, and checks after each raise
 * that the edge is still there. A bare read-modify-write in either loses
 * some of them within a few thousand rounds on any multi-core host.
 * ===========================================================================*/
#define ACK_TAG_EVENT  0x1u    /* MFC tag-status update: what the ack asks for */
#define ACK_ROUNDS     200000

static volatile int g_ack_stop;
static volatile long g_ack_count;

static void* ack_thread(void* arg)
{
    (void)arg;
    while (!g_ack_stop) {
        __atomic_fetch_or(&g_a.event_status, ACK_TAG_EVENT, __ATOMIC_SEQ_CST);
        wrch(&g_a, SPU_WrEventAck, ACK_TAG_EVENT);
        g_ack_count++;
    }
    return NULL;
}

static void test_ack_keeps_a_concurrent_raise(void)
{
    reset_pair();
    g_ack_stop = 0;
    g_ack_count = 0;

    spu_atomic(&g_a, LSA_A, LINE_A, MFC_GETLLAR_CMD);
    g_a.event_status = 0;

    pthread_t t;
    if (pthread_create(&t, NULL, ack_thread, NULL) != 0) {
        TEST("the acknowledging thread starts");
        CHECK(0);
        return;
    }
    while (!g_ack_count) YieldProcessor();

    int lost = 0;
    for (int i = 0; i < ACK_ROUNDS; i++) {
        /* A fresh reservation each round, so the notify below raises again. */
        spu_lockline_lock();
        g_a.resv_ea = LINE_A; g_a.resv_valid = 1;
        spu_coh_notify_write(LINE_A);        /* the raise a peer's commit makes */
        spu_lockline_unlock();
        if (!(__atomic_load_n(&g_a.event_status, __ATOMIC_SEQ_CST) & SPU_EVENT_LR)) lost++;
        __atomic_fetch_and(&g_a.event_status, ~SPU_EVENT_LR, __ATOMIC_SEQ_CST);
    }
    g_ack_stop = 1;
    pthread_join(t, NULL);

    TEST("acknowledges racing the raises ran");
    CHECK(g_ack_count > 0);
    TEST("no raise was lost to an acknowledge of another event");
    CHECK_EQ_U32((uint32_t)lost, 0);
    TEST("the acknowledge still clears what it asked for");
    g_a.event_status = ACK_TAG_EVENT | SPU_EVENT_LR;
    wrch(&g_a, SPU_WrEventAck, ACK_TAG_EVENT);
    CHECK_EQ_U32(g_a.event_status, SPU_EVENT_LR);
}

/* ===========================================================================
 * A host read-modify-write against an SPU's PUTLLC (spu_coh_rmw32)
 *
 * SPURS keeps its task bitsets in one line: the runtime clears an exited
 * task's bits while SPU code sets another task's bits with GETLLAR/PUTLLC, and
 * tasks 0-7 share a byte. As a plain read then write, a commit landing in
 * between was erased -- Demon's Souls' Havok step then waited forever for a
 * task whose enabled bit was gone. Here an SPU toggles bit 1 with PUTLLC while
 * the host sets and clears bit 0; at the end bit 1 must match the number of
 * toggles that committed, and bit 0 the host's last write.
 * ===========================================================================*/
#define RMW_ROUNDS 200000
#define RMW_SPU_BIT  0x40000000u   /* task 1 */
#define RMW_HOST_BIT 0x80000000u   /* task 0 */

static volatile int g_rmw_stop;
static volatile long g_rmw_toggles;

static uint32_t be32_at(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void* rmw_spu_thread(void* arg)
{
    (void)arg;
    while (!g_rmw_stop) {
        spu_atomic(&g_b, LSA_B, LINE_A, MFC_GETLLAR_CMD);
        uint8_t* w = &g_b.ls[LSA_B];
        const uint32_t v = be32_at(w) ^ RMW_SPU_BIT;
        w[0] = (uint8_t)(v >> 24); w[1] = (uint8_t)(v >> 16);
        w[2] = (uint8_t)(v >> 8);  w[3] = (uint8_t)v;
        spu_atomic(&g_b, LSA_B, LINE_A, MFC_PUTLLC_CMD);
        if (g_b.atomic_stat == 0) g_rmw_toggles++;
    }
    return NULL;
}

static void test_host_rmw_keeps_an_spu_commit(void)
{
    reset_pair();
    g_rmw_stop = 0;
    g_rmw_toggles = 0;

    pthread_t t;
    if (pthread_create(&t, NULL, rmw_spu_thread, NULL) != 0) {
        TEST("the PUTLLC thread starts");
        CHECK(0);
        return;
    }
    while (!g_rmw_toggles) YieldProcessor();
    for (int i = 0; i < RMW_ROUNDS; i++)
        spu_coh_rmw32(LINE_A, (i & 1) ? 0 : RMW_HOST_BIT, (i & 1) ? RMW_HOST_BIT : 0);
    spu_coh_rmw32(LINE_A, RMW_HOST_BIT, 0);
    g_rmw_stop = 1;
    pthread_join(t, NULL);

    const uint32_t w = be32_at(g_guest_mem + LINE_A);
    TEST("PUTLLCs committed while the host was writing");
    CHECK(g_rmw_toggles > 0);
    TEST("no SPU commit was erased by a host read-modify-write");
    CHECK_EQ_U32(w & RMW_SPU_BIT, (g_rmw_toggles & 1) ? RMW_SPU_BIT : 0);
    TEST("no host write was erased by an SPU commit");
    CHECK_EQ_U32(w & RMW_HOST_BIT, RMW_HOST_BIT);
    TEST("the read-modify-write returns the old value");
    CHECK_EQ_U32(spu_coh_rmw32(LINE_A, 0, RMW_HOST_BIT) & RMW_HOST_BIT, RMW_HOST_BIT);
    CHECK_EQ_U32(be32_at(g_guest_mem + LINE_A) & RMW_HOST_BIT, 0);
}

/* ===========================================================================
 * main
 * ===========================================================================*/
int main(void)
{
    test_putllc_breaks_peer();
    test_noop_putllc();
    test_failed_putllc_is_quiet();
    test_putlluc_breaks_peer();
    test_plain_put_breaks_peer();
    test_plain_put_elsewhere_is_quiet();
    test_wide_put_finds_the_reserved_line();
    test_ack_keeps_a_concurrent_raise();
    test_host_rmw_keeps_an_spu_commit();

    printf("\nSPU-to-SPU lock-line coherence tests: %d passed, %d failed\n",
           g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * The runtime beyond the SPU channel layer, stubbed. These are what
 * spu_channels.c reaches for outside its own subsystem: the PPU's reservation
 * and watch helpers, the host clock, and the title-specific SPURS hooks. None
 * of them is on the path this test drives.
 * -----------------------------------------------------------------------*/
void ppu_resv_break_store(uint64_t ea)                 { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w)
{
    (void)a; (void)v; (void)w;
}
unsigned long long ps3_ms_now(void)                    { return 0; }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int      g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;

void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void)                   { }

/* The fork's SPURS completion hooks (libs/spurs/cellSpurs.c, spurs_task.c),
 * the lv2 event-queue receive (sys_event.c) and the guard-page re-arm
 * (ppu_loader.cpp). With no exit-code address set and no bound queue (an SPU
 * receive gets CELL_EINVAL), none of them is on the path this test drives. */
uint32_t g_spurs_exitcode_ea = 0;
int  spu_spurs_task_write_exit_code(spu_context* c, uint32_t ea) { (void)c; (void)ea; return 0; }
void spurs_notify_task_completion(void)                { }
void spurs_ef_notify_taskset_round(uint32_t ea)        { (void)ea; }
int  sys_event_queue_receive_host(uint32_t q, sys_event_t* ev, unsigned ms)
{
    (void)q; (void)ev; (void)ms; return -1;
}
void ppu_guard_page_rearm(uint32_t ea, int noaccess)   { (void)ea; (void)noaccess; }
uint32_t spu_thread_bound_queue(uint32_t tid, uint32_t n) { (void)tid; (void)n; return 0; }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d)
{
    (void)c; (void)a; (void)b; (void)d;
}

/* Loader diagnostics and worker publication are outside this fixture. */
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }
