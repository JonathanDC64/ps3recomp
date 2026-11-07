/* clang-cl /nologo /W0 /D_CRT_SECURE_NO_WARNINGS /I include /Fe:test_savedata_held.exe
 *   libs/system/tests/test_savedata_held.c
 *
 * Windows only: another program's hold on a save file, as a virus scanner or
 * a folder's sync client keeps one on a file it reads the moment it is
 * written. A hold that lets go is waited out and the new file swapped in; one
 * that outlasts the waits gets the file written in place; one that refuses
 * that write too fails the save and leaves the old file as it was. */
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
/* (Windows' linker resolves what an unused function names too; Linux CI's --gc-sections drops it first) */
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { (void)nid; (void)name; (void)fn; }

static void put(const char* p, const char* s)
{
    FILE* f = fopen(p, "wb");
    assert(f);
    fputs(s, f);
    fclose(f);
}

/* 1 if `p` holds exactly `s`. */
static int holds(const char* p, const char* s)
{
    char buf[256] = "";
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    return n == strlen(s) && !memcmp(buf, s, n);
}

static int gone(const char* p) { return GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES; }

/* Another program's read handle on `p`, sharing what `share` lets others do. */
static HANDLE hold(const char* p, DWORD share)
{
    HANDLE h = CreateFileA(p, GENERIC_READ, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(h != INVALID_HANDLE_VALUE);
    return h;
}

static DWORD WINAPI let_go(LPVOID h)
{
    Sleep(150);
    CloseHandle((HANDLE)h);
    return 0;
}

int main(void)
{
    char base[MAX_PATH], dir[MAX_PATH], path[MAX_PATH], tmp[MAX_PATH], sfo[MAX_PATH];
    assert(GetTempPathA(sizeof base, base));
    snprintf(dir, sizeof dir, "%sps3recomp-savedata-held-%lu", base, (unsigned long)GetCurrentProcessId());
    assert(CreateDirectoryA(dir, NULL));
    snprintf(path, sizeof path, "%s\\ICON0.PNG", dir);
    snprintf(tmp, sizeof tmp, "%s\\.ICON0.PNG.tmp", dir);
    put(path, "old");

    /* No hold: the temp file is swapped in. */
    assert(save_replace_file("test", path, tmp, dir, "one", 3) == 0);
    assert(holds(path, "one") && gone(tmp));

    /* A scanner's hold: it shares reading and writing but not deleting, so
     * Windows refuses the swap onto the file as a hold. */
    HANDLE h = hold(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    put(tmp, "x");
    assert(!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) && save_file_held());
    assert(DeleteFileA(tmp));

    /* A hold that lets go after 150 ms is waited out: the swap goes through,
     * well before the waits run out (about 2 s) and the in-place write. */
    HANDLE t = CreateThread(NULL, 0, let_go, h, 0, NULL);
    assert(t);
    const DWORD t0 = GetTickCount();
    assert(save_replace_file("test", path, tmp, dir, "two", 3) == 0);
    const DWORD took = GetTickCount() - t0;
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    assert(took < 1500);
    assert(holds(path, "two") && gone(tmp));

    /* A hold that outlasts the waits: the file is written in place. */
    h = hold(path, FILE_SHARE_READ | FILE_SHARE_WRITE);
    assert(save_replace_file("test", path, tmp, dir, "three", 5) == 0);
    CloseHandle(h);
    assert(holds(path, "three") && gone(tmp));

    /* A hold that shares nothing refuses the in-place write too: the save
     * fails and the old file is as it was. */
    h = hold(path, 0);
    assert(save_replace_file("test", path, tmp, dir, "four", 4) != 0);
    CloseHandle(h);
    assert(holds(path, "three") && gone(tmp));

    /* A read-only file is refused the same way (access denied) and stays. */
    assert(SetFileAttributesA(path, FILE_ATTRIBUTE_READONLY));
    assert(save_replace_file("test", path, tmp, dir, "five", 4) != 0);
    assert(SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL));
    assert(holds(path, "three") && gone(tmp));

    /* PARAM.SFO goes the same way: held past the waits, written in place. */
    snprintf(sfo, sizeof sfo, "%s\\PARAM.SFO", dir);
    put(sfo, "TITLE=Old\n");
    CellSaveDataSystemFileParam param;
    memset(&param, 0, sizeof param);
    strcpy(param.title, "Held test");
    h = hold(sfo, FILE_SHARE_READ | FILE_SHARE_WRITE);
    assert(write_param_sfo(dir, &param) == 0);
    CloseHandle(h);
    assert(holds(sfo, "TITLE=Held test\nSUB_TITLE=\nDETAIL=\nATTRIBUTE=0\nLIST_PARAM=\n"));

    /* The title's file ops: an ICON0.PNG refused outright is left out and the
     * save goes on (Demon's Souls loads a save without it); the title's own
     * file refused the same way fails the save. */
    static char icon[] = "new icon", data[] = "new data";
    char dpath[MAX_PATH];
    snprintf(dpath, sizeof dpath, "%s\\DATA.BIN", dir);
    put(dpath, "old data");
    CellSaveDataFileSet op;
    memset(&op, 0, sizeof op);
    op.fileOperation = CELL_SAVEDATA_FILEOP_WRITE;
    op.fileType = CELL_SAVEDATA_FILETYPE_CONTENT_ICON0;
    op.fileBuf = icon;
    op.fileSize = op.fileBufSize = (u32)strlen(icon);
    h = hold(path, 0);
    assert(process_file_op(dir, &op) == (s32)strlen(icon));
    CloseHandle(h);
    assert(holds(path, "three"));
    op.fileType = CELL_SAVEDATA_FILETYPE_NORMALFILE;
    op.fileName = "DATA.BIN";
    op.fileBuf = data;
    op.fileSize = op.fileBufSize = (u32)strlen(data);
    h = hold(dpath, 0);
    assert(process_file_op(dir, &op) == CELL_SAVEDATA_ERROR_ACCESS_ERROR);
    CloseHandle(h);
    assert(holds(dpath, "old data"));
    assert(process_file_op(dir, &op) == (s32)strlen(data) && holds(dpath, "new data"));

    assert(DeleteFileA(path) && DeleteFileA(sfo) && DeleteFileA(dpath) && RemoveDirectoryA(dir));
    puts("Held save files: waited out, written in place, or refused with the old file kept");
    return 0;
}
