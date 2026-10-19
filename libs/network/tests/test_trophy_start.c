/* clang -std=gnu17 -I include libs/network/tests/test_trophy_start.c
 *   -Wl,-dead_strip -o /tmp/test_trophy_start && /tmp/test_trophy_start
 */
#include "../sceNpTrophy.c"
#include "../np_trophy_set.c"
#include <assert.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_lockline_lock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
void spu_lockline_unlock_ea(uint32_t ea, uint32_t len) { (void)ea; (void)len; }
ps3_guest_caller_fn g_ps3_guest_caller;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8]) { (void)opd; (void)args; return CELL_OK; }
void sys_fs_translate_path(const char* p, char* host, int n) { snprintf(host, (size_t)n, "missing%s", p); }
const char* cellGame_get_title_id(void) { return ""; }
int main(void)
{
    vm_base = calloc(1, 65536);
    assert(vm_base);
    memcpy(vm_base + 0x100, "NPWR00000", 9);
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) ==
           SCE_NP_TROPHY_ERROR_NOT_INITIALIZED);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    assert(sceNpTrophyCreateContext(NULL, (void*)0x100, NULL, 0) ==
           SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    for (int i = 0; i < SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == CELL_OK);
        assert(vm_read32(0x200) == (u32)(i + 1));
        assert(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
        assert(vm_read32(0x204) == (u32)(i + 1));
    }
    assert(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == SCE_NP_TROPHY_ERROR_EXCEEDS_MAX);
    assert(sceNpTrophyCreateHandle((void*)0x204) == SCE_NP_TROPHY_ERROR_EXCEEDS_MAX);
    assert(sceNpTrophyDestroyContext(0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyDestroyHandle(0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    assert(sceNpTrophyInit(NULL, 0, 0, 0) == SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED);
    assert(sceNpTrophyGetRequiredDiskSpace(1, 1, (void*)0x208, 0) == CELL_OK);
    assert(vm_read64(0x208) == 1024 * 1024);
    assert(sceNpTrophyDestroyContext(SCE_NP_TROPHY_MAX_CONTEXTS) == CELL_OK);
    assert(sceNpTrophyDestroyHandle(SCE_NP_TROPHY_MAX_HANDLES) == CELL_OK);
    assert(s_contexts[1].in_use && s_handles[1].in_use);
    assert(sceNpTrophyTerm() == CELL_OK);
    free(vm_base);
    puts("Trophy initialization and guest-endian checks passed");
}
