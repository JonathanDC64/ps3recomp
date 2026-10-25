/*
 * test_np_signin - what the NP manager reports, and the status event the
 * title's manager callback gets: OFFLINE unless online with psnr or with a
 * player name of its own ("local" sign-in, no matching server). The name
 * and the online switch may be set after main ran (a title's options hook),
 * and ps3_net_lock_offline overrides them all. No server is contacted.
 *
 *   clang-cl /I include /Fe:test_np_signin.exe libs/network/tests/test_np_signin.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_np_signin libs/network/tests/test_np_signin.c third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c
 */
#include "../psnr/psnr.c"
#include "../np_psnr.c"
#include "../ps3_net.c"
#include "../ps3_net_signin.c"
#include "../sceNp.c"

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

/* The manager callback, queued for cellSysutilCheckCallback or called at once. */
static int s_nqueued, s_nfired;
static s32 s_queued_status, s_fired_status;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    (void)opd;
    s_nqueued++;
    s_queued_status = (s32)args[0];
    return 0;
}
uint64_t ppu_guest_call(uint32_t opd, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    (void)opd; (void)b; (void)c; (void)d;
    s_nfired++;
    s_fired_status = (s32)a;
    return 0;
}

enum { STATUS = 0x100, NPID = 0x200, CB = 0x1000, CB_ARG = 0x2000 };

static void env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    if (*v) setenv(k, v, 1); else unsetenv(k);
#endif
}

/* A new process: main's np_psnr_setup, with no status event sent yet. */
static void boot(const char* username)
{
    np_psnr_setup(username, NULL);
    s_npmgr_cb_fired = 0;
    s_nqueued = s_nfired = 0;
}

static s32 status(void)
{
    assert(sceNpManagerGetStatus((s32*)(uintptr_t)STATUS) == CELL_OK);
    return (s32)vm_read32(STATUS);
}

/* The title registers its manager callback, then polls its NP ID. */
static const char* title_registers(void)
{
    assert(sceNpManagerRegisterCallback((SceNpManagerCallback)(uintptr_t)CB, (void*)(uintptr_t)CB_ARG) == CELL_OK);
    assert(sceNpManagerGetNpId((SceNpId*)(uintptr_t)NPID) == CELL_OK);
    assert(sceNpManagerGetNpId((SceNpId*)(uintptr_t)NPID) == CELL_OK);
    return (const char*)vm_base + NPID;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, 65536);
    env("PS3_NET_ONLINE", "");
    env("PSNR_SERVER", "");
    env("PS3_NP_ONLINE_ID", "");
    assert(sceNpInit(0, NULL) == CELL_OK);

    /* Offline: OFFLINE, and the callback gets OFFLINE once, from the getters. */
    boot(NULL);
    assert(status() == SCE_NP_MANAGER_STATUS_OFFLINE);
    assert(!strcmp(title_registers(), "PS3Player"));
    assert(s_nqueued == 0 && s_nfired == 1 && s_fired_status == SCE_NP_MANAGER_STATUS_OFFLINE);

    /* A name alone doesn't sign in. */
    env("PS3_NP_ONLINE_ID", "solaire");
    boot(NULL);
    assert(status() == SCE_NP_MANAGER_STATUS_OFFLINE);
    assert(!strcmp(title_registers(), "solaire"));
    assert(s_nfired == 1 && s_fired_status == SCE_NP_MANAGER_STATUS_OFFLINE);

    /* Online with no name and no psnr: still OFFLINE. */
    env("PS3_NP_ONLINE_ID", "");
    env("PS3_NET_ONLINE", "1");
    boot(NULL);
    assert(status() == SCE_NP_MANAGER_STATUS_OFFLINE);
    title_registers();
    assert(s_nqueued == 0 && s_fired_status == SCE_NP_MANAGER_STATUS_OFFLINE);

    /* Local sign-in, with the switch and the name set after main ran, as a
     * title's options hook applies its settings: ONLINE, the callback gets
     * ONLINE once (queued at registration), and the NP ID is the name.
     * psnr isn't set, so Matching2 finds no server. */
    env("PS3_NET_ONLINE", "");
    boot(NULL);
    env("PS3_NET_ONLINE", "1");
    env("PS3_NP_ONLINE_ID", "solaire");
    assert(status() == SCE_NP_MANAGER_STATUS_ONLINE);
    assert(!strcmp(title_registers(), "solaire"));
    assert(s_nqueued == 1 && s_queued_status == SCE_NP_MANAGER_STATUS_ONLINE && s_nfired == 0);
    assert(!np_psnr_enabled() && np_psnr_connect("NPWR00881_00") == -1);

    /* --username works the same way. */
    env("PS3_NP_ONLINE_ID", "");
    boot("knight");
    assert(status() == SCE_NP_MANAGER_STATUS_ONLINE);
    assert(!strcmp(title_registers(), "knight"));

    /* Locked offline: OFFLINE whatever is set, psnr included. */
    env("PSNR_SERVER", "127.0.0.1:1");
    ps3_net_lock_offline();
    boot("knight");
    assert(status() == SCE_NP_MANAGER_STATUS_OFFLINE);
    title_registers();
    assert(s_nqueued == 0 && s_fired_status == SCE_NP_MANAGER_STATUS_OFFLINE);
    assert(!np_psnr_enabled() && np_psnr_connect("NPWR00881_00") == -1);

    printf("test_np_signin: all passed\n");
    return 0;
}
