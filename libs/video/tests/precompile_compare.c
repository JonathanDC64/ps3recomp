/*
 * precompile_compare -- setup's shader pass (rsx_shader_precompile.c) over a
 * game's programs, one target a run, and checks of what it wrote.
 *
 *   precompile_compare --game DIR [--eboot ELF] [--index TSV] [--index-only] [--target T]
 *                      [--cache DIR] [--threads N] [--rows FILE]
 *   precompile_compare --diff DIR_A DIR_B
 *   precompile_compare --spv-check CACHE [--out DIR]
 *
 * A pass feeds the pass the game's programs as the port's des_prework.cpp
 * does: every .vpo/.fpo of the .shaderbnd archives in DIR/PS3_GAME/USRDIR/shader,
 * in name order, the 2D forms for the archives drawn without a depth test, the Cg
 * programs in the game's executable (--eboot), Demon's Souls' texture
 * kinds, and the pipeline index (--index; --index-only as over an up-to-date
 * cache). --cache sets PS3_SHADER_CACHE. --rows writes every pipeline index
 * row the pass hands over, and the index's hash when it is done, one line
 * each, to compare across builds and targets. Built with the portable
 * renderer (PS3RECOMP_RSX_HAL), --target picks the sink: d3d12 (the
 * default), dxbc or spv. Without it the pass is today's, compiled into this
 * program (precompile_compare_today.c) with its two index calls going
 * through the recorder first.
 *
 * --diff compares two folders: the same file names (all levels) and bytes.
 * --spv-check checks a cache the spv target filled: every file is
 * gpu/spv/<vs|ps>_<T>.spv, with the header the compile service checks
 * ("GSPV", version 1, T as in the name, the module's size, 0) and a SPIR-V
 * module after it, which validates (SPIRV-Tools, Vulkan 1.1) as stored and
 * after the post-passes; --out keeps each module both ways (raw/, post/) for
 * other tools (spirv-val).
 *
 * What it reads and writes comes from a game: keep it out of the
 * repository. Exit 0 when the pass finished with no failure, or the check
 * holds.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* setenv, under -std=c99 too */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "rsx_shader_precompile.h"
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_SPV)
#include "gpu_shader_spv.h"   /* the post-passes and the validator */
#endif

/* What the runtime library expects from a game's loader; nothing runs here. */
uint8_t* vm_base = NULL;
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

/* des_prework.cpp's (port/des_prework.cpp): the unit kinds Demon's Souls
 * binds, and the archives it draws without a depth test. */
static const char* const kUnits = "7=shadow,8=cube,11=cube,12=cube,13=cube,14=cube";
static const char* const kAlts  = "1=d2c,2=d2c";
static const char* const kFlat[] = { "ds_filter.shaderbnd", "ds_menu.shaderbnd", "dbgfont.shaderbnd" };

/* ---- small helpers ---- */
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int same_nocase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        const char x = (char)(*a >= 'A' && *a <= 'Z' ? *a + 32 : *a);
        const char y = (char)(*b >= 'A' && *b <= 'Z' ? *b + 32 : *b);
        if (x != y) return 0;
    }
    return *a == *b;
}

static char* dup_str(const char* s)
{
    const size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static uint8_t* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = len > 0 ? (uint8_t*)malloc((size_t)len) : NULL;
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    if (b) *n = (size_t)len;
    return b;
}

static int write_file(const char* path, const void* p, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void make_dir(const char* path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void set_env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* A list of names, sorted byte by byte (std::sort on std::string, as
 * des_prework sorts the archives). */
typedef struct { char** v; int n, cap; } Names;
static void names_add(Names* l, const char* s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = (char**)realloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = dup_str(s);
}
static int cmp_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }
static void names_sort(Names* l) { if (l->n) qsort(l->v, (size_t)l->n, sizeof *l->v, cmp_str); }

/* The files under dir, all levels, as paths relative to it with '/'. */
static void list_tree(const char* dir, const char* rel, Names* out)
{
    char path[4200];
#ifdef _WIN32
    char pat[4200];
    snprintf(pat, sizeof pat, "%s/%s%s*", dir, rel, rel[0] ? "/" : "");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(path, sizeof path, "%s%s%s", rel, rel[0] ? "/" : "", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) list_tree(dir, path, out);
        else names_add(out, path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    char full[4200];
    snprintf(full, sizeof full, "%s%s%s", dir, rel[0] ? "/" : "", rel);
    DIR* d = opendir(full);
    struct dirent* e;
    while (d && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(path, sizeof path, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        char fp[8400];
        struct stat st;
        snprintf(fp, sizeof fp, "%s/%s", dir, path);
        if (stat(fp, &st) == 0 && S_ISDIR(st.st_mode)) list_tree(dir, path, out);
        else names_add(out, path);
    }
    if (d) closedir(d);
#endif
}

/* ---- feeding the pass, as des_prework.cpp does ---- */

/* Every .vpo/.fpo in one BND3 archive; returns how many. */
static int queue_archive(const char* path, const char* name)
{
    unsigned flags = 0;
    for (size_t i = 0; i < sizeof kFlat / sizeof kFlat[0]; i++)
        if (same_nocase(name, kFlat[i])) flags = RSX_PRECOMPILE_2D;
    size_t n = 0;
    uint8_t* b = read_file(path, &n);
    if (!b || n < 0x20 || memcmp(b, "BND3", 4)) {
        fprintf(stderr, "precompile_compare: %s is not a BND3 archive\n", name);
        free(b);
        return 0;
    }
    const uint32_t count = be32(b + 0x10);
    int q = 0;
    for (uint32_t i = 0; i < count; i++) {
        const size_t o = 0x20 + (size_t)i * 0x18;
        if (o + 0x18 > n) break;
        const uint32_t size = be32(b + o + 4), off = be32(b + o + 8), name_off = be32(b + o + 16);
        if (off > n || size > n - off || name_off >= n || size < 8) continue;
        const char* ename = (const char*)b + name_off;
        size_t elen = 0;
        while (name_off + elen < n && ename[elen]) elen++;
        if (elen < 4) continue;
        char ext[5];
        memcpy(ext, ename + elen - 4, 4);
        ext[4] = 0;
        if (!same_nocase(ext, ".vpo") && !same_nocase(ext, ".fpo")) continue;
        const uint32_t hdr = be32(b + off + 4);
        if (hdr >= size) continue;
        char label[256];
        snprintf(label, sizeof label, "%s#%u", name, i);
        rsx_shader_precompile_add_flags(label, b + off + hdr, size - hdr, flags);
        q++;
    }
    free(b);
    return q;
}

/* The Cg programs in the game's executable (des_prework.cpp's queue_elf). */
static int queue_elf(const char* path)
{
    size_t n = 0;
    uint8_t* b = path ? read_file(path, &n) : NULL;
    if (!b) return 0;
    int q = 0;
    for (size_t o = 0; o + 32 <= n; o += 4) {
        const uint32_t profile = be32(b + o);
        if ((profile != 0x1B5Bu && profile != 0x1B5Cu) || be32(b + o + 4) != 6) continue;
        const uint32_t total = be32(b + o + 8), params = be32(b + o + 16), prog = be32(b + o + 20);
        const uint32_t usz = be32(b + o + 24), uoff = be32(b + o + 28);
        if (total < 32 || total > n - o || params > total || (uint64_t)prog + 24 > total || uoff > total ||
            !usz || usz > total - uoff || usz % 16)
            continue;
        char label[64];
        snprintf(label, sizeof label, "EBOOT@%08llx", (unsigned long long)o);
        rsx_shader_precompile_add_flags(label, b + o, total, RSX_PRECOMPILE_2D);
        q++;
        o += (total - 4) & ~(size_t)3;
    }
    free(b);
    return q;
}

/* ---- the rows the pass hands over ---- */
static FILE* g_rows;
static unsigned g_nrows;

static void rows_add(int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key, uint32_t blend, int nrt,
                     uint32_t rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth)
{
    g_nrows++;
    if (g_rows)
        fprintf(g_rows, "add %d %016llx %d %016llx %x %d %x %x %x %x\n", vs_fetch, (unsigned long long)vs_key, clamp,
                (unsigned long long)ps_key, blend, nrt, rtfmt, cmask, cull, depth);
}
static void rows_ready(uint64_t hash)
{
    if (g_rows) fprintf(g_rows, "ready %016llx\n", (unsigned long long)hash);
}

#if defined(PS3RECOMP_RSX_HAL)
/* The target's sink with the rows recorded on their way to it (a sink that
 * hands none over records them all the same). */
static rsx_shader_precompile_sink g_rec;
static const rsx_shader_precompile_sink* g_orig;
static void rec_add(int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key, uint32_t blend, int nrt,
                    uint32_t rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth)
{
    rows_add(vs_fetch, vs_key, clamp, ps_key, blend, nrt, rtfmt, cmask, cull, depth);
    if (g_orig->pso_index_add) g_orig->pso_index_add(vs_fetch, vs_key, clamp, ps_key, blend, nrt, rtfmt, cmask, cull, depth);
}
static void rec_ready(uint64_t hash)
{
    rows_ready(hash);
    if (g_orig->pso_index_ready) g_orig->pso_index_ready(hash);
}
#else
/* Today's pass, compiled into this program (precompile_compare_today.c),
 * calls these in place of the renderer's; they go on to it. */
void rsx_d3d12_pso_index_add(int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key, uint32_t blend, int nrt,
                             uint32_t rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth);
void rsx_d3d12_pso_index_ready(uint64_t hash);
void pcc_index_add(int vs_fetch, uint64_t vs_key, int clamp, uint64_t ps_key, uint32_t blend, int nrt,
                   uint32_t rtfmt, uint32_t cmask, uint32_t cull, uint32_t depth)
{
    rows_add(vs_fetch, vs_key, clamp, ps_key, blend, nrt, rtfmt, cmask, cull, depth);
    rsx_d3d12_pso_index_add(vs_fetch, vs_key, clamp, ps_key, blend, nrt, rtfmt, cmask, cull, depth);
}
void pcc_index_ready(uint64_t hash)
{
    rows_ready(hash);
    rsx_d3d12_pso_index_ready(hash);
}
#endif

static int run_pass(const char* game, const char* eboot, const char* index, int index_only, const char* target,
                    int threads, const char* rows)
{
#if defined(PS3RECOMP_RSX_HAL)
    int t = -1;
    if (!strcmp(target, "d3d12")) t = RSX_PRECOMPILE_D3D12;
    else if (!strcmp(target, "dxbc")) t = RSX_PRECOMPILE_DXBC;
    else if (!strcmp(target, "spv")) t = RSX_PRECOMPILE_SPV;
    g_orig = t >= 0 ? rsx_shader_precompile_sink_of(t) : NULL;
    if (!g_orig) { fprintf(stderr, "precompile_compare: no target %s in this build\n", target); return 2; }
    g_rec = *g_orig;
    g_rec.pso_index_add = rec_add;
    g_rec.pso_index_ready = rec_ready;
    rsx_shader_precompile_use(&g_rec);
#else
    if (strcmp(target, "d3d12")) { fprintf(stderr, "precompile_compare: only d3d12 without PS3RECOMP_RSX_HAL\n"); return 2; }
#endif
    if (rows && !(g_rows = fopen(rows, "wb"))) { fprintf(stderr, "precompile_compare: cannot write %s\n", rows); return 2; }

    char dir[4200];
    snprintf(dir, sizeof dir, "%s/PS3_GAME/USRDIR/shader", game);
    Names arc = { 0, 0, 0 };
    Names all = { 0, 0, 0 };
    list_tree(dir, "", &all);
    for (int i = 0; i < all.n; i++) {
        const size_t l = strlen(all.v[i]);
        if (!strchr(all.v[i], '/') && l > 10 && !strcmp(all.v[i] + l - 10, ".shaderbnd")) names_add(&arc, all.v[i]);
    }
    names_sort(&arc);
    if (!arc.n) { fprintf(stderr, "precompile_compare: no archives in %s\n", dir); return 2; }
    if (index) {
        size_t n = 0;
        uint8_t* text = read_file(index, &n);
        if (!text) { fprintf(stderr, "precompile_compare: no index at %s\n", index); return 2; }
        const int r = rsx_shader_precompile_index((const char*)text, n, index_only);
        printf("index: %d rows%s\n", r, index_only ? " (index only)" : "");
        free(text);
    }
    if (!rsx_shader_precompile_kinds(kUnits, kAlts)) return 2;
    int programs = 0;
    for (int i = 0; i < arc.n; i++) {
        char p[4300];
        snprintf(p, sizeof p, "%s/%s", dir, arc.v[i]);
        programs += queue_archive(p, arc.v[i]);
    }
    programs += queue_elf(eboot);
    if (rsx_shader_precompile_start(threads) != 0) { fprintf(stderr, "precompile_compare: the pass did not start\n"); return 2; }
    rsx_shader_precompile_wait();
    rsx_shader_precompile_stats s;
    rsx_shader_precompile_get(&s);
    printf("pass %s: %d programs (%u not Cg, %u not decompiled), %u shaders (%u duplicates) -> %u compiled, "
           "%u already cached, %u failed; index %u of %u rows found, %u handed over; %.1f s on %d threads\n",
           target, programs, s.skipped, s.bad, s.shaders, s.dups, s.built, s.cached, s.failed, s.index_found,
           s.index_rows, g_nrows, s.seconds, s.threads);
    if (g_rows) fclose(g_rows);
    return s.failed || s.cancelled ? 1 : 0;
}

/* ---- --diff ---- */
static int run_diff(const char* a, const char* b)
{
    Names la = { 0, 0, 0 }, lb = { 0, 0, 0 };
    list_tree(a, "", &la);
    list_tree(b, "", &lb);
    names_sort(&la);
    names_sort(&lb);
    int i = 0, j = 0, same = 0, differ = 0, only_a = 0, only_b = 0, shown = 0;
    while (i < la.n || j < lb.n) {
        const int c = i >= la.n ? 1 : j >= lb.n ? -1 : strcmp(la.v[i], lb.v[j]);
        if (c < 0) { only_a++; if (shown++ < 20) printf("  only in A: %s\n", la.v[i]); i++; continue; }
        if (c > 0) { only_b++; if (shown++ < 20) printf("  only in B: %s\n", lb.v[j]); j++; continue; }
        char pa[4300], pb[4300];
        snprintf(pa, sizeof pa, "%s/%s", a, la.v[i]);
        snprintf(pb, sizeof pb, "%s/%s", b, lb.v[j]);
        size_t na = 0, nb = 0;
        uint8_t* xa = read_file(pa, &na);
        uint8_t* xb = read_file(pb, &nb);
        const int eq = (!xa && !xb) || (xa && xb && na == nb && !memcmp(xa, xb, na));
        if (eq) same++;
        else { differ++; if (shown++ < 20) printf("  differs: %s\n", la.v[i]); }
        free(xa); free(xb);
        i++; j++;
    }
    printf("diff: %d files in A, %d in B; %d the same, %d differ, %d only in A, %d only in B\n",
           la.n, lb.n, same, differ, only_a, only_b);
    return differ || only_a || only_b ? 1 : 0;
}

/* ---- --spv-check ---- */
static int run_spv_check(const char* cache, const char* out)
{
#if defined(PS3RECOMP_RSX_HAL) && defined(PS3RECOMP_RSX_HAL_SPV)
    Names l = { 0, 0, 0 };
    list_tree(cache, "", &l);
    names_sort(&l);
    char raw_dir[4300], post_dir[4300];
    if (out) {
        make_dir(out);
        snprintf(raw_dir, sizeof raw_dir, "%s/raw", out);
        snprintf(post_dir, sizeof post_dir, "%s/post", out);
        make_dir(raw_dir);
        make_dir(post_dir);
    }
    const unsigned opts = gpu_sc_spv_options();
    int modules = 0, vs = 0, ps = 0, stray = 0, bad_hdr = 0, bad_raw = 0, bad_post = 0, shown = 0;
    for (int i = 0; i < l.n; i++) {
        const char* nm = l.v[i];
        const size_t len = strlen(nm);
        /* gpu/spv/<vs|ps>_<16 hex>.spv */
        int ok_name = len == 8 + 3 + 16 + 4 && !strncmp(nm, "gpu/spv/", 8) && (!strncmp(nm + 8, "vs_", 3) ||
                      !strncmp(nm + 8, "ps_", 3)) && !strcmp(nm + len - 4, ".spv");
        uint64_t t = 0;
        for (size_t k = 11; ok_name && k < 27; k++) {
            const char c = nm[k];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) ok_name = 0;
            else t = t << 4 | (uint64_t)d;
        }
        if (!ok_name) { stray++; if (shown++ < 20) printf("  not a module of the spv target: %s\n", nm); continue; }
        modules++;
        const gpu_sc_stage stage = nm[8] == 'v' ? GPU_SC_VS : GPU_SC_PS;
        if (stage == GPU_SC_VS) vs++; else ps++;
        char path[4300];
        snprintf(path, sizeof path, "%s/%s", cache, nm);
        size_t n = 0;
        uint8_t* b = read_file(path, &n);
        if (!b || n < 24 + 20 || memcmp(b, "GSPV", 4) || le32(b + 4) != 1u || le32(b + 8) != (uint32_t)t ||
            le32(b + 12) != (uint32_t)(t >> 32) || le32(b + 16) != (uint32_t)(n - 24) || le32(b + 20) != 0u ||
            (n - 24) % 4 || le32(b + 24) != 0x07230203u) {
            bad_hdr++;
            if (shown++ < 20) printf("  header: %s\n", nm);
            free(b);
            continue;
        }
        const size_t nw = (n - 24) / 4;
        uint32_t* w = (uint32_t*)malloc(nw * 4);
        memcpy(w, b + 24, nw * 4);
        char err[1024];
        if (!gpu_spv_validate(w, nw, err, sizeof err)) {
            bad_raw++;
            if (shown++ < 20) printf("  invalid as stored: %s: %s\n", nm, err);
        }
        uint32_t* pw = NULL;
        size_t pn = 0;
        if (gpu_spv_post(stage, w, nw, opts, &pw, &pn) != GPU_SC_OK || !gpu_spv_validate(pw, pn, err, sizeof err)) {
            bad_post++;
            if (shown++ < 20) printf("  invalid after the post-passes: %s: %s\n", nm, pw ? err : "malformed");
        }
        if (out) {
            char op[4400];
            snprintf(op, sizeof op, "%s/%s", raw_dir, nm + 8);
            write_file(op, w, nw * 4);
            if (pw) {
                snprintf(op, sizeof op, "%s/%s", post_dir, nm + 8);
                write_file(op, pw, pn * 4);
            }
        }
        free(pw);
        free(w);
        free(b);
    }
    printf("spv-check: %d files, %d modules (%d vs, %d ps), %d not modules, %d bad headers, %d invalid as stored, "
           "%d invalid after the post-passes (options %u)\n", l.n, modules, vs, ps, stray, bad_hdr, bad_raw,
           bad_post, opts);
    return modules && !stray && !bad_hdr && !bad_raw && !bad_post ? 0 : 1;
#else
    (void)cache; (void)out;
    fprintf(stderr, "precompile_compare: --spv-check needs the spv target (PS3RECOMP_RSX_HAL_SPV)\n");
    return 2;
#endif
}

int main(int argc, char** argv)
{
    const char* game = NULL;
    const char* eboot = NULL;
    const char* index = NULL;
    const char* target = "d3d12";
    const char* rows = NULL;
    const char* out = NULL;
    int index_only = 0, threads = 4;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const int more = i + 1 < argc;
        if (!strcmp(a, "--diff") && i + 2 < argc) return run_diff(argv[i + 1], argv[i + 2]);
        if (!strcmp(a, "--spv-check") && more) {
            for (int k = i + 2; k + 1 < argc; k++)
                if (!strcmp(argv[k], "--out")) out = argv[k + 1];
            return run_spv_check(argv[i + 1], out);
        }
        if (!strcmp(a, "--game") && more) game = argv[++i];
        else if (!strcmp(a, "--eboot") && more) eboot = argv[++i];
        else if (!strcmp(a, "--index") && more) index = argv[++i];
        else if (!strcmp(a, "--index-only")) index_only = 1;
        else if (!strcmp(a, "--target") && more) target = argv[++i];
        else if (!strcmp(a, "--cache") && more) set_env("PS3_SHADER_CACHE", argv[++i]);
        else if (!strcmp(a, "--threads") && more) threads = atoi(argv[++i]);
        else if (!strcmp(a, "--rows") && more) rows = argv[++i];
        else { game = NULL; break; }
    }
    if (!game) {
        fprintf(stderr,
                "usage: precompile_compare --game DIR [--eboot ELF] [--index TSV] [--index-only] [--target d3d12|dxbc|spv]\n"
                "                          [--cache DIR] [--threads N] [--rows FILE]\n"
                "       precompile_compare --diff DIR_A DIR_B\n"
                "       precompile_compare --spv-check CACHE [--out DIR]\n");
        return 2;
    }
    return run_pass(game, eboot, index, index_only, target, threads, rows);
}
