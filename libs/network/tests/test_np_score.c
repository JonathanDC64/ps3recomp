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
static u8 s_body[256];
static uint8_t s_reply_type;
static u8 s_reply[8192];
static u32 s_reply_len;
static int s_connected = 1;

int np_psnr_connect(const char* comm_id) { (void)comm_id; return 0; }
int np_psnr_connected(void) { return s_connected; }
uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len, np_psnr_reply_fn fn, void* user)
{
    (void)type;
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

int main(void)
{
    vm_base = (uint8_t*)calloc(1, 1u << 20);

    s32 title = sceNpScoreCreateTitleCtx(0, 0, 0);
    assert(title > 0);
    s32 trans = sceNpScoreCreateTransactionCtx(title);
    assert(trans > 0);

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
    puts("sceNpScore range checks passed");
    return 0;
}
