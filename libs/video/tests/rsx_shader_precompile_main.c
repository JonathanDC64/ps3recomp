/*
 * rsx_shader_precompile -- fill the D3D12 backend's shader disk cache from a
 * title's Cg binaries, before the title ever runs.
 *
 *   rsx_shader_precompile [--unit U=KIND[,U=KIND...]] [--alt U=KIND[,...]]
 *                         <file|dir> ...
 *
 * Each file is a CgBinaryProgram (.vpo/.fpo as the SDK's cgc writes them:
 * big-endian, profile sce_vp_rsx 0x1B5B or sce_fp_rsx 0x1B5C at offset 0);
 * anything else is skipped. A directory is searched recursively. Every
 * program is decompiled by the same code a draw uses
 * (rsx_d3d12_shader_hlsl) and compiled into the cache
 * (rsx_d3d12_shader_cache_put), so a draw that later binds it finds it there
 * instead of stalling on FXC. The cache is keyed by the HLSL text: a program
 * the title modifies before binding, or binds with other state than assumed
 * here, simply misses and is compiled at draw time as before.
 *
 * A fragment program's text also depends on what kind of texture each unit
 * it samples is bound to (KIND: cube, shadow = depth compare, d2c = depth
 * read as colour), which is draw state, not part of the binary. Each program
 * is built:
 *   - with every unit plain, and 32-bit exports unless the binary says the
 *     colour comes from H0;
 *   - with the title's conventions: --unit 7=shadow makes unit 7 a shadow
 *     unit in every program that samples it;
 *   - once more per --alt unit it samples, on top of the conventions (a
 *     unit only some programs bind a depth surface to, say);
 *   - with every variant earlier runs recorded for it (fp_variants.txt in
 *     the cache), which covers whatever the rules above do not.
 *
 * PS3_SHADER_CACHE chooses the cache directory, as for the title itself; it
 * must name the directory the title will use. PRECOMPILE_THREADS=<n> sets
 * the number of compile threads (default: one per logical processor).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

uint8_t* vm_base = NULL;

/* What the runtime library expects from a game's loader; nothing runs here. */
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

int rsx_d3d12_shader_hlsl(int fragment, const uint8_t* ucode, uint32_t ucode_len,
                          uint32_t vp_start_slot, int exp32, uint32_t cube_mask,
                          uint32_t d2c_mask, char* out, uint32_t cap);
int rsx_d3d12_shader_cache_put(int fragment, const char* hlsl);
const char* rsx_d3d12_shader_cache_path(void);
int rsx_d3d12_shader_fp_variants(const uint8_t* ucode, uint32_t ucode_len, uint32_t* out, int max);
uint32_t rsx_fp_texture_units(const uint8_t* ucode, uint32_t max_bytes);

#define CG_PROFILE_SCE_VP_RSX 0x1B5Bu
#define CG_PROFILE_SCE_FP_RSX 0x1B5Cu
#define HLSL_CAP              (256u * 1024u)
#define MAX_VARIANTS          64

/* A texture-kind rule set as the decompiler takes it: cube bits 0-15,
 * shadow bits 16-31 of "cube", depth-as-colour in "d2c". */
typedef struct { uint32_t cube, d2c; } Kinds;
static Kinds s_conv = { 0, 0 };
static Kinds s_alt[16];
static int   s_nalt = 0;

typedef struct { int fragment; char* hlsl; char name[MAX_PATH]; } Job;
static Job*  s_jobs = NULL;
static int   s_njobs = 0, s_cap = 0;
static int   s_seen = 0, s_skipped = 0, s_badhlsl = 0, s_dups = 0;
static volatile LONG s_next = 0, s_hit = 0, s_built = 0, s_failed = 0;

/* HLSL texts already queued (FNV-1a 64), so one text is compiled once. */
static uint64_t* s_texts = NULL;
static int s_ntexts = 0, s_textcap = 0;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int parse_kinds(const char* s, Kinds* conv, Kinds* alts, int* nalt)
{
    while (*s) {
        char* end;
        const long u = strtol(s, &end, 10);
        if (end == s || *end != '=' || u < 0 || u > 15) return 0;
        s = end + 1;
        Kinds k = { 0, 0 };
        if (!strncmp(s, "cube", 4))        { k.cube = 1u << u;         s += 4; }
        else if (!strncmp(s, "shadow", 6)) { k.cube = 1u << (16 + u);  s += 6; }
        else if (!strncmp(s, "d2c", 3))    { k.d2c = 1u << u;          s += 3; }
        else return 0;
        if (conv) { conv->cube |= k.cube; conv->d2c |= k.d2c; }
        else if (*nalt < 16) alts[(*nalt)++] = k;
        if (*s == ',') s++;
        else if (*s) return 0;
    }
    return 1;
}

static int text_seen(const char* t)
{
    uint64_t h = 14695981039346656037ull;
    for (const char* p = t; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    for (int i = 0; i < s_ntexts; i++) if (s_texts[i] == h) return 1;
    if (s_ntexts == s_textcap) {
        s_textcap = s_textcap ? s_textcap * 2 : 1024;
        s_texts = (uint64_t*)realloc(s_texts, sizeof(uint64_t) * (size_t)s_textcap);
        if (!s_texts) { fprintf(stderr, "rsx_shader_precompile: out of memory\n"); exit(1); }
    }
    s_texts[s_ntexts++] = h;
    return 0;
}

static void queue(const char* path, int fragment, const uint8_t* uc, uint32_t usz, uint32_t slot,
                  uint32_t exp32, uint32_t cube, uint32_t d2c)
{
    char* hlsl = (char*)malloc(HLSL_CAP);
    if (!hlsl) return;
    if (rsx_d3d12_shader_hlsl(fragment, uc, usz, slot, (int)exp32, cube, d2c, hlsl, HLSL_CAP) <= 0) {
        fprintf(stderr, "rsx_shader_precompile: cannot decompile %s\n", path);
        s_badhlsl++; free(hlsl); return;
    }
    if (text_seen(hlsl)) { s_dups++; free(hlsl); return; }
    if (s_njobs == s_cap) {
        s_cap = s_cap ? s_cap * 2 : 256;
        s_jobs = (Job*)realloc(s_jobs, sizeof(Job) * (size_t)s_cap);
        if (!s_jobs) { fprintf(stderr, "rsx_shader_precompile: out of memory\n"); exit(1); }
    }
    s_jobs[s_njobs].fragment = fragment;
    s_jobs[s_njobs].hlsl = hlsl;
    snprintf(s_jobs[s_njobs].name, MAX_PATH, "%s", path);
    s_njobs++;
}

/* One Cg binary: queue the HLSL a draw would compile for it. */
static void add_program(const char* path, const uint8_t* b, size_t n)
{
    if (n < 32) { s_skipped++; return; }
    const uint32_t profile = be32(b), total = be32(b + 8);
    const uint32_t prog = be32(b + 20), usz = be32(b + 24), uoff = be32(b + 28);
    const int fragment = profile == CG_PROFILE_SCE_FP_RSX;
    if ((profile != CG_PROFILE_SCE_VP_RSX && !fragment) || total > n ||
        uoff > total || usz > total - uoff || (uint64_t)prog + 24 > total) { s_skipped++; return; }
    s_seen++;
    const uint8_t* uc = b + uoff;
    if (!fragment) {   /* CgBinaryVertexProgram.instructionSlot: where it is loaded */
        queue(path, 0, uc, usz, be32(b + prog + 4), 0, 0, 0);
        return;
    }
    const uint32_t exp32 = b[prog + 19] ? 0 : 1;   /* CgBinaryFragmentProgram.outputFromH0 */
    const uint32_t used = rsx_fp_texture_units(uc, usz);
    queue(path, 1, uc, usz, 0, exp32, 0, 0);
    const uint32_t cc = s_conv.cube & (used | used << 16), cd = s_conv.d2c & used;
    queue(path, 1, uc, usz, 0, exp32, cc, cd);
    for (int i = 0; i < s_nalt; i++)
        if ((s_alt[i].cube & (used | used << 16)) || (s_alt[i].d2c & used)) {
            /* The alternative replaces whatever the convention made that unit. */
            const uint32_t u = (s_alt[i].cube | s_alt[i].cube >> 16 | s_alt[i].d2c) & 0xFFFFu;
            queue(path, 1, uc, usz, 0, exp32,
                  (cc & ~(u | u << 16)) | s_alt[i].cube, (cd & ~u) | s_alt[i].d2c);
        }
    uint32_t v[MAX_VARIANTS * 3];
    const int nv = rsx_d3d12_shader_fp_variants(uc, usz, v, MAX_VARIANTS);
    for (int i = 0; i < nv; i++) queue(path, 1, uc, usz, 0, v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
}

static void add_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "rsx_shader_precompile: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) add_program(path, b, (size_t)n);
    free(b);
    fclose(f);
}

static void add_path(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) { fprintf(stderr, "rsx_shader_precompile: no such path %s\n", path); return; }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { add_file(path); return; }
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sub[MAX_PATH];
        snprintf(sub, sizeof sub, "%s\\%s", path, fd.cFileName);
        add_path(sub);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    for (;;) {
        const LONG i = InterlockedIncrement(&s_next) - 1;
        if (i >= s_njobs) return 0;
        const int r = rsx_d3d12_shader_cache_put(s_jobs[i].fragment, s_jobs[i].hlsl);
        if (r == 1) InterlockedIncrement(&s_hit);
        else if (r == 2) InterlockedIncrement(&s_built);
        else { InterlockedIncrement(&s_failed);
               fprintf(stderr, "rsx_shader_precompile: FXC failed on %s\n", s_jobs[i].name); }
    }
}

int main(int argc, char** argv)
{
    int first = 1;
    while (first + 1 < argc && (!strcmp(argv[first], "--unit") || !strcmp(argv[first], "--alt"))) {
        const int conv = !strcmp(argv[first], "--unit");
        if (!parse_kinds(argv[first + 1], conv ? &s_conv : NULL, s_alt, &s_nalt)) {
            fprintf(stderr, "rsx_shader_precompile: bad %s list '%s' (want U=cube|shadow|d2c,...)\n",
                    argv[first], argv[first + 1]);
            return 2;
        }
        first += 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: rsx_shader_precompile [--unit U=KIND,...] [--alt U=KIND,...] <file|dir> ...\n"
                        "       KIND = cube | shadow | d2c; PS3_SHADER_CACHE = cache directory\n");
        return 2;
    }
    const char* dir = rsx_d3d12_shader_cache_path();   /* also creates it */
    if (!dir[0]) { fprintf(stderr, "rsx_shader_precompile: the cache is off (PS3_SHADER_CACHE=0)\n"); return 2; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = first; i < argc; i++) add_path(argv[i]);

    SYSTEM_INFO si; GetSystemInfo(&si);
    int nt = (int)si.dwNumberOfProcessors;
    { const char* e = getenv("PRECOMPILE_THREADS"); if (e && atoi(e) > 0) nt = atoi(e); }
    if (nt > 64) nt = 64;
    if (nt > s_njobs) nt = s_njobs ? s_njobs : 1;
    HANDLE th[64];
    for (int i = 0; i < nt; i++) th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)nt, th, TRUE, INFINITE);
    for (int i = 0; i < nt; i++) CloseHandle(th[i]);
    QueryPerformanceCounter(&t1);

    printf("rsx_shader_precompile: %d programs (%d not Cg binaries, %d not decompiled), "
           "%d shaders (%d duplicate texts dropped) -> %ld compiled, %ld already cached, %ld failed; %.1fs on %d threads; cache %s\n",
           s_seen, s_skipped, s_badhlsl, s_njobs, s_dups, s_built, s_hit, s_failed,
           (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart, nt, dir);
    for (int i = 0; i < s_njobs; i++) free(s_jobs[i].hlsl);
    free(s_jobs); free(s_texts);
    return s_failed ? 1 : 0;
}
