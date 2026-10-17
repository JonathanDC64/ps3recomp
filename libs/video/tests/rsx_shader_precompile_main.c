/*
 * rsx_shader_precompile -- fill the D3D12 backend's shader disk cache from a
 * title's Cg binaries, before the title ever runs.
 *
 *   rsx_shader_precompile [--unit U=KIND[,U=KIND...]] [--alt U=KIND[,...]]
 *                         <file|dir> ...
 *
 * Each file is a CgBinaryProgram (.vpo/.fpo as the SDK's cgc writes them);
 * anything else is skipped. A directory is searched recursively. The work
 * is rsx_shader_precompile.c's (what is built for each program is described
 * in rsx_shader_precompile.h; KIND: cube, shadow = depth compare, d2c =
 * depth read as colour), which a title can also run at startup.
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

#include "rsx_shader_precompile.h"

uint8_t* vm_base = NULL;

/* What the runtime library expects from a game's loader; nothing runs here. */
const uint64_t function_table[2] = { 0, 0 };
const uint64_t function_table_count = 0;

const char* rsx_d3d12_shader_cache_path(void);

static void add_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "rsx_shader_precompile: cannot open %s\n", path); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) == (size_t)n) rsx_shader_precompile_add(path, b, (size_t)n);
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

int main(int argc, char** argv)
{
    int first = 1;
    const char* units = NULL;
    const char* alts = NULL;
    while (first + 1 < argc && (!strcmp(argv[first], "--unit") || !strcmp(argv[first], "--alt"))) {
        if (!strcmp(argv[first], "--unit")) units = argv[first + 1]; else alts = argv[first + 1];
        first += 2;
    }
    if (!rsx_shader_precompile_kinds(units, alts)) {
        fprintf(stderr, "rsx_shader_precompile: bad --unit/--alt list (want U=cube|shadow|d2c,...)\n");
        return 2;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: rsx_shader_precompile [--unit U=KIND,...] [--alt U=KIND,...] <file|dir> ...\n"
                        "       KIND = cube | shadow | d2c; PS3_SHADER_CACHE = cache directory\n");
        return 2;
    }
    const char* dir = rsx_d3d12_shader_cache_path();   /* also creates it */
    if (!dir[0]) { fprintf(stderr, "rsx_shader_precompile: the cache is off (PS3_SHADER_CACHE=0)\n"); return 2; }
    for (int i = first; i < argc; i++) add_path(argv[i]);

    int nt = 0;
    { const char* e = getenv("PRECOMPILE_THREADS"); if (e && atoi(e) > 0) nt = atoi(e); }
    if (rsx_shader_precompile_start(nt) == 0) rsx_shader_precompile_wait();
    rsx_shader_precompile_stats st;
    rsx_shader_precompile_get(&st);
    printf("rsx_shader_precompile: %u programs (%u not Cg binaries, %u not decompiled), "
           "%u shaders (%u duplicate texts dropped) -> %u compiled, %u already cached, %u failed; %.1fs on %d threads; cache %s\n",
           st.programs - st.skipped, st.skipped, st.bad, st.shaders, st.dups, st.built, st.cached, st.failed,
           st.seconds, st.threads, dir);
    return st.failed ? 1 : 0;
}
