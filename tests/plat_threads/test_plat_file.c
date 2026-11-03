/* tests/plat_threads -- runtime/platform/plat.h's file mapped read-only (what
 * the D3D12 pipeline library is made from).
 *
 * A file is mapped and read through the view; then, as the library's save
 * does, a new copy written under a temporary name is renamed over it while
 * it is still mapped. The rename must succeed, the view must still read the
 * old bytes (also the pages not touched before the rename) and the file the
 * new ones. A missing file and an empty one map to nothing.
 *
 * Exit code 0 if every check passed; each failure prints a line.
 */
#include "platform/plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Larger than a few pages, so pages are still untouched when the file is
 * replaced. */
#define FILE_BYTES (3u * 1024u * 1024u + 123u)

static unsigned char byte_at(size_t i, unsigned seed) { return (unsigned char)((i * 2654435761u + seed) >> 13); }

static int put(const char* path, unsigned seed, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    int ok = 1;
    for (size_t i = 0; i < n && ok; i++) ok = fputc(byte_at(i, seed), f) != EOF;
    if (fclose(f)) ok = 0;
    return ok;
}

/* 1 if the n bytes at p are the seed's. */
static int same(const unsigned char* p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != byte_at(i, seed)) return 0;
    return 1;
}

/* The bytes of the file at path are the seed's (n of them, no more). */
static int file_is(const char* path, unsigned seed, size_t n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    int ok = 1;
    size_t i = 0;
    for (int c; ok && (c = fgetc(f)) != EOF; i++) ok = i < n && (unsigned char)c == byte_at(i, seed);
    fclose(f);
    return ok && i == n;
}

/* The save's move: a temporary renamed over the real name. */
static int move_over(const char* tmp, const char* path)
{
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

int main(void)
{
    char path[64], tmp[96];
#ifdef _WIN32
    snprintf(path, sizeof path, "plat_file_%lu.bin", (unsigned long)GetCurrentProcessId());
#else
    snprintf(path, sizeof path, "plat_file_%ld.bin", (long)getpid());
#endif
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    /* Missing, then empty: nothing mapped, and the map zeroed (it held
     * something else before). */
    static const unsigned char k_other[1];
    remove(path);
    plat_file_map m = { k_other, 1 };
    CHECK(!plat_file_map_read(path, &m));
    CHECK(m.p == NULL && m.n == 0);
    plat_file_unmap(&m);   /* (a zeroed one: nothing) */
    CHECK(put(path, 0, 0));
    m.p = k_other;
    m.n = 1;
    CHECK(!plat_file_map_read(path, &m));
    CHECK(m.p == NULL && m.n == 0);

    /* A file mapped, read through the view. */
    CHECK(put(path, 1, FILE_BYTES));
    CHECK(plat_file_map_read(path, &m));
    CHECK(m.p != NULL && m.n == FILE_BYTES);
    if (!m.p || m.n != FILE_BYTES) {
        remove(path);
        fprintf(stderr, "plat_file: %d check(s) failed\n", g_fail);
        return 1;
    }
    const unsigned char* v = (const unsigned char*)m.p;
    CHECK(same(v, 4096, 1));   /* the first page only, for now */

    /* Replaced while mapped: the rename succeeds, the view keeps the old
     * bytes (all of them, the pages read after the rename too) and the file
     * holds the new ones. */
    CHECK(put(tmp, 2, FILE_BYTES / 2));
    CHECK(move_over(tmp, path));
    CHECK(file_is(path, 2, FILE_BYTES / 2));
    CHECK(same(v, FILE_BYTES, 1));

    /* The new file maps beside the old view. */
    plat_file_map m2;
    CHECK(plat_file_map_read(path, &m2));
    CHECK(m2.p && m2.n == FILE_BYTES / 2 && same((const unsigned char*)m2.p, m2.n, 2));
    plat_file_unmap(&m2);

    plat_file_unmap(&m);
    CHECK(m.p == NULL && m.n == 0);
    plat_file_unmap(&m);   /* (twice: the second is nothing) */

    /* Once unmapped, the file can go. */
    CHECK(remove(path) == 0);
    remove(tmp);

    if (g_fail) { fprintf(stderr, "plat_file: %d check(s) failed\n", g_fail); return 1; }
    printf("plat_file: ok (a %u-byte file mapped, replaced by a rename while mapped)\n", FILE_BYTES);
    return 0;
}
