/*
 * ps3recomp - sceNpScore (leaderboards) over psnr
 *
 * Title contexts and transactions are local ids. The requests go to the psnr
 * server's leaderboards (np_psnr.h); offline, every transaction finishes with
 * SCE_NP_COMMUNITY_SERVER_ERROR_... so titles show "leaderboards unavailable".
 *
 * Async is the SDK's poll model: *Async returns at once, the title polls
 * PollAsync(transId) until it returns 0 and reads the result there. PollAsync
 * pumps psnr itself, so a title that polls from a worker thread without
 * calling cellSysutilCheckCallback still finishes. The synchronous forms send
 * and then pump until the reply lands.
 *
 * Game data goes up and comes down whole: the runtime gathers the title's
 * chunks and sends one message, and on a read fetches once and serves the
 * title's chunks from that. It runs only when the port has set the hook in
 * sceNp.h; those sessions use the newer score messages, which carry a
 * character id. A server that doesn't know them answers ERROR bad-request,
 * and the session goes back to the first messages with no game data.
 *
 * The ranking calls take up to 15 arguments; everything past r10 comes from
 * the caller's parameter save area (PPC64 ELFv1: arg n>8 at SP+112+8*(n-9)),
 * so those are ctx handlers registered by np_score_register_ctx() -- called
 * from ppu_sysprx_register().
 *
 * Layouts (SDK ABI):
 *   SceNpScoreRankData                                             128 bytes
 *     0 npId (36) | 36 onlineName (52) | 88 pcId | 92 serialRank | 96 rank
 *     100 highestRank | 104 hasGameData | 112 scoreValue s64 | 120 recordDate u64
 *   SceNpScorePlayerRankData { s32 hasData; pad[4]; RankData }    136 bytes
 *   SceNpScoreComment { char utf8Comment[64] }                     64 bytes
 */

#include "sceNp.h"
#include "np_psnr.h"
#include "../../runtime/ppu/ppu_context.h"
#include "../../runtime/ppu/ppu_memory.h"
#include "../../include/ps3emu/nid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static void sleep_10ms(void) { Sleep(10); }
#else
#  include <time.h>
static void sleep_10ms(void) { struct timespec ts = { 0, 10000000L }; nanosleep(&ts, NULL); }
#endif

#define SCORE_ERROR_NO_SERVER      0x8002A1A1u   /* community server error: unavailable */
#define SCORE_ERROR_INVALID_ID     0x8002A106u
#define SCORE_ERROR_ABORTED        0x8002A10Au
#define SCORE_ERROR_RANKING_NOT_FOUND 0x8002A412u   /* SCE_NP_COMMUNITY_SERVER_ERROR_GAME_RANKING_NOT_FOUND */

#define PSNR_ERR_BAD_REQUEST 1u                /* a server that predates the newer messages */
#define GAME_DATA_MAX 0xF000u                 /* one psnr frame */

#define TITLE_MAX 8
#define TRANS_MAX 32

enum { T_FREE, T_IDLE, T_PENDING, T_DONE };

typedef struct {
    int state;
    int title;
    s32 result;
    /* where the reply goes */
    u32 tmp_rank_ea;
    u32 rank_ea, comment_ea, total_ea, last_sort_ea;
    u32 array_num;
    int by_id;
    s32 pc_id;              /* sceNpScoreSetPlayerCharacterId */
    /* a request in the newer messages, kept in the first ones for an old server */
    int v2;
    u8* resend;
    u32 resend_len;
    u8  resend_type;
    np_psnr_reply_fn resend_fn;
    /* game data going up: the title's chunks, gathered */
    u8* up;
    u32 up_total, up_have, up_board;
    s64 up_score;
    /* game data coming down: fetched once, served in the title's chunks */
    u8* down;
    u32 down_size, down_pos, down_board, down_recv, down_total_ea, down_data_ea;
    s32 down_pc;
    char down_id[16];
} score_trans;

static int         s_titles[TITLE_MAX + 1];   /* 1-based ids */
static score_trans s_trans[TRANS_MAX + 1];

extern const char* np_psnr_online_id(void);

static np_score_game_data_hook_fn s_game_data_hook;
static int s_old_server;   /* for this session, the first messages only */

void np_score_set_game_data_hook(np_score_game_data_hook_fn fn) { s_game_data_hook = fn; }

static int use_v2(void) { return s_game_data_hook != NULL && !s_old_server; }

/* ---------------------------------------------------------------------------
 * Contexts
 * -----------------------------------------------------------------------*/

s32 sceNpScoreCreateTitleCtx(u32 commId, u32 passphrase, u32 selfNpId)
{
    (void)passphrase; (void)selfNpId;
    for (int i = 1; i <= TITLE_MAX; i++)
        if (!s_titles[i]) {
            s_titles[i] = 1;
            /* psnr scopes boards by the title's communication ID, which the
             * connection already carries; connect now in case Matching2 hasn't. */
            if (commId) {
                char id[16] = {0}, data[10] = {0};
                memcpy(data, vm_ptr8(commId), 9);
                snprintf(id, sizeof(id), "%s_%02u", data, vm_read8(commId + 10));
                np_psnr_connect(id);
            }
            return i;
        }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

s32 sceNpScoreDestroyTitleCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)SCORE_ERROR_INVALID_ID;
    s_titles[titleCtxId] = 0;
    return CELL_OK;
}

s32 sceNpScoreCreateTransactionCtx(s32 titleCtxId)
{
    if (titleCtxId < 1 || titleCtxId > TITLE_MAX || !s_titles[titleCtxId])
        return (s32)SCORE_ERROR_INVALID_ID;
    for (int i = 1; i <= TRANS_MAX; i++)
        if (s_trans[i].state == T_FREE) {
            memset(&s_trans[i], 0, sizeof(s_trans[i]));
            s_trans[i].state = T_IDLE;
            s_trans[i].title = titleCtxId;
            return i;
        }
    return (s32)SCE_NP_ERROR_OUT_OF_MEMORY;
}

static score_trans* trans_get(s32 id)
{
    return (id >= 1 && id <= TRANS_MAX && s_trans[id].state != T_FREE) ? &s_trans[id] : NULL;
}

s32 sceNpScoreDestroyTransactionCtx(s32 transId)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    free(t->resend); free(t->up); free(t->down);
    t->resend = t->up = t->down = NULL;
    t->state = T_FREE;   /* a reply still in flight finds the slot free and is dropped */
    return CELL_OK;
}

s32 sceNpScoreSetTimeout(s32 id, u32 timeout)  { (void)id; (void)timeout; return CELL_OK; }

s32 sceNpScoreSetPlayerCharacterId(s32 id, s32 pcId)
{
    score_trans* t = trans_get(id);
    if (t) t->pc_id = pcId;
    return CELL_OK;
}

s32 sceNpScoreAbortTransaction(s32 transId)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    if (t->state == T_PENDING) { t->state = T_DONE; t->result = (s32)SCORE_ERROR_ABORTED; }
    return CELL_OK;
}

/* 0 = finished (result in *result), 1 = still running. */
s32 sceNpScorePollAsync(s32 transId, u32 result)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    if (t->state == T_PENDING) np_psnr_pump();
    if (t->state == T_PENDING) return 1;
    if (result) vm_write32(result, (u32)t->result);
    return 0;
}

s32 sceNpScoreWaitAsync(s32 transId, u32 result)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    for (int i = 0; i < 1000 && t->state == T_PENDING; i++) {   /* ~10 s */
        np_psnr_pump();
        if (t->state == T_PENDING) sleep_10ms();
    }
    if (t->state == T_PENDING) { t->state = T_DONE; t->result = (s32)SCORE_ERROR_NO_SERVER; }
    if (result) vm_write32(result, (u32)t->result);
    return t->result;
}

/* ---------------------------------------------------------------------------
 * Requests
 * -----------------------------------------------------------------------*/

/* Finish a transaction that never reached the server. */
static s32 fail_now(score_trans* t, s32 err)
{
    t->state = T_DONE;
    t->result = err;
    return CELL_OK;
}

static s32 trans_id_of(score_trans* t) { return (s32)(t - s_trans); }

/* Send a request in the newer messages when this session can, keeping the
 * first form in case the server turns out not to know them. Non-zero if sent. */
static int send_req(score_trans* t, u8 type2, const void* body2, u32 len2,
                    u8 type1, const void* body1, u32 len1, np_psnr_reply_fn fn)
{
    void* user = (void*)(uintptr_t)trans_id_of(t);
    free(t->resend);
    t->resend = NULL;
    t->v2 = 0;
    if (!use_v2()) return np_psnr_request(type1, body1, len1, fn, user) != 0;
    t->resend = (u8*)malloc(len1 ? len1 : 1);
    if (!t->resend) return 0;
    memcpy(t->resend, body1, len1);
    t->resend_len = len1;
    t->resend_type = type1;
    t->resend_fn = fn;
    t->v2 = 1;
    return np_psnr_request(type2, body2, len2, fn, user) != 0;
}

/* ERROR bad-request to a newer message: the server predates them. Go back to
 * the first messages for this session and send this request again. Non-zero
 * if it went out; the reply then comes to the same handler. */
static int fall_back(score_trans* t, const psnr_msg* m)
{
    if (!t->v2 || !t->resend || m->type != PSNR_ERROR || m->len < 4 ||
        psnr_get32(m->data) != PSNR_ERR_BAD_REQUEST)
        return 0;
    s_old_server = 1;
    t->v2 = 0;
    printf("[sceNpScore] the server has no game data support; off for this session\n");
    return np_psnr_request(t->resend_type, t->resend, t->resend_len, t->resend_fn,
                           (void*)(uintptr_t)trans_id_of(t)) != 0;
}

static void on_recorded(void* user, const psnr_msg* m)
{
    score_trans* t = trans_get((s32)(uintptr_t)user);
    if (!t || t->state != T_PENDING) return;
    if (fall_back(t, m)) return;
    t->state = T_DONE;
    if (m->type != PSNR_SCORE_RECORDED || m->len < 4) { t->result = (s32)SCORE_ERROR_NO_SERVER; return; }
    if (t->tmp_rank_ea) vm_write32(t->tmp_rank_ea, psnr_get32(m->data));
    t->result = CELL_OK;
}

/* RECORD_SCORE: board u32 | order u8 | score s64 | comment blob.
 * RECORD_SCORE2 has the character id after the board. */
static s32 record(s32 transId, u32 boardId, s64 score, u32 comment, u32 tmpRank)
{
    score_trans* t = trans_get(transId);
    u8 body[4 + 1 + 8 + 2 + 64], body2[4 + 4 + 1 + 8 + 2 + 64];
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    t->state = T_PENDING;
    t->tmp_rank_ea = tmpRank;
    if (!np_psnr_connected()) return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);

    u8* p = psnr_put32(body, boardId);
    /* ponytail: every board ranks higher-is-better. A title with lap-time
     * boards needs its order passed (1) -- a per-title board table. */
    *p++ = 0;
    p = psnr_put64(p, (u64)score);
    size_t cl = comment ? strnlen((const char*)vm_ptr8(comment), 63) : 0;
    p = psnr_put16(p, (u16)cl);
    if (cl) { memcpy(p, vm_ptr8(comment), cl); p += cl; }
    printf("[sceNpScore] RecordScore(board %u, %lld)\n", boardId, (long long)score);
    u8* q = psnr_put32(body2, boardId);
    q = psnr_put32(q, (u32)t->pc_id);
    memcpy(q, body + 4, (size_t)(p - (body + 4)));
    q += p - (body + 4);
    if (!send_req(t, PSNR_RECORD_SCORE2, body2, (u32)(q - body2),
                  PSNR_RECORD_SCORE, body, (u32)(p - body), on_recorded))
        return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    return CELL_OK;
}

/* (transId, boardId, score, comment*, gameInfo*, tmpRank*, prio, option) */
s32 sceNpScoreRecordScoreAsync(s32 transId, u32 boardId, s64 score, u32 comment,
                               u32 gameInfo, u32 tmpRank, s32 prio, u32 option)
{
    (void)gameInfo; (void)prio; (void)option;
    return record(transId, boardId, score, comment, tmpRank);
}

/* (transId, boardId, score, comment*, gameInfo*, tmpRank*, option) */
s32 sceNpScoreRecordScore(s32 transId, u32 boardId, s64 score, u32 comment,
                          u32 gameInfo, u32 tmpRank, u32 option)
{
    (void)gameInfo; (void)option;
    s32 r = record(transId, boardId, score, comment, tmpRank);
    return r < 0 ? r : sceNpScoreWaitAsync(transId, 0);
}

/* Write one SceNpScoreRankData from a RANKING entry, or a RANKING2 one, which
 * also has the character id and whether the row has game data. */
static const u8* put_rank(u32 at, const u8* e, const u8* end, u32 comment_at, int v2)
{
    if (e + 4 + 16 + (v2 ? 4 : 0) + 8 + (v2 ? 1 : 0) + 2 > end) return NULL;
    u32 rank = psnr_get32(e);
    char id[17] = {0};
    memcpy(id, e + 4, 16);
    const u8* p = e + 20;
    u32 pc = 0;
    if (v2) { pc = psnr_get32(p); p += 4; }
    u64 score = psnr_get64(p);
    p += 8;
    u32 has_game_data = v2 ? *p++ != 0 : 0;
    u16 cl = psnr_get16(p);
    const u8* c = p + 2;
    if (c + cl + 8 > end) return NULL;
    u64 when = psnr_get64(c + cl);

    memset(vm_ptr8(at), 0, 128);
    memcpy(vm_ptr8(at), id, strnlen(id, 16));           /* npId.handle */
    memcpy(vm_ptr8(at + 36), id, strnlen(id, 16));      /* onlineName */
    vm_write32(at + 88, pc);                            /* pcId */
    vm_write32(at + 92, rank);                          /* serialRank */
    vm_write32(at + 96, rank);                          /* rank */
    vm_write32(at + 100, rank);                         /* highestRank */
    vm_write32(at + 104, has_game_data);                /* hasGameData */
    vm_write64(at + 112, score);
    /* CellRtcTick: microseconds since 0001-01-01 */
    vm_write64(at + 120, when ? (when + 62135596800ull) * 1000000ull : 0);
    if (comment_at) {
        memset(vm_ptr8(comment_at), 0, 64);
        memcpy(vm_ptr8(comment_at), c, cl < 63 ? cl : 63);
    }
    return c + cl + 8;
}

static void on_ranking(void* user, const psnr_msg* m)
{
    score_trans* t = trans_get((s32)(uintptr_t)user);
    if (!t || t->state != T_PENDING) return;
    if (fall_back(t, m)) return;
    t->state = T_DONE;
    if ((m->type != PSNR_RANKING && m->type != PSNR_RANKING2) || m->len < 6) {
        t->result = (s32)SCORE_ERROR_NO_SERVER;
        return;
    }
    const int v2 = m->type == PSNR_RANKING2;
    u32 total = psnr_get32(m->data);
    u16 n = psnr_get16(m->data + 4);
    const u8 *e = m->data + 6, *end = m->data + m->len;
    u32 filled = 0;
    for (u16 i = 0; i < n && i < t->array_num && e; i++) {
        u32 stride = t->by_id ? 136 : 128;
        u32 at = t->rank_ea + i * stride;
        if (t->by_id) vm_write32(at, psnr_get32(e) ? 1 : 0);   /* hasData */
        e = put_rank(t->by_id ? at + 8 : at, e, end, t->comment_ea ? t->comment_ea + i * 64 : 0, v2);
        filled++;
    }
    if (t->total_ea) vm_write32(t->total_ea, total);
    if (t->last_sort_ea) vm_write64(t->last_sort_ea, 0);
    /* The SDK returns the number of entries written. A range with nothing in
     * it (an empty board, or a start past the last rank) is the server's
     * GAME_RANKING_NOT_FOUND, not a count of 0: titles page a board until they
     * see it, and a 0 never advances their position. */
    t->result = (!t->by_id && filled == 0) ? (s32)SCORE_ERROR_RANKING_NOT_FOUND : (s32)filled;
}

static u64 stack_arg(ppu_context* ctx, int n)   /* n >= 9 */
{
    return vm_read64((u32)ctx->gpr[1] + 112u + 8u * (u32)(n - 9));
}

/* GetRankingByRange[Async](transId, boardId, startSerialRank, rankArray*, rankArraySize,
 *   commentArray*, commentArraySize, infoArray*, infoArraySize, arrayNum,
 *   lastSortDate*, totalRecord*, [prio,] option) */
static void ranking_by_range(ppu_context* ctx)
{
    score_trans* t = trans_get((s32)ctx->gpr[3]);
    u8 body[10];
    if (!t) { ctx->gpr[3] = (u64)(s64)(s32)SCORE_ERROR_INVALID_ID; return; }
    t->state = T_PENDING;
    t->by_id = 0;
    t->rank_ea = (u32)ctx->gpr[6];
    t->comment_ea = (u32)ctx->gpr[8];
    t->array_num = (u32)stack_arg(ctx, 10);
    t->last_sort_ea = (u32)stack_arg(ctx, 11);
    t->total_ea = (u32)stack_arg(ctx, 12);
    ctx->gpr[3] = 0;
    if (!np_psnr_connected()) { fail_now(t, (s32)SCORE_ERROR_NO_SERVER); return; }
    u8* p = psnr_put32(body, (u32)ctx->gpr[4]);
    p = psnr_put32(p, (u32)ctx->gpr[5] ? (u32)ctx->gpr[5] : 1);
    p = psnr_put16(p, (u16)(t->array_num > 100 ? 100 : t->array_num));
    if (!send_req(t, PSNR_GET_RANKING2, body, 10, PSNR_GET_RANKING, body, 10, on_ranking))
        fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
}

static void hle_ranking_by_range_async(ppu_context* ctx) { ranking_by_range(ctx); }
static void hle_ranking_by_range(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    ranking_by_range(ctx);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

/* GetRankingByNpId[Async](transId, boardId, npIdArray*, npIdArraySize, rankArray*,
 *   rankArraySize, commentArray*, commentArraySize, infoArray*, infoArraySize,
 *   arrayNum, lastSortDate*, totalRecord*, [prio,] option).
 * The PcId forms take SceNpScoreNpIdPcId entries (44 bytes: npId, pcId, pad). */
static void ranking_by_npid(ppu_context* ctx, int with_pc)
{
    score_trans* t = trans_get((s32)ctx->gpr[3]);
    if (!t) { ctx->gpr[3] = (u64)(s64)(s32)SCORE_ERROR_INVALID_ID; return; }
    t->state = T_PENDING;
    t->by_id = 1;
    t->rank_ea = (u32)ctx->gpr[7];
    t->comment_ea = (u32)ctx->gpr[9];
    t->array_num = (u32)stack_arg(ctx, 11);
    t->last_sort_ea = (u32)stack_arg(ctx, 12);
    t->total_ea = (u32)stack_arg(ctx, 13);
    ctx->gpr[3] = 0;
    if (!np_psnr_connected()) { fail_now(t, (s32)SCORE_ERROR_NO_SERVER); return; }
    u32 ids = (u32)ctx->gpr[5], n = t->array_num > 100 ? 100 : t->array_num;
    const u32 stride = with_pc ? 44 : 36;
    u8* body = (u8*)malloc(6 + 16 * n), *body2 = (u8*)malloc(6 + 20 * n);
    if (!body || !body2) { free(body); free(body2); fail_now(t, (s32)SCORE_ERROR_NO_SERVER); return; }
    u8* p = psnr_put32(body, (u32)ctx->gpr[4]);
    p = psnr_put16(p, (u16)n);
    u8* q = psnr_put32(body2, (u32)ctx->gpr[4]);
    q = psnr_put16(q, (u16)n);
    for (u32 i = 0; i < n; i++) {   /* SceNpId is 36 bytes; the handle leads */
        const u32 at = ids + i * stride;
        memset(p, 0, 16);
        memcpy(p, vm_ptr8(at), strnlen((const char*)vm_ptr8(at), 16));
        memcpy(q, p, 16);
        p += 16;
        q = psnr_put32(q + 16, with_pc ? vm_read32(at + 36) : 0);
    }
    if (!send_req(t, PSNR_GET_RANKING_ID2, body2, (u32)(q - body2),
                  PSNR_GET_RANKING_BY_ID, body, (u32)(p - body), on_ranking))
        fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    free(body);
    free(body2);
}

static void hle_ranking_by_npid_async(ppu_context* ctx) { ranking_by_npid(ctx, 0); }
static void hle_ranking_by_npid(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    ranking_by_npid(ctx, 0);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

static void hle_ranking_by_npid_pcid_async(ppu_context* ctx) { ranking_by_npid(ctx, 1); }
static void hle_ranking_by_npid_pcid(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    ranking_by_npid(ctx, 1);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

/* ---------------------------------------------------------------------------
 * Game data
 * -----------------------------------------------------------------------*/

static void on_game_data_recorded(void* user, const psnr_msg* m)
{
    score_trans* t = trans_get((s32)(uintptr_t)user);
    if (!t || t->state != T_PENDING) return;
    t->state = T_DONE;
    t->result = m->type == PSNR_OK ? CELL_OK : (s32)SCORE_ERROR_NO_SERVER;
}

/* One chunk of a title's game data. RECORD_GAME_DATA goes out when the last
 * chunk is in: board u32 | character id u32 | score s64 | data blob */
static s32 record_game_data(s32 transId, u32 board, s64 score, u32 total, u32 send, u32 data)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    if (!use_v2()) return fail_now(t, CELL_OK);   /* accepted and dropped */
    if (!total || total > GAME_DATA_MAX || send > total)
        return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    if (!t->up || t->up_total != total || t->up_board != board || t->up_score != score ||
        t->up_have + send > total) {
        free(t->up);
        t->up = (u8*)malloc(total);
        if (!t->up) return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
        t->up_total = total;
        t->up_board = board;
        t->up_score = score;
        t->up_have = 0;
    }
    memcpy(t->up + t->up_have, vm_ptr8(data), send);
    t->up_have += send;
    if (t->up_have < total) return fail_now(t, CELL_OK);

    u8* rec = t->up;
    t->up = NULL;
    if (!s_game_data_hook(1, board, rec, total)) { free(rec); return fail_now(t, CELL_OK); }
    u8* body = (u8*)malloc(4 + 4 + 8 + 2 + total);
    if (!body || !np_psnr_connected()) {
        free(rec);
        free(body);
        return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    }
    u8* p = psnr_put32(body, board);
    p = psnr_put32(p, (u32)t->pc_id);
    p = psnr_put64(p, (u64)score);
    p = psnr_put16(p, (u16)total);
    memcpy(p, rec, total);
    p += total;
    free(rec);
    t->state = T_PENDING;
    u32 sent = np_psnr_request(PSNR_RECORD_GAME_DATA, body, (u32)(p - body), on_game_data_recorded,
                               (void*)(uintptr_t)trans_id_of(t));
    free(body);
    if (!sent) return fail_now(t, (s32)SCORE_ERROR_NO_SERVER);
    return CELL_OK;
}

/* (transId, boardId, score, totalSize, sendSize, data*, [prio,] option) */
static void hle_record_game_data_async(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)record_game_data((s32)ctx->gpr[3], (u32)ctx->gpr[4], (s64)ctx->gpr[5],
                                             (u32)ctx->gpr[6], (u32)ctx->gpr[7], (u32)ctx->gpr[8]);
}
static void hle_record_game_data(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    hle_record_game_data_async(ctx);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

/* Give the title its next chunk. The result is the bytes written this call. */
static void serve_game_data(score_trans* t)
{
    u32 n = t->down_size - t->down_pos;
    if (n > t->down_recv) n = t->down_recv;
    if (t->down_total_ea) vm_write32(t->down_total_ea, t->down_size);
    if (n && t->down_data_ea) memcpy(vm_ptr8(t->down_data_ea), t->down + t->down_pos, n);
    t->down_pos += n;
    t->result = (s32)n;
    if (t->down_pos >= t->down_size) { free(t->down); t->down = NULL; }
}

/* GAME_DATA: score s64 | data blob. Anything else, or a record the hook turns
 * down, is no game data for this row. */
static void on_game_data(void* user, const psnr_msg* m)
{
    score_trans* t = trans_get((s32)(uintptr_t)user);
    if (!t || t->state != T_PENDING) return;
    t->state = T_DONE;
    t->result = (s32)SCORE_ERROR_RANKING_NOT_FOUND;
    if (m->type != PSNR_GAME_DATA || m->len < 10) return;
    const u32 n = psnr_get16(m->data + 8);
    if (!n || m->len < 10 + n || !s_game_data_hook) return;
    u8* d = (u8*)malloc(n);
    if (!d) return;
    memcpy(d, m->data + 10, n);
    if (!s_game_data_hook(0, t->down_board, d, n)) { free(d); return; }
    t->down = d;
    t->down_size = n;
    t->down_pos = 0;
    serve_game_data(t);
}

/* GET_GAME_DATA: board u32 | online id [16] | character id u32 */
static s32 get_game_data(s32 transId, u32 board, u32 npId, u32 total_ea, u32 recv, u32 data)
{
    score_trans* t = trans_get(transId);
    if (!t) return (s32)SCORE_ERROR_INVALID_ID;
    if (!use_v2()) return fail_now(t, CELL_OK);   /* nothing to read */
    char id[16] = {0};
    memcpy(id, vm_ptr8(npId), strnlen((const char*)vm_ptr8(npId), 16));
    t->down_total_ea = total_ea;
    t->down_data_ea = data;
    t->down_recv = recv;
    if (t->down && t->down_board == board && t->down_pc == t->pc_id && !memcmp(t->down_id, id, 16)) {
        t->state = T_DONE;
        serve_game_data(t);
        return CELL_OK;
    }
    free(t->down);
    t->down = NULL;
    t->down_board = board;
    t->down_pc = t->pc_id;
    memcpy(t->down_id, id, 16);
    if (!np_psnr_connected()) return fail_now(t, (s32)SCORE_ERROR_RANKING_NOT_FOUND);
    u8 body[4 + 16 + 4];
    u8* p = psnr_put32(body, board);
    memcpy(p, id, 16);
    p = psnr_put32(p + 16, (u32)t->pc_id);
    t->state = T_PENDING;
    if (!np_psnr_request(PSNR_GET_GAME_DATA, body, (u32)(p - body), on_game_data,
                         (void*)(uintptr_t)trans_id_of(t)))
        return fail_now(t, (s32)SCORE_ERROR_RANKING_NOT_FOUND);
    return CELL_OK;
}

/* (transId, boardId, npId*, totalSize*, recvSize, data*, [prio,] option) */
static void hle_get_game_data_async(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(s64)get_game_data((s32)ctx->gpr[3], (u32)ctx->gpr[4], (u32)ctx->gpr[5],
                                          (u32)ctx->gpr[6], (u32)ctx->gpr[7], (u32)ctx->gpr[8]);
}
static void hle_get_game_data(ppu_context* ctx)
{
    s32 id = (s32)ctx->gpr[3];
    hle_get_game_data_async(ctx);
    if ((s32)ctx->gpr[3] == 0) ctx->gpr[3] = (u64)(s64)sceNpScoreWaitAsync(id, 0);
}

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

void np_score_register_ctx(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByRangeAsync"),
                         "sceNpScoreGetRankingByRangeAsync", hle_ranking_by_range_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByRange"),
                         "sceNpScoreGetRankingByRange", hle_ranking_by_range);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByNpIdAsync"),
                         "sceNpScoreGetRankingByNpIdAsync", hle_ranking_by_npid_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByNpId"),
                         "sceNpScoreGetRankingByNpId", hle_ranking_by_npid);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByNpIdPcIdAsync"),
                         "sceNpScoreGetRankingByNpIdPcIdAsync", hle_ranking_by_npid_pcid_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetRankingByNpIdPcId"),
                         "sceNpScoreGetRankingByNpIdPcId", hle_ranking_by_npid_pcid);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreRecordGameDataAsync"),
                         "sceNpScoreRecordGameDataAsync", hle_record_game_data_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreRecordGameData"),
                         "sceNpScoreRecordGameData", hle_record_game_data);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetGameDataAsync"),
                         "sceNpScoreGetGameDataAsync", hle_get_game_data_async);
    ps3_hle_register_ctx(ps3_compute_nid("sceNpScoreGetGameData"),
                         "sceNpScoreGetGameData", hle_get_game_data);
}
