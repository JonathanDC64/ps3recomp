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

/* Call a range form as a title does. The arguments past r10 live in the
 * caller's parameter save area. */
static s32 call_range(void (*form)(ppu_context*), s32 trans, u32 start, u32 num)
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
    form(ctx);
    s32 r = (s32)ctx->gpr[3];
    free(ctx);
    return r;
}

static s32 get_range(s32 trans, u32 start, u32 num) { return call_range(hle_ranking_by_range, trans, start, num); }

/* The asynchronous form: the request goes out and its reply stays with the test. */
static void start_range(s32 trans, u32 num) { assert(call_range(hle_ranking_by_range_async, trans, 1, num) == 0); }

/* The synchronous by-id form; rows are SceNpScorePlayerRankData. */
static s32 get_by_id(s32 trans, u32 num)
{
    enum { NPIDS = 0x48000 };
    ppu_context* ctx = (ppu_context*)calloc(1, sizeof(ppu_context));
    memset(vm_ptr8(NPIDS), 0, 36 * num);
    ctx->gpr[1] = STACK;
    ctx->gpr[3] = (u64)(s64)trans;
    ctx->gpr[4] = 7;
    ctx->gpr[5] = NPIDS;
    ctx->gpr[6] = 36 * num;
    ctx->gpr[7] = RANKS;
    ctx->gpr[8] = 136 * num;
    ctx->gpr[9] = COMMENTS;
    ctx->gpr[10] = 64 * num;
    vm_write64(STACK + 112 + 8 * (11 - 9), num);       /* arrayNum */
    vm_write64(STACK + 112 + 8 * (12 - 9), LAST_SORT);
    vm_write64(STACK + 112 + 8 * (13 - 9), TOTAL);
    hle_ranking_by_npid(ctx);
    s32 r = (s32)ctx->gpr[3];
    free(ctx);
    return r;
}

/* A by-id reply can be cut short anywhere: only whole rows count, the rest
 * read as no data, and nothing past the reply is read. */
static void truncated_row_checks(s32 trans)
{
    /* One row promised, none there. */
    s_reply_type = PSNR_RANKING2;
    psnr_put32(s_reply, 1);
    psnr_put16(s_reply + 4, 1);
    memset(s_reply + 6, 0x11, 64);          /* what a read past the end would see */
    s_reply_len = 6;
    vm_write32(RANKS, 0xAAAAAAAAu);
    assert(get_by_id(trans, 1) == 0);
    assert(vm_read32(RANKS) == 0);

    /* A row with a rank, cut off inside it. */
    set_reply2(100);
    s_reply_len = 6 + 12;
    vm_write32(RANKS, 0xAAAAAAAAu);
    assert(get_by_id(trans, 1) == 0);
    assert(vm_read32(RANKS) == 0);

    /* A whole row and then four bytes of another. */
    set_reply2(100);
    psnr_put16(s_reply + 4, 2);
    psnr_put32(s_reply + s_reply_len, 9);
    s_reply_len += 4;
    vm_write32(RANKS, 0xAAAAAAAAu);
    vm_write32(RANKS + 136, 0xAAAAAAAAu);
    assert(get_by_id(trans, 2) == 1);
    assert(vm_read32(RANKS) == 1);
    assert(vm_read32(RANKS + 8 + 96) == 1);
    assert(vm_read32(RANKS + 136) == 0);
}

/* A reply that outlives its request writes nothing, wherever it lands. */
static void late_reply_checks(s32 title)
{
    enum { RESULT = 0xC200 };
    s32 a = sceNpScoreCreateTransactionCtx(title);
    assert(a > 0);
    set_reply(5, 1);
    psnr_msg late = { PSNR_RANKING, 1, s_reply, s_reply_len };

    /* Onto the same slot's next transaction: the game's next row. */
    start_range(a, 1);
    np_psnr_reply_fn old_fn = s_fn;
    void* old_user = s_user;
    assert(sceNpScoreAbortTransaction(a) == 0);
    assert(sceNpScoreDestroyTransactionCtx(a) == 0);
    s32 b = sceNpScoreCreateTransactionCtx(title);
    assert(b == a);
    start_range(b, 1);
    memset(vm_ptr8(RANKS), 0x55, 128);
    vm_write32(TOTAL, 0x55555555u);
    old_fn(old_user, &late);
    assert(vm_read8(RANKS) == 0x55 && vm_read32(TOTAL) == 0x55555555u);
    assert(trans_state(&s_trans[b]) == T_PENDING);
    assert(sceNpScorePollAsync(b, RESULT) == 0);          /* its own reply still lands */
    assert(vm_read32(RESULT) == 1 && vm_read32(RANKS + 96) == 1 && vm_read32(TOTAL) == 5);

    /* Onto the same transaction's next request. */
    start_range(b, 1);
    old_fn = s_fn;
    old_user = s_user;
    assert(sceNpScoreAbortTransaction(b) == 0);
    start_range(b, 1);
    memset(vm_ptr8(RANKS), 0x55, 128);
    old_fn(old_user, &late);
    assert(vm_read8(RANKS) == 0x55);
    assert(trans_state(&s_trans[b]) == T_PENDING);
    assert(sceNpScorePollAsync(b, RESULT) == 0 && vm_read32(RESULT) == 1);

    /* After the abort, with nothing new: the abort stands. */
    start_range(b, 1);
    old_fn = s_fn;
    old_user = s_user;
    s_fn = NULL;
    assert(sceNpScoreAbortTransaction(b) == 0);
    memset(vm_ptr8(RANKS), 0x55, 128);
    old_fn(old_user, &late);
    assert(vm_read8(RANKS) == 0x55);
    assert(sceNpScorePollAsync(b, RESULT) == 0 && vm_read32(RESULT) == SCORE_ERROR_ABORTED);

    /* After the destroy: the slot is free and stays free. */
    assert(sceNpScoreDestroyTransactionCtx(b) == 0);
    old_fn(old_user, &late);
    assert(trans_state(&s_trans[b]) == T_FREE);
    assert(vm_read8(RANKS) == 0x55);
}

/* Two threads: one is inside a reply handler, one calls a thing that ends the
 * transaction. It must not return until the handler has finished. */
#ifdef _WIN32
typedef HANDLE test_thread;
#  define THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
static test_thread thread_go(LPTHREAD_START_ROUTINE fn, void* arg) { return CreateThread(NULL, 0, fn, arg, 0, NULL); }
static void thread_join(test_thread t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
#else
#  include <pthread.h>
typedef pthread_t test_thread;
#  define THREAD_FN(name) static void* name(void* arg)
static test_thread thread_go(void* (*fn)(void*), void* arg) { test_thread t; pthread_create(&t, NULL, fn, arg); return t; }
static void thread_join(test_thread t) { pthread_join(t, NULL); }
#endif

static int s_in_hook, s_let_go, s_ending, s_ended, s_end_by_destroy;
static np_psnr_reply_fn s_held_fn;
static void* s_held_user;

static int peek(int* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void poke(int* p) { __atomic_store_n(p, 1, __ATOMIC_RELEASE); }

static int held_hook(int upload, uint32_t board, uint8_t* data, uint32_t size)
{
    (void)upload; (void)board; (void)data; (void)size;
    poke(&s_in_hook);
    while (!peek(&s_let_go)) sleep_10ms();
    return 1;
}

THREAD_FN(deliver_reply)
{
    psnr_msg m = { PSNR_GAME_DATA, 1, s_reply, s_reply_len };
    (void)arg;
    s_held_fn(s_held_user, &m);
    return 0;
}

THREAD_FN(end_transaction)
{
    poke(&s_ending);
    s32 id = (s32)(intptr_t)arg;
    if (peek(&s_end_by_destroy)) sceNpScoreDestroyTransactionCtx(id);
    else sceNpScoreAbortTransaction(id);
    poke(&s_ended);
    return 0;
}

static void held_handler_check(s32 title, int by_destroy)
{
    enum { RECV = 0x30000, NPID = 0x40000, TOTAL_EA = 0x40100 };
    s32 t = sceNpScoreCreateTransactionCtx(title);
    assert(t > 0);
    __atomic_store_n(&s_in_hook, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_let_go, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_ended, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_end_by_destroy, by_destroy, __ATOMIC_RELEASE);
    np_score_set_game_data_hook(held_hook);
    memcpy(vm_ptr8(NPID), "host", 5);

    ppu_context* ctx = (ppu_context*)calloc(1, sizeof(ppu_context));
    ctx->gpr[3] = (u64)(s64)t;
    ctx->gpr[4] = 5;
    ctx->gpr[5] = NPID;
    ctx->gpr[6] = TOTAL_EA;
    ctx->gpr[7] = 16;
    ctx->gpr[8] = RECV;
    hle_get_game_data_async(ctx);
    assert(ctx->gpr[3] == 0);
    free(ctx);
    s_held_fn = s_fn;
    s_held_user = s_user;
    s_fn = NULL;

    u8* r = s_reply;
    s_reply_type = PSNR_GAME_DATA;
    r = psnr_put64(r, 100);
    r = psnr_put16(r, 16);
    memset(r, 0x42, 16);
    s_reply_len = (u32)(r + 16 - s_reply);
    memset(vm_ptr8(RECV), 0, 16);

    test_thread handler = thread_go(deliver_reply, NULL);
    while (!peek(&s_in_hook)) sleep_10ms();
    test_thread ender = thread_go(end_transaction, (void*)(intptr_t)t);
    while (!peek(&s_ending)) sleep_10ms();
    for (int i = 0; i < 5; i++) {           /* a wrong build returns well inside this */
        sleep_10ms();
        assert(!peek(&s_ended));
    }
    assert(trans_state(&s_trans[t]) == T_BUSY);
    poke(&s_let_go);
    thread_join(handler);
    thread_join(ender);
    assert(peek(&s_ended));
    assert(vm_read8(RECV) == 0x42 && vm_read8(RECV + 15) == 0x42);
    if (by_destroy) {
        assert(trans_state(&s_trans[t]) == T_FREE);
    } else {
        assert(s_trans[t].result == 16);    /* the handler got there first and stands */
        assert(sceNpScoreDestroyTransactionCtx(t) == 0);
    }
    np_score_set_game_data_hook(NULL);
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

    /* A server that turns the record down: the record was sent, and the title
     * hears CELL_OK, so it goes on to its next board (a failure makes Demon's
     * Souls drop every board after this one). The session keeps the newer
     * messages, and the next upload goes out as usual. */
    set_error(PSNR_ERR_BAD_REQUEST);
    before = s_sent;
    assert(record_chunk(trans, 1, 100, size, chunk, DATA) == 0);
    assert(record_chunk(trans, 1, 100, size, size - chunk, DATA + chunk) == 0);
    assert(s_sent == before + 1 && s_sent_type == PSNR_RECORD_GAME_DATA);
    assert(!s_old_server);
    set_error(6);                           /* any other error reads the same */
    assert(record_chunk(trans, 2, 100, size, size, DATA) == 0);
    assert(s_sent == before + 2 && psnr_get32(s_body) == 2);
    s_reply_type = PSNR_OK;
    s_reply_len = 0;
    assert(record_chunk(trans, 3, 100, size, size, DATA) == 0);
    assert(s_sent == before + 3 && psnr_get32(s_body) == 3);

    /* Offline is still a failure: nothing was sent. */
    {
        int keep = s_connected;
        s_connected = 0;
        assert(record_chunk(trans, 1, 100, size, size, DATA) < 0);
        s_connected = keep;
    }

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
    truncated_row_checks(trans);
    late_reply_checks(title);
    held_handler_check(title, 0);
    held_handler_check(title, 1);

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
