/* Trophy set reader, unlock rules, unlock file and host API, on a made-up
 * TROPHY.TRP (no game data).
 *
 *   clang -std=gnu17 -I include libs/network/tests/test_trophy_set.c -o /tmp/test_trophy_set && /tmp/test_trophy_set
 *   clang-cl /nologo /W0 /D_CRT_SECURE_NO_WARNINGS /I include /Fe:test_trophy_set.exe libs/network/tests/test_trophy_set.c
 */
#include "../sceNpTrophy.c"
#include "../np_trophy_set.c"

#ifdef _WIN32
#define test_rmdir(p) _rmdir(p)
#else
#define test_rmdir(p) rmdir(p)
#endif

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

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* --- stubs for what the module calls outside itself ---------------------- */

static u64 s_queued[8];
static int s_queued_n;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    (void)opd;
    memcpy(s_queued, args, sizeof s_queued);
    s_queued_n++;
    return CELL_OK;
}

static char s_root[512];
void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size)
{
    const char* rel = ps3_path;
    if (strncmp(rel, "/dev_bdvd/", 10) == 0 || strncmp(rel, "/dev_hdd0/", 10) == 0)
        rel += 10;
    snprintf(host_path, (size_t)host_path_size, "%s/%s", s_root, rel);
}

const char* cellGame_get_title_id(void) { return "TEST00000"; }

static int s_cb_calls;
static u64 s_cb_status[8];
static void guest_caller(u32 opd, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7)
{
    (void)opd; (void)a0; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7;
    s_cb_status[s_cb_calls++ & 7] = a1;
}

static int s_hook_n;
static sceNpTrophyUnlockEvent s_hook_ev[8];
static void* s_hook_user_seen;
static void hook(const sceNpTrophyUnlockEvent* ev, void* user)
{
    if (s_hook_n < 8)
        s_hook_ev[s_hook_n] = *ev;
    s_hook_n++;
    s_hook_user_seen = user;
}

/* --- a made-up TRP --------------------------------------------------------- */

typedef struct { const char* name; const char* data; size_t size; } trp_file;

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

static void write_trp(const char* path, const trp_file* files, int n)
{
    uint8_t hdr[0x40] = {0};
    uint64_t off = 0x40 + (uint64_t)n * 0x40, total = off;
    FILE* f = fopen(path, "wb");
    CHECK(f);
    for (int i = 0; i < n; i++)
        total += files[i].size;
    put32(hdr, 0xDCA24D00); put32(hdr + 4, 1); put64(hdr + 8, total);
    put32(hdr + 16, (uint32_t)n); put32(hdr + 20, 0x40);
    CHECK(fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr);
    for (int i = 0; i < n; i++) {
        uint8_t e[0x40] = {0};
        memcpy(e, files[i].name, strlen(files[i].name));
        put64(e + 32, off);
        put64(e + 40, files[i].size);
        CHECK(fwrite(e, 1, sizeof e, f) == sizeof e);
        off += files[i].size;
    }
    for (int i = 0; i < n; i++)
        CHECK(fwrite(files[i].data, 1, files[i].size, f) == files[i].size);
    fclose(f);
}

#define LONG_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"   /* 63 */

static const char k_trop[] =
    "<!--Sce-Np-Trophy-Signature: made-up-->"
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<trophyconf version=\"1.0\" policy=\"large\"><npcommid>TEST00001_00</npcommid>"
    "<trophyset-version>01.00</trophyset-version>"
    "<parental-level license-area=\"default\">0</parental-level>"
    "<title-name>Test &amp; Title</title-name><title-detail>A made-up set</title-detail>"
    "<trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"><name>All of them</name>"
    "<detail>Every trophy</detail></trophy>"
    "<trophy id=\"001\" hidden=\"no\" ttype=\"G\" pid=\"000\"><name>Gold &lt;one&gt;</name>"
    "<detail>Smile &#x263A; caf&#233;</detail></trophy>\n"
    "<trophy id='002' hidden='yes' ttype='S' pid='000'>\n  <name>Secret</name>\n"
    "  <detail><![CDATA[Raw <text> & more]]></detail>\n</trophy>\n"
    "<trophy id=\"003\" hidden=\"no\" ttype=\"B\" pid=\"000\"><name>" LONG_A LONG_A
    "\xC3\xA9tail</name><detail>Bronze</detail></trophy>"
    "<trophy id=\"004\" hidden=\"no\" ttype=\"B\" pid=\"-1\"><name>Extra</name>"
    "<detail>Not linked</detail></trophy></trophyconf>";

static const char k_conf[] =
    "<!--Sce-Np-Trophy-Signature: made-up--><trophyconf version=\"1.0\">"
    "<npcommid>TEST00001_00</npcommid>"
    "<trophy id=\"000\" hidden=\"no\" ttype=\"P\" pid=\"-1\"/>"
    "<trophy id=\"001\" hidden=\"no\" ttype=\"G\" pid=\"000\"/></trophyconf>";

static const char k_lang01[] =
    "<trophyconf><title-name>Lang 01</title-name>"
    "<trophy id=\"000\" ttype=\"B\" pid=\"-1\"><name>x</name></trophy></trophyconf>";
static const char k_default[] =
    "<trophyconf><title-name>Default</title-name>"
    "<trophy id=\"000\" ttype=\"B\" pid=\"-1\"><name>y</name></trophy></trophyconf>";

#define FILE_OF(n, s) { n, s, sizeof(s) - 1 }

static void set_env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static void make_root(void)
{
#ifdef _WIN32
    char tmp[MAX_PATH];
    CHECK(GetTempPathA(sizeof tmp, tmp) > 0);
    snprintf(s_root, sizeof s_root, "%sps3recomp-trophy-%lu", tmp, (unsigned long)GetCurrentProcessId());
    mkdir_p(s_root);
#else
    snprintf(s_root, sizeof s_root, "/tmp/ps3recomp-trophy-XXXXXX");
    CHECK(mkdtemp(s_root));
#endif
}

static const char* G(u32 ea) { return (const char*)vm_base + ea; }

static int all_zero(u32 ea, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (vm_base[ea + i])
            return 0;
    return 1;
}

int main(void)
{
    char dir[600], trp[700], path[700], save[600], json[700];
    np_trophy_set* set = (np_trophy_set*)calloc(1, sizeof *set);
    sceNpTrophySnapshot* snap = (sceNpTrophySnapshot*)calloc(1, sizeof *snap);
    TrophyProgress prog;
    void* png;
    u32 png_size;
    char folder[16];
    u64 t0, t1;

    CHECK(set && snap);
    vm_base = (uint8_t*)calloc(1, 1 << 20);
    CHECK(vm_base);
    make_root();

    snprintf(dir, sizeof dir, "%s/PS3_GAME", s_root);                 mkdir_p(dir);
    snprintf(dir, sizeof dir, "%s/PS3_GAME/TROPDIR", s_root);         mkdir_p(dir);
    snprintf(path, sizeof path, "%s/TEST00000_00", dir);              mkdir_p(path);   /* no TRP in it */
    snprintf(path, sizeof path, "%s/not-a-set", dir);                 mkdir_p(path);
    snprintf(path, sizeof path, "%s/TEST00001_00", dir);              mkdir_p(path);
    snprintf(trp, sizeof trp, "%s/TEST00001_00/TROPHY.TRP", dir);
    {
        const trp_file files[] = {
            FILE_OF("TROPCONF.SFM", k_conf), FILE_OF("TROP.SFM", k_trop),
            FILE_OF("ICON0.PNG", "ICON0-BYTES"), FILE_OF("TROP001.PNG", "TROP001-BYTES"),
        };
        write_trp(trp, files, 4);
    }
    snprintf(save, sizeof save, "%s/save", s_root);
    set_env("PS3_TROPHY_DIR", save);
    snprintf(json, sizeof json, "%s/TEST00001.json", save);

    /* --- the reader on its own ------------------------------------------ */
    CHECK(np_trophy_set_load(set, trp, 1) == 0);
    CHECK(set->count == 5 && strcmp(set->sfm, "TROP.SFM") == 0);
    CHECK(strcmp(set->title, "Test & Title") == 0 && strcmp(set->title_detail, "A made-up set") == 0);
    CHECK(set->trophy[0].present && set->trophy[0].grade == 1 && set->trophy[0].pid == -1);
    CHECK(strcmp(set->trophy[1].name, "Gold <one>") == 0);
    CHECK(strcmp(set->trophy[1].detail, "Smile \xE2\x98\xBA caf\xC3\xA9") == 0);
    CHECK(set->trophy[2].hidden == 1 && set->trophy[2].grade == 3 && set->trophy[2].pid == 0);
    CHECK(strcmp(set->trophy[2].name, "Secret") == 0);
    CHECK(strcmp(set->trophy[2].detail, "Raw <text> & more") == 0);
    CHECK(strlen(set->trophy[3].name) == 126);            /* the 2-byte character would not fit */
    CHECK(set->trophy[4].pid == -1 && !set->trophy[5].present);
    CHECK(set->per_grade[1] == 1 && set->per_grade[2] == 1 && set->per_grade[3] == 1 &&
          set->per_grade[4] == 2);
    CHECK(set->icon0_size == 11 && set->trophy[1].icon_size == 13 && set->trophy[2].icon_size == 0);

    snprintf(path, sizeof path, "%s/lang.trp", s_root);
    {
        const trp_file files[] = { FILE_OF("TROP.SFM", k_default), FILE_OF("TROP_01.SFM", k_lang01) };
        write_trp(path, files, 2);
    }
    CHECK(np_trophy_set_load(set, path, 1) == 0 && strcmp(set->title, "Lang 01") == 0 &&
          strcmp(set->sfm, "TROP_01.SFM") == 0);
    CHECK(np_trophy_set_load(set, path, 5) == 0 && strcmp(set->title, "Default") == 0);
    CHECK(np_trophy_set_load(set, path, -1) == 0 && strcmp(set->title, "Default") == 0);
    remove(path);

    snprintf(path, sizeof path, "%s/conf.trp", s_root);
    {
        const trp_file files[] = { FILE_OF("TROPCONF.SFM", k_conf) };
        write_trp(path, files, 1);
    }
    CHECK(np_trophy_set_load(set, path, 1) == 0 && set->count == 2 && set->trophy[1].name[0] == 0 &&
          strcmp(set->sfm, "TROPCONF.SFM") == 0);
    {
        const trp_file files[] = { FILE_OF("ICON0.PNG", "x") };
        write_trp(path, files, 1);
    }
    CHECK(np_trophy_set_load(set, path, 1) == NP_TROPHY_SET_ERR_NO_SFM);
    {
        FILE* f = fopen(path, "wb");
        CHECK(f);
        fputs("this is not a trophy container, just some text that is long enough", f);
        fclose(f);
    }
    CHECK(np_trophy_set_load(set, path, 1) == NP_TROPHY_SET_ERR_FORMAT);
    remove(path);
    CHECK(np_trophy_set_load(set, path, 1) == NP_TROPHY_SET_ERR_OPEN);

    CHECK(np_trophy_set_find_folder(dir, folder, sizeof folder) == 0);
    CHECK(strcmp(folder, "TEST00001_00") == 0);

    /* --- the HLE with the set ------------------------------------------- */
    g_ps3_guest_caller = guest_caller;
    sceNpTrophy_set_unlock_hook(hook, (void*)0x1234);
    memcpy(vm_base + 0x100, "TEST00001", 9);
    memcpy(vm_base + 0x120, "TEST00002", 9);
    memcpy(vm_base + 0x140, "TEST00001", 9);
    vm_base[0x140 + 10] = 100;                              /* num > 99 */

    /* the test knob's entry point waits quietly for a registered context */
    CHECK(sceNpTrophy_debug_unlock(1) == SCE_NP_TROPHY_ERROR_NOT_INITIALIZED && s_hook_n == 0);
    CHECK(sceNpTrophyInit(NULL, 0, 0, 1) == SCE_NP_TROPHY_ERROR_NOT_SUPPORTED);
    CHECK(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x140, NULL, 0) ==
          SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID);
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 2) ==
          SCE_NP_TROPHY_ERROR_NOT_SUPPORTED);
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == CELL_OK);
    CHECK(vm_read32(0x200) == 1);
    CHECK(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK && vm_read32(0x204) == 1);

    CHECK(sceNpTrophyGetGameInfo(1, 1, (void*)0x1000, NULL) == SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED);
    CHECK(sceNpTrophyGetGameInfo(2, 1, (void*)0x1000, NULL) == SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT);
    CHECK(sceNpTrophyGetGameInfo(0, 1, (void*)0x1000, NULL) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    CHECK(sceNpTrophyGetGameInfo(1, 3, (void*)0x1000, NULL) == SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE);
    CHECK(sceNpTrophyGetGameInfo(1, 5, (void*)0x1000, NULL) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x208) == SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED);
    CHECK(sceNpTrophy_debug_unlock(1) == SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED && s_hook_n == 0);

    CHECK(sceNpTrophyRegisterContext(1, 1, 0x300, 0xBEEF, 2) == SCE_NP_TROPHY_ERROR_NOT_SUPPORTED);
    CHECK(sceNpTrophyRegisterContext(1, 1, 0, 0xBEEF, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    CHECK(sceNpTrophyRegisterContext(1, 1, 0x300, 0xBEEF, 0) == CELL_OK);
    CHECK(s_cb_calls == 1 && s_cb_status[0] == SCE_NP_TROPHY_STATUS_INSTALLED);
    CHECK(s_queued_n == 1 && s_queued[0] == 1 && s_queued[1] == SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE &&
          s_queued[4] == 0xBEEF);
    CHECK(s_contexts[1].registered && s_contexts[1].set);

    CHECK(sceNpTrophyGetGameInfo(1, 1, NULL, NULL) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    memset(vm_base + 0x1000, 0xAA, sizeof(SceNpTrophyGameDetails));
    memset(vm_base + 0x2000, 0xAA, sizeof(SceNpTrophyGameData));
    CHECK(sceNpTrophyGetGameInfo(1, 1, (void*)0x1000, (void*)0x2000) == CELL_OK);
    CHECK(vm_read32(0x1000) == 5 && vm_read32(0x1004) == 1 && vm_read32(0x1008) == 1 &&
          vm_read32(0x100C) == 1 && vm_read32(0x1010) == 2);
    CHECK(strcmp(G(0x1000 + 20), "Test & Title") == 0 && all_zero(0x1000 + 20 + 12, 128 - 12));
    CHECK(strcmp(G(0x1000 + 148), "A made-up set") == 0 && all_zero(0x1000 + 1172, 4));
    CHECK(all_zero(0x2000, 20));

    CHECK(sceNpTrophyGetTrophyInfo(1, 1, -1, (void*)0x3000, NULL) == SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 128, (void*)0x3000, NULL) == SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 5, (void*)0x3000, NULL) == SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 1, NULL, NULL) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 2, (void*)0x3000, (void*)0x4000) == SCE_NP_TROPHY_ERROR_HIDDEN);
    memset(vm_base + 0x3000, 0xAA, sizeof(SceNpTrophyDetails));
    memset(vm_base + 0x4000, 0xAA, sizeof(SceNpTrophyData));
    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 1, (void*)0x3000, (void*)0x4000) == CELL_OK);
    CHECK(vm_read32(0x3000) == 1 && vm_read32(0x3004) == SCE_NP_TROPHY_GRADE_GOLD);
    CHECK(strcmp(G(0x3008), "Gold <one>") == 0 && all_zero(0x3008 + 10, 118));
    CHECK(strcmp(G(0x3000 + 136), "Smile \xE2\x98\xBA caf\xC3\xA9") == 0);
    CHECK(all_zero(0x3000 + 1160, 4));                      /* hidden = 0, reserved */
    CHECK(vm_read64(0x4000) == 0 && vm_read32(0x4008) == 1 && all_zero(0x400C, 4));

    /* --- unlock rules ----------------------------------------------------- */
    vm_write32(0x208, 0x12345678);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 0, (void*)0x208) == SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 5, (void*)0x208) == SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, -3, (void*)0x208) == SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID);
    CHECK(vm_read32(0x208) == 0x12345678);                  /* only written on success */
    t0 = (u64)time(NULL);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x208) == CELL_OK);
    CHECK(vm_read32(0x208) == 0xFFFFFFFFu);
    CHECK(s_hook_n == 1 && s_hook_ev[0].trophyId == 1 && s_hook_ev[0].grade == 2 &&
          !s_hook_ev[0].platinum && s_hook_ev[0].from_set &&
          strcmp(s_hook_ev[0].name, "Gold <one>") == 0 && strcmp(s_hook_ev[0].commId, "TEST00001_00") == 0 &&
          s_hook_user_seen == (void*)0x1234);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 1, (void*)0x208) == SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);
    CHECK(s_hook_n == 1);

    /* the test knob's path: same rules, same file, same hook */
    CHECK(sceNpTrophy_debug_unlock(4) == CELL_OK);
    CHECK(s_hook_n == 2 && s_hook_ev[1].trophyId == 4 && !s_hook_ev[1].platinum);
    CHECK(sceNpTrophy_debug_unlock(4) == SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);
    CHECK(sceNpTrophy_debug_unlock(0) == SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM);

    CHECK(sceNpTrophyUnlockTrophy(1, 1, 2, (void*)0x208) == CELL_OK && vm_read32(0x208) == 0xFFFFFFFFu);
    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 2, (void*)0x3000, NULL) == CELL_OK);
    CHECK(strcmp(G(0x3008), "Secret") == 0 && vm_base[0x3000 + 1160] == 1);

    s_hook_n = 0;
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 3, (void*)0x208) == CELL_OK);
    CHECK(vm_read32(0x208) == 0);                           /* the platinum fell with the last link */
    CHECK(s_hook_n == 2 && s_hook_ev[0].trophyId == 3 && s_hook_ev[0].grade == 4 &&
          s_hook_ev[1].trophyId == 0 && s_hook_ev[1].grade == 1 && s_hook_ev[1].platinum);
    t1 = (u64)time(NULL);

    CHECK(sceNpTrophyGetTrophyInfo(1, 1, 0, (void*)0x3000, (void*)0x4000) == CELL_OK);
    CHECK(vm_base[0x400C] == 1 && vm_read32(0x4008) == 0);
    CHECK(vm_read64(0x4000) >= t0 * 1000000ULL + 62135596800000000ULL &&
          vm_read64(0x4000) <= (t1 + 1) * 1000000ULL + 62135596800000000ULL);
    CHECK(sceNpTrophyGetGameInfo(1, 1, NULL, (void*)0x2000) == CELL_OK);
    CHECK(vm_read32(0x2000) == 5 && vm_read32(0x2004) == 1 && vm_read32(0x2008) == 1 &&
          vm_read32(0x200C) == 1 && vm_read32(0x2010) == 2);
    memset(vm_base + 0x5000, 0xAA, 16);
    CHECK(sceNpTrophyGetTrophyUnlockState(1, 1, (void*)0x5000, (void*)0x5010) == CELL_OK);
    CHECK(vm_read32(0x5000) == 0x1F && all_zero(0x5004, 12) && vm_read32(0x5010) == 5);
    CHECK(sceNpTrophyGetTrophyUnlockState(1, 1, NULL, (void*)0x5010) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);
    CHECK(sceNpTrophyGetGameProgress(1, 1, (void*)0x5020) == CELL_OK && vm_read32(0x5020) == 100);
    CHECK(sceNpTrophyGetRequiredDiskSpace(1, 1, (void*)0x6000, 0) == CELL_OK &&
          vm_read64(0x6000) == 1024 * 1024);
    CHECK(sceNpTrophyGetRequiredDiskSpace(1, 1, NULL, 0) == SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT);

    /* the unlock file */
    memset(&prog, 0, sizeof prog);
    CHECK(trophy_load_file(json, &prog) == 1);
    for (int i = 0; i < 5; i++)
        CHECK(prog.unlocked[i] && prog.unlock_time[i] >= t0 && prog.unlock_time[i] <= t1 + 1);
    CHECK(!prog.unlocked[5]);

    /* --- host API ----------------------------------------------------------- */
    CHECK(sceNpTrophy_snapshot(snap) == 0);
    CHECK(snap->count == 5 && snap->unlocked == 5 && snap->percent == 100 &&
          strcmp(snap->commId, "TEST00001_00") == 0 && strcmp(snap->title, "Test & Title") == 0);
    CHECK(snap->trophies[1].trophyId == 1 && strcmp(snap->trophies[1].name, "Gold <one>") == 0 &&
          snap->trophies[2].hidden && snap->per_grade[4] == 2 && snap->unlocked_per_grade[1] == 1);
    CHECK(sceNpTrophy_icon(1, &png, &png_size) == 0 && png_size == 13 && memcmp(png, "TROP001-BYTES", 13) == 0);
    sceNpTrophy_free(png);
    CHECK(sceNpTrophy_icon(-1, &png, &png_size) == 0 && png_size == 11 && memcmp(png, "ICON0-BYTES", 11) == 0);
    sceNpTrophy_free(png);
    CHECK(sceNpTrophy_icon(2, &png, &png_size) == -1 && png == NULL);

    /* --- the placeholder set when there is no TROPHY.TRP ------------------- */
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x120, NULL, 0) == CELL_OK && vm_read32(0x200) == 2);
    CHECK(sceNpTrophyRegisterContext(2, 1, 0x300, 0, 0) == CELL_OK && !s_contexts[2].set);
    CHECK(sceNpTrophyGetGameInfo(2, 1, (void*)0x1000, NULL) == CELL_OK);
    CHECK(vm_read32(0x1000) == 32 && vm_read32(0x1004) == 1 && vm_read32(0x1008) == 2 &&
          vm_read32(0x100C) == 8 && vm_read32(0x1010) == 21 && strcmp(G(0x1014), "PS3 Game") == 0);
    CHECK(sceNpTrophyGetTrophyInfo(2, 1, 100, (void*)0x3000, NULL) == CELL_OK);
    CHECK(vm_read32(0x3000) == 100 && vm_read32(0x3004) == SCE_NP_TROPHY_GRADE_BRONZE &&
          strcmp(G(0x3008), "Trophy 100") == 0);
    CHECK(sceNpTrophyUnlockTrophy(2, 1, 100, (void*)0x208) == CELL_OK && vm_read32(0x208) == 0xFFFFFFFFu);
    CHECK(sceNpTrophyUnlockTrophy(2, 1, 0, (void*)0x208) == CELL_OK);
    CHECK(sceNpTrophyGetTrophyUnlockState(2, 1, (void*)0x5000, (void*)0x5010) == CELL_OK);
    CHECK(vm_read32(0x5000) == 1 && vm_read32(0x500C) == (1u << 4) && vm_read32(0x5010) == 128);

    /* a read-only context cannot unlock */
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 1) == CELL_OK && vm_read32(0x200) == 3);
    CHECK(sceNpTrophyRegisterContext(3, 1, 0x300, 0, 0) == CELL_OK);
    CHECK(sceNpTrophyUnlockTrophy(3, 1, 1, (void*)0x208) == SCE_NP_TROPHY_ERROR_INVALID_CONTEXT);
    CHECK(sceNpTrophyDestroyContext(3) == CELL_OK);
    CHECK(sceNpTrophyDestroyContext(3) == SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT);
    CHECK(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK && vm_read32(0x204) == 2);
    CHECK(sceNpTrophyDestroyHandle(2) == CELL_OK);
    CHECK(sceNpTrophyDestroyHandle(2) == SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE);
    CHECK(sceNpTrophyTerm() == CELL_OK);

    /* --- without a context the host API finds the set on disk --------------- */
    memset(snap, 0, sizeof *snap);
    CHECK(sceNpTrophy_snapshot(snap) == 0 && snap->count == 5 && snap->unlocked == 5 &&
          strcmp(snap->commId, "TEST00001_00") == 0);
    CHECK(sceNpTrophy_icon(1, &png, &png_size) == 0 && png_size == 13);
    sceNpTrophy_free(png);

    /* --- the saved state comes back ----------------------------------------- */
    CHECK(sceNpTrophyInit(NULL, 0, 0, 0) == CELL_OK);
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x100, NULL, 0) == CELL_OK);
    CHECK(sceNpTrophyCreateHandle((void*)0x204) == CELL_OK);
    CHECK(sceNpTrophyRegisterContext(1, 1, 0x300, 0, 0) == CELL_OK);
    CHECK(sceNpTrophyGetTrophyUnlockState(1, 1, (void*)0x5000, (void*)0x5010) == CELL_OK);
    CHECK(vm_read32(0x5000) == 0x1F && vm_read32(0x5010) == 5);
    CHECK(sceNpTrophyUnlockTrophy(1, 1, 3, (void*)0x208) == SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED);

    /* --- an unreadable unlock file is kept, not overwritten ---------------- */
    CHECK(sceNpTrophyCreateContext((void*)0x200, (void*)0x120, NULL, 0) == CELL_OK && vm_read32(0x200) == 2);
    snprintf(path, sizeof path, "%s/TEST00002.json", save);
    {
        FILE* f = fopen(path, "wb");
        CHECK(f);
        fputs("not json at all", f);
        fclose(f);
    }
    CHECK(sceNpTrophyRegisterContext(2, 1, 0x300, 0, 0) == CELL_OK);
    t0 = (u64)time(NULL);
    CHECK(sceNpTrophyUnlockTrophy(2, 1, 7, (void*)0x208) == CELL_OK);
    t1 = (u64)time(NULL);
    memset(&prog, 0, sizeof prog);
    CHECK(trophy_load_file(path, &prog) == 1 && prog.unlocked[7] && !prog.unlocked[100]);
    {
        int found = 0;
        for (u64 t = t0; t <= t1 && !found; t++) {
            char bad[800];
            FILE* f;
            snprintf(bad, sizeof bad, "%s.%llu.bad", path, (unsigned long long)t);
            f = fopen(bad, "rb");
            if (f) {
                char buf[32] = {0};
                CHECK(fread(buf, 1, sizeof buf - 1, f) > 0 && strcmp(buf, "not json at all") == 0);
                fclose(f);
                remove(bad);
                found = 1;
            }
        }
        CHECK(found);
    }
    CHECK(sceNpTrophyTerm() == CELL_OK);

    remove(path);
    remove(json);
    test_rmdir(save);
    remove(trp);
    snprintf(path, sizeof path, "%s/TEST00001_00", dir); test_rmdir(path);
    snprintf(path, sizeof path, "%s/TEST00000_00", dir); test_rmdir(path);
    snprintf(path, sizeof path, "%s/not-a-set", dir);    test_rmdir(path);
    test_rmdir(dir);
    snprintf(path, sizeof path, "%s/PS3_GAME", s_root);  test_rmdir(path);
    test_rmdir(s_root);
    free(snap);
    free(set);
    free(vm_base);
    puts("Trophy set, unlock rules, unlock file and host API checks passed");
    return 0;
}
