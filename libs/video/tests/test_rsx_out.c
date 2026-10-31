/*
 * test_rsx_out -- the route switch's choice (libs/video/rsx_out.c, step V6),
 * with no renderer: rsx_out.c is built here against stand-ins for today's
 * renderer (rsx_d3d12_*) and for the new path's tables (rsxc_route_d3d12,
 * _null, _vk), which note each call and do nothing else.
 *
 *   test_rsx_out               runs every case, each in a child of its own
 *   test_rsx_out --case <name> the child (the route is picked once a process)
 *
 * Each child does what a host does at its start: hands the renderer its
 * overlay and the game's tag, starts it, presents once; then checks the
 * route, rsx_out_renderer_info's reason, which renderer got each call, and
 * the start mark in RSX_STATE_DIR. The cases: RSX_BACKEND unset, d3d12,
 * vulkan (in any case), hal-d3d12, null, metal; a Vulkan start that fails at
 * each step (TEST_VK_INIT=fail:<step>: the stand-in's start fails as the
 * core's does, and rsxc_vk_start_failed says where), which must start
 * today's renderer in the same process with what it was handed before; a
 * Vulkan start whose process dies before its first present (the mark
 * stays), the start after it (d3d12, saying why; the mark goes) and the one
 * after that (Vulkan again); a mark left and d3d12 asked for; no
 * RSX_STATE_DIR (no mark). Each child gets the system's environment and the
 * case's variables only. Prints a line a case and "test_rsx_out: N passed,
 * M failed"; exit 1 on a failure.
 *
 * On Linux the switch is built as a Linux host builds it, with today's
 * renderer on vkd3d (PS3RECOMP_RSX_D3D12_VKD3D; rsx_out.c is empty without
 * it). It has the null and vulkan routes there too, with the same fallback
 * and start mark, and the same cases but one: hal-d3d12 is Windows' only, so
 * on Linux it runs today's renderer and says why.
 */
#if defined(_WIN32) || defined(__linux__)

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "rsx_out.h"
#include "rsx_d3d12_backend.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAX_PATH 4096
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the few system calls, on both systems ---- */
#ifdef _WIN32
#define SEP "\\"
static int  file_there(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void file_del(const char* p) { DeleteFileA(p); }
static void dir_del(const char* p) { RemoveDirectoryA(p); }
static void env_put(const char* name, const char* v) { SetEnvironmentVariableA(name, v); }
#else
#define SEP "/"
static int  file_there(const char* p) { return access(p, F_OK) == 0; }
static void file_del(const char* p) { unlink(p); }
static void dir_del(const char* p) { rmdir(p); }
static void env_put(const char* name, const char* v) { if (v) setenv(name, v, 1); else unsetenv(name); }
#endif

/* ---- what each renderer was asked, in order ---- */
static char g_log[8192];
static void logc(const char* who)
{
    const size_t n = strlen(g_log);
    snprintf(g_log + n, sizeof g_log - n, "%s ", who);
}

/* ---- today's renderer, standing in ---- */
void rsx_d3d12_composite_from_offscreen(u32 raw_off);
void rsx_prof_begin(const char* prefix);
int  rsx_prof_end(char* digest, size_t cap);
void rsx_prof_title(const char* title);

static void* g_d3d12_overlay_user;   /* the overlay today's renderer was given */
static char g_d3d12_game[64];

/* (three are written out below: the table's stand-ins for them get other names) */
#define rsx_d3d12_backend_init   unused_d3d12_backend_init
#define rsx_d3d12_set_overlay    unused_d3d12_set_overlay
#define rsx_d3d12_pso_index_game unused_d3d12_pso_index_game
#define STUB_V_(name, legacy, params, args)      void legacy params { logc("d3d12:" #name); }
#define STUB_R_(ret, name, legacy, params, args) ret legacy params { logc("d3d12:" #name); return (ret)0; }
RSX_OUT_FUNCS(STUB_V_, STUB_R_)
#undef rsx_d3d12_backend_init
#undef rsx_d3d12_set_overlay
#undef rsx_d3d12_pso_index_game

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    (void)width; (void)height; (void)title;
    logc("d3d12:backend_init");
    return 0;
}
void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay)
{
    logc("d3d12:set_overlay");
    g_d3d12_overlay_user = overlay ? overlay->user : NULL;
}
void rsx_d3d12_pso_index_game(const char* game)
{
    logc("d3d12:pso_index_game");
    snprintf(g_d3d12_game, sizeof g_d3d12_game, "%s", game ? game : "");
}

/* The start mark: RSX_STATE_DIR/renderer_starting, "" without the knob. */
static char g_mark[MAX_PATH + 32];
static int g_mark_at_init = -1;      /* the mark during the Vulkan start; -1: no Vulkan start */
static int mark_there(void) { return g_mark[0] && file_there(g_mark); }

/* ---- the new path's tables, standing in (hal-d3d12's on Windows only) ---- */
#define TAB_V_(tag, name, params)     static void tag##_##name params { logc(#tag ":" #name); }
#define TAB_R_(tag, ret, name, params) static ret tag##_##name params { logc(#tag ":" #name); return (ret)0; }
#define HAL_V_(name, legacy, params, args)      TAB_V_(hal, name, params)
#define HAL_R_(ret, name, legacy, params, args) TAB_R_(hal, ret, name, params)
#define NUL_V_(name, legacy, params, args)      TAB_V_(null, name, params)
#define NUL_R_(ret, name, legacy, params, args) TAB_R_(null, ret, name, params)
#define VK_V_(name, legacy, params, args)       TAB_V_(vk, name, params)
#define VK_R_(ret, name, legacy, params, args)  TAB_R_(vk, ret, name, params)
#ifdef _WIN32
RSX_OUT_FUNCS(HAL_V_, HAL_R_)
#endif
RSX_OUT_FUNCS(NUL_V_, NUL_R_)
#define vk_backend_init    unused_vk_backend_init
#define vk_backend_present unused_vk_backend_present
RSX_OUT_FUNCS(VK_V_, VK_R_)
#undef vk_backend_init
#undef vk_backend_present

/* The Vulkan stand-in's start: TEST_VK_INIT=fail:<step> fails it at that
 * step, as the core's start_step does. It notes whether the start mark was
 * there while it ran. */
static const char* g_vk_failed;

static int test_vk_backend_init(uint32_t width, uint32_t height, const char* title)
{
    (void)width; (void)height; (void)title;
    logc("vk:backend_init");
    g_mark_at_init = mark_there();
    const char* f = getenv("TEST_VK_INIT");
    if (f && !strncmp(f, "fail:", 5)) {
        g_vk_failed = f + 5;
        return -1;
    }
    return 0;
}
/* The core's first present tells the switch (rsxc_vk.c's rc_be_presented). */
static void test_vk_backend_present(void)
{
    static int n;
    logc("vk:backend_present");
    if (!n++) rsx_out_route_presented();
}
const char* rsxc_vk_start_failed(void) { return g_vk_failed; }

#define FIELD_V_(name, legacy, params, args)      .name = TAB_PREFIX(name),
#define FIELD_R_(ret, name, legacy, params, args) .name = TAB_PREFIX(name),
#ifdef _WIN32
#define TAB_PREFIX(name) hal_##name
const rsx_route rsxc_route_d3d12 = {.name = "hal-d3d12", RSX_OUT_FUNCS(FIELD_V_, FIELD_R_)};
#undef TAB_PREFIX
#endif
#define TAB_PREFIX(name) null_##name
const rsx_route rsxc_route_null = {.name = "null", RSX_OUT_FUNCS(FIELD_V_, FIELD_R_)};
#undef TAB_PREFIX
#define TAB_PREFIX(name) vk_##name
#define vk_backend_init    test_vk_backend_init
#define vk_backend_present test_vk_backend_present
const rsx_route rsxc_route_vk = {.name = "vulkan", RSX_OUT_FUNCS(FIELD_V_, FIELD_R_)};
#undef vk_backend_init
#undef vk_backend_present
#undef TAB_PREFIX

/* The overlay v2's tables (rsx_out.h, step VM): nothing to draw here. */
#if defined(_WIN32)
static void test_set_overlay_gpu(const rsx_out_overlay_gpu* overlay) { (void)overlay; }
static struct gpu_device* test_gpu_device(void) { return NULL; }
static struct gpu_cmd* test_overlay_gpu_flush(void) { return NULL; }
const rsx_route_gpu rsxc_route_gpu_d3d12 = {test_set_overlay_gpu, test_gpu_device, test_overlay_gpu_flush};
const rsx_route_gpu rsxc_route_gpu_null = {test_set_overlay_gpu, test_gpu_device, test_overlay_gpu_flush};
const rsx_route_gpu rsxc_route_gpu_vk = {test_set_overlay_gpu, test_gpu_device, test_overlay_gpu_flush};
#endif

/* ---- the child: one case ---- */
static int g_fail;
static void check(int ok, const char* what)
{
    if (!ok) {
        printf("    FAIL: %s\n", what);
        g_fail++;
    }
}
static int has(const char* text, const char* part) { return text && strstr(text, part) != NULL; }

/* What the child does, and what it must see. */
typedef struct {
    const char* name;
    const char* backend;   /* RSX_BACKEND, NULL unset */
    const char* vk_init;   /* TEST_VK_INIT */
    int state_dir;         /* RSX_STATE_DIR set (the parent's folder) */
    int die_after_init;    /* exit before the first present (a start that crashed) */
    int mark_before;       /* the parent leaves a mark before the child runs (-1: as the last case left it) */
    const char* route;     /* rsx_out_renderer after the start */
    const char* why;       /* a part of rsx_out_renderer_info's reason, NULL for none */
    const char* starts;    /* the starts called, in order, as the log has them */
    int vk_mark;           /* g_mark_at_init: the mark during the Vulkan start, -1 for no Vulkan start */
    int mark_after;        /* the mark once the case is done */
} test_case;

static const test_case k_cases[] = {
    {"unset",        NULL,        NULL, 1, 0, 0, "d3d12",     NULL, "d3d12:backend_init", -1, 0},
    {"d3d12",        "d3d12",     NULL, 1, 0, 0, "d3d12",     NULL, "d3d12:backend_init", -1, 0},
    {"vulkan",       "vulkan",    NULL, 1, 0, 0, "vulkan",    NULL, "vk:backend_init", 1, 0},
    {"vulkan-case",  "VuLkAn",    NULL, 1, 0, 0, "vulkan",    NULL, "vk:backend_init", 1, 0},
    {"vulkan-nodir", "vulkan",    NULL, 0, 0, 0, "vulkan",    NULL, "vk:backend_init", 0, 0},
#ifdef _WIN32
    {"hal-d3d12",    "hal-d3d12", NULL, 1, 0, 0, "hal-d3d12", NULL, "hal:backend_init", -1, 0},
#else
    {"hal-d3d12",    "hal-d3d12", NULL, 1, 0, 0, "d3d12",     "RSX_BACKEND=hal-d3d12 is not in this build",
     "d3d12:backend_init", -1, 0},
#endif
    {"null",         "null",      NULL, 1, 0, 0, "null",      NULL, "null:backend_init", -1, 0},
    {"metal",        "metal",     NULL, 1, 0, 0, "d3d12",     "RSX_BACKEND=metal is not in this build",
     "d3d12:backend_init", -1, 0},
    {"fail-loader",  "vulkan", "fail:loader", 1, 0, 0, "d3d12", "Vulkan failed to start: no Vulkan 1.3 driver (loader)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-instance", "vulkan", "fail:instance", 1, 0, 0, "d3d12", "(instance)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-adapter", "vulkan", "fail:adapter", 1, 0, 0, "d3d12", "(adapter)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-features", "vulkan", "fail:features", 1, 0, 0, "d3d12", "(features)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-device",  "vulkan", "fail:device", 1, 0, 0, "d3d12", "(device)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-window",  "vulkan", "fail:window", 1, 0, 0, "d3d12", "(window)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-swap",    "vulkan", "fail:swap chain", 1, 0, 0, "d3d12", "swap chain could not be made (swap chain)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-targets", "vulkan", "fail:targets", 1, 0, 0, "d3d12", "(targets)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-shaders", "vulkan", "fail:shaders", 1, 0, 0, "d3d12", "(shaders)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-rings",   "vulkan", "fail:rings", 1, 0, 0, "d3d12", "(rings)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 1, 0},
    {"fail-nodir",   "vulkan", "fail:window", 0, 0, 0, "d3d12", "(window)",
     "vk:backend_init d3d12:set_overlay d3d12:pso_index_game d3d12:backend_init", 0, 0},
    /* A start that dies in the driver, then the two after it, in order. */
    {"crash",        "vulkan", NULL, 1, 1, 0, NULL, NULL, NULL, 1, 1},
    {"after-crash",  "vulkan", NULL, 1, 0, -1, "d3d12", "the last start on Vulkan did not finish",
     "d3d12:backend_init", -1, 0},
    {"after-that",   "vulkan", NULL, 1, 0, -1, "vulkan", NULL, "vk:backend_init", 1, 0},
    /* A mark left, and d3d12 asked for: done with too. */
    {"left-d3d12",   NULL,     NULL, 1, 0, 1, "d3d12", NULL, "d3d12:backend_init", -1, 0},
};

static const test_case* case_named(const char* name)
{
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++)
        if (!strcmp(k_cases[i].name, name)) return &k_cases[i];
    return NULL;
}

/* The starts in the log (the calls named backend_init, and today's overlay
 * and tag calls after a fallback), as one string. */
static void starts_of(const char* log, char* out, size_t cap)
{
    out[0] = 0;
    char copy[sizeof g_log];
    snprintf(copy, sizeof copy, "%s", log);
    int after_vk = 0;
    for (char* t = strtok(copy, " "); t; t = strtok(NULL, " ")) {
        const int start = strstr(t, ":backend_init") != NULL;
        if (!strcmp(t, "vk:backend_init")) after_vk = 1;
        if (start || (after_vk && (!strcmp(t, "d3d12:set_overlay") || !strcmp(t, "d3d12:pso_index_game")))) {
            const size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s", n ? " " : "", t);
        }
    }
}

static int run_case(const char* name)
{
    const test_case* c = case_named(name);
    if (!c) {
        printf("    unknown case %s\n", name);
        return 2;
    }
    const char* dir = getenv("RSX_STATE_DIR");
    if (dir && *dir) snprintf(g_mark, sizeof g_mark, "%s" SEP "renderer_starting", dir);

    const char* why = "x";
    check(rsx_out_renderer_info(&why) == NULL && why == NULL, "no route before the first call");

    /* A host's start: the overlay and the tag first (des_ui_early_init,
     * ps3_title_prework), then the window, then a frame. */
    static const rsx_d3d12_overlay ov = {(void*)(uintptr_t)0x1234, NULL, NULL, NULL};
    rsx_out_set_overlay(&ov);
    rsx_out_pso_index_game("TEST00000");
    const int r = rsx_out_backend_init(1280, 720, "test_rsx_out");
    if (c->die_after_init) {
        fflush(stdout);
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 3);   /* (a start that never reaches its first frame) */
#else
        _exit(3);
#endif
    }
    rsx_out_backend_present();

    check(r == 0, "the start returned 0");
    const char* route = rsx_out_renderer();
    char msg[512];
    snprintf(msg, sizeof msg, "route %s, want %s", route, c->route);
    check(!strcmp(route, c->route), msg);
    const char* info = rsx_out_renderer_info(&why);
    check(info && !strcmp(info, c->route), "rsx_out_renderer_info names the same route");
    snprintf(msg, sizeof msg, "why \"%s\", want \"%s\"", why ? why : "(none)", c->why ? c->why : "(none)");
    check(c->why ? has(why, c->why) : why == NULL, msg);
    char starts[512];
    starts_of(g_log, starts, sizeof starts);
    snprintf(msg, sizeof msg, "starts \"%s\", want \"%s\"", starts, c->starts);
    check(!strcmp(starts, c->starts), msg);
    snprintf(msg, sizeof msg, "the mark during the Vulkan start: %d, want %d", g_mark_at_init, c->vk_mark);
    check(g_mark_at_init == c->vk_mark, msg);
    check(mark_there() == c->mark_after, "the mark after the first present");

    /* The calls after the start go to the renderer that runs. */
    const char* const pres = !strcmp(c->route, "vulkan") ? "vk:backend_present" : !strcmp(c->route, "hal-d3d12")
                             ? "hal:backend_present" : !strcmp(c->route, "null") ? "null:backend_present"
                             : "d3d12:backend_present";
    snprintf(msg, sizeof msg, "the present went to %s (%s)", pres, g_log);
    check(has(g_log, pres), msg);
    if (!strcmp(c->route, "d3d12") && c->vk_mark >= 0) {   /* a fallback: today's renderer has the overlay and tag */
        check(g_d3d12_overlay_user == (void*)(uintptr_t)0x1234, "today's renderer got the overlay");
        check(!strcmp(g_d3d12_game, "TEST00000"), "today's renderer got the game's tag");
        check(!has(g_log, "vk:backend_present"), "nothing more went to Vulkan");
    }
    if (!strcmp(c->route, "d3d12") && c->vk_mark < 0)
        check(!has(g_log, "vk:"), "nothing went to Vulkan");
    if (g_fail) printf("    log: %s\n", g_log);
    return g_fail ? 1 : 0;
}

/* ---- the parent ---- */
static int run_child(const char* exe, const test_case* c, const char* dir)
{
    env_put("RSX_BACKEND", c->backend);
    env_put("TEST_VK_INIT", c->vk_init);
    env_put("RSX_STATE_DIR", c->state_dir ? dir : NULL);
    env_put("RSX_FAIL_START_AT", NULL);
#ifndef _WIN32
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        alarm(30);   /* (as the Windows wait's 30 s) */
        char* const argv[] = { (char*)exe, (char*)"--case", (char*)c->name, NULL };
        execv(exe, argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
#else
    char cmd[MAX_PATH * 2];
    snprintf(cmd, sizeof cmd, "\"%s\" --case %s", exe, c->name);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 99;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
#endif
}

int main(int argc, char** argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--case")) return run_case(argv[2]);

    char exe[MAX_PATH], tmp[MAX_PATH], dir[MAX_PATH], mark[MAX_PATH + 32];
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof exe);
    GetTempPathA(sizeof tmp, tmp);
    snprintf(dir, sizeof dir, "%stest_rsx_out_%lu", tmp, (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
#else
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    const char* const t = getenv("TMPDIR");
    snprintf(tmp, sizeof tmp, "%s", t && *t ? t : "/tmp");
    snprintf(dir, sizeof dir, "%s/test_rsx_out_%lu", tmp, (unsigned long)getpid());
    mkdir(dir, 0777);
#endif
    snprintf(mark, sizeof mark, "%s" SEP "renderer_starting", dir);
    int passed = 0, failed = 0;
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        const test_case* c = &k_cases[i];
        if (c->mark_before >= 0) {
            file_del(mark);
            if (c->mark_before) {
                FILE* f = fopen(mark, "wb");
                if (f) fclose(f);
            }
        }
        fflush(stdout);
        const int code = run_child(exe, c, dir);
        int ok = c->die_after_init ? code == 3 : code == 0;
        if (c->die_after_init) {
            const int left = file_there(mark);
            if (!left) printf("    FAIL: the start mark is not there after a start that died\n");
            ok = ok && left;
        }
        printf("%s %s\n", ok ? "PASS" : "FAIL", c->name);
        if (ok) passed++; else failed++;
    }
    file_del(mark);
    dir_del(dir);
    printf("test_rsx_out: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_rsx_out: Windows and Linux only\n");
    return 0;
}
#endif
