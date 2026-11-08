/* edat.c -- which file the filesystem layer opens in place of an NPD (EDAT/SDAT) file. See edat.h.
 *
 * The decryptor and its keys are in edat_decrypt.c, which a build with PS3RECOMP_NO_DECRYPT leaves out.
 */

#include "edat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static uint64_t hdr_be64(const uint8_t* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

int edat_is_npd(const char* host_path)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && m[0] == 'N' && m[1] == 'P' && m[2] == 'D' && m[3] == 0;
}

#ifndef PS3RECOMP_NO_DECRYPT
static void make_dir(const char* path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

/* The folder and each folder above it that is missing. */
static void make_dirs(const char* dir)
{
    char path[1024];
    if ((size_t)snprintf(path, sizeof path, "%s", dir) >= sizeof path) return;
    for (char* p = path + 1; *p; ++p) {
        if ((*p != '/' && *p != '\\') || p[-1] == ':') continue;   /* (not the root of C:\) */
        const char c = *p;
        *p = 0;
        make_dir(path);
        *p = c;
    }
    make_dir(path);
}

#endif

/* The plaintext's size, as the NPD file's header gives it. 0 if it cannot be read. */
static int edat_plain_size(const char* host_path, uint64_t* size)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t hdr[0x90];
    const size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n != sizeof hdr) return 0;
    *size = hdr_be64(hdr + 0x88);
    return 1;
}

/* What follows `root` in host_path, or NULL when host_path is not below it. */
static const char* below_root(const char* host_path, const char* root)
{
    if (!root || !root[0]) return NULL;
    size_t n = strlen(root);
    while (n && (root[n - 1] == '/' || root[n - 1] == '\\')) --n;
    for (size_t i = 0; i < n; ++i) {                   /* (either separator; on Windows, either case) */
        int a = (unsigned char)host_path[i], b = (unsigned char)root[i];
        if (!a) return NULL;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#ifdef _WIN32
        a = tolower(a);
        b = tolower(b);
#endif
        if (a != b) return NULL;
    }
    if (host_path[n] != '/' && host_path[n] != '\\') return NULL;
    return host_path + n + 1;
}

/* The player's own decrypted copy of host_path, with PS3_EDAT_PLAIN=<folder>: the file at host_path's path below
 * PS3_VFS_ROOT in that folder, or, for a file of the game's update (PS3_VFS_UPDATE), below that folder's PS3_GAME,
 * named as RPCS3's Decrypt PS3 Binaries names its output ("<name>.unedat"), and only as long as the header says the
 * plaintext is (a copy of another size is not this file's). Then nothing is decrypted and nothing is written. Its
 * path goes into buf; 0 when there is none. */
static int edat_plain_copy(const char* host_path, char* buf, size_t cap)
{
    const char* plain = getenv("PS3_EDAT_PLAIN");
    if (!plain || !plain[0]) return 0;
    const char* rel = below_root(host_path, getenv("PS3_VFS_ROOT"));
    const char* game = "";
    if (!rel) {
        rel = below_root(host_path, getenv("PS3_VFS_UPDATE"));
        game = "PS3_GAME/";
    }
    if (!rel) return 0;
    const int len = snprintf(buf, cap, "%s/%s%s.unedat", plain, game, rel);
    if (len < 0 || (size_t)len >= cap) return 0;
    struct stat st;
    uint64_t size;
    return stat(buf, &st) == 0 && edat_plain_size(host_path, &size) && (uint64_t)st.st_size == size;
}

#ifndef PS3RECOMP_NO_DECRYPT
/* The decrypted copy's path for host_path. Beside it as "<name>.dec"; or, with PS3_EDAT_CACHE=<folder> (`dir`), in
 * that folder, so a game folder that must stay as it is (or cannot be written) is only read. There a copy is named
 * after its source's file name and a hash of the source's whole path (FNV-1a), so two sources of one name never share
 * a copy. snprintf's result. */
static int edat_cache_path(const char* host_path, const char* dir, char* buf, size_t cap)
{
    if (!dir) return snprintf(buf, cap, "%s.dec", host_path);
    uint64_t h = 1469598103934665603ull;
    const char* base = host_path;
    for (const char* p = host_path; *p; ++p) {
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    return snprintf(buf, cap, "%s/%s.%016llx.dec", dir, base, (unsigned long long)h);
}
#endif

const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("PS3_EDAT");
        enabled = (e && e[0] == '0') ? 0 : 1;
    }
    if (!enabled || !host_path || !edat_is_npd(host_path)) return host_path;
    if (edat_plain_copy(host_path, buf, cap)) return buf;

#ifdef PS3RECOMP_NO_DECRYPT
    fprintf(stderr, "[edat] %s is encrypted and PS3_EDAT_PLAIN has no decrypted copy of it; this build cannot decrypt\n",
            host_path);
    return host_path;
#else

    /* PS3_EDAT_CACHE=<folder>: the decrypted copies go there, not beside their sources (the game folder is only read). */
    const char* dir = getenv("PS3_EDAT_CACHE");
    if (dir && !dir[0]) dir = NULL;
    const int n = edat_cache_path(host_path, dir, buf, cap);
    if (n < 0 || (size_t)n >= cap) return host_path;

    struct stat ss, ds;
    if (stat(host_path, &ss) == 0 && stat(buf, &ds) == 0 && ds.st_mtime >= ss.st_mtime)
        return buf;                                    /* cache still valid */
    if (dir) make_dirs(dir);                           /* (the first time it is needed) */

    if (edat_decrypt_file(host_path, buf) != 0) return host_path;
    return buf;
#endif
}
