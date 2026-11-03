/*
 * test_rsx_out -- the route switch's choice (libs/video/rsx_out.c), with no
 * renderer: rsx_out.c is built here against stand-ins for the legacy
 * renderer (rsx_d3d12_*) and for the new path's tables (rsxc_route_d3d12,
 * _null, _vk), which note each call and do nothing else.
 *
 *   test_rsx_out               runs every case, each in a child of its own
 *   test_rsx_out --case <name> the child (the route is picked once a process)
 *
 * Each child does what a host does at its start: hands the renderer its
 * overlay, the menus' overlay v2 and the game's tag, starts it, presents
 * once; then checks the route, rsx_out_renderer_info's reason, which
 * renderer got each call, and the start mark in RSX_STATE_DIR. The cases:
 * RSX_BACKEND unset, d3d12, hal-d3d12, legacy (in any case), vulkan (in any
 * case), null, metal; a Vulkan start that fails at each step
 * (TEST_VK_INIT=fail:<step>: the stand-in's start fails as the core's does,
 * and rsxc_vk_start_failed says where), which must start the default in the
 * same process with what it was handed before; a D3D12 start of the new path
 * that fails (TEST_D3D12_INIT, likewise), which must start the legacy
 * renderer so, and both one after the other; a Vulkan start whose process
 * dies before its first present (the mark stays), the start after it (D3D12,
 * saying why; the mark goes) and the one after that (Vulkan again); a mark
 * left and the default asked for; no RSX_STATE_DIR (no mark). Each child
 * gets the system's environment and the case's variables only. Prints a line
 * a case and "test_rsx_out: N passed, M failed"; exit 1 on a failure.
 *
 * On Linux the switch is built as a Linux host builds it, with the legacy
 * renderer on vkd3d (PS3RECOMP_RSX_D3D12_VKD3D; rsx_out.c is empty without
 * it) and Vulkan, which is the default there: the new path has no D3D12 off
 * Windows, so d3d12 asks for the legacy renderer, hal-d3d12 is not in the
 * build (the default runs), and a failed Vulkan start falls back to the
 * legacy renderer. The null and vulkan routes, the fallback and the start
 * mark are the same.
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

/* What each renderer was handed before its start: the overlay's user, the
 * overlay v2's user and the game's tag. */
typedef struct {
    void* overlay_user;
    void* overlay_gpu_user;
    char game[64];
} handed;
static handed g_legacy, g_hal, g_vk;

/* ---- the legacy renderer, standing in ---- */
void rsx_d3d12_composite_from_offscreen(u32 raw_off);
void rsx_prof_begin(const char* prefix);
int  rsx_prof_end(char* digest, size_t cap);
void rsx_prof_title(const char* title);

/* (three are written out below: the table's stand-ins for them get other names) */
#define rsx_d3d12_backend_init   unused_d3d12_backend_init
#define rsx_d3d12_set_overlay    unused_d3d12_set_overlay
#define rsx_d3d12_pso_index_game unused_d3d12_pso_index_game
#define STUB_V_(name, legacy, params, args)      void legacy params { logc("legacy:" #name); }
#define STUB_R_(ret, name, legacy, params, args) ret legacy params { logc("legacy:" #name); return (ret)0; }
RSX_OUT_FUNCS(STUB_V_, STUB_R_)
#undef rsx_d3d12_backend_init
#undef rsx_d3d12_set_overlay
#undef rsx_d3d12_pso_index_game

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    (void)width; (void)height; (void)title;
    logc("legacy:backend_init");
    return 0;
}
void rsx_d3d12_set_overlay(const rsx_d3d12_overlay* overlay)
{
    logc("legacy:set_overlay");
    g_legacy.overlay_user = overlay ? overlay->user : NULL;
}
void rsx_d3d12_pso_index_game(const char* game)
{
    logc("legacy:pso_index_game");
    snprintf(g_legacy.game, sizeof g_legacy.game, "%s", game ? game : "");
}

/* The start mark: RSX_STATE_DIR/renderer_starting, "" without the knob. */
static char g_mark[MAX_PATH + 32];
static int g_mark_at_init = -1;      /* the mark during the Vulkan start; -1: no Vulkan start */
static int mark_there(void) { return g_mark[0] && file_there(g_mark); }

/* ---- the new path's tables, standing in (d3d12's on Windows only) ---- */
#define TAB_V_(tag, name, params)     static void tag##_##name params { logc(#tag ":" #name); }
#define TAB_R_(tag, ret, name, params) static ret tag##_##name params { logc(#tag ":" #name); return (ret)0; }
#define HAL_V_(name, legacy, params, args)      TAB_V_(hal, name, params)
#define HAL_R_(ret, name, legacy, params, args) TAB_R_(hal, ret, name, params)
#define NUL_V_(name, legacy, params, args)      TAB_V_(null, name, params)
#define NUL_R_(ret, name, legacy, params, args) TAB_R_(null, ret, name, params)
#define VK_V_(name, legacy, params, args)       TAB_V_(vk, name, params)
#define VK_R_(ret, name, legacy, params, args)  TAB_R_(vk, ret, name, params)
/* (the starts, the overlay and the tag are written out below for d3d12 and
 * vulkan) */
#define hal_backend_init    unused_hal_backend_init
#define hal_set_overlay     unused_hal_set_overlay
#define hal_pso_index_game  unused_hal_pso_index_game
#define vk_backend_init     unused_vk_backend_init
#define vk_backend_present  unused_vk_backend_present
#define vk_set_overlay      unused_vk_set_overlay
#define vk_pso_index_game   unused_vk_pso_index_game
#ifdef _WIN32
RSX_OUT_FUNCS(HAL_V_, HAL_R_)
#endif
RSX_OUT_FUNCS(NUL_V_, NUL_R_)
RSX_OUT_FUNCS(VK_V_, VK_R_)
#undef hal_backend_init
#undef hal_set_overlay
#undef hal_pso_index_game
#undef vk_backend_init
#undef vk_backend_present
#undef vk_set_overlay
#undef vk_pso_index_game

/* A stand-in's start: <var>=fail:<step> fails it at that step, as the
 * core's start_step does; its start_failed then says where. */
static int start_as(const char* var, const char** failed)
{
    const char* f = getenv(var);
    if (f && !strncmp(f, "fail:", 5)) {
        *failed = f + 5;
        return -1;
    }
    return 0;
}

#ifdef _WIN32
static const char* g_hal_failed;
static int test_hal_backend_init(uint32_t width, uint32_t height, const char* title)
{
    (void)width; (void)height; (void)title;
    logc("hal:backend_init");
    return start_as("TEST_D3D12_INIT", &g_hal_failed);
}
static void test_hal_set_overlay(const struct rsx_d3d12_overlay* overlay)
{
    logc("hal:set_overlay");
    g_hal.overlay_user = overlay ? overlay->user : NULL;
}
static void test_hal_pso_index_game(const char* game)
{
    logc("hal:pso_index_game");
    snprintf(g_hal.game, sizeof g_hal.game, "%s", game ? game : "");
}
const char* rsxc_d3d12_start_failed(void) { return g_hal_failed; }
#endif

/* The Vulkan stand-in's start notes whether the start mark was there while
 * it ran. */
static const char* g_vk_failed;
static int test_vk_backend_init(uint32_t width, uint32_t height, const char* title)
{
    (void)width; (void)height; (void)title;
    logc("vk:backend_init");
    g_mark_at_init = mark_there();
    return start_as("TEST_VK_INIT", &g_vk_failed);
}
/* The core's first present tells the switch (rsxc_vk.c's rc_be_presented). */
static void test_vk_backend_present(void)
{
    static int n;
    logc("vk:backend_present");
    if (!n++) rsx_out_route_presented();
}
static void test_vk_set_overlay(const struct rsx_d3d12_overlay* overlay)
{
    logc("vk:set_overlay");
    g_vk.overlay_user = overlay ? overlay->user : NULL;
}
static void test_vk_pso_index_game(const char* game)
{
    logc("vk:pso_index_game");
    snprintf(g_vk.game, sizeof g_vk.game, "%s", game ? game : "");
}
const char* rsxc_vk_start_failed(void) { return g_vk_failed; }

#define FIELD_V_(name, legacy, params, args)      .name = TAB_PREFIX(name),
#define FIELD_R_(ret, name, legacy, params, args) .name = TAB_PREFIX(name),
#ifdef _WIN32
#define TAB_PREFIX(name) hal_##name
#define hal_backend_init   test_hal_backend_init
#define hal_set_overlay    test_hal_set_overlay
#define hal_pso_index_game test_hal_pso_index_game
const rsx_route rsxc_route_d3d12 = {.name = "d3d12", RSX_OUT_FUNCS(FIELD_V_, FIELD_R_)};
#undef hal_backend_init
#undef hal_set_overlay
#undef hal_pso_index_game
#undef TAB_PREFIX
#endif
#define TAB_PREFIX(name) null_##name
const rsx_route rsxc_route_null = {.name = "null", RSX_OUT_FUNCS(FIELD_V_, FIELD_R_)};
#undef TAB_PREFIX
#define TAB_PREFIX(name) vk_##name
#define vk_backend_init    test_vk_backend_init
#define vk_backend_present test_vk_backend_present
#define vk_set_overlay     test_vk_set_overlay
#define vk_pso_index_game  test_vk_pso_index_game
const rsx_route rsxc_route_vk = {.name = "vulkan", RSX_OUT_FUNCS(FIELD_V_, FIELD_R_)};
#undef vk_backend_init
#undef vk_backend_present
#undef vk_set_overlay
#undef vk_pso_index_game
#undef TAB_PREFIX

/* The overlay v2's tables (rsx_out.h, step VM): each notes the overlay it
 * was given; nothing to draw here. */
static struct gpu_device* test_gpu_device(void) { return NULL; }
static struct gpu_cmd* test_overlay_gpu_flush(void) { return NULL; }
#if defined(_WIN32)
static void hal_set_overlay_gpu(const rsx_out_overlay_gpu* overlay)
{
    logc("hal:set_overlay_gpu");
    g_hal.overlay_gpu_user = overlay ? overlay->user : NULL;
}
const rsx_route_gpu rsxc_route_gpu_d3d12 = {hal_set_overlay_gpu, test_gpu_device, test_overlay_gpu_flush};
#endif
static void null_set_overlay_gpu(const rsx_out_overlay_gpu* overlay) { (void)overlay; logc("null:set_overlay_gpu"); }
static void vk_set_overlay_gpu(const rsx_out_overlay_gpu* overlay)
{
    logc("vk:set_overlay_gpu");
    g_vk.overlay_gpu_user = overlay ? overlay->user : NULL;
}
const rsx_route_gpu rsxc_route_gpu_null = {null_set_overlay_gpu, test_gpu_device, test_overlay_gpu_flush};
const rsx_route_gpu rsxc_route_gpu_vk = {vk_set_overlay_gpu, test_gpu_device, test_overlay_gpu_flush};

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
    const char* d3d12_init;/* TEST_D3D12_INIT (Windows) */
    int state_dir;         /* RSX_STATE_DIR set (the parent's folder) */
    int die_after_init;    /* exit before the first present (a start that crashed) */
    int mark_before;       /* the parent leaves a mark before the child runs (-1: as the last case left it) */
    const char* route;     /* rsx_out_renderer after the start */
    const char* why;       /* a part of rsx_out_renderer_info's reason, NULL for none */
    const char* starts;    /* the starts called, in order, with the hand-overs after a failed one */
    int vk_mark;           /* g_mark_at_init: the mark during the Vulkan start, -1 for no Vulkan start */
    int mark_after;        /* the mark once the case is done */
} test_case;

/* The default route (RSX_BACKEND unset), its start and its start mark (-1:
 * no Vulkan start); D3D, what d3d12 asks for and what a failed Vulkan start
 * runs instead, with what it is handed. On Windows both are the new path's
 * D3D12; on Linux the default is Vulkan and d3d12 the legacy renderer. */
#ifdef _WIN32
#define DEF       "d3d12"
#define DEF_START "hal:backend_init"
#define DEF_MARK  (-1)
#define D3D       "d3d12"
#define D3D_START "hal:backend_init"
#define D3D_HAND  "hal:set_overlay hal:set_overlay_gpu hal:pso_index_game"
#else
#define DEF       "vulkan"
#define DEF_START "vk:backend_init"
#define DEF_MARK  1
#define D3D       "legacy"
#define D3D_START "legacy:backend_init"
#define D3D_HAND  "legacy:set_overlay legacy:pso_index_game"
#endif
#define LEG_START "legacy:backend_init"
#define LEG_HAND  "legacy:set_overlay legacy:pso_index_game"
#define VK_FAILS  "vk:backend_init " D3D_HAND " " D3D_START

static const test_case k_cases[] = {
    {"unset",        NULL,        NULL, NULL, 1, 0, 0, DEF,      NULL, DEF_START, DEF_MARK, 0},
    {"d3d12",        "d3d12",     NULL, NULL, 1, 0, 0, D3D,      NULL, D3D_START, -1, 0},
    {"legacy",       "legacy",    NULL, NULL, 1, 0, 0, "legacy", NULL, LEG_START, -1, 0},
    {"legacy-case",  "LeGaCy",    NULL, NULL, 1, 0, 0, "legacy", NULL, LEG_START, -1, 0},
    {"vulkan",       "vulkan",    NULL, NULL, 1, 0, 0, "vulkan", NULL, "vk:backend_init", 1, 0},
    {"vulkan-case",  "VuLkAn",    NULL, NULL, 1, 0, 0, "vulkan", NULL, "vk:backend_init", 1, 0},
    {"vulkan-nodir", "vulkan",    NULL, NULL, 0, 0, 0, "vulkan", NULL, "vk:backend_init", 0, 0},
#ifdef _WIN32
    {"hal-d3d12",    "hal-d3d12", NULL, NULL, 1, 0, 0, "d3d12",  NULL, "hal:backend_init", -1, 0},
#else
    {"hal-d3d12",    "hal-d3d12", NULL, NULL, 1, 0, 0, DEF,      "RSX_BACKEND=hal-d3d12 is not in this build",
     DEF_START, DEF_MARK, 0},
#endif
    {"null",         "null",      NULL, NULL, 1, 0, 0, "null",   NULL, "null:backend_init", -1, 0},
    {"metal",        "metal",     NULL, NULL, 1, 0, 0, DEF,      "RSX_BACKEND=metal is not in this build",
     DEF_START, DEF_MARK, 0},
    {"fail-loader",  "vulkan", "fail:loader", NULL, 1, 0, 0, D3D, "Vulkan failed to start: no Vulkan 1.3 driver (loader)",
     VK_FAILS, 1, 0},
    {"fail-instance", "vulkan", "fail:instance", NULL, 1, 0, 0, D3D, "(instance)", VK_FAILS, 1, 0},
    {"fail-adapter", "vulkan", "fail:adapter", NULL, 1, 0, 0, D3D, "(adapter)", VK_FAILS, 1, 0},
    {"fail-features", "vulkan", "fail:features", NULL, 1, 0, 0, D3D, "(features)", VK_FAILS, 1, 0},
    {"fail-device",  "vulkan", "fail:device", NULL, 1, 0, 0, D3D, "the GPU device could not be made (device)",
     VK_FAILS, 1, 0},
    {"fail-window",  "vulkan", "fail:window", NULL, 1, 0, 0, D3D, "(window)", VK_FAILS, 1, 0},
    {"fail-swap",    "vulkan", "fail:swap chain", NULL, 1, 0, 0, D3D, "swap chain could not be made (swap chain)",
     VK_FAILS, 1, 0},
    {"fail-targets", "vulkan", "fail:targets", NULL, 1, 0, 0, D3D, "(targets)", VK_FAILS, 1, 0},
    {"fail-shaders", "vulkan", "fail:shaders", NULL, 1, 0, 0, D3D, "(shaders)", VK_FAILS, 1, 0},
    {"fail-rings",   "vulkan", "fail:rings", NULL, 1, 0, 0, D3D, "(rings)", VK_FAILS, 1, 0},
    {"fail-nodir",   "vulkan", "fail:window", NULL, 0, 0, 0, D3D, "(window)", VK_FAILS, 0, 0},
#ifdef _WIN32
    /* The new path's D3D12 start fails: the legacy renderer, with what it was handed. */
    {"d3d12-fail-device", NULL, NULL, "fail:device", 1, 0, 0, "legacy",
     "D3D12 failed to start: the GPU device could not be made (device)",
     "hal:backend_init " LEG_HAND " " LEG_START, -1, 0},
    {"d3d12-fail-swap", "d3d12", NULL, "fail:swap chain", 1, 0, 0, "legacy",
     "D3D12 failed to start: the window's swap chain could not be made (swap chain)",
     "hal:backend_init " LEG_HAND " " LEG_START, -1, 0},
    {"d3d12-fail-unnamed", NULL, NULL, "fail:", 1, 0, 0, "legacy",
     "D3D12 failed to start: an error the renderer did not name",
     "hal:backend_init " LEG_HAND " " LEG_START, -1, 0},
    /* Both fail, one after the other: the legacy renderer, saying why the last did. */
    {"both-fail",    "vulkan", "fail:device", "fail:targets", 1, 0, 0, "legacy",
     "D3D12 failed to start: the render targets could not be made (targets)",
     VK_FAILS " " LEG_HAND " " LEG_START, 1, 0},
#endif
    /* A start that dies in the driver, then the two after it, in order. */
    {"crash",        "vulkan", NULL, NULL, 1, 1, 0, NULL, NULL, NULL, 1, 1},
    {"after-crash",  "vulkan", NULL, NULL, 1, 0, -1, D3D, "the last start on Vulkan did not finish", D3D_START, -1, 0},
    {"after-that",   "vulkan", NULL, NULL, 1, 0, -1, "vulkan", NULL, "vk:backend_init", 1, 0},
    /* A mark left, and the default asked for: on Windows done with too; on
     * Linux, where the default is Vulkan, the last Vulkan start's, as above. */
#ifdef _WIN32
    {"left-default", NULL,     NULL, NULL, 1, 0, 1, DEF, NULL, DEF_START, -1, 0},
#else
    {"left-default", NULL,     NULL, NULL, 1, 0, 1, D3D, "the last start on Vulkan did not finish", D3D_START, -1, 0},
#endif
};

static const test_case* case_named(const char* name)
{
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++)
        if (!strcmp(k_cases[i].name, name)) return &k_cases[i];
    return NULL;
}

/* The starts in the log (the calls named backend_init, and the overlays and
 * the tag handed over after the first), as one string. */
static void starts_of(const char* log, char* out, size_t cap)
{
    out[0] = 0;
    char copy[sizeof g_log];
    snprintf(copy, sizeof copy, "%s", log);
    int after_start = 0;
    for (char* t = strtok(copy, " "); t; t = strtok(NULL, " ")) {
        const int start = strstr(t, ":backend_init") != NULL;
        const int hand = has(t, ":set_overlay") || has(t, ":pso_index_game");
        if (start || (after_start && hand)) {
            const size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s", n ? " " : "", t);
        }
        if (start) after_start = 1;
    }
}

/* What the renderer named by a route was handed. */
static const handed* handed_to(const char* route)
{
    if (!strcmp(route, "legacy")) return &g_legacy;
    if (!strcmp(route, "vulkan")) return &g_vk;
    return &g_hal;
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

    /* A host's start: the overlays and the tag first (des_ui_early_init,
     * ps3_title_prework), then the window, then a frame. */
    static const rsx_d3d12_overlay ov = {(void*)(uintptr_t)0x1234, NULL, NULL, NULL};
    static const rsx_out_overlay_gpu ovg = {(void*)(uintptr_t)0x5678, NULL};
    rsx_out_set_overlay(&ov);
    rsx_out_set_overlay_gpu(&ovg);
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
    const char* const pres = !strcmp(c->route, "vulkan") ? "vk:backend_present" : !strcmp(c->route, "d3d12")
                             ? "hal:backend_present" : !strcmp(c->route, "null") ? "null:backend_present"
                             : "legacy:backend_present";
    snprintf(msg, sizeof msg, "the present went to %s (%s)", pres, g_log);
    check(has(g_log, pres), msg);
    /* The renderer that runs has the overlay and the tag, whether it was
     * handed them at the start or after a failed one; the overlay v2 too,
     * but on legacy, which has none. */
    if (strcmp(c->route, "null")) {
        const handed* const h = handed_to(c->route);
        check(h->overlay_user == (void*)(uintptr_t)0x1234, "the renderer that runs got the overlay");
        check(!strcmp(h->game, "TEST00000"), "the renderer that runs got the game's tag");
        if (strcmp(c->route, "legacy"))
            check(h->overlay_gpu_user == (void*)(uintptr_t)0x5678, "the renderer that runs got the overlay v2");
    }
    if (c->vk_mark >= 0 && strcmp(c->route, "vulkan"))
        check(!has(g_log, "vk:backend_present"), "nothing more went to Vulkan");
    if (c->vk_mark < 0)
        check(!has(g_log, "vk:"), "nothing went to Vulkan");
    if (!strcmp(c->route, "legacy") && has(c->starts, "hal:"))
        check(!has(g_log, "hal:backend_present"), "nothing more went to the new path's D3D12");
    if (g_fail) printf("    log: %s\n", g_log);
    return g_fail ? 1 : 0;
}

/* ---- the parent ---- */
static int run_child(const char* exe, const test_case* c, const char* dir)
{
    env_put("RSX_BACKEND", c->backend);
    env_put("TEST_VK_INIT", c->vk_init);
    env_put("TEST_D3D12_INIT", c->d3d12_init);
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
