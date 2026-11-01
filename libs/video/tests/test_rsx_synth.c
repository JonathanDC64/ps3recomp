/*
 * test_rsx_synth -- the portable renderer's core (libs/video/rsxcore) on the
 * null backend, over synthetic scenes: the commands it issues, as the null
 * backend's stream hash, against a golden list.
 *
 *   test_rsx_synth [--update-golden] [--keep] [--out <dir>] [--env NAME=VALUE]...
 *   test_rsx_synth --route vulkan [--no-validation] [--keep] [--out <dir>] [--env NAME=VALUE]...
 *   test_rsx_synth --route vulkan --fallback <step>|mark [...]   the start falls back to today's renderer
 *   test_rsx_synth --stores vulkan|hal-d3d12 [--out <dir>] [--env NAME=VALUE]...
 *                                   the new path's pipeline stores (stores_test says how)
 *   test_rsx_synth --caps <dir>     only write the scenes' captures (for rsx_replay)
 *   test_rsx_synth --run <dir>      the child the test starts: plays them
 *
 * Nothing here comes from a game. Each scene is a few frames, each frame a
 * capture as RSX_REPLAY_CAP writes one (cellGcmSys.c): the RSX state and the
 * GCM tables at its start, the guest memory it reads and its NV4097 method
 * stream, ending with the display clear that presents it. The vertex and
 * fragment programs come from rsx_prog_gen.h with fixed seeds; the vertex
 * data, indices, textures (linear and swizzled, a dozen formats, mipmaps, a
 * cube) and surfaces are written here.
 *
 * A child process plays every capture in turn through the route switch on
 * RSX_BACKEND=null, as rsx_replay plays a chain (cellGcm_replay_load, the
 * methods through cellGcm_replay_method, a present), then all of them again:
 * the second pass plays on what the first left (the memo, the caches, the
 * rings, the grave). Then it shuts the renderer down, which the null backend
 * checks as well. After each present the null instance prints the stream's
 * hash so far (rsxc_null.c); a scene's hash is the one at its last present.
 * The hash runs on from the device's creation, so the second pass's hashes
 * are their own; the second process is what plays every scene again from
 * the start. (A shutdown and a second init in one process are no fresh
 * start: the core keeps targets, views and its query pool from the first
 * device, which the null checks catch.)
 *
 * The test runs the child twice, each in a clean environment (the system's
 * variables only: no knob in the shell reaches it), the first with the dump
 * and PSO_THREADS=4, VP_FETCH_THREADS=2, the second with PSO_THREADS=1,
 * VP_FETCH_THREADS=0 and no dump. It passes
 * when
 *   - no null check failed, in the frames or at the shutdown;
 *   - every frame presented once;
 *   - both children give the same hashes: no timing, thread order, shader
 *     compiler (the null route names a shader by its text's key) or machine
 *     is in them;
 *   - every hash is its golden (k_golden below);
 *   - each scene's frames hold the records it is there for (k_scene's needs),
 *     counted in the first child's GPU_NULL_DUMP, so a golden cannot come to
 *     stand for a scene that stopped drawing.
 * On a failure it names the scene, the expected and the actual hash, and
 * keeps <out> (default test_rsx_synth.out in the working directory): each
 * child's log, the first child's stream dump (a/dump.txt, one record a line,
 * to diff against a good run's) and the captures. --keep keeps it on a pass
 * too. --update-golden prints the list for a deliberate change.
 *
 * To look at a scene: test_rsx_synth --caps <dir>, then rsx_replay on its
 * captures, on any route (RSX_BACKEND=d3d12, hal-d3d12 or null). All of them
 * in their order, twice, is the child's chain:
 *   RSX_BACKEND=null rsx_replay <the captures> <the captures again> 1
 * prints the second pass's last hash as its last present line.
 *
 * --route vulkan plays the same chain on the Vulkan route (rsxc_vk.c), in
 * one child, with the validation layer on (RSX_VK_VALIDATION=1, with
 * synchronization validation). There is no stream hash there; it passes
 * when every frame played, the child got to the end, the layer was there
 * (--no-validation runs without it) and it said nothing: any validation
 * message fails. --env RSX_VK_MASK=<bits> runs the backend's fallbacks.
 *
 * --route vulkan --fallback <step> asks for Vulkan with the start made to
 * fail at that step (RSX_FAIL_START_AT, rsxc_init.inc's steps), and
 * --fallback mark with a start mark left (RSX_STATE_DIR/renderer_starting,
 * a start that did not finish): the route switch must say why and start
 * today's renderer in Vulkan's place (rsx_out.c, step V6), which plays the
 * same chain; the mark must be gone at the end. Every Vulkan run has
 * RSX_STATE_DIR, so the plain one checks the mark goes at the first present.
 *
 * Windows and Linux, as the null route (rsxc_null.c). The child plays
 * through the route switch (rsx_out.c, RSX_BACKEND) on Windows and in
 * Linux's vkd3d builds (PS3RECOMP_RSX_D3D12_VKD3D: the switch with today's
 * renderer on vkd3d); a Linux build without vkd3d has no switch, and the
 * child calls the route's table (rsxc_route_null, rsxc_route_vk) itself
 * (no --fallback there). The hashes are the same on both: one golden list.
 */
#if defined(PS3RECOMP_RSX_HAL) && (defined(_WIN32) || defined(__linux__))

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "rsx_commands.h"           /* rsx_state, NV4097 methods */
#include "cellGcmSys.h"             /* CellGcmConfig, CellGcmDisplayInfo: capture sections */
#include "rsx_out.h"                /* the route switch */
#include "rsx_fp_decompiler.h"      /* rsx_fp_program_size, rsx_fp_texture_units */
#include "rsxcore/rsxc_shader.h"    /* rsxc_shader_hlsl: does a program decompile */
#include "rsx_prog_gen.h"           /* the seeded program generator */
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

/* What the runtime library expects from a game's loader. No guest code runs. */
uint8_t* vm_base = NULL;
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

/* cellGcmSys.c's replay side, as rsx_replay_main.c declares it. */
int  cellGcm_replay_load(const char* file, uint32_t** methods);
void cellGcm_replay_reset(void);
void cellGcm_replay_method(uint32_t mfull, uint32_t data);

/* ---------------------------------------------------------------------------
 * The golden hashes: the null backend's stream hash at each scene's last
 * present, in the first pass (a new renderer) and the second (the same
 * captures again, on what the first left).
 *
 * Update them (--update-golden prints this table) only in a commit that
 * means to change the commands the core issues, or the null backend's stream
 * format (gpu_null.h), or these scenes, or rsx_prog_gen.h's generator, and
 * say so in its message. Any other change that moves a hash is a bug in it.
 * -------------------------------------------------------------------------*/
typedef struct { const char* scene; uint64_t pass1, pass2; } Golden;
static const Golden k_golden[] = {
    { "clears",    0x36051ca52a5fffaaull, 0xc31ac4564fbe079full },
    { "draws",     0xa7ba76be99884fe3ull, 0xec5cfd8d2ff830fdull },
    { "states",    0xbb47e5374abb2605ull, 0x80048e8a62527110ull },
    { "textures",  0xe9662a21292240a7ull, 0xd3b9c4e6502a78a3ull },
    { "offscreen", 0x0a2dc318b59ff233ull, 0x8833caea42ea5fc0ull },
    { "zpass",     0xaaa70f5fdadde81aull, 0x9fb362330df35148ull },
    { "frames",    0x7513073b1e2ba8beull, 0xe36b6ddac7a20266ull },
    { "mainmem",   0xcf70025a99857609ull, 0x0798f184c9ed0965ull },
};

/* ---------------------------------------------------------------------------
 * Guest memory, as rsx_replay sets it up: the 4 GB guest space reserved,
 * pages committed on first touch (reads of untouched memory see zero). On
 * Linux an anonymous mapping without reserve does the same by itself.
 * -------------------------------------------------------------------------*/
#define VM_SPAN 0x100010000ull

#ifdef _WIN32
static LONG WINAPI commit_on_touch(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
    if (a < vm_base || a >= vm_base + VM_SPAN) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t* page = (uint8_t*)((uintptr_t)a & ~(uintptr_t)0xFFFF);
    return VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION
                                                                   : EXCEPTION_CONTINUE_SEARCH;
}

static int vm_reserve(void)
{
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SPAN, MEM_RESERVE, PAGE_READWRITE);
    if (!vm_base) return 0;
    AddVectoredExceptionHandler(1, commit_on_touch);
    return 1;
}
#else
static int vm_reserve(void)
{
    void* const p = mmap(NULL, VM_SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return 0;
    vm_base = (uint8_t*)p;
    return 1;
}
#endif

/* ---------------------------------------------------------------------------
 * The few system calls the test makes, on both systems.
 * -------------------------------------------------------------------------*/
#ifdef _WIN32
static void dir_make(const char* d) { CreateDirectoryA(d, NULL); }
static void file_del(const char* f) { DeleteFileA(f); }
static void dir_del(const char* d)  { RemoveDirectoryA(d); }
static void env_set(const char* name, const char* v) { _putenv_s(name, v); }
static int  self_exe(char* out, size_t cap) { return GetModuleFileNameA(NULL, out, (DWORD)cap) != 0; }
#else
static void dir_make(const char* d) { mkdir(d, 0777); }
static void file_del(const char* f) { unlink(f); }
static void dir_del(const char* d)  { rmdir(d); }
static void env_set(const char* name, const char* v) { setenv(name, v, 1); }
static int  self_exe(char* out, size_t cap)
{
    const ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}
#endif

/* ---------------------------------------------------------------------------
 * The route the child plays on, as RSX_BACKEND names it: through the route
 * switch where the build has one, else the core's table itself (the file's
 * comment says why). ROUTE(f) is the route's function f.
 * -------------------------------------------------------------------------*/
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
extern volatile uint32_t gpu_vk_n_validation_all;   /* gpu_vk.c: the layer's messages, every device's */
#endif
#if defined(_WIN32) || defined(PS3RECOMP_RSX_D3D12_VKD3D)
#define SYNTH_SWITCH 1
#define ROUTE(f) rsx_out_##f
static int route_open(const char* name) { (void)name; return 1; }
static const char* route_name(void) { return rsx_out_renderer(); }
static const char* route_why(void)
{
    const char* why = NULL;
    rsx_out_renderer_info(&why);
    return why;
}
#else
#define SYNTH_SWITCH 0
extern const rsx_route rsxc_route_null;
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
extern const rsx_route rsxc_route_vk;
#endif
static const rsx_route* g_route;
#define ROUTE(f) g_route->f
static int route_open(const char* name)
{
    if (!strcmp(name, "null")) g_route = &rsxc_route_null;
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
    else if (!strcmp(name, "vulkan")) g_route = &rsxc_route_vk;
#endif
    return g_route != NULL;
}
static const char* route_name(void) { return g_route->name; }
static const char* route_why(void) { return NULL; }
#endif

/* ---------------------------------------------------------------------------
 * The guest's memory map. RSX offsets are local memory (guest 0xC0000000 +
 * offset) unless the IO table maps their megabyte: one megabyte of main
 * memory is mapped, at IO offset IO_OFF, for the main-memory scene.
 * -------------------------------------------------------------------------*/
#define LOCAL_EA    0xC0000000u
#define LOCAL_SIZE  0x0F900000u
#define IO_OFF      0x0E000000u
#define IO_EA       0x30000000u
#define IO_SIZE     0x00100000u

#define DISP_W      1280u
#define DISP_H      720u
#define DISP0       0x00100000u   /* the two display buffers, A8R8G8B8, pitch DISP_W * 4 */
#define DISP1       0x00500000u
#define ZETA        0x00900000u   /* their depth buffer, Z24S8 */
#define RT_A        0x00D00000u   /* offscreen targets, up to 256 x 256 x 4 bytes */
#define RT_Z        0x00D40000u
#define RT_B        0x00D80000u
#define RT_F16      0x00DC0000u   /* 128 x 128 float targets and their Z16 depth */
#define RT_X32      0x00E00000u
#define RT_Z16      0x00E20000u
#define FP_AT(k)    (0x01000000u + (k) * 0x4000u)
#define VB_AT(k)    (0x01100000u + (k) * 0x40000u)
#define IB_AT(k)    (0x01900000u + (k) * 0x10000u)
#define TEX_AT(k)   (0x02000000u + (k) * 0x10000u)

/* SET_SURFACE_FORMAT: colour [4:0], depth [7:5], pitch type [11:8]. */
#define SURF_ARGB8  0x08u
#define SURF_F16X4  0x0Bu
#define SURF_X32    0x0Du
#define SURF_Z16    (1u << 5)
#define SURF_Z24S8  (2u << 5)
#define SURF_PITCH  (1u << 8)

/* GL enums, as the blend, depth, stencil and cull registers hold them. */
enum {
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x300, GL_ONE_MINUS_SRC_COLOR = 0x301, GL_SRC_ALPHA = 0x302,
    GL_ONE_MINUS_SRC_ALPHA = 0x303, GL_DST_ALPHA = 0x304, GL_DST_COLOR = 0x306, GL_CONSTANT_COLOR = 0x8001,
    GL_FUNC_ADD = 0x8006, GL_MIN = 0x8007, GL_MAX = 0x8008, GL_FUNC_SUBTRACT = 0x800A,
    GL_NEVER = 0x200, GL_LESS = 0x201, GL_EQUAL = 0x202, GL_LEQUAL = 0x203, GL_GREATER = 0x204,
    GL_NOTEQUAL = 0x205, GL_GEQUAL = 0x206, GL_ALWAYS = 0x207,
    GL_KEEP = 0x1E00, GL_REPLACE = 0x1E01, GL_INCR = 0x1E02, GL_DECR = 0x1E03, GL_INVERT = 0x150A,
    GL_INCR_WRAP = 0x8507, GL_FRONT = 0x404, GL_BACK = 0x405, GL_CW = 0x900, GL_CCW = 0x901
};

/* ---------------------------------------------------------------------------
 * The capture being written: its sections, and its methods (subchannel 0,
 * so a method's full number is its own).
 * -------------------------------------------------------------------------*/
typedef struct { u8* p; size_t n, cap; } Buf;

static void buf_add(Buf* b, const void* d, size_t n)
{
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 65536;
        while (c < b->n + n) c *= 2;
        u8* q = (u8*)realloc(b->p, c);
        if (!q) { fprintf(stderr, "test_rsx_synth: out of memory\n"); exit(2); }
        b->p = q;
        b->cap = c;
    }
    if (d) memcpy(b->p + b->n, d, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
}
static void buf_u32(Buf* b, u32 v)
{
    const u8 le[4] = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    buf_add(b, le, 4);
}

static Buf g_sec, g_meth;
static rsx_state g_st0;   /* every capture's starting state: rsx_state_init's */

static void sec(const char* tag, const void* d, u32 n)
{
    buf_add(&g_sec, tag, 4);
    buf_u32(&g_sec, n);
    buf_add(&g_sec, d, n);
}
/* Guest bytes at a guest address (an MSEG section). */
static void mem(u32 ea, const void* d, u32 n)
{
    buf_add(&g_sec, "MSEG", 4);
    buf_u32(&g_sec, n + 4);
    buf_u32(&g_sec, ea);
    buf_add(&g_sec, d, n);
}
static void M(u32 method, u32 data)
{
    buf_u32(&g_meth, method);
    buf_u32(&g_meth, data);
}

static void cap_begin(void)
{
    g_sec.n = g_meth.n = 0;
    sec("RSXS", &g_st0, (u32)sizeof g_st0);
    CellGcmConfig cf;
    memset(&cf, 0, sizeof cf);
    cf.localAddress = LOCAL_EA;
    cf.ioAddress = IO_EA;
    cf.localSize = LOCAL_SIZE;
    cf.ioSize = IO_SIZE;
    cf.memoryFrequency = 650000000u;
    cf.coreFrequency = 500000000u;
    sec("CONF", &cf, (u32)sizeof cf);
    /* The offset tables: IOTB is main memory's megabytes to IO, EATB the
     * reverse; 0xFFFF is "not mapped". */
    static u16 io[65536], ea[65536];
    for (u32 i = 0; i < 65536; i++) io[i] = ea[i] = 0xFFFF;
    ea[IO_OFF >> 20] = (u16)(IO_EA >> 20);
    io[IO_EA >> 20] = (u16)(IO_OFF >> 20);
    sec("EATB", ea, (u32)sizeof ea);
    sec("IOTB", io, (u32)sizeof io);
    CellGcmDisplayInfo disp[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
    int dset[CELL_GCM_MAX_DISPLAY_BUFFER_NUM];
    memset(disp, 0, sizeof disp);
    memset(dset, 0, sizeof dset);
    disp[0].offset = DISP0; disp[1].offset = DISP1;
    for (int i = 0; i < 2; i++) { disp[i].pitch = DISP_W * 4; disp[i].width = DISP_W; disp[i].height = DISP_H; dset[i] = 1; }
    const u32 cur = 0;
    sec("DISP", disp, (u32)sizeof disp);
    sec("DSET", dset, (u32)sizeof dset);
    sec("DCUR", &cur, 4);
}

static int cap_write(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "test_rsx_synth: cannot write %s\n", path); return 0; }
    const u32 n = (u32)g_meth.n, z = 0;
    int ok = fwrite("PS3RCAP1", 1, 8, f) == 8 && fwrite(g_sec.p, 1, g_sec.n, f) == g_sec.n &&
             fwrite("METH", 1, 4, f) == 4 && fwrite(&n, 4, 1, f) == 1 && fwrite(g_meth.p, 1, g_meth.n, f) == g_meth.n &&
             fwrite("END ", 1, 4, f) == 4 && fwrite(&z, 4, 1, f) == 1;
    ok = fclose(f) == 0 && ok;
    if (!ok) fprintf(stderr, "test_rsx_synth: cannot write %s\n", path);
    return ok;
}

/* ---------------------------------------------------------------------------
 * The programs: rsx_prog_gen.h's, with fixed seeds. Program k of a kind is
 * the first of its own run of seeds that has an end and decompiles (and, for
 * a fragment program, samples the units it must and no others), so a scene
 * never draws with a program the core drops.
 * -------------------------------------------------------------------------*/
#define PROG_SEED 0x53594E5448ull   /* the scenes' own: streams 20 (vertex) and 21 (fragment) */
enum { VP_INSTR_MAX = 160, FP_BYTES_MAX = 8192, VP_COUNT = 6, FP_COUNT = 10 };
typedef struct { u32 w[4 * VP_INSTR_MAX]; u32 n, base; } Vp;
typedef struct { u8 b[FP_BYTES_MAX]; u32 len, units; } Fp;
static Vp g_vp[VP_COUNT];
static Fp g_fp[FP_COUNT];
static char g_text[262144];

static int vp_make(u32 k, u32 base, Vp* out)
{
    static u8 be[16 * VP_INSTR_MAX];
    for (u32 i = 0; i < 2000; i++) {
        Rng r = rng_for(PROG_SEED, 20, 10000ull * k + i);
        out->n = gen_vp(&r, out->w, VP_INSTR_MAX, base, 1);
        out->base = base;
        for (u32 j = 0; j < out->n * 4; j++) put_be(be + 4 * j, out->w[j]);
        if (rsxc_shader_hlsl(0, be, out->n * 16, base, 0, 0, 0, g_text, sizeof g_text) > 0) return 1;
    }
    fprintf(stderr, "test_rsx_synth: no vertex program %u decompiles\n", k);
    return 0;
}

/* want: units it must sample; allow: units it may. exp32: its export width. */
static int fp_make(u32 k, u32 want, u32 allow, int exp32, Fp* out)
{
    for (u32 i = 0; i < 20000; i++) {
        Rng r = rng_for(PROG_SEED, 21, 100000ull * k + i);
        const u32 len = gen_fp(&r, out->b, FP_BYTES_MAX);
        if (!len || rsx_fp_program_size(out->b, len) == 0) continue;
        const u32 units = rsx_fp_texture_units(out->b, len);
        if ((units & want) != want || (units & ~allow)) continue;
        if (rsxc_shader_hlsl(1, out->b, len, 0, exp32, 0, 0, g_text, sizeof g_text) <= 0) continue;
        out->len = len;
        out->units = units;
        return 1;
    }
    fprintf(stderr, "test_rsx_synth: no fragment program %u fits\n", k);
    return 0;
}

static int programs_make(void)
{
    static const u32 base[VP_COUNT] = { 0, 0, 64, 0, 200, 0 };
    for (u32 k = 0; k < VP_COUNT; k++)
        if (!vp_make(k, base[k], &g_vp[k])) return 0;
    /* Which units each fragment program must and may sample. */
    static const struct { u32 want, allow; int exp32; } f[FP_COUNT] = {
        { 0, 0, 1 },         /* 0 clears */
        { 0, 0, 1 },         /* 1 draws */
        { 0, 0, 0 },         /* 2 states (half exports) */
        { 1, 0xFFFF, 1 },    /* 3-5 textures: every unit has a texture */
        { 2, 0xFFFF, 1 },
        { 0x10, 0xFFFF, 1 },
        { 0, 0, 1 },         /* 6 offscreen: its targets */
        { 1, 0x7, 1 },       /* 7 offscreen: samples them */
        { 0, 0, 1 },         /* 8 zpass */
        { 1, 0x1, 1 },       /* 9 main memory */
    };
    for (u32 k = 0; k < FP_COUNT; k++)
        if (!fp_make(k, f[k].want, f[k].allow, f[k].exp32, &g_fp[k])) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Method helpers
 * -------------------------------------------------------------------------*/
static u32 fu(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void surface(u32 color, u32 pitch, u32 zeta, u32 w, u32 h, u32 fmt, u32 target, u32 color_b)
{
    M(NV4097_SET_CONTEXT_DMA_COLOR_A, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    M(NV4097_SET_CONTEXT_DMA_COLOR_B, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    M(NV4097_SET_CONTEXT_DMA_COLOR_C, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    M(NV4097_SET_CONTEXT_DMA_COLOR_D, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    M(NV4097_SET_CONTEXT_DMA_ZETA, CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER);
    M(NV4097_SET_SURFACE_FORMAT, fmt | SURF_PITCH);
    M(NV4097_SET_SURFACE_PITCH_A, pitch);
    M(NV4097_SET_SURFACE_COLOR_AOFFSET, color);
    M(NV4097_SET_SURFACE_PITCH_B, pitch);
    M(NV4097_SET_SURFACE_COLOR_BOFFSET, color_b);
    M(NV4097_SET_SURFACE_PITCH_C, 64);
    M(NV4097_SET_SURFACE_COLOR_COFFSET, 0);
    M(NV4097_SET_SURFACE_PITCH_D, 64);
    M(NV4097_SET_SURFACE_COLOR_DOFFSET, 0);
    M(NV4097_SET_SURFACE_ZETA_OFFSET, zeta);
    M(NV4097_SET_SURFACE_PITCH_Z, pitch);
    M(NV4097_SET_SURFACE_COLOR_TARGET, target);
    M(NV4097_SET_SURFACE_CLIP_HORIZONTAL, w << 16);
    M(NV4097_SET_SURFACE_CLIP_VERTICAL, h << 16);
}

static void scissor(u32 x, u32 y, u32 w, u32 h)
{
    M(NV4097_SET_SCISSOR_HORIZONTAL, (w << 16) | x);
    M(NV4097_SET_SCISSOR_VERTICAL, (h << 16) | y);
}

/* The viewport over w x h, its transform (window = ndc * scale + offset, z
 * into [0, 1]) and the scissor. */
static void viewport(u32 w, u32 h)
{
    M(NV4097_SET_VIEWPORT_HORIZONTAL, w << 16);
    M(NV4097_SET_VIEWPORT_VERTICAL, h << 16);
    M(NV4097_SET_CLIP_MIN, fu(0.0f));
    M(NV4097_SET_CLIP_MAX, fu(1.0f));
    const float off[4] = { (float)w * 0.5f, (float)h * 0.5f, 0.5f, 0.0f };
    const float scl[4] = { (float)w * 0.5f, -(float)h * 0.5f, 0.5f, 0.0f };
    for (u32 i = 0; i < 4; i++) M(0x0A20 + 4 * i, fu(off[i]));
    for (u32 i = 0; i < 4; i++) M(0x0A30 + 4 * i, fu(scl[i]));
    scissor(0, 0, w, h);
}

static void display(u32 buf)
{
    surface(buf, DISP_W * 4, ZETA, DISP_W, DISP_H, SURF_ARGB8 | SURF_Z24S8, CELL_GCM_SURFACE_TARGET_0, 0);
    viewport(DISP_W, DISP_H);
}

static void clear(u32 mask, u32 argb, u32 zs)
{
    M(NV4097_SET_COLOR_CLEAR_VALUE, argb);
    M(NV4097_SET_ZSTENCIL_CLEAR_VALUE, zs);
    M(NV4097_CLEAR_SURFACE, mask);
}
#define CLEAR_ALL 0xF3u   /* Z, S, R, G, B, A */

/* A frame starts on the display, cleared, and ends with the same clear: the
 * cut that presents it (as a capture's first and last method). */
static void frame_open(u32 buf)  { display(buf); clear(CLEAR_ALL, 0xFF102030u, 0xFFFFFF00u); }
static void frame_close(u32 buf) { display(buf); clear(CLEAR_ALL, 0xFF102030u, 0xFFFFFF00u); }

/* The vertex program into the store at its base slot, and started there;
 * the fragment program at fp (an RSX offset), local or main. */
static void programs(const Vp* vp, u32 fp, int fp_in_main, int exp32)
{
    M(NV4097_SET_TRANSFORM_PROGRAM_LOAD, vp->base);
    for (u32 i = 0; i < vp->n * 4; i++) M(NV4097_SET_TRANSFORM_PROGRAM + 4 * (i & 31), vp->w[i]);
    M(NV4097_SET_TRANSFORM_PROGRAM_START, vp->base);
    M(NV4097_SET_SHADER_PROGRAM, fp | (fp_in_main ? 2u : 1u));
    M(NV4097_SET_SHADER_CONTROL, (exp32 ? CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS : 0u) | (8u << 24));
}

/* n vertex constants from slot, in [-2, 2] (the first four an identity). */
static void constants(u32 slot, u32 n, u64 seed)
{
    Rng r = rng_for(PROG_SEED, 22, seed);
    for (u32 v = 0; v < n; v += 8) {
        M(NV4097_SET_TRANSFORM_CONSTANT_LOAD, slot + v);
        for (u32 j = 0; j < 32 && v + j / 4 < n; j++) {
            const u32 at = (v + j / 4) * 4 + (j & 3);
            const float f = slot == 0 && at < 16 ? (at % 5 == 0 ? 1.0f : 0.0f)
                                                 : (float)((int)rn(&r, 4001) - 2000) / 1000.0f;
            M(NV4097_SET_TRANSFORM_CONSTANT + 4 * j, fu(f));
        }
    }
}

/* One vertex array: type (1 snorm16, 2 float, 3 half, 4 ubyte, 5 s16,
 * 6 11:11:10, 7 ubyte 0-255), components, stride, RSX offset (bit 31: main). */
static void attr(u32 i, u32 type, u32 size, u32 stride, u32 off)
{
    M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * i, type | (size << 4) | (stride << 8));
    M(NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 4 * i, off);
}
static void attrs_off(void)
{
    for (u32 i = 0; i < 16; i++) M(NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 4 * i, 0x2u);   /* size 0: off */
}

/* The common layout, 24 bytes a vertex: float3 position, ubyte4 colour,
 * float2 uv (attributes 0, 3 and 8). */
#define VTX 24u
static void layout_std(u32 off)
{
    attrs_off();
    attr(0, 2, 3, VTX, off);
    attr(3, 4, 4, VTX, off + 12);
    attr(8, 2, 2, VTX, off + 16);
    M(NV4097_SET_VERTEX_ATTRIB_INPUT_MASK, (1u << 0) | (1u << 3) | (1u << 8));
    M(NV4097_SET_VERTEX_ATTRIB_OUTPUT_MASK, 0x0000FFFFu);
}
static void put_bef(u8* p, float f) { put_be(p, fu(f)); }
static float rf(Rng* r, float lo, float hi) { return lo + (hi - lo) * (float)rn(r, 10001) / 10000.0f; }
static void verts(u32 ea, u32 n, u64 seed)
{
    u8* p = (u8*)calloc(n, VTX);
    if (!p) exit(2);
    Rng r = rng_for(PROG_SEED, 23, seed);
    for (u32 i = 0; i < n; i++) {
        u8* v = p + i * VTX;
        put_bef(v + 0, rf(&r, -1.0f, 1.0f));
        put_bef(v + 4, rf(&r, -1.0f, 1.0f));
        put_bef(v + 8, rf(&r, 0.0f, 1.0f));
        put_be(v + 12, (u32)rnext(&r));
        put_bef(v + 16, rf(&r, -0.5f, 1.5f));
        put_bef(v + 20, rf(&r, -0.5f, 1.5f));
    }
    mem(ea, p, n * VTX);
    free(p);
}
static void bytes(u32 ea, u32 n, u64 seed)
{
    u8* p = (u8*)malloc(n);
    if (!p) exit(2);
    Rng r = rng_for(PROG_SEED, 24, seed);
    for (u32 i = 0; i < n; i++) p[i] = (u8)rnext(&r);
    mem(ea, p, n);
    free(p);
}
/* n big-endian indices (u16 or u32) at ea: a strip's run with jumps back, below
 * nv; restart (if not 0) every so often. */
static void indices(u32 ea, u32 n, int w16, u32 nv, u32 restart, u64 seed)
{
    u8* p = (u8*)calloc(n, 4);
    if (!p) exit(2);
    Rng r = rng_for(PROG_SEED, 25, seed);
    u32 at = 0;
    for (u32 i = 0; i < n; i++) {
        u32 x = at;
        if (restart && i && chance(&r, 24)) x = restart;
        else at = chance(&r, 6) ? rn(&r, nv) : (at + 1) % nv;
        if (w16) { p[2 * i] = (u8)(x >> 8); p[2 * i + 1] = (u8)x; }
        else put_be(p + 4 * i, x);
    }
    mem(ea, p, n * (w16 ? 2u : 4u));
    free(p);
}

static void draw_arrays(u32 prim, u32 first, u32 count)
{
    M(NV4097_SET_BEGIN_END, prim);
    while (count) {   /* at most 256 vertices a method, as the RSX takes them */
        const u32 n = count < 256 ? count : 256;
        M(NV4097_DRAW_ARRAYS, ((n - 1) << 24) | first);
        first += n;
        count -= n;
    }
    M(NV4097_SET_BEGIN_END, 0);
}
static void draw_indexed(u32 prim, u32 off, int in_main, int w16, u32 first, u32 count)
{
    M(NV4097_SET_INDEX_ARRAY_ADDRESS, off);
    M(NV4097_SET_INDEX_ARRAY_DMA, (in_main ? 1u : 0u) | (w16 ? 0x10u : 0u));
    M(NV4097_SET_BEGIN_END, prim);
    while (count) {
        const u32 n = count < 256 ? count : 256;
        M(NV4097_DRAW_INDEX_ARRAY, ((n - 1) << 24) | first);
        first += n;
        count -= n;
    }
    M(NV4097_SET_BEGIN_END, 0);
}

/* A texture unit. fmt: the format byte (bit 0x20: linear, else swizzled);
 * address: TEXTURE_ADDRESS (wrap S [3:0], T [11:8], R [19:16], zfunc [31:28]);
 * filter: TEXTURE_FILTER (min [18:16], mag [26:24]). */
static void texture(u32 unit, u32 off, int in_main, u32 fmt, u32 w, u32 h, u32 mips, int cube, u32 pitch,
                    u32 address, u32 filter, u32 aniso)
{
    const u32 b = NV4097_SET_TEXTURE_OFFSET + unit * 0x20;
    M(b + 0x00, off);
    M(b + 0x04, (mips << 16) | (fmt << 8) | (2u << 4) | (cube ? 4u : 0u) | (in_main ? 2u : 1u));
    M(b + 0x08, address);
    M(b + 0x0C, 0x80000000u | ((12u * 256u) << 7) | (aniso << 4));   /* on, LOD 0..12 */
    M(b + 0x10, 0x0000AAE4u);                                         /* the identity remap */
    M(b + 0x14, filter);
    M(b + 0x18, (w << 16) | h);
    M(b + 0x1C, 0xFF804020u);
    M(NV4097_SET_TEXTURE_CONTROL3 + 4 * unit, pitch);
}
#define WRAP3(s, t, r) ((s) | ((t) << 8) | ((r) << 16))
#define FILT(min, mag) (((min) << 16) | ((mag) << 24))

/* The default states, for a scene to start from. */
static void states_default(void)
{
    M(NV4097_SET_BLEND_ENABLE, 0);
    M(NV4097_SET_DEPTH_TEST_ENABLE, 1);
    M(NV4097_SET_DEPTH_FUNC, GL_LESS);
    M(NV4097_SET_DEPTH_MASK, 1);
    M(NV4097_SET_STENCIL_TEST_ENABLE, 0);
    M(NV4097_SET_CULL_FACE_ENABLE, 0);
    M(NV4097_SET_COLOR_MASK, 0x01010101u);
    M(NV4097_SET_ALPHA_TEST_ENABLE, 0);
}

/* ---------------------------------------------------------------------------
 * The scenes. Each builds frame k of itself into the capture begun.
 * -------------------------------------------------------------------------*/
typedef struct {
    const char* name;
    int frames;
    void (*build)(int k);
    const char* needs;   /* records its frames must hold: "name:count" (at least), space-separated */
} Scene;

/* Clears: whole, colour, depth, stencil, masked and scissored, on the
 * display and on offscreen targets (ARGB8 with Z24S8, FP16 and X32 with
 * Z16); and a draw on each. */
static void scene_clears(int k)
{
    (void)k;
    frame_open(DISP0);
    mem(LOCAL_EA + FP_AT(0), g_fp[0].b, g_fp[0].len);
    verts(LOCAL_EA + VB_AT(0), 12, 0);
    programs(&g_vp[0], FP_AT(0), 0, 1);
    constants(0, 16, 0);
    layout_std(VB_AT(0));
    states_default();
    clear(0xF0, 0xFF808080u, 0);              /* colour */
    clear(0x01, 0, 0x80000055u);              /* depth */
    clear(0x02, 0, 0xFFFFFF7Fu);              /* stencil */
    M(NV4097_SET_COLOR_MASK, 0x01000100u);    /* A and G only */
    clear(0xF0, 0xFF00FF00u, 0);
    M(NV4097_SET_COLOR_MASK, 0x01010101u);
    scissor(100, 100, 640, 360);
    clear(CLEAR_ALL, 0xFF0000FFu, 0x00000000u);
    scissor(0, 0, DISP_W, DISP_H);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 3);
    surface(RT_A, 128 * 4, RT_Z, 128, 128, SURF_ARGB8 | SURF_Z24S8, CELL_GCM_SURFACE_TARGET_0, 0);
    viewport(128, 128);
    clear(CLEAR_ALL, 0xFF00FF00u, 0xFFFFFF00u);
    clear(0x90, 0x80FF0000u, 0);              /* R and A */
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 3, 6);
    surface(RT_F16, 128 * 8, RT_Z16, 128, 128, SURF_F16X4 | SURF_Z16, CELL_GCM_SURFACE_TARGET_0, 0);
    clear(CLEAR_ALL, 0x40404040u, 0xFFFF0000u);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 6, 3);
    surface(RT_X32, 128 * 4, RT_Z16, 128, 128, SURF_X32 | SURF_Z16, CELL_GCM_SURFACE_TARGET_0, 0);
    clear(0xF1, 0x3F800000u, 0x00008000u);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 9, 3);
    frame_close(DISP0);
}

/* Draws: arrays and indexed (u16, u32, main and local below), lists,
 * strips, fans and quads, a stream split over several methods in one
 * begin/end, a primitive restart, and lines and points (not drawn by the
 * VP path). */
static void scene_draws(int k)
{
    (void)k;
    frame_open(DISP0);
    mem(LOCAL_EA + FP_AT(1), g_fp[1].b, g_fp[1].len);
    verts(LOCAL_EA + VB_AT(1), 600, 1);
    indices(LOCAL_EA + IB_AT(0), 96, 1, 600, 0, 0);
    indices(LOCAL_EA + IB_AT(1), 700, 0, 600, 0, 1);
    indices(LOCAL_EA + IB_AT(2), 64, 1, 600, 0xFFFF, 2);
    programs(&g_vp[1], FP_AT(1), 0, 1);
    constants(0, 32, 1);
    layout_std(VB_AT(1));
    states_default();
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 12);
    draw_arrays(RSX_PRIMITIVE_TRIANGLE_STRIP, 12, 10);
    draw_arrays(RSX_PRIMITIVE_TRIANGLE_FAN, 22, 8);
    draw_arrays(RSX_PRIMITIVE_QUADS, 30, 16);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 46, 300);   /* 256 + 44 in one begin/end */
    draw_arrays(RSX_PRIMITIVE_LINES, 0, 8);
    draw_arrays(RSX_PRIMITIVE_POINTS, 0, 4);
    draw_indexed(RSX_PRIMITIVE_TRIANGLES, IB_AT(0), 0, 1, 0, 48);
    draw_indexed(RSX_PRIMITIVE_QUADS, IB_AT(0), 0, 1, 48, 32);
    draw_indexed(RSX_PRIMITIVE_TRIANGLE_STRIP, IB_AT(1), 0, 0, 0, 700);   /* 256 + 256 + 188 */
    draw_indexed(RSX_PRIMITIVE_TRIANGLE_FAN, IB_AT(0), 0, 1, 0, 20);
    M(NV4097_SET_RESTART_INDEX_ENABLE, 1);
    M(NV4097_SET_RESTART_INDEX, 0xFFFF);
    draw_indexed(RSX_PRIMITIVE_TRIANGLE_STRIP, IB_AT(2), 0, 1, 0, 64);
    M(NV4097_SET_RESTART_INDEX_ENABLE, 0);
    /* Other vertex formats: half positions, snorm16 and s16 normals, ubyte 0-255. */
    attrs_off();
    attr(0, 3, 4, 16, VB_AT(1));
    attr(2, 1, 3, 16, VB_AT(1) + 8);
    attr(3, 7, 4, 16, VB_AT(1) + 12);
    attr(9, 5, 2, 16, VB_AT(1) + 4);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 30);
    draw_indexed(RSX_PRIMITIVE_TRIANGLE_STRIP, IB_AT(1), 0, 0, 100, 60);
    frame_close(DISP0);
}

/* States: blend factors and equations, a blend colour, depth tests and
 * writes, stencil tests and operations, culling, colour masks, the alpha
 * test, half exports. One draw each. */
static void scene_states(int k)
{
    (void)k;
    frame_open(DISP0);
    mem(LOCAL_EA + FP_AT(2), g_fp[2].b, g_fp[2].len);
    verts(LOCAL_EA + VB_AT(2), 48, 2);
    programs(&g_vp[2], FP_AT(2), 0, 0);
    constants(0, 24, 2);
    layout_std(VB_AT(2));
    states_default();
    u32 v = 0;
#define DRAW3() (draw_arrays(RSX_PRIMITIVE_TRIANGLES, v, 3), v += 3)
    DRAW3();
    M(NV4097_SET_BLEND_ENABLE, 1);
    M(NV4097_SET_BLEND_FUNC_SFACTOR, (GL_SRC_ALPHA << 16) | GL_SRC_ALPHA);
    M(NV4097_SET_BLEND_FUNC_DFACTOR, (GL_ONE_MINUS_SRC_ALPHA << 16) | GL_ONE_MINUS_SRC_ALPHA);
    M(NV4097_SET_BLEND_EQUATION, (GL_FUNC_ADD << 16) | GL_FUNC_ADD);
    DRAW3();
    M(NV4097_SET_BLEND_FUNC_SFACTOR, (GL_ZERO << 16) | GL_ONE);
    M(NV4097_SET_BLEND_FUNC_DFACTOR, (GL_ONE << 16) | GL_ONE);
    M(NV4097_SET_BLEND_EQUATION, (GL_FUNC_ADD << 16) | GL_MAX);
    DRAW3();
    M(NV4097_SET_BLEND_COLOR, 0x80402010u);
    M(NV4097_SET_BLEND_FUNC_SFACTOR, (GL_DST_ALPHA << 16) | GL_CONSTANT_COLOR);
    M(NV4097_SET_BLEND_FUNC_DFACTOR, (GL_ZERO << 16) | GL_ONE_MINUS_SRC_COLOR);
    M(NV4097_SET_BLEND_EQUATION, (GL_MIN << 16) | GL_FUNC_SUBTRACT);
    DRAW3();
    M(NV4097_SET_BLEND_ENABLE, 0);
    M(NV4097_SET_DEPTH_FUNC, GL_GEQUAL);
    M(NV4097_SET_DEPTH_MASK, 0);
    DRAW3();
    M(NV4097_SET_DEPTH_TEST_ENABLE, 0);
    DRAW3();
    M(NV4097_SET_DEPTH_TEST_ENABLE, 1);
    M(NV4097_SET_DEPTH_FUNC, GL_LEQUAL);
    M(NV4097_SET_DEPTH_MASK, 1);
    M(NV4097_SET_STENCIL_TEST_ENABLE, 1);
    M(NV4097_SET_STENCIL_FUNC, GL_ALWAYS);
    M(NV4097_SET_STENCIL_FUNC_REF, 1);
    M(NV4097_SET_STENCIL_FUNC_MASK, 0xFF);
    M(NV4097_SET_STENCIL_OP_FAIL, GL_KEEP);
    M(NV4097_SET_STENCIL_OP_ZFAIL, GL_KEEP);
    M(NV4097_SET_STENCIL_OP_ZPASS, GL_REPLACE);
    DRAW3();
    M(NV4097_SET_STENCIL_FUNC, GL_EQUAL);
    M(NV4097_SET_STENCIL_FUNC_MASK, 0x0F);
    M(NV4097_SET_STENCIL_OP_FAIL, GL_INCR);
    M(NV4097_SET_STENCIL_OP_ZFAIL, GL_DECR);
    M(NV4097_SET_STENCIL_OP_ZPASS, GL_INVERT);
    DRAW3();
    M(NV4097_SET_STENCIL_TEST_ENABLE, 0);
    M(NV4097_SET_CULL_FACE_ENABLE, 1);
    M(NV4097_SET_CULL_FACE, GL_FRONT);
    M(NV4097_SET_FRONT_FACE, GL_CW);
    DRAW3();
    M(NV4097_SET_CULL_FACE, GL_BACK);
    M(NV4097_SET_FRONT_FACE, GL_CCW);
    DRAW3();
    M(NV4097_SET_CULL_FACE_ENABLE, 0);
    M(NV4097_SET_COLOR_MASK, 0x00010100u);    /* R and G */
    DRAW3();
    M(NV4097_SET_COLOR_MASK, 0x01010101u);
    M(NV4097_SET_ALPHA_TEST_ENABLE, 1);
    M(NV4097_SET_ALPHA_FUNC, GL_GREATER);
    M(NV4097_SET_ALPHA_REF, 0x80);
    DRAW3();
    M(NV4097_SET_ALPHA_TEST_ENABLE, 0);
    DRAW3();
#undef DRAW3
    frame_close(DISP0);
}

/* Textures on all 16 units, three fragment programs sampling them: linear
 * and swizzled, ARGB8, DXT1/3/5, 565, B8, 4444, 1555, G8B8, D8R8G8B8, float
 * formats, mipmaps and a cube; every wrap mode, filters and anisotropy. */
typedef struct { u32 fmt, w, h, mips, cube, bpp; } TexDesc;   /* bpp 0: block compressed */
static const TexDesc k_tex[16] = {
    { 0xA5, 64, 64, 1, 0, 4 },  { 0x85, 64, 32, 1, 0, 4 },  { 0x86, 64, 64, 1, 0, 0 },  { 0x88, 32, 32, 6, 0, 0 },
    { 0x84, 32, 32, 1, 0, 2 },  { 0xA1, 16, 16, 1, 0, 1 },  { 0xA3, 32, 16, 1, 0, 2 },  { 0x85, 16, 16, 1, 1, 4 },
    { 0x85, 64, 64, 7, 0, 4 },  { 0xBC, 16, 16, 1, 0, 4 },  { 0xBA, 16, 16, 1, 0, 8 },  { 0x87, 16, 16, 1, 0, 0 },
    { 0xAB, 16, 16, 1, 0, 2 },  { 0x82, 16, 16, 1, 0, 2 },  { 0x9E, 16, 16, 1, 0, 4 },  { 0xBF, 16, 16, 1, 0, 4 },
};
static void textures_bind(u32 at0, int in_main)
{
    for (u32 u = 0; u < 16; u++) {
        const TexDesc* t = &k_tex[u];
        const u32 wrap = 1 + u % 5, filt_min = 1 + u % 6, filt_mag = u & 1 ? 2 : 1;
        texture(u, at0 + u * 0x10000u, in_main, t->fmt, t->w, t->h, t->mips, (int)t->cube,
                (t->fmt & 0x20) ? t->w * t->bpp : 0, WRAP3(wrap, 1 + (u + 2) % 5, 1), FILT(filt_min, filt_mag),
                u % 4 == 3 ? 4 : 0);
    }
}
static void scene_textures(int k)
{
    (void)k;
    frame_open(DISP0);
    for (u32 i = 3; i <= 5; i++) mem(LOCAL_EA + FP_AT(i), g_fp[i].b, g_fp[i].len);
    for (u32 u = 0; u < 16; u++) bytes(LOCAL_EA + TEX_AT(u), 0x10000u, 100 + u);
    verts(LOCAL_EA + VB_AT(3), 24, 3);
    programs(&g_vp[3], FP_AT(3), 0, 1);
    constants(0, 16, 3);
    layout_std(VB_AT(3));
    states_default();
    textures_bind(TEX_AT(0), 0);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 6);
    M(NV4097_SET_SHADER_PROGRAM, FP_AT(4) | 1u);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 6, 6);
    /* Unit 0 again with other sampling, then the third program. */
    texture(0, TEX_AT(0), 0, k_tex[0].fmt, 64, 64, 1, 0, 256, WRAP3(4, 4, 4), FILT(2, 2), 0);
    M(NV4097_SET_SHADER_PROGRAM, FP_AT(5) | 1u);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 12, 6);
    M(NV4097_SET_SHADER_PROGRAM, FP_AT(3) | 1u);
    draw_arrays(RSX_PRIMITIVE_QUADS, 0, 8);
    frame_close(DISP0);
}

/* An offscreen pass into two colour targets and a depth target (MRT), then
 * a display draw sampling both colour targets and the depth as a shadow map.
 * Two frames: the second finds the targets made. */
static void scene_offscreen(int k)
{
    frame_open(DISP0);
    mem(LOCAL_EA + FP_AT(6), g_fp[6].b, g_fp[6].len);
    mem(LOCAL_EA + FP_AT(7), g_fp[7].b, g_fp[7].len);
    verts(LOCAL_EA + VB_AT(4), 30, 4);
    programs(&g_vp[4], FP_AT(6), 0, 1);
    constants(0, 16, 4 + (u64)k);
    layout_std(VB_AT(4));
    states_default();
    surface(RT_A, 256 * 4, RT_Z, 256, 256, SURF_ARGB8 | SURF_Z24S8, CELL_GCM_SURFACE_TARGET_MRT1, RT_B);
    viewport(256, 256);
    clear(CLEAR_ALL, 0x00000000u, 0xFFFFFF00u);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 12);
    draw_arrays(RSX_PRIMITIVE_TRIANGLE_STRIP, 12, 8);
    display(DISP0);
    M(NV4097_SET_SHADER_PROGRAM, FP_AT(7) | 1u);
    texture(0, RT_A, 0, 0xA5, 256, 256, 1, 0, 256 * 4, WRAP3(3, 3, 3), FILT(2, 2), 0);
    texture(1, RT_Z, 0, 0xB0, 256, 256, 1, 0, 256 * 4, WRAP3(3, 3, 3) | (3u << 28), FILT(2, 2), 0);   /* LEQUAL */
    texture(2, RT_B, 0, 0xA5, 256, 256, 1, 0, 256 * 4, WRAP3(1, 1, 1), FILT(1, 1), 0);
    draw_arrays(RSX_PRIMITIVE_QUADS, 20, 4);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 3);
    frame_close(DISP0);
}

/* Occlusion queries: counting on over three draws and a report, off for a
 * draw and a report, on again for an indexed draw. Two frames (a report
 * lands a frame later). */
static void scene_zpass(int k)
{
    (void)k;
    frame_open(DISP0);
    mem(LOCAL_EA + FP_AT(8), g_fp[8].b, g_fp[8].len);
    verts(LOCAL_EA + VB_AT(5), 64, 5);
    indices(LOCAL_EA + IB_AT(3), 48, 1, 64, 0, 3);
    programs(&g_vp[5], FP_AT(8), 0, 1);
    constants(0, 16, 5);
    layout_std(VB_AT(5));
    states_default();
    M(0x17C8, 1);                      /* CLEAR_REPORT_VALUE: ZPASS_PIXEL_CNT */
    M(0x17CC, 1);                      /* SET_ZPASS_PIXEL_COUNT_ENABLE */
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 6);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 6, 6);
    draw_arrays(RSX_PRIMITIVE_TRIANGLE_STRIP, 12, 6);
    M(0x1800, (1u << 24) | (5u * 16u)); /* GET_REPORT: ZPASS into report 5 */
    M(0x17CC, 0);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 18, 6);
    M(0x1800, (1u << 24) | (6u * 16u));
    M(0x17C8, 1);
    M(0x17CC, 1);
    draw_indexed(RSX_PRIMITIVE_TRIANGLES, IB_AT(3), 0, 1, 0, 48);
    M(0x1800, (1u << 24) | (7u * 16u));
    M(0x17CC, 0);
    frame_close(DISP0);
}

/* Four frames in a row, on the two display buffers in turn: the constants
 * change every frame, a texture's bytes from the third, another texture's
 * size and the offscreen target's size in the fourth (both made again, the
 * old ones to the grave), the number of draws and vertices each frame (the
 * rings), the blend in the third (a new pipeline) and back in the fourth. */
static void scene_frames(int k)
{
    const u32 buf = (k & 1) ? DISP1 : DISP0;
    frame_open(buf);
    mem(LOCAL_EA + FP_AT(3), g_fp[3].b, g_fp[3].len);
    mem(LOCAL_EA + FP_AT(6), g_fp[6].b, g_fp[6].len);
    verts(LOCAL_EA + VB_AT(6), 4000, 6);
    indices(LOCAL_EA + IB_AT(4), 3000, 0, 4000, 0, 4);
    bytes(LOCAL_EA + TEX_AT(20), 0x4000u, k >= 2 ? 200 + (u64)k : 200);
    bytes(LOCAL_EA + TEX_AT(21), 0x4000u, 201);
    programs(&g_vp[0], FP_AT(6), 0, 1);
    constants(0, 40, 600 + (u64)k);
    layout_std(VB_AT(6));
    states_default();
    const u32 rw = k == 3 ? 256u : 128u;
    surface(RT_A, rw * 4, RT_Z, rw, 128, SURF_ARGB8 | SURF_Z24S8, CELL_GCM_SURFACE_TARGET_0, 0);
    viewport(rw, 128);
    clear(CLEAR_ALL, 0, 0xFFFFFF00u);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 30 + 30 * (u32)k);
    display(buf);
    M(NV4097_SET_SHADER_PROGRAM, FP_AT(3) | 1u);
    texture(0, TEX_AT(20), 0, 0xA5, 32, 32, 1, 0, 128, WRAP3(1, 1, 1), FILT(2, 2), 0);
    if (k == 3) texture(1, TEX_AT(21), 0, 0xA5, 64, 16, 1, 0, 256, WRAP3(1, 1, 1), FILT(2, 2), 0);
    else        texture(1, TEX_AT(21), 0, 0xA5, 32, 32, 1, 0, 128, WRAP3(1, 1, 1), FILT(2, 2), 0);
    texture(2, RT_A, 0, 0xA5, rw, 128, 1, 0, rw * 4, WRAP3(3, 3, 3), FILT(2, 2), 0);
    if (k == 2) {
        M(NV4097_SET_BLEND_ENABLE, 1);
        M(NV4097_SET_BLEND_FUNC_SFACTOR, (GL_ONE << 16) | GL_SRC_ALPHA);
        M(NV4097_SET_BLEND_FUNC_DFACTOR, (GL_ZERO << 16) | GL_ONE_MINUS_SRC_ALPHA);
    }
    for (u32 d = 0; d < 4 + 3 * (u32)k; d++) draw_arrays(RSX_PRIMITIVE_TRIANGLES, 3 * d, 3);
    draw_indexed(RSX_PRIMITIVE_TRIANGLE_STRIP, IB_AT(4), 0, 0, 0, 1000 + 600 * (u32)k);
    frame_close(buf);
}

/* Main memory: the fragment program, the vertices, the indices and a
 * texture in the IO-mapped megabyte (location bits and offset bit 31). */
static void scene_mainmem(int k)
{
    (void)k;
    frame_open(DISP0);
    const u32 fp = 0x1000u, vb = 0x10000u, ib = 0x30000u, tx = 0x40000u;
    mem(IO_EA + fp, g_fp[9].b, g_fp[9].len);
    verts(IO_EA + vb, 200, 7);
    indices(IO_EA + ib, 300, 1, 200, 0, 5);
    bytes(IO_EA + tx, 0x4000u, 300);
    programs(&g_vp[3], IO_OFF + fp, 1, 1);
    constants(0, 16, 7);
    attrs_off();
    attr(0, 2, 3, VTX, 0x80000000u | (IO_OFF + vb));
    attr(3, 4, 4, VTX, 0x80000000u | (IO_OFF + vb + 12));
    attr(8, 2, 2, VTX, 0x80000000u | (IO_OFF + vb + 16));
    states_default();
    texture(0, IO_OFF + tx, 1, 0xA5, 32, 32, 1, 0, 128, WRAP3(1, 1, 1), FILT(2, 2), 0);
    draw_arrays(RSX_PRIMITIVE_TRIANGLES, 0, 30);
    draw_indexed(RSX_PRIMITIVE_TRIANGLES, IO_OFF + ib, 1, 1, 0, 90);
    draw_indexed(RSX_PRIMITIVE_TRIANGLE_STRIP, IO_OFF + ib, 1, 1, 90, 200);
    frame_close(DISP0);
}

/* The needs count the first pass's frames of the scene. Every frame has
 * one draw more than the scene's: the present's copy to the swap chain.
 * The display's colour clears are the frame's clear colour, not records. */
static const Scene k_scene[] = {
    { "clears",    1, scene_clears,    "clear_color:5 clear_depth:8 draw:5" },
    { "draws",     1, scene_draws,     "draw:8 draw_indexed:4" },
    { "states",    1, scene_states,    "draw:14 set_pipeline:13" },
    { "textures",  1, scene_textures,  "copy_buf_to_tex:16 texture_create:13 table_write_sampler:16 draw:5" },
    { "offscreen", 2, scene_offscreen, "texture_create:3 pass_begin:12 draw:10" },
    { "zpass",     2, scene_zpass,     "query_begin:8 query_end:8 query_resolve:2" },
    { "frames",    4, scene_frames,    "draw:42 draw_indexed:4 copy_buf_to_tex:5 texture_destroy:1" },
    { "mainmem",   1, scene_mainmem,   "draw:3 draw_indexed:1 copy_buf_to_tex:1" },
};
enum { NSCENES = sizeof k_scene / sizeof k_scene[0], NPASSES = 2 };

static int frames_total(void)
{
    int n = 0;
    for (int s = 0; s < NSCENES; s++) n += k_scene[s].frames;
    return n;
}

static void cap_path(char* out, size_t cap, const char* dir, int s, int k)
{
    snprintf(out, cap, "%s/%02d_%s_%d.rcap", dir, s, k_scene[s].name, k);
}

/* Every scene's frames, as captures in dir. */
static int caps_write(const char* dir)
{
    rsx_state_init(&g_st0);
    dir_make(dir);
    if (!programs_make()) return 0;
    for (int s = 0; s < NSCENES; s++)
        for (int k = 0; k < k_scene[s].frames; k++) {
            char path[MAX_PATH];
            cap_path(path, sizeof path, dir, s, k);
            cap_begin();
            k_scene[s].build(k);
            if (!cap_write(path)) return 0;
        }
    return 1;
}

/* ---------------------------------------------------------------------------
 * The child: every capture, twice over, through the null route (or the
 * Vulkan route, RSX_BACKEND=vulkan).
 * -------------------------------------------------------------------------*/
static int play(const char* path)
{
    uint32_t* m = NULL;
    const int n = cellGcm_replay_load(path, &m);
    if (n < 0) { fprintf(stderr, "test_rsx_synth: cannot read %s\n", path); return 0; }
    cellGcm_replay_reset();
    for (int i = 0; i < n; i++) cellGcm_replay_method(m[2 * i], m[2 * i + 1]);
    free(m);
    ROUTE(backend_present)();
    ROUTE(backend_pump_messages)();
    return 1;
}

static int child_main(const char* dir)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* in order with the backend's stderr */
    const char* be = getenv("RSX_BACKEND");
    const char* const want = be && !strcmp(be, "vulkan") ? "vulkan" : be && !strcmp(be, "hal-d3d12") ? "hal-d3d12"
                                                                                                     : "null";
    env_set("RSX_BACKEND", want);
    if (!getenv("RSX_PRESENT")) env_set("RSX_PRESENT", "1");
    if (!route_open(want)) { fprintf(stderr, "test_rsx_synth: no %s route in this build\n", want); return 3; }
    char caps[MAX_PATH];
    dir_make(dir);
    snprintf(caps, sizeof caps, "%s/caps", dir);
    if (!caps_write(caps)) return 3;
    if (!vm_reserve()) { fprintf(stderr, "test_rsx_synth: cannot reserve the guest space\n"); return 3; }
    /* SYNTH_ROUTE: the route the start must end on, when not the one asked
     * for (--fallback: d3d12). */
    const char* const end = getenv("SYNTH_ROUTE") ? getenv("SYNTH_ROUTE") : want;
    if (ROUTE(backend_init)(DISP_W, DISP_H, "test_rsx_synth") != 0 || strcmp(route_name(), end)) {
        fprintf(stderr, "test_rsx_synth: the %s route did not start\n", end);
        return 3;
    }
    const char* const why = route_why();
    printf("[synth] route %s%s%s\n", route_name(), why ? ": " : "", why ? why : "");
    /* --stores: pipeline index rows to hand over (RSX_SYNTH_ROWS, a rows
     * file as rsx_replay's RSX_REPLAY_INDEX takes), and RSX_SYNTH_PLAY=0: no
     * frames, only the start's builds (with PS3_PSO_WAIT=1). */
    if (getenv("RSX_SYNTH_ROWS")) {
        FILE* f = fopen(getenv("RSX_SYNTH_ROWS"), "rb");
        if (!f) { fprintf(stderr, "test_rsx_synth: cannot read %s\n", getenv("RSX_SYNTH_ROWS")); return 3; }
        char line[256];
        unsigned rows = 0;
        unsigned long long hash = 0;
        while (fgets(line, sizeof line, f)) {
            int fetch, clamp, nrt;
            unsigned long long vk, pk;
            unsigned blend, rtfmt, cmask, cull, depth;
            if (sscanf(line, "add %d %llx %d %llx %x %d %x %x %x %x", &fetch, &vk, &clamp, &pk, &blend, &nrt, &rtfmt,
                       &cmask, &cull, &depth) == 10) {
                ROUTE(pso_index_add)(fetch, vk, clamp, pk, blend, nrt, rtfmt, cmask, cull, depth);
                rows++;
            } else
                (void)sscanf(line, "ready %llx", &hash);
        }
        fclose(f);
        ROUTE(pso_index_ready)(hash);
        printf("[synth] rows %u\n", rows);
    }
    const int nplay = getenv("RSX_SYNTH_PLAY") && !atoi(getenv("RSX_SYNTH_PLAY")) ? 0 : NPASSES;
    for (int p = 1; p <= nplay; p++)
        for (int s = 0; s < NSCENES; s++)
            for (int k = 0; k < k_scene[s].frames; k++) {
                char path[MAX_PATH];
                cap_path(path, sizeof path, caps, s, k);
                printf("[synth] play %d %d %d\n", p, s, k);
                if (!play(path)) return 3;
            }
    printf("[synth] shutdown\n");
    ROUTE(backend_shutdown)();
#if defined(PS3RECOMP_RSX_HAL_VULKAN)
    if (!strcmp(want, "vulkan")) printf("[synth] validation messages %u\n", (unsigned)gpu_vk_n_validation_all);
#endif
    printf("[synth] done\n");
    fflush(stdout);
    fflush(stderr);
    /* The backend's threads go with the process, as rsx_replay's do. */
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#else
    _exit(0);
#endif
    return 0;
}

/* ---------------------------------------------------------------------------
 * The test: two children, their logs and the first one's dump.
 * -------------------------------------------------------------------------*/
#ifdef _WIN32
static int env_cmp(const void* a, const void* b) { return _stricmp(*(const char* const*)a, *(const char* const*)b); }

/* The child's environment: the system's variables from ours (with what finds
 * the Vulkan driver and layers, such as the SDK's VK_ADD_LAYER_PATH), then
 * extra (NAME=VALUE each). */
static char* env_block(const char* const* extra, int nextra)
{
    static const char* const keep[] = { "SystemRoot", "SystemDrive", "windir", "PATH", "PATHEXT", "TEMP", "TMP",
                                        "ComSpec", "OS", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE",
                                        "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH",
                                        "VK_LOADER_LAYERS_DISABLE" };
    enum { NKEEP = sizeof keep / sizeof keep[0] };
    char* items[NKEEP + 64];
    int n = 0;
    for (int i = 0; i < NKEEP; i++) {
        char v[32768];
        const DWORD l = GetEnvironmentVariableA(keep[i], v, sizeof v);
        if (!l || l >= sizeof v) continue;
        const size_t sz = strlen(keep[i]) + 1 + l + 1;
        items[n] = (char*)malloc(sz);
        if (!items[n]) exit(2);
        snprintf(items[n++], sz, "%s=%s", keep[i], v);
    }
    for (int i = 0; i < nextra && n < NKEEP + 64; i++) items[n++] = _strdup(extra[i]);
    qsort(items, (size_t)n, sizeof *items, env_cmp);
    size_t total = 1;
    for (int i = 0; i < n; i++) total += strlen(items[i]) + 1;
    char* b = (char*)malloc(total);
    if (!b) exit(2);
    char* p = b;
    for (int i = 0; i < n; i++) {
        const size_t l = strlen(items[i]) + 1;
        memcpy(p, items[i], l);
        p += l;
        free(items[i]);
    }
    *p = 0;
    return b;
}

/* Runs the child into dir (its log at dir/log.txt); its exit code, or -1. */
static int run_child(const char* exe, const char* dir, const char* const* extra, int nextra)
{
    char log[MAX_PATH], cmd[3 * MAX_PATH];
    dir_make(dir);
    snprintf(log, sizeof log, "%s/log.txt", dir);
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE h = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("[synth] cannot write %s\n", log); return -1; }
    snprintf(cmd, sizeof cmd, "\"%s\" --run \"%s\"", exe, dir);
    char* env = env_block(extra, nextra);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = si.hStdError = h;
    int code = -1;
    if (CreateProcessA(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, env, NULL, &si, &pi)) {
        /* Ten minutes is far more than the scenes take; then the child is stuck. */
        if (WaitForSingleObject(pi.hProcess, 600000) != WAIT_OBJECT_0) {
            printf("[synth] the child in %s did not finish in 10 minutes: stopped\n", dir);
            TerminateProcess(pi.hProcess, 99);
            WaitForSingleObject(pi.hProcess, 10000);
        }
        DWORD c = 0;
        if (GetExitCodeProcess(pi.hProcess, &c)) code = (int)c;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else
        printf("[synth] cannot start %s (error %lu)\n", exe, GetLastError());
    free(env);
    CloseHandle(h);
    return code;
}
#else
/* The child's environment: the system's variables from ours (with what finds
 * the display and the Vulkan driver), then extra (NAME=VALUE each). */
static char** env_list(const char* const* extra, int nextra)
{
    static const char* const keep[] = { "PATH", "HOME", "TMPDIR", "LANG", "LD_LIBRARY_PATH", "DISPLAY",
                                        "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XAUTHORITY", "VK_ICD_FILENAMES",
                                        "VK_DRIVER_FILES", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH" };
    enum { NKEEP = sizeof keep / sizeof keep[0] };
    char** items = (char**)calloc(NKEEP + 64 + 1, sizeof *items);
    if (!items) exit(2);
    int n = 0;
    for (int i = 0; i < NKEEP; i++) {
        const char* v = getenv(keep[i]);
        if (!v) continue;
        const size_t sz = strlen(keep[i]) + 1 + strlen(v) + 1;
        items[n] = (char*)malloc(sz);
        if (!items[n]) exit(2);
        snprintf(items[n++], sz, "%s=%s", keep[i], v);
    }
    for (int i = 0; i < nextra && n < NKEEP + 64; i++) items[n++] = strdup(extra[i]);
    return items;
}

/* Runs the child into dir (its log at dir/log.txt); its exit code, or -1. */
static int run_child(const char* exe, const char* dir, const char* const* extra, int nextra)
{
    char log[MAX_PATH];
    dir_make(dir);
    snprintf(log, sizeof log, "%s/log.txt", dir);
    const int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("[synth] cannot write %s\n", log); return -1; }
    char** env = env_list(extra, nextra);
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        const int in = open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        alarm(600);   /* ten minutes is far more than the scenes take; then the child is stuck */
        char* const argv[] = { (char*)exe, (char*)"--run", (char*)dir, NULL };
        execve(exe, argv, env);
        _exit(127);
    }
    close(fd);
    int code = -1;
    if (pid > 0) {
        int st = 0;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (WIFEXITED(st))
            code = WEXITSTATUS(st);
        else if (WIFSIGNALED(st)) {
            if (WTERMSIG(st) == SIGALRM) printf("[synth] the child in %s did not finish in 10 minutes: stopped\n", dir);
            else printf("[synth] the child in %s died of signal %d\n", dir, WTERMSIG(st));
            code = 128 + WTERMSIG(st);
        }
    } else
        printf("[synth] cannot start %s (error %d)\n", exe, errno);
    for (char** p = env; *p; p++) free(*p);
    free(env);
    return code;
}
#endif

static char* read_file(const char* path, size_t* len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((size_t)(n > 0 ? n : 0) + 1);
    if (!b) { fclose(f); return NULL; }
    const size_t got = n > 0 ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    b[got] = 0;
    if (len) *len = got;
    return b;
}

/* What one child said: per pass and frame, its presents and the hash at the
 * last one; the failed checks; whether it got to the end. */
enum { MAXFRAMES = 64 };
typedef struct {
    int code, done, shutdown_fail, check_lines;
    uint64_t hash[NPASSES][MAXFRAMES];
    int presents[NPASSES][MAXFRAMES];
    unsigned failed;     /* at the last present */
    int played;          /* frames begun */
    char first_check[256];
    int vk_seen, no_layer;   /* the Vulkan route: its count line; "no validation layer" */
    unsigned vk_msgs;        /* the validation layer's messages, by the count line */
    char first_vk[512];      /* the first message, and its text's line */
    char route[256];         /* the child's route line, after "[synth] route " */
} Run;

static int frame_index(int s, int k)
{
    int n = 0;
    for (int i = 0; i < s; i++) n += k_scene[i].frames;
    return n + k;
}

static int run_parse(const char* dir, Run* r)
{
    char log[MAX_PATH];
    snprintf(log, sizeof log, "%s/log.txt", dir);
    char* t = read_file(log, NULL);
    if (!t) return 0;
    int cp = -1, cf = -1;
    for (char* line = t; line && *line; ) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        int p, s, k;
        unsigned n, fails;
        unsigned long long h;
        if (sscanf(line, "[synth] play %d %d %d", &p, &s, &k) == 3 && p >= 1 && p <= NPASSES && s >= 0 &&
            s < NSCENES && k >= 0 && k < k_scene[s].frames) {
            cp = p - 1;
            cf = frame_index(s, k);
            r->played++;
        } else if (sscanf(line, "[null] present %u: stream %llx, %u failed checks", &n, &h, &fails) == 3) {
            if (cp >= 0) { r->hash[cp][cf] = h; r->presents[cp][cf]++; }
            r->failed = fails;
        } else if (strstr(line, "[GPU null] check failed")) {
            if (!r->check_lines++) snprintf(r->first_check, sizeof r->first_check, "%s", line);
        } else if (strstr(line, "[GPU null]") && strstr(line, "checks failed")) {
            r->shutdown_fail = 1;
            if (!r->first_check[0]) snprintf(r->first_check, sizeof r->first_check, "%s", line);
        } else if (sscanf(line, "[synth] validation messages %u", &n) == 1) {
            r->vk_seen = 1;
            r->vk_msgs = n;
        } else if (!strncmp(line, "[VK] error:", 11) || !strncmp(line, "[VK] warning:", 13)) {
            if (!r->first_vk[0]) {   /* (its text is the next line, indented) */
                const char* tx = next ? next + strspn(next, " ") : "";
                const size_t tn = strcspn(tx, "\r\n");
                snprintf(r->first_vk, sizeof r->first_vk, "%.200s %.*s", line, (int)(tn < 300 ? tn : 300), tx);
            }
        } else if (strstr(line, "RSX_VK_VALIDATION: no ")) {
            r->no_layer = 1;
        } else if (!strncmp(line, "[synth] route ", 14)) {
            snprintf(r->route, sizeof r->route, "%.*s", (int)strcspn(line + 14, "\r"), line + 14);
        } else if (!strncmp(line, "[synth] done", 12))
            r->done = 1;
        line = next;
    }
    free(t);
    return 1;
}

/* Record counts per scene (pass 1) from the dump: a frame's records end at
 * its swap_present. */
typedef struct { char name[32]; int count; } Count;
typedef struct { Count c[96]; int n; } Counts;
static void count_add(Counts* cs, const char* name, size_t len)
{
    if (len >= sizeof cs->c[0].name) return;
    for (int i = 0; i < cs->n; i++)
        if (strlen(cs->c[i].name) == len && !memcmp(cs->c[i].name, name, len)) { cs->c[i].count++; return; }
    if (cs->n < (int)(sizeof cs->c / sizeof cs->c[0])) {
        memcpy(cs->c[cs->n].name, name, len);
        cs->c[cs->n].name[len] = 0;
        cs->c[cs->n++].count = 1;
    }
}
static int count_of(const Counts* cs, const char* name)
{
    for (int i = 0; i < cs->n; i++)
        if (!strcmp(cs->c[i].name, name)) return cs->c[i].count;
    return 0;
}
static int dump_counts(const char* path, Counts* per_scene, int* frames_seen)
{
    char* t = read_file(path, NULL);
    if (!t) return 0;
    int frame = 0, scene = 0, k = 0;
    *frames_seen = 0;
    for (char* line = t; line && *line; ) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        const char* name = line + strspn(line, " ");   /* a command is indented under its "cmd" */
        const size_t len = strcspn(name, " \r");
        if (frame < frames_total() && len) count_add(&per_scene[scene], name, len);
        if (len == 12 && !memcmp(name, "swap_present", 12)) {
            frame++;
            if (++k >= k_scene[scene].frames && scene + 1 < NSCENES) { scene++; k = 0; }
        }
        line = next;
    }
    *frames_seen = frame;
    free(t);
    return 1;
}

static int needs_check(int s, const Counts* cs, int verbose)
{
    int ok = 1;
    const char* p = k_scene[s].needs;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char name[32];
        int want = 0, n = 0;
        if (sscanf(p, "%31[^:]:%d%n", name, &want, &n) != 2) { printf("[synth] bad needs in %s\n", k_scene[s].name); return 0; }
        p += n;
        const int got = count_of(cs, name);
        if (got < want) {
            printf("[synth] FAIL %s: %d %s records, needs %d\n", k_scene[s].name, got, name, want);
            ok = 0;
        } else if (verbose)
            printf("[synth]   %s: %s %d (needs %d)\n", k_scene[s].name, name, got, want);
    }
    return ok;
}

/* Removes what the test wrote in dir: the logs, the dumps, the captures. */
static void out_remove(const char* out)
{
    static const char* const sub[] = { "a", "b" };
    for (int i = 0; i < 2; i++) {
        char d[MAX_PATH], f[MAX_PATH];
        snprintf(d, sizeof d, "%s/%s", out, sub[i]);
        snprintf(f, sizeof f, "%s/log.txt", d); file_del(f);
        snprintf(f, sizeof f, "%s/dump.txt", d); file_del(f);
        snprintf(f, sizeof f, "%s/caps", d);
        for (int s = 0; s < NSCENES; s++)
            for (int k = 0; k < k_scene[s].frames; k++) {
                char c[MAX_PATH];
                cap_path(c, sizeof c, f, s, k);
                file_del(c);
            }
        dir_del(f);
        dir_del(d);
    }
    dir_del(out);
}

static int file_there(const char* f)
{
    FILE* x = fopen(f, "rb");
    if (x) fclose(x);
    return x != NULL;
}

/* --route vulkan: one child on the Vulkan route, with the validation layer
 * unless validation is 0. The shader cache is off, so every program goes
 * through the spv target in every run. fallback (--fallback): NULL, a start
 * step to fail at, or "mark"; the child must then end on d3d12, saying why. */
static int vk_test(const char* exe, const char* out, const char* const* user, int nuser, int validation, int keep,
                   const char* fallback)
{
    const char* e[60];
    int n = 0;
    e[n++] = "RSX_BACKEND=vulkan";
    e[n++] = "RSX_PRESENT=1";
    e[n++] = "PS3_SHADER_CACHE=0";
    if (validation) e[n++] = "RSX_VK_VALIDATION=1";
    e[n++] = "PSO_THREADS=4";
    e[n++] = "VP_FETCH_THREADS=2";
    /* The start mark's folder (rsx_out.c): empty, or the mark alone with
     * --fallback mark. */
    char state[MAX_PATH], mark[MAX_PATH + 32], state_env[MAX_PATH + 16], fail_env[96];
    snprintf(state, sizeof state, "%s/state", out);
    snprintf(mark, sizeof mark, "%s/renderer_starting", state);
    snprintf(state_env, sizeof state_env, "RSX_STATE_DIR=%s", state);
    dir_make(state);
    file_del(mark);
    e[n++] = state_env;
    if (fallback) {
        e[n++] = "SYNTH_ROUTE=d3d12";
        if (!strcmp(fallback, "mark")) {
            FILE* f = fopen(mark, "wb");
            if (f) fclose(f);
        } else {
            snprintf(fail_env, sizeof fail_env, "RSX_FAIL_START_AT=%s", fallback);
            e[n++] = fail_env;
        }
    }
    const char* mask = "none";
    for (int i = 0; i < nuser; i++) {
        e[n++] = user[i];
        if (!strncmp(user[i], "RSX_VK_MASK=", 12)) mask = user[i] + 12;
    }
    char da[MAX_PATH];
    snprintf(da, sizeof da, "%s/a", out);
    Run r;
    memset(&r, 0, sizeof r);
    r.code = run_child(exe, da, e, n);

    int ok = 1;
    const int nf = frames_total();
    if (!run_parse(da, &r)) {
        printf("[synth] FAIL: no log in %s\n", da);
        ok = 0;
    } else {
        if (r.code != 0 || !r.done) {
            printf("[synth] FAIL: the child in %s ended with %d, %s (see %s/log.txt)\n", da, r.code,
                   r.done ? "done" : "not done", da);
            ok = 0;
        }
        if (r.played != NPASSES * nf) {
            printf("[synth] FAIL: the child in %s played %d frames of %d\n", da, r.played, NPASSES * nf);
            ok = 0;
        }
        if (validation && r.no_layer) {
            printf("[synth] FAIL: no validation layer (VK_LAYER_KHRONOS_validation); --no-validation runs without it\n");
            ok = 0;
        }
        if (r.done && !r.vk_seen) {
            printf("[synth] FAIL: the child gave no count of validation messages\n");
            ok = 0;
        }
        if (r.vk_msgs || r.first_vk[0]) {
            printf("[synth] FAIL: %u validation messages; the first: %s\n", r.vk_msgs, r.first_vk);
            ok = 0;
        }
        /* The route, and why when it is d3d12: the step's name in brackets
         * (rsx_out.c's step_words), or the mark's reason. */
        char want[96];
        if (!fallback) snprintf(want, sizeof want, "vulkan");
        else if (!strcmp(fallback, "mark")) snprintf(want, sizeof want, "d3d12: the last start on Vulkan did not finish");
        else snprintf(want, sizeof want, "(%s)", fallback);
        const int route_ok = !fallback ? !strcmp(r.route, "vulkan")
                                       : !strncmp(r.route, "d3d12: ", 7) && strstr(r.route, want) != NULL;
        if (!route_ok) {
            printf("[synth] FAIL: the child's route is \"%s\", not %s\"%s\"\n", r.route[0] ? r.route : "(none)",
                   fallback ? "d3d12 with " : "", want);
            ok = 0;
        }
        if (file_there(mark)) {
            printf("[synth] FAIL: the start mark %s is still there at the end\n", mark);
            ok = 0;
        }
    }
    file_del(mark);
    dir_del(state);
    if (ok && fallback) {
        printf("[synth] PASS: RSX_BACKEND=vulkan with %s%s went to d3d12 (\"%s\"), which played %d scenes, %d frames a "
               "pass, %d passes; the mark gone; %s\n", strcmp(fallback, "mark") ? "the start failing at " : "",
               strcmp(fallback, "mark") ? fallback : "a start mark left", r.route + 7, NSCENES, nf, NPASSES,
               validation ? "0 validation messages" : "validation off");
        if (!keep) out_remove(out);
        return 0;
    }
    if (ok) {
        printf("[synth] PASS: the Vulkan route played %d scenes, %d frames a pass, %d passes; %s; RSX_VK_MASK %s%s\n",
               NSCENES, nf, NPASSES, validation ? "0 validation messages" : "validation off", mask,
               SYNTH_SWITCH ? "; through the route switch, the start mark gone" : "");
        if (!keep) out_remove(out);
        return 0;
    }
    /* The child's last lines, but for the play marks and the decompiler's notes. */
    char log[MAX_PATH];
    snprintf(log, sizeof log, "%s/log.txt", da);
    char* t = read_file(log, NULL);
    if (t) {
        const char* keep[24];
        int nk = 0;
        for (char* line = t; line && *line; ) {
            char* next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (strncmp(line, "[synth] play", 12) && strncmp(line, "[FP]", 4)) {
                if (nk == 24) { memmove(keep, keep + 1, sizeof keep - sizeof keep[0]); nk--; }
                keep[nk++] = line;
            }
            line = next;
        }
        printf("[synth] the child's last lines:\n");
        for (int i = 0; i < nk; i++) printf("    %s\n", keep[i]);
        free(t);
    }
    printf("[synth] FAILED. The log and the captures are in %s\n", out);
    return 1;
}

#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * --stores <vulkan|hal-d3d12>: the new path's pipeline stores (step P3S) on a
 * real device, over the same chain, in seven children with
 * PS3_SHADER_CACHE set and PS3_PSO_WAIT=1 (the start's builds run to their
 * end). The cache's library is gpu/d3d12/pipelines.lib, or Vulkan's
 * gpu/vulkan/<driver>.vkpc; the neutral manifest gpu/manifest.bin
 * (rsxc_psolib.inc's records: 56 bytes, "GNM1").
 *   A  an empty cache: the chain makes pipelines; the manifest holds M
 *      records, all whole, none twice.
 *   B  A's cache, its manifest with a torn record at its end: the tail is
 *      cut (M records again) and the chain compiles no pipeline.
 *   C  a cache with A's shaders and manifest only, no frames: the start
 *      builds the M records, none fails.
 *   D  C's cache: the chain compiles no guest-FP pipeline (the records are
 *      the draws' pipelines), and nothing is left to build.
 *   E  A's shaders only, and A's records handed over as pipeline index rows,
 *      no frames: all M rows map to a pipeline (none not built), and the
 *      index is marked built (gpu/<dir>/index.built).
 *   F  E's cache: no guest-FP pipeline compiled.
 *   G  A's cache, its library made another driver's (a byte of the file
 *      changed), no frames: the library starts empty, and the M records are
 *      built again.
 * -------------------------------------------------------------------------*/
#define GM_SIZE  56u
#define GM_MAGIC 0x314D4E47u

typedef struct {
    int code, done;
    long loaded, compiled, fp_loaded, fp_compiled;    /* "pipelines this run" (-1: no line) */
    long mb[4], ix[4];         /* the manifest's and the index's builds: built, there already, not built, of */
    long gm_recs, gm_build;    /* "pipeline manifest gpu/manifest.bin: %u records, %u to build" */
    long torn;                 /* "keeping the first %u whole records" */
    int lib_new;
} StoreRun;

static void store_parse(const char* dir, StoreRun* r)
{
    char log[MAX_PATH];
    snprintf(log, sizeof log, "%s/log.txt", dir);
    r->loaded = r->compiled = r->fp_loaded = r->fp_compiled = -1;
    r->gm_recs = r->gm_build = r->torn = -1;
    for (int i = 0; i < 4; i++) r->mb[i] = r->ix[i] = -1;
    char* t = read_file(log, NULL);
    for (char* line = t; line && *line; ) {
        char* next = strchr(line, '\n');
        if (next) *next++ = 0;
        unsigned a, b;
        if (sscanf(line, "[D3D12] pipelines this run: %ld loaded from the library, %ld compiled; guest FP: %ld loaded, "
                         "%ld compiled", &r->loaded, &r->compiled, &r->fp_loaded, &r->fp_compiled) == 4) {
        } else if (sscanf(line, "[D3D12] pipeline manifest build: %ld built, %ld there already, %ld not built, of %ld",
                          &r->mb[0], &r->mb[1], &r->mb[2], &r->mb[3]) == 4) {
        } else if (sscanf(line, "[D3D12] pipeline index: %ld built, %ld there already, %ld not built, of %ld",
                          &r->ix[0], &r->ix[1], &r->ix[2], &r->ix[3]) == 4) {
        } else if (sscanf(line, "[D3D12] pipeline manifest gpu/manifest.bin: %u records, %u to build", &a, &b) == 2) {
            r->gm_recs = a; r->gm_build = b;
        } else if (sscanf(line, "[D3D12] pipeline manifest gpu/manifest.bin: keeping the first %u whole", &a) == 1) {
            r->torn = a;
        } else if (!strncmp(line, "[D3D12] pipeline library: new", 29)) {
            r->lib_new = 1;
        } else if (!strncmp(line, "[synth] done", 12))
            r->done = 1;
        line = next;
    }
    free(t);
}

/* The manifest's records (NULL with *n -1 when the file is not whole). */
static uint8_t* gm_read(const char* cache, long* n)
{
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s/gpu/manifest.bin", cache);
    size_t len = 0;
    char* b = read_file(p, &len);
    *n = -1;
    if (!b) return NULL;
    if (len % GM_SIZE) { free(b); return NULL; }
    for (size_t o = 0; o < len; o += GM_SIZE) {
        uint32_t m, s;
        memcpy(&m, b + o, 4);
        memcpy(&s, b + o + 4, 4);
        if (m != GM_MAGIC || s != GM_SIZE) { free(b); return NULL; }
        for (size_t q = 0; q < o; q += GM_SIZE)
            if (!memcmp(b + q, b + o, GM_SIZE)) { free(b); return NULL; }   /* (none twice) */
    }
    *n = (long)(len / GM_SIZE);
    return (uint8_t*)b;
}

/* Copies dir src's files matching pat into dst (made). */
static int copy_files(const char* src, const char* pat, const char* dst)
{
    char q[MAX_PATH];
    snprintf(q, sizeof q, "%s/%s", src, pat);
    dir_make(dst);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(q, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char a[MAX_PATH], b[MAX_PATH];
        snprintf(a, sizeof a, "%s/%s", src, fd.cFileName);
        snprintf(b, sizeof b, "%s/%s", dst, fd.cFileName);
        if (CopyFileA(a, b, FALSE)) n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}
/* A cache's shaders (the route's target's files) into a new cache. */
static int copy_shaders(const char* route, const char* src, const char* dst)
{
    dir_make(dst);
    if (strcmp(route, "vulkan")) return copy_files(src, "*.dxbc", dst);
    char a[MAX_PATH], b[MAX_PATH];
    snprintf(a, sizeof a, "%s/gpu/spv", src);
    snprintf(b, sizeof b, "%s/gpu", dst);
    dir_make(b);
    snprintf(b, sizeof b, "%s/gpu/spv", dst);
    return copy_files(a, "*.spv", b);
}
/* The library's file in a cache (the first of the route's), "" if none. */
static void lib_file(const char* route, const char* cache, char* out, size_t cap)
{
    char d[MAX_PATH];
    const int vk = !strcmp(route, "vulkan");
    snprintf(d, sizeof d, "%s/gpu/%s", cache, vk ? "vulkan" : "d3d12");
    char q[MAX_PATH];
    snprintf(q, sizeof q, "%s/%s", d, vk ? "*.vkpc" : "pipelines.lib");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(q, &fd);
    out[0] = 0;
    if (h == INVALID_HANDLE_VALUE) return;
    snprintf(out, cap, "%s/%s", d, fd.cFileName);
    FindClose(h);
}

static int g_store_ok;
static void store_check(int ok, const char* child, const char* what)
{
    printf("[synth] %s %s: %s\n", ok ? "ok  " : "FAIL", child, what);
    if (!ok) g_store_ok = 0;
}

/* One child on the route into out/<name>, cache PS3_SHADER_CACHE, with
 * more NAME=VALUE (may be NULL). */
static void store_child(const char* exe, const char* out, const char* name, const char* route, const char* cache,
                        const char* const* user, int nuser, const char* more1, const char* more2, StoreRun* r)
{
    char dir[MAX_PATH], be[64], sc[MAX_PATH + 32];
    snprintf(dir, sizeof dir, "%s/%s", out, name);
    snprintf(be, sizeof be, "RSX_BACKEND=%s", route);
    snprintf(sc, sizeof sc, "PS3_SHADER_CACHE=%s", cache);
    const char* e[64];
    int n = 0;
    e[n++] = be;
    e[n++] = sc;
    e[n++] = "RSX_PRESENT=1";
    e[n++] = "PS3_PSO_WAIT=1";
    e[n++] = "PSO_THREADS=4";
    for (int i = 0; i < nuser && n < 60; i++) e[n++] = user[i];
    if (more1) e[n++] = more1;
    if (more2) e[n++] = more2;
    memset(r, 0, sizeof *r);
    r->code = run_child(exe, dir, e, n);
    store_parse(dir, r);
    char what[160];
    snprintf(what, sizeof what, "the child ended with %d, %s", r->code, r->done ? "done" : "not done");
    store_check(r->code == 0 && r->done, name, what);
}

static int stores_test(const char* exe, const char* out, const char* route, const char* const* user, int nuser)
{
    g_store_ok = 1;
    const int vk = !strcmp(route, "vulkan");
    char ca[MAX_PATH], cc[MAX_PATH], ce[MAX_PATH], cg[MAX_PATH], p[MAX_PATH], q[MAX_PATH], what[256];
    snprintf(ca, sizeof ca, "%s/cache_a", out);
    snprintf(cc, sizeof cc, "%s/cache_c", out);
    snprintf(ce, sizeof ce, "%s/cache_e", out);
    snprintf(cg, sizeof cg, "%s/cache_g", out);
    snprintf(p, sizeof p, "%s/cache_a/gpu", out);
    if (GetFileAttributesA(ca) != INVALID_FILE_ATTRIBUTES) {
        printf("[synth] %s is there from an earlier run: give another --out, or remove it\n", out);
        return 2;
    }
    dir_make(ca);
    StoreRun r;

    store_child(exe, out, "A", route, ca, user, nuser, NULL, NULL, &r);
    long m = -1;
    uint8_t* recs = gm_read(ca, &m);
    snprintf(what, sizeof what, "pipelines compiled %ld (guest FP %ld); the manifest holds %ld whole records, none twice",
             r.compiled, r.fp_compiled, m);
    store_check(r.compiled > 0 && m > 0 && m <= r.fp_loaded + r.fp_compiled, "A", what);
    char lib[MAX_PATH];
    lib_file(route, ca, lib, sizeof lib);
    store_check(lib[0] != 0, "A", vk ? "the library gpu/vulkan/<driver>.vkpc is there" : "the library gpu/d3d12/pipelines.lib is there");
    if (m <= 0) { free(recs); printf("[synth] FAILED: no manifest to go on with. Everything is in %s\n", out); return 1; }

    /* The rows file: A's records as pipeline index rows. */
    char rows[MAX_PATH], rows_env[MAX_PATH + 32];
    snprintf(rows, sizeof rows, "%s/rows.txt", out);
    FILE* f = fopen(rows, "wb");
    for (long i = 0; f && i < m; i++) {
        const uint8_t* b = recs + i * GM_SIZE;
        uint64_t vs, ps;
        uint32_t s[8];
        memcpy(&vs, b + 8, 8);
        memcpy(&ps, b + 16, 8);
        memcpy(s, b + 24, 32);   /* fetch clamp blend nrt rtfmt cmask cull depth */
        fprintf(f, "add %u %016llx %u %016llx %x %u %x %x %x %x\n", s[0], (unsigned long long)vs, s[1],
                (unsigned long long)ps, s[2], s[3], s[4], s[5], s[6], s[7]);
    }
    if (f) { fprintf(f, "ready 00000000005d1e5e\n"); fclose(f); }
    snprintf(rows_env, sizeof rows_env, "RSX_SYNTH_ROWS=%s", rows);

    /* The caches C, E and G, from A's before B adds to it. */
    copy_shaders(route, ca, cc);
    snprintf(p, sizeof p, "%s/gpu/manifest.bin", ca);
    snprintf(q, sizeof q, "%s/gpu", cc);
    dir_make(q);
    snprintf(q, sizeof q, "%s/gpu/manifest.bin", cc);
    CopyFileA(p, q, FALSE);
    copy_shaders(route, ca, ce);
    copy_shaders(route, ca, cg);
    snprintf(q, sizeof q, "%s/gpu", cg);
    dir_make(q);
    snprintf(q, sizeof q, "%s/gpu/manifest.bin", cg);
    CopyFileA(p, q, FALSE);
    snprintf(q, sizeof q, "%s/gpu/%s", cg, vk ? "vulkan" : "d3d12");
    dir_make(q);
    {   /* G's library: A's with one byte changed (Vulkan: our header's driver; D3D12: its first byte) */
        char lg[MAX_PATH];
        const char* leaf = strrchr(lib, '/');
        snprintf(lg, sizeof lg, "%s%s", q, leaf ? leaf : "/pipelines.lib");
        CopyFileA(lib, lg, FALSE);
        FILE* g = fopen(lg, "r+b");
        if (g) {
            const long at = vk ? 24 : 0;
            fseek(g, at, SEEK_SET);
            const int c = fgetc(g);
            fseek(g, at, SEEK_SET);
            fputc(c ^ 0x5A, g);
            fclose(g);
        }
    }

    /* B: a torn record at the manifest's end first. */
    f = fopen(p, "ab");
    if (f) { fwrite("GNM1torn-tail", 1, 13, f); fclose(f); }
    store_child(exe, out, "B", route, ca, user, nuser, NULL, NULL, &r);
    long mb = -1;
    free(gm_read(ca, &mb));
    snprintf(what, sizeof what, "the torn tail cut (kept %ld, the file holds %ld of %ld)", r.torn, mb, m);
    store_check(r.torn == m && mb == m, "B", what);
    snprintf(what, sizeof what, "the second run compiles %ld pipelines (%ld loaded), %ld records to build", r.compiled,
             r.loaded, r.gm_build);
    store_check(r.compiled == 0 && r.loaded > 0 && r.gm_build == 0, "B", what);

    store_child(exe, out, "C", route, cc, user, nuser, "RSX_SYNTH_PLAY=0", NULL, &r);
    snprintf(what, sizeof what, "the manifest's build: %ld built, %ld there already, %ld not built, of %ld (of %ld records)",
             r.mb[0], r.mb[1], r.mb[2], r.mb[3], m);
    store_check(r.gm_recs == m && r.mb[3] == m && r.mb[2] == 0 && r.mb[0] + r.mb[1] == m, "C", what);

    store_child(exe, out, "D", route, cc, user, nuser, NULL, NULL, &r);
    snprintf(what, sizeof what, "guest-FP pipelines compiled %ld (loaded %ld); %ld records to build", r.fp_compiled,
             r.fp_loaded, r.gm_build);
    store_check(r.fp_compiled == 0 && r.fp_loaded > 0 && r.gm_build == 0, "D", what);

    store_child(exe, out, "E", route, ce, user, nuser, "RSX_SYNTH_PLAY=0", rows_env, &r);
    snprintf(what, sizeof what, "index rows mapped: %ld built, %ld there already, %ld not built, of %ld (of %ld rows)",
             r.ix[0], r.ix[1], r.ix[2], r.ix[3], m);
    store_check(r.ix[3] == m && r.ix[2] == 0 && r.ix[0] + r.ix[1] == m, "E", what);
    snprintf(q, sizeof q, "%s/gpu/%s/index.built", ce, vk ? "vulkan" : "d3d12");
    {
        char* t = read_file(q, NULL);
        store_check(t && !strncmp(t, "00000000005d1e5e", 16), "E", "the index is marked built (gpu/<dir>/index.built)");
        free(t);
    }

    store_child(exe, out, "F", route, ce, user, nuser, NULL, NULL, &r);
    snprintf(what, sizeof what, "guest-FP pipelines compiled %ld (loaded %ld)", r.fp_compiled, r.fp_loaded);
    store_check(r.fp_compiled == 0 && r.fp_loaded > 0, "F", what);

    store_child(exe, out, "G", route, cg, user, nuser, "RSX_SYNTH_PLAY=0", NULL, &r);
    snprintf(what, sizeof what, "another driver's library: %s; the manifest's build: %ld built, %ld there already, "
             "%ld not built, of %ld", r.lib_new ? "a new one" : "not refused", r.mb[0], r.mb[1], r.mb[2], r.mb[3]);
    store_check(r.lib_new && r.gm_build == m && r.mb[2] == 0 && r.mb[0] + r.mb[1] == m, "G", what);

    free(recs);
    if (g_store_ok) {
        printf("[synth] PASS: the pipeline stores on %s: %ld records, every one built from the manifest and as an "
               "index row; the second run compiled none. Everything is in %s\n", route, m, out);
        return 0;
    }
    printf("[synth] FAILED. Everything is in %s\n", out);
    return 1;
}
#endif

static int test_main(int argc, char** argv)
{
    const char* out = "test_rsx_synth.out";
    int update = 0, keep = 0, verbose = 0, vulkan = 0, validation = 1;
    const char* fallback = NULL;
    const char* stores = NULL;
    const char* extra[48];
    int nextra = 0;
    extra[nextra++] = "RSX_BACKEND=null";
    extra[nextra++] = "RSX_PRESENT=1";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--update-golden")) update = 1;
        else if (!strcmp(argv[i], "--keep")) keep = 1;
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--env") && i + 1 < argc && nextra < 40) extra[nextra++] = argv[++i];
        else if (!strcmp(argv[i], "--route") && i + 1 < argc && (!strcmp(argv[i + 1], "null") ||
                                                                 !strcmp(argv[i + 1], "vulkan")))
            vulkan = !strcmp(argv[++i], "vulkan");
        else if (!strcmp(argv[i], "--no-validation")) validation = 0;
        else if (!strcmp(argv[i], "--fallback") && i + 1 < argc) fallback = argv[++i];
        else if (!strcmp(argv[i], "--stores") && i + 1 < argc && (!strcmp(argv[i + 1], "vulkan") ||
                                                                  !strcmp(argv[i + 1], "hal-d3d12")))
            stores = argv[++i];
        else {
            fprintf(stderr, "usage: test_rsx_synth [--update-golden] [--keep] [--verbose] [--out <dir>] [--env NAME=VALUE]...\n"
                            "       test_rsx_synth --route vulkan [--no-validation] [--fallback <step>|mark] [--keep] [--out <dir>]\n"
                            "                      [--env NAME=VALUE]...\n"
                            "       test_rsx_synth --stores vulkan|hal-d3d12 [--out <dir>] [--env NAME=VALUE]...\n"
                            "       test_rsx_synth --caps <dir> | --run <dir>\n");
            return 2;
        }
    }
    if (fallback && (!vulkan || !SYNTH_SWITCH)) {
        fprintf(stderr, "test_rsx_synth: --fallback is --route vulkan's, in a build with the route switch\n");
        return 2;
    }
    char exe[MAX_PATH];
    if (!self_exe(exe, sizeof exe)) return 2;
    dir_make(out);
    if (stores) {
#ifdef _WIN32
        return stores_test(exe, out, stores, extra + 2, nextra - 2);
#else
        printf("[synth] --stores runs on Windows only for now\n");
        return 0;
#endif
    }
    if (vulkan) return vk_test(exe, out, extra + 2, nextra - 2, validation, keep, fallback);
    char da[MAX_PATH], db[MAX_PATH], dump[MAX_PATH], envdump[MAX_PATH + 16];
    snprintf(da, sizeof da, "%s/a", out);
    snprintf(db, sizeof db, "%s/b", out);
    snprintf(dump, sizeof dump, "%s/dump.txt", da);
    snprintf(envdump, sizeof envdump, "GPU_NULL_DUMP=%s", dump);

    /* The first child with the dump and several threads, the second with one
     * pipeline thread and no fetch thread. The counts are set, not left to
     * their defaults, which follow the processor count (a 4-processor runner
     * would give both children one pipeline thread). */
    const char* ea[52];
    int na = 0;
    for (int i = 0; i < nextra; i++) ea[na++] = extra[i];
    ea[na++] = envdump;
    ea[na++] = "PSO_THREADS=4";
    ea[na++] = "VP_FETCH_THREADS=2";
    Run ra, rb;
    memset(&ra, 0, sizeof ra);
    memset(&rb, 0, sizeof rb);
    ra.code = run_child(exe, da, ea, na);
    const char* eb[52];
    int nb = 0;
    for (int i = 0; i < nextra; i++) eb[nb++] = extra[i];
    eb[nb++] = "PSO_THREADS=1";
    eb[nb++] = "VP_FETCH_THREADS=0";
    rb.code = run_child(exe, db, eb, nb);

    int ok = 1;
    Run* const runs[2] = { &ra, &rb };
    const char* dirs[2] = { da, db };
    const int nf = frames_total();
    for (int c = 0; c < 2; c++) {
        Run* r = runs[c];
        if (!run_parse(dirs[c], r)) { printf("[synth] FAIL: no log in %s\n", dirs[c]); ok = 0; continue; }
        if (r->code != 0 || !r->done) {
            printf("[synth] FAIL: the child in %s ended with %d, %s (see %s/log.txt)\n", dirs[c], r->code,
                   r->done ? "done" : "not done", dirs[c]);
            ok = 0;
        }
        if (r->played != NPASSES * nf) {
            printf("[synth] FAIL: the child in %s played %d frames of %d\n", dirs[c], r->played, NPASSES * nf);
            ok = 0;
        }
        if (r->failed || r->check_lines || r->shutdown_fail) {
            printf("[synth] FAIL: null checks failed in %s: %u by the last present%s; first: %s\n", dirs[c],
                   r->failed, r->shutdown_fail ? ", and at the shutdown" : "", r->first_check);
            ok = 0;
        }
        for (int p = 0; p < NPASSES; p++)
            for (int f = 0; f < nf; f++)
                if (r->presents[p][f] != 1) {
                    printf("[synth] FAIL: in %s, pass %d frame %d presented %d times (once each)\n", dirs[c], p + 1,
                           f, r->presents[p][f]);
                    ok = 0;
                }
    }

    /* The hashes: the children against each other, then against the goldens. */
    printf("[synth] %d scenes, %d frames a pass, %d passes, 2 processes\n", NSCENES, nf, NPASSES);
    int same = 1, golden_ok = 1;
    for (int s = 0; s < NSCENES; s++) {
        const int last = frame_index(s, k_scene[s].frames - 1);
        uint64_t got[NPASSES];
        for (int p = 0; p < NPASSES; p++) {
            got[p] = ra.hash[p][last];
            for (int f = frame_index(s, 0); f <= last; f++)
                if (ra.hash[p][f] != rb.hash[p][f]) {
                    if (same) printf("[synth] FAIL: the two children differ from %s pass %d frame %d: %016llx and %016llx\n",
                                     k_scene[s].name, p + 1, f - frame_index(s, 0),
                                     (unsigned long long)ra.hash[p][f], (unsigned long long)rb.hash[p][f]);
                    same = 0;
                }
        }
        const Golden* g = s < (int)(sizeof k_golden / sizeof k_golden[0]) && !strcmp(k_golden[s].scene, k_scene[s].name)
                              ? &k_golden[s] : NULL;
        const int gok = g && g->pass1 == got[0] && g->pass2 == got[1];
        if (!gok && !update) {
            printf("[synth] FAIL %-9s expected %016llx %016llx, got %016llx %016llx\n", k_scene[s].name,
                   g ? (unsigned long long)g->pass1 : 0ull, g ? (unsigned long long)g->pass2 : 0ull,
                   (unsigned long long)got[0], (unsigned long long)got[1]);
            golden_ok = 0;
        } else if (verbose || update)
            printf("[synth] %-9s %016llx %016llx%s\n", k_scene[s].name, (unsigned long long)got[0],
                   (unsigned long long)got[1], gok ? "" : " (new)");
    }
    if (!same) ok = 0;
    if (!golden_ok) ok = 0;

    /* What each scene is there for, from the first child's dump. */
    Counts cs[NSCENES];
    memset(cs, 0, sizeof cs);
    int seen = 0;
    if (!dump_counts(dump, cs, &seen)) { printf("[synth] FAIL: no dump %s\n", dump); ok = 0; }
    else {
        if (seen < NPASSES * nf) { printf("[synth] FAIL: the dump has %d presents of %d\n", seen, NPASSES * nf); ok = 0; }
        for (int s = 0; s < NSCENES; s++)
            if (!needs_check(s, &cs[s], verbose)) ok = 0;
    }

    if (update) {
        printf("\n/* test_rsx_synth --update-golden */\nstatic const Golden k_golden[] = {\n");
        for (int s = 0; s < NSCENES; s++) {
            const int last = frame_index(s, k_scene[s].frames - 1);
            char q[16];
            snprintf(q, sizeof q, "\"%s\",", k_scene[s].name);
            printf("    { %-12s 0x%016llxull, 0x%016llxull },\n", q, (unsigned long long)ra.hash[0][last],
                   (unsigned long long)ra.hash[1][last]);
        }
        printf("};\n");
        if (!ok) printf("[synth] not a list to keep: the run failed above\n");
        return ok ? 0 : 1;
    }
    if (ok) {
        printf("[synth] PASS: %d hashes as their goldens in both processes, 0 failed checks\n", NSCENES * NPASSES);
        if (!keep) out_remove(out);
        return 0;
    }
    printf("[synth] FAILED. The logs, the stream dump (%s) and the captures are in %s\n", dump, out);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc == 3 && !strcmp(argv[1], "--run")) return child_main(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--caps")) {
        if (!caps_write(argv[2])) return 1;
        printf("[synth] %d captures in %s\n", frames_total(), argv[2]);
        return 0;
    }
    return test_main(argc, argv);
}

#else
#include <stdio.h>
int main(void)
{
    printf("test_rsx_synth: built without PS3RECOMP_RSX_HAL, or not on Windows or Linux\n");
    return 0;
}
#endif
