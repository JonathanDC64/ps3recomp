/*
 * test_np_score - sceNpScore's range requests without a server.
 *
 * Compiles sceNpScore.c into this file with psnr stubbed: the stubs capture
 * what would go to the server, and "pumping" delivers the reply the test put
 * there. Checks what a title paging through a board relies on: the number of
 * rows comes back while there are rows, and a range with none is the server's
 * GAME_RANKING_NOT_FOUND rather than a count of 0, and where each field of
 * a row lands (the SDK's layout, which titles read directly).
 *
 *   clang-cl /I include /Fe:test_np_score.exe libs/network/tests/test_np_score.c
 *   cc -std=gnu17 -I include -o test_np_score libs/network/tests/test_np_score.c
 */

#include "../sceNpScore.c"

#include <assert.h>

uint8_t* vm_base;
int g_resv_store_active = 0;
uint32_t g_ww_lo = 0, g_ww_hi = 0;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
int  spu_coh_is_reserved(uint32_t addr) { (void)addr; return 0; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_coh_notify_write(uint32_t addr) { (void)addr; }
void ps3_ww_report_inline(uint32_t addr, uint64_t val, int width) { (void)addr; (void)val; (void)width; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{ (void)nid; (void)name; (void)fn; }
const char* np_psnr_online_id(void) { return "PS3Player"; }

/* psnr: requests are captured; a pump delivers the reply the test prepared. */
static np_psnr_reply_fn s_fn;
static void* s_user;
static u8 s_body[0x4000];
static u32 s_body_len;
static uint8_t s_sent_type;
static int s_sent;
static uint8_t s_reply_type;
static u8 s_reply[8192];
static u32 s_reply_len;
static uint8_t s_next_type;            /* the reply after this one, when there is one */
static u8 s_next[8192];
static u32 s_next_len;
static int s_connected = 1;

int np_psnr_connect(const char* comm_id) { (void)comm_id; return 0; }
int np_psnr_connected(void) { return s_connected; }
uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len, np_psnr_reply_fn fn, void* user)
{
    s_sent_type = type;
    s_body_len = len;
    s_sent++;
    if (len <= sizeof(s_body)) memcpy(s_body, body, len);
    s_fn = fn; s_user = user;
    return 1;
}
void np_psnr_pump(void)
{
    if (!s_fn) return;
    psnr_msg m = { s_reply_type, 1, s_reply, s_reply_len };
    np_psnr_reply_fn fn = s_fn;
    s_fn = NULL;
    fn(s_user, &m);
    if (s_next_type) {
        s_reply_type = s_next_type;
        memcpy(s_reply, s_next, s_next_len);
        s_reply_len = s_next_len;
        s_next_type = 0;
    }
}

enum { RANKS = 0x1000, COMMENTS = 0x8000, LAST_SORT = 0xC000, TOTAL = 0xC100, STACK = 0xD000 };

/* A RANKING reply with `rows` entries; total is the size of the whole board. */
static void set_reply(u32 total, u16 rows)
{
    u8* p = s_reply;
    s_reply_type = PSNR_RANKING;
    p = psnr_put32(p, total);
    p = psnr_put16(p, rows);
    for (u16 i = 0; i < rows; i++) {
        p = psnr_put32(p, i + 1u);          /* rank */
        memset(p, 0, 16);
        memcpy(p, "player", 6);             /* online id */
        p += 16;
        p = psnr_put64(p, 100u - i);        /* score */
        p = psnr_put16(p, 0);               /* no comment */
        p = psnr_put64(p, 1000u);           /* recorded */
    }
    s_reply_len = (u32)(p - s_reply);
}

/* An ERROR reply carrying a psnr error code. */
static void set_error(u32 code)
{
    s_reply_type = PSNR_ERROR;
    psnr_put32(s_reply, code);
    s_reply_len = 4;
}

/* One RANKING2 row, for player "host", character 3, with game data. */
static void set_reply2(u64 score)
{
    u8* p = s_reply;
    s_reply_type = PSNR_RANKING2;
    p = psnr_put32(p, 1);
    p = psnr_put16(p, 1);
    p = psnr_put32(p, 1);                   /* rank */
    memset(p, 0, 16);
    memcpy(p, "host", 4);
    p += 16;
    p = psnr_put32(p, 3);                   /* character */
    p = psnr_put64(p, score);
    *p++ = 1;                               /* has game data */
    p = psnr_put16(p, 0);                   /* no comment */
    p = psnr_put64(p, 1000u);               /* recorded */
    s_reply_len = (u32)(p - s_reply);
}

/* The hook under test: counts, keeps what it saw, and can refuse. */
static int s_hook_calls, s_hook_upload, s_hook_refuse;
static u32 s_hook_size;
static int game_data_hook(int upload, uint32_t board, uint8_t* data, uint32_t size)
{
    (void)board;
    s_hook_calls++;
    s_hook_upload = upload;
    s_hook_size = size;
    data[1] = 0x5A;                         /* it may change the bytes */
    return !s_hook_refuse;
}

/* Call the synchronous range form as a title does. The arguments past r10
 * live in the caller's parameter save area. */
static s32 get_range(s32 trans, u32 start, u32 num)
{
    ppu_context* ctx = (ppu_context*)calloc(1, sizeof(ppu_context));   /* large: not on the stack */
    ctx->gpr[1] = STACK;
    ctx->gpr[3] = (u64)(s64)trans;
    ctx->gpr[4] = 7;            /* boardId */
    ctx->gpr[5] = start;
    ctx->gpr[6] = RANKS;
    ctx->gpr[7] = num * 128;
    ctx->gpr[8] = COMMENTS;
    ctx->gpr[9] = num * 64;
    ctx->gpr[10] = 0;           /* infoArray */
    vm_write64(STACK + 112 + 8 * (9 - 9), 0);          /* infoArraySize */
    vm_write64(STACK + 112 + 8 * (10 - 9), num);       /* arrayNum */
    vm_write64(STACK + 112 + 8 * (11 - 9), LAST_SORT);
    vm_write64(STACK + 112 + 8 * (12 - 9), TOTAL);
    vm_write64(STACK + 112 + 8 * (13 - 9), 0);         /* option */
    hle_ranking_by_range(ctx);
    s32 r = (s32)ctx->gpr[3];
    free(ctx);
    return r;
}

static s32 record_chunk(s32 trans, u32 board, s64 score, u32 total, u32 send, u32 data)
{
    ppu_context* ctx = (ppu_context*)calloc(1, sizeof(ppu_context));
    ctx->gpr[3] = (u64)(s64)trans;
    ctx->gpr[4] = board;
    ctx->gpr[5] = (u64)score;
    ctx->gpr[6] = total;
    ctx->gpr[7] = send;
    ctx->gpr[8] = data;
    hle_record_game_data(ctx);
    s32 r = (s32)ctx->gpr[3];
    free(ctx);
    return r;
}

static s32 get_chunk(s32 trans, u32 board, u32 npid, u32 total_ea, u32 recv, u32 data)
{
    ppu_context* ctx = (ppu_context*)calloc(1, sizeof(ppu_context));
    ctx->gpr[3] = (u64)(s64)trans;
    ctx->gpr[4] = board;
    ctx->gpr[5] = npid;
    ctx->gpr[6] = total_ea;
    ctx->gpr[7] = recv;
    ctx->gpr[8] = data;
    hle_get_game_data(ctx);
    s32 r = (s32)ctx->gpr[3];
    free(ctx);
    return r;
}

static void game_data_checks(s32 trans)
{
    enum { DATA = 0x20000, RECV = 0x30000, NPID = 0x40000, TOTAL_EA = 0x40100 };
    const u32 size = 0x1160, chunk = 0x1000;
    for (u32 i = 0; i < size; i++) vm_write8(DATA + i, (u8)i);
    memcpy(vm_ptr8(NPID), "host", 5);

    /* No hook: recording is accepted and dropped, reading finds nothing, and
     * the server hears nothing new. */
    np_score_set_game_data_hook(NULL);
    int before = s_sent;
    assert(record_chunk(trans, 1, 100, size, chunk, DATA) == 0);
    assert(get_chunk(trans, 1, NPID, TOTAL_EA, chunk, RECV) == 0);
    assert(s_sent == before);

    np_score_set_game_data_hook(game_data_hook);
    sceNpScoreSetPlayerCharacterId(trans, 3);

    /* Rows come over the newer message, with the character and the flag. */
    set_reply2(0x07EA000B00080014ull);
    assert(get_range(trans, 1, 100) == 1);
    assert(s_sent_type == PSNR_GET_RANKING2);
    assert(vm_read32(RANKS + 88) == 3);
    assert(vm_read32(RANKS + 104) == 1);
    assert(vm_read64(RANKS + 112) == 0x07EA000B00080014ull);

    /* An upload goes out once, when the last chunk is in. */
    before = s_sent;
    s_hook_calls = 0;
    assert(record_chunk(trans, 1, 100, size, chunk, DATA) == 0);
    assert(s_sent == before && s_hook_calls == 0);
    s_reply_type = PSNR_OK;
    s_reply_len = 0;
    assert(record_chunk(trans, 1, 100, size, size - chunk, DATA + chunk) == 0);
    assert(s_sent == before + 1 && s_sent_type == PSNR_RECORD_GAME_DATA);
    assert(s_hook_calls == 1 && s_hook_upload == 1 && s_hook_size == size);
    assert(s_body_len == 4 + 4 + 8 + 2 + size);
    assert(psnr_get32(s_body) == 1 && psnr_get32(s_body + 4) == 3);
    assert(psnr_get64(s_body + 8) == 100 && psnr_get16(s_body + 16) == size);
    assert(s_body[18] == 0 && s_body[19] == 0x5A && s_body[18 + 0xFFF] == 0xFF);

    /* A refused upload isn't sent, and the title isn't told. */
    s_hook_refuse = 1;
    before = s_sent;
    assert(record_chunk(trans, 1, 100, size, size, DATA) == 0);
    assert(s_sent == before);
    s_hook_refuse = 0;

    /* A server's refusal fails only that call. */
    set_error(PSNR_ERR_BAD_REQUEST);
    assert(record_chunk(trans, 1, 100, size, size, DATA) < 0);
    assert(!s_old_server);

    /* A read fetches once and serves the title's chunks. */
    {
        u8* r = s_reply;
        s_reply_type = PSNR_GAME_DATA;
        r = psnr_put64(r, 100);
        r = psnr_put16(r, (u16)size);
        for (u32 i = 0; i < size; i++) *r++ = (u8)(i * 3);
        s_reply_len = (u32)(r - s_reply);
    }
    before = s_sent;
    s_hook_calls = 0;
    assert(get_chunk(trans, 2, NPID, TOTAL_EA, chunk, RECV) == (s32)chunk);
    assert(s_sent == before + 1 && s_sent_type == PSNR_GET_GAME_DATA);
    assert(psnr_get32(s_body) == 2 && !memcmp(s_body + 4, "host", 5) && psnr_get32(s_body + 20) == 3);
    assert(s_hook_calls == 1 && s_hook_upload == 0);
    assert(vm_read32(TOTAL_EA) == size);
    assert(vm_read8(RECV + 2) == 6 && vm_read8(RECV + 1) == 0x5A);
    assert(get_chunk(trans, 2, NPID, TOTAL_EA, chunk, RECV + chunk) == (s32)(size - chunk));
    assert(s_sent == before + 1 && s_hook_calls == 1);
    assert(vm_read8(RECV + size - 1) == (u8)((size - 1) * 3));

    /* No game data, a refused record, or a late error: no game data, and the
     * session carries on. */
    set_error(2);
    assert((u32)get_chunk(trans, 3, NPID, TOTAL_EA, chunk, RECV) == SCORE_ERROR_RANKING_NOT_FOUND);
    set_error(PSNR_ERR_BAD_REQUEST);
    assert((u32)get_chunk(trans, 3, NPID, TOTAL_EA, chunk, RECV) == SCORE_ERROR_RANKING_NOT_FOUND);
    assert(!s_old_server);
    {
        u8* r = s_reply;
        s_reply_type = PSNR_GAME_DATA;
        r = psnr_put64(r, 100);
        r = psnr_put16(r, 16);
        memset(r, 7, 16);
        s_reply_len = (u32)(r + 16 - s_reply);
    }
    s_hook_refuse = 1;
    assert((u32)get_chunk(trans, 4, NPID, TOTAL_EA, chunk, RECV) == SCORE_ERROR_RANKING_NOT_FOUND);
    s_hook_refuse = 0;

    /* A record over the character limit fails that record and nothing more. */
    set_error(6);
    assert(sceNpScoreRecordScore(trans, 1, 500, 0, 0, 0, 0) < 0);
    assert(s_sent_type == PSNR_RECORD_SCORE2 && !s_old_server);

    /* An old server: a newer message is refused as bad, the request goes again
     * in the first form, and the rest of the session stays there. */
    set_error(PSNR_ERR_BAD_REQUEST);
    s_next_type = PSNR_RANKING;
    {
        u8 keep_type = s_reply_type; u32 keep_len = s_reply_len; u8 keep[16];
        memcpy(keep, s_reply, keep_len);
        set_reply(1, 1);
        memcpy(s_next, s_reply, s_reply_len);
        s_next_len = s_reply_len;
        s_next_type = s_reply_type;
        s_reply_type = keep_type;
        memcpy(s_reply, keep, keep_len);
        s_reply_len = keep_len;
    }
    assert(get_range(trans, 1, 100) == 1);
    assert(s_old_server && s_sent_type == PSNR_GET_RANKING);
    assert(vm_read32(RANKS + 104) == 0);
    set_reply(1, 1);
    assert(get_range(trans, 1, 100) == 1 && s_sent_type == PSNR_GET_RANKING);
    before = s_sent;
    assert(record_chunk(trans, 1, 100, size, size, DATA) == 0);
    assert(get_chunk(trans, 1, NPID, TOTAL_EA, chunk, RECV) == 0);
    assert(s_sent == before);
    s_old_server = 0;
    np_score_set_game_data_hook(NULL);
    sceNpScoreSetPlayerCharacterId(trans, 0);
}

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 20);

    s32 title = sceNpScoreCreateTitleCtx(0, 0, 0);
    assert(title > 0);
    s32 trans = sceNpScoreCreateTransactionCtx(title);
    assert(trans > 0);

    game_data_checks(trans);

    /* A page with rows: the count, the first row and the board's size. */
    set_reply(150, 100);
    assert(get_range(trans, 1, 100) == 100);
    assert(vm_read32(TOTAL) == 150);
    assert(!memcmp(vm_ptr8(RANKS), "player", 7));         /* npId.handle */
    assert(!memcmp(vm_ptr8(RANKS + 36), "player", 7));    /* onlineName */
    assert(vm_read32(RANKS + 88) == 0);                   /* pcId */
    assert(vm_read32(RANKS + 92) == 1);                   /* serialRank */
    assert(vm_read32(RANKS + 96) == 1);                   /* rank */
    assert(vm_read32(RANKS + 100) == 1);                  /* highestRank */
    assert(vm_read32(RANKS + 104) == 0);                  /* hasGameData */
    assert(vm_read64(RANKS + 112) == 100);                /* scoreValue */
    assert(vm_read64(RANKS + 120) == (1000u + 62135596800ull) * 1000000ull);
    assert(vm_read32(RANKS + 128 + 96) == 2);
    assert(vm_read64(RANKS + 128 + 112) == 99);

    /* A score with its high word set is still only a score: it must not
     * reach hasGameData. */
    set_reply(1, 1);
    psnr_put64(s_reply + 6 + 4 + 16, 0x07EA000B00080014ull);
    assert(get_range(trans, 1, 100) == 1);
    assert(vm_read32(RANKS + 104) == 0);
    assert(vm_read64(RANKS + 112) == 0x07EA000B00080014ull);

    /* A short last page is still a count. */
    set_reply(150, 50);
    assert(get_range(trans, 101, 100) == 50);

    /* Past the last rank: not found, so a paging title stops. */
    set_reply(150, 0);
    assert((u32)get_range(trans, 151, 100) == SCORE_ERROR_RANKING_NOT_FOUND);

    /* An empty board reads the same. */
    set_reply(0, 0);
    assert((u32)get_range(trans, 1, 100) == SCORE_ERROR_RANKING_NOT_FOUND);

    /* An error reply stays a failure; offline stays a failure. */
    s_reply_type = PSNR_ERROR;
    s_reply_len = 0;
    assert((u32)get_range(trans, 1, 100) == SCORE_ERROR_NO_SERVER);
    s_connected = 0;
    assert((u32)get_range(trans, 1, 100) == SCORE_ERROR_NO_SERVER);

    free(vm_base);
    puts("sceNpScore checks passed");
    return 0;
}
