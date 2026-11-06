/* clang -std=gnu17 -I include libs/system/tests/test_savedata_io.c
 * -Wl,-dead_strip -o /tmp/test_savedata_io && /tmp/test_savedata_io */
#include "../cellSaveData.c"
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


static unsigned step;
static int loading;
static const char payload[] = "save round trip";
static void callback(u32 opd, u64 cb, u64 get, u64 set,
                     u64 a, u64 b, u64 c, u64 d, u64 e)
{
    assert(!a && !b && !c && !d && !e);
    if (opd == 0x800) {
        assert(vm_read32(cb + 16) == 7);
        vm_write32(cb + 16, 0xBEEF);
        vm_write32(set, 0x300); vm_write32(set + 8, 1);
    } else if (opd == 0x900) {
        assert(vm_read32(cb + 16) == 0xBEEF);
        assert(vm_read32(get + 4) == (loading ? 0 : 1));
        if (!loading) {
            strcpy((char*)vm_base + get + 64, "Save test");
            vm_write32(get + 64 + 1280, 0x1234);
            vm_write32(set, get + 64);
        } else {
            assert(strcmp((char*)vm_base + get + 64, "Save test") == 0);
            assert(vm_read32(get + 64 + 1280) == 0x1234);
            u32 n = vm_read32(get + 1632), list = vm_read32(get + 1636);
            assert(n == 2 && vm_read32(get + 1628) == 2);
            int found = 0, icon = 0;
            for (u32 i = 0; i < n; i++) {
                if (!strcmp((char*)vm_base + list + i * 56 + 40, "ICON0.PNG")) {
                    assert(vm_read32(list + i * 56) == CELL_SAVEDATA_FILETYPE_CONTENT_ICON0); icon = 1;
                }
                if (!strcmp((char*)vm_base + list + i * 56 + 40, "DATA.BIN")) {
                    assert(vm_read64(list + i * 56 + 8) == sizeof(payload)); found = 1;
                }
            }
            assert(found && icon);
        }
        vm_write32(cb + 16, 0xCAFE);
    } else {
        assert(opd == 0xA00);
        assert(vm_read32(cb + 16) == (step ? 0xCAFE + step : 0xCAFE));
        assert(vm_read32(get) == (step ? sizeof(payload) : 0));
        if (step == 2) { vm_write32(cb, 1); step++; return; }
        vm_write32(set, loading ? 0 : 1);
        vm_write32(set + 8, step ? 2 : 1); /* ICON0 has no explicit filename. */
        if (!step) vm_write32(set + 28, 0x340);
        vm_write32(set + 36, sizeof(payload));
        vm_write32(set + 40, sizeof(payload));
        vm_write32(set + 44, loading ? 0x500 + step * 0x100 : 0x400);
        step++; vm_write32(cb + 16, 0xCAFE + step);
    }
}
/* The saves that cannot be written: a stat callback that always gives its
 * PARAM.SFO, and a file callback that writes DATA.BIN once, then ends. */
static int stat_calls, file_calls;
static void write_callback(u32 opd, u64 cb, u64 get, u64 set,
                           u64 a, u64 b, u64 c, u64 d, u64 e)
{
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (opd == 0x910) {
        stat_calls++;
        strcpy((char*)vm_base + get + 64, "Write test");
        vm_write32(set, get + 64);
        return;
    }
    assert(opd == 0xA10);
    if (file_calls++) { vm_write32(cb, CELL_SAVEDATA_CBRESULT_OK_LAST); return; }
    vm_write32(set, CELL_SAVEDATA_FILEOP_WRITE);
    vm_write32(set + 8, CELL_SAVEDATA_FILETYPE_NORMALFILE);
    vm_write32(set + 28, 0x340);
    vm_write32(set + 36, sizeof(payload));
    vm_write32(set + 40, sizeof(payload));
    vm_write32(set + 44, 0x400);
}
static s32 auto_save(const char* dir)
{
    strcpy((char*)vm_base + 0xB00, dir);
    stat_calls = file_calls = 0;
    return cellSaveDataAutoSave2(0, (void*)0xB00, 0, (void*)0x200, (void*)0x910,
                                 (void*)0xA10, 0, (void*)7);
}
int main(void)
{
    char root[] = "/tmp/ps3recomp-savedata-io-XXXXXX";
    assert(mkdtemp(root)); strcpy(s_save_root, root);
    vm_base = calloc(1, 0x200000); assert(vm_base);
    assert(cellSaveData_set_scratch_region(0x100000, 0x20000) == CELL_OK);
    strcpy((char*)vm_base + 0x300, "TEST00000");
    strcpy((char*)vm_base + 0x340, "DATA.BIN");
    memcpy(vm_base + 0x400, payload, sizeof(payload));
    vm_write32(0x108, 0x300); vm_write32(0x200, 4); vm_write32(0x204, 2);
    g_ps3_guest_caller = callback;
    assert(cellSaveDataFixedSave2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, (void*)0xA00, 0, (void*)7) == CELL_OK);
    assert(step == 3);
    char path[256], bytes[sizeof(payload)];
    snprintf(path, sizeof(path), "%s/TEST00000/DATA.BIN", root);
    FILE* f = fopen(path, "rb"); assert(f);
    assert(fread(bytes, 1, sizeof(bytes), f) == sizeof(bytes)); fclose(f);
    assert(memcmp(bytes, payload, sizeof(payload)) == 0);
    loading = 1; step = 0;
    assert(cellSaveDataFixedLoad2(0, (void*)0x100, (void*)0x200, (void*)0x800,
                                (void*)0x900, (void*)0xA00, 0, (void*)7) == CELL_OK);
    assert(step == 3);
    assert(memcmp(vm_base + 0x500, payload, sizeof(payload)) == 0);
    assert(memcmp(vm_base + 0x600, payload, sizeof(payload)) == 0);
    /* A name that is not one plain directory is refused before any callback
     * runs, and nothing is made (the rmdir of root below needs it empty). */
    static const char* const bad[] = { "..", "../TEST00000", "A/B", "A\\B", "NUL", "X:Y", "", "." };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        strcpy((char*)vm_base + 0xB00, bad[i]);
        step = 99;
        assert(cellSaveDataAutoSave2(0, (void*)0xB00, 0, (void*)0x200, (void*)0x900,
                                     (void*)0xA00, 0, (void*)7) == CELL_SAVEDATA_ERROR_PARAM);
        assert(cellSaveDataAutoLoad2(0, (void*)0xB00, 0, (void*)0x200, (void*)0x900,
                                     (void*)0xA00, 0, (void*)7) == CELL_SAVEDATA_ERROR_PARAM);
        assert(step == 99);
    }
    /* Saves that cannot be written fail, and say so, instead of passing as
     * saves that read back as none (Demon's Souls then makes its save again
     * and again behind "Creating SAVE DATA..."). */
    g_ps3_guest_caller = write_callback;
    char root2[sizeof s_save_root], p2[300];
    snprintf(root2, sizeof(root2), "%s", s_save_root);
    /* A save folder that cannot be made: the save root is under a file. */
    snprintf(p2, sizeof(p2), "%s/blocker", root);
    f = fopen(p2, "wb"); assert(f); fclose(f);
    snprintf(s_save_root, sizeof(s_save_root), "%s/blocker/saves", root);
    assert(auto_save("TEST00001") == CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    assert(stat_calls == 1 && file_calls == 0);
    snprintf(s_save_root, sizeof(s_save_root), "%s", root2);
    assert(unlink(p2) == 0);
    /* A new save whose PARAM.SFO cannot be written (a folder where its temp
     * file goes): no save is left looking made. */
    snprintf(p2, sizeof(p2), "%s/TEST00002", root); assert(mkdir(p2, 0755) == 0);
    snprintf(p2, sizeof(p2), "%s/TEST00002/.PARAM.SFO.tmp", root); assert(mkdir(p2, 0755) == 0);
    assert(auto_save("TEST00002") == CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    assert(stat_calls == 1 && file_calls == 0);
    snprintf(p2, sizeof(p2), "%s/TEST00002/PARAM.SFO", root); assert(access(p2, F_OK) != 0);
    snprintf(p2, sizeof(p2), "%s/TEST00002/.PARAM.SFO.tmp", root); assert(rmdir(p2) == 0);
    snprintf(p2, sizeof(p2), "%s/TEST00002", root); assert(rmdir(p2) == 0);
    /* An existing save whose PARAM.SFO cannot be replaced keeps the old one
     * and goes on with its files. */
    snprintf(p2, sizeof(p2), "%s/TEST00000/.PARAM.SFO.tmp", root); assert(mkdir(p2, 0755) == 0);
    assert(auto_save("TEST00000") == CELL_OK);
    assert(stat_calls == 1 && file_calls == 2);
    assert(rmdir(p2) == 0);
    snprintf(p2, sizeof(p2), "%s/TEST00000/PARAM.SFO", root);
    {
        char line[64] = "";
        FILE* sfo = fopen(p2, "rb"); assert(sfo);
        assert(fgets(line, sizeof(line), sfo)); fclose(sfo);
        assert(strcmp(line, "TITLE=Save test\n") == 0);
    }
    /* A file that cannot be written fails the save. */
    snprintf(p2, sizeof(p2), "%s/TEST00000/.DATA.BIN.tmp", root); assert(mkdir(p2, 0755) == 0);
    assert(auto_save("TEST00000") == CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    assert(stat_calls == 1 && file_calls == 1);
    assert(rmdir(p2) == 0);
    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00000/ICON0.PNG", root); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00000/PARAM.SFO", root); assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/TEST00000", root); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0); free(vm_base);
    puts("Save/load round trip, callback userdata, metadata, byte-count and write-failure checks passed");
}
