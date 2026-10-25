#pragma once
/* The checks every guest path passes before it becomes a host path, in one
 * place for the three translators (runtime/syscalls/sys_fs.c,
 * runtime/ppu/ppu_fs.cpp, libs/filesystem/cellFs.c) and save data's names
 * (libs/system/cellSaveData.c). A guest path stays inside the virtual file
 * system: no ".." above its root, no ':' (Windows streams and drives), no
 * control characters, and no Windows device names (CON, NUL, COM1, ...).
 * Header-only, C and C++. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* A Windows device name, whatever the case and with any extension:
 * "con", "NUL.txt", "com1". [b, e) is one path component. */
static inline int ps3_vfs_device_name(const char* b, const char* e)
{
    static const char* const names[] = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$" };
    const char* dot = (const char*)memchr(b, '.', (size_t)(e - b));
    const char* be = dot ? dot : e;
    while (be > b && be[-1] == ' ') be--;   /* (Windows ignores trailing spaces) */
    size_t n = (size_t)(be - b);
    char up[8];
    if (n == 0 || n >= sizeof up) return 0;
    for (size_t i = 0; i < n; i++) up[i] = (char)(b[i] >= 'a' && b[i] <= 'z' ? b[i] - 32 : b[i]);
    up[n] = 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(up, names[i])) return 1;
    return n == 4 && (!memcmp(up, "COM", 3) || !memcmp(up, "LPT", 3)) && up[3] >= '0' && up[3] <= '9';
}

/* One path component [b, e) that may be a file or directory name: not empty,
 * no separator, ':' or control character, not only dots and spaces ("." and
 * ".." are the caller's to handle), not a device name. */
static inline int ps3_vfs_component_ok(const char* b, const char* e)
{
    int other = 0;
    if (b == e) return 0;
    for (const char* p = b; p < e; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':') return 0;
        if (c != '.' && c != ' ') other = 1;
    }
    return other && !ps3_vfs_device_name(b, e);
}

/* A save data directory or file name the title gives: one component. */
static inline int ps3_vfs_name_ok(const char* name)
{
    return name && ps3_vfs_component_ok(name, name + strlen(name));
}

/* `in` with '/' and '\' both separators, empty and "." components dropped and
 * ".." taking the one before it, into out[cap] with '/' separators (a leading
 * and a trailing '/' kept). 1 if it is usable; 0 if it climbs above its root,
 * has a component ps3_vfs_component_ok refuses, or does not fit. A first
 * component of one letter and ':' ("e:/app_home/..." in some titles) is kept
 * as it is. */
static inline int ps3_vfs_clean_path(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    size_t marks[128];   /* where each kept component starts in out */
    int depth = 0;
    const char* p = in;
    if (!in || !out || cap < 2) return 0;
    if (*p == '/' || *p == '\\') out[o++] = '/';
    int first = 1;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char* b = p;
        while (*p && *p != '/' && *p != '\\') p++;
        const char* e = p;
        size_t n = (size_t)(e - b);
        int drive = first && o == 0 && n == 2 && b[1] == ':' &&
                    ((b[0] >= 'a' && b[0] <= 'z') || (b[0] >= 'A' && b[0] <= 'Z'));
        first = 0;
        if (n == 1 && b[0] == '.') continue;
        if (n == 2 && b[0] == '.' && b[1] == '.') {
            if (depth == 0) return 0;   /* above the root */
            o = marks[--depth];
            continue;
        }
        if (!drive && !ps3_vfs_component_ok(b, e)) return 0;
        if (depth == (int)(sizeof marks / sizeof marks[0])) return 0;
        size_t start = o;
        if (o && out[o - 1] != '/') { if (o + 1 >= cap) return 0; out[o++] = '/'; }
        if (o + n >= cap) return 0;
        memcpy(out + o, b, n);
        o += n;
        if (!drive) marks[depth++] = start;   /* (a drive is a root: ".." stops at it) */
    }
    size_t inl = strlen(in);
    if (inl && (in[inl - 1] == '/' || in[inl - 1] == '\\') && o && out[o - 1] != '/') {
        if (o + 1 >= cap) return 0;
        out[o++] = '/';
    }
    out[o] = 0;
    return 1;
}

/* ps3_vfs_clean_path for a translator: 1 and the clean path in out, or 0 and,
 * the first few times, a line in the log naming `who`. */
static inline int ps3_vfs_guest_path(const char* in, char* out, size_t cap, const char* who)
{
    static int said;
    if (ps3_vfs_clean_path(in, out, cap)) return 1;
    if (said < 8) {
        char shown[96];
        size_t i = 0;
        for (; in && in[i] && i + 1 < sizeof shown; i++) {
            unsigned char c = (unsigned char)in[i];
            shown[i] = (char)(c < 0x20 || c == 0x7F ? '?' : c);
        }
        shown[i] = 0;
        said++;
        fprintf(stderr, "[vfs] %s: refused the path \"%s%s\" (it leaves the game's files, or names a device)\n",
                who, shown, in && in[i] ? "..." : "");
    }
    return 0;
}

/* Where a refused guest path goes: a host path every file call fails on, with
 * anything appended to it too (Windows: characters no name may have; POSIX:
 * below a file that is not a directory). */
#ifdef _WIN32
#  define PS3_VFS_REFUSED_PATH "<refused>"
#else
#  define PS3_VFS_REFUSED_PATH "/dev/null/refused"
#endif
