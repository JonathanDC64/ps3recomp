/*
 * ps3recomp - self-contained tests for where edat_resolve finds or puts an NPD file's plaintext
 *
 * Build and run directly, no framework, from the repository's root:
 *   cc -std=gnu17 -I include -I libs/filesystem -o /tmp/t libs/filesystem/tests/test_edat_paths.c && /tmp/t
 *   clang-cl /nologo /D_CRT_SECURE_NO_WARNINGS /I include /I libs/filesystem /Fe:t.exe libs/filesystem/tests/test_edat_paths.c
 *
 * edat.c is included rather than linked, so the test can make an SDAT with edat.c's own AES and CMAC, each block
 * encrypted and hashed the way edat_decrypt_file reads it, and needs no game file. The plaintext can be in three
 * places: beside the source (no knob), in a cache folder (PS3_EDAT_CACHE), or the player's own decrypted copy
 * (PS3_EDAT_PLAIN, at the file's path below PS3_VFS_ROOT). The test works in ./test_edat_paths.tmp and removes what
 * it made there when every check passes.
 */
#include "../edat.c"

#include <time.h>
#ifdef _WIN32
#include <sys/utime.h>
#define utime _utime
#define utimbuf _utimbuf
#define rmdir _rmdir
#else
#include <unistd.h>
#include <utime.h>
#endif

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                        g_fail++; } } while (0)

#define TMP "test_edat_paths.tmp"
#define SRC TMP "/game/PS3_GAME/USRDIR/a.sdat"
#define SRC2 TMP "/other/a.sdat"
#define CACHE TMP "/cache/x/y"
#define PLAIN TMP "/plain"
#define PLAIN_COPY PLAIN "/PS3_GAME/USRDIR/a.sdat.unedat"
#define NOT_NPD TMP "/game/PS3_GAME/USRDIR/b.dat"

static void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");   /* "" removes it */
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)(v >> 32)); put32(p + 4, (uint32_t)v); }

static int exists(const char* p) { struct stat s; return stat(p, &s) == 0; }
static long long mtime_of(const char* p) { struct stat s; return stat(p, &s) == 0 ? (long long)s.st_mtime : -1; }
static void set_mtime(const char* p, long long t) { struct utimbuf u; u.actime = (time_t)t; u.modtime = (time_t)t; utime(p, &u); }

static int write_file(const char* path, const uint8_t* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int file_is(const char* path, const uint8_t* data, size_t n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t* got = (uint8_t*)malloc(n + 1);
    const size_t r = fread(got, 1, n + 1, f);
    fclose(f);
    const int same = r == n && memcmp(got, data, n) == 0;
    free(got);
    return same;
}

/* The cache's name for a source: its file name, then FNV-1a 64 of its whole path. */
static void cache_name(char* out, size_t cap, const char* src)
{
    uint64_t h = 1469598103934665603ull;
    for (const char* p = src; *p; ++p) h = (h ^ (unsigned char)*p) * 1099511628211ull;
    snprintf(out, cap, "%s/a.sdat.%016llx.dec", CACHE, (unsigned long long)h);
}

static const uint8_t kDevHash[16] = {0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
                                     0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F};
static const uint8_t kDigest[16]  = {0xC0, 0xFF, 0xEE, 0x00, 0x11, 0x22, 0x33, 0x44,
                                     0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};

/* An SDAT of `plain`: version 2 (the block keys seed from dev_hash, every block's IV is the digest), no flags but
 * SDAT's, so the 16-byte hashes form one table after the header and AES-CMAC is the hash. */
static int write_sdat(const char* path, const uint8_t* plain, size_t size, uint32_t block)
{
    const uint32_t blocks = (uint32_t)((size + block - 1) / block);
    const size_t total = 0x100 + (size_t)blocks * 0x10 + (size_t)blocks * block;
    uint8_t* f = (uint8_t*)calloc(1, total);
    memcpy(f, "NPD\0", 4);
    put32(f + 0x04, 2);
    put32(f + 0x08, 3);
    memcpy(f + 0x40, kDigest, 16);
    memcpy(f + 0x60, kDevHash, 16);
    put32(f + 0x80, SDAT_FLAG);
    put32(f + 0x84, block);
    put64(f + 0x88, size);

    uint8_t key[16], rk_file[176];
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(kDevHash[i] ^ SDAT_KEY[i]);
    aes128_expand(key, rk_file);
    for (uint32_t b = 0; b < blocks; b++) {
        const size_t len = (b == blocks - 1 && size % block) ? size % block : block;
        const size_t padded = (len + 15) & ~(size_t)15;
        uint8_t bk[16] = {0}, kres[16], rk[176], prev[16];
        memcpy(bk, kDevHash, 12);
        put32(bk + 12, b);
        aes128_encrypt_block(rk_file, bk, kres);
        aes128_expand(kres, rk);
        uint8_t* enc = f + 0x100 + (size_t)blocks * 0x10 + (size_t)b * block;
        memcpy(prev, kDigest, 16);
        for (size_t off = 0; off < padded; off += 16) {
            uint8_t x[16] = {0};
            const size_t take = off + 16 <= len ? 16 : (off < len ? len - off : 0);
            memcpy(x, plain + (size_t)b * block + off, take);
            for (int i = 0; i < 16; i++) x[i] ^= prev[i];
            aes128_encrypt_block(rk, x, enc + off);
            memcpy(prev, enc + off, 16);
        }
        aes128_cmac(kres, enc, padded, f + 0x100 + (size_t)b * 0x10);
    }
    const int ok = write_file(path, f, total);
    free(f);
    return ok;
}

static void clean(void)
{
    char c1[512], c2[512];
    cache_name(c1, sizeof c1, SRC);
    cache_name(c2, sizeof c2, SRC2);
    const char* files[] = {SRC, SRC ".dec", SRC2, NOT_NPD, c1, c2, PLAIN_COPY};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) remove(files[i]);
    const char* dirs[] = {TMP "/game/PS3_GAME/USRDIR", TMP "/game/PS3_GAME", TMP "/game", TMP "/other",
                          CACHE, TMP "/cache/x", TMP "/cache", PLAIN "/PS3_GAME/USRDIR", PLAIN "/PS3_GAME", PLAIN, TMP};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) rmdir(dirs[i]);
}

int main(void)
{
    clean();
    make_dirs(TMP "/game/PS3_GAME/USRDIR");
    make_dirs(TMP "/other");
    make_dirs(PLAIN "/PS3_GAME/USRDIR");
    set_env("PS3_EDAT_CACHE", NULL);
    set_env("PS3_EDAT_PLAIN", NULL);
    set_env("PS3_VFS_ROOT", NULL);

    uint8_t plain[200];
    for (size_t i = 0; i < sizeof plain; i++) plain[i] = (uint8_t)(i * 7 + 3);
    CHECK(write_sdat(SRC, plain, sizeof plain, 0x40));   /* four blocks, the last one 8 bytes */
    CHECK(write_sdat(SRC2, plain, sizeof plain, 0x40));
    CHECK(write_file(NOT_NPD, plain, sizeof plain));
    char buf[1024], c1[512], c2[512];
    cache_name(c1, sizeof c1, SRC);
    cache_name(c2, sizeof c2, SRC2);

    /* The made SDAT decrypts to its plaintext: everything below rests on it. */
    CHECK(edat_decrypt_file(SRC, SRC ".dec") == 0);
    CHECK(file_is(SRC ".dec", plain, sizeof plain));
    remove(SRC ".dec");

    /* A file that is not NPD is opened as it is. */
    const char* not_npd = NOT_NPD;   /* (one pointer: two equal literals need not be one object) */
    CHECK(edat_resolve(not_npd, buf, sizeof buf) == not_npd);

    /* No knob: "<name>.dec" beside the source, as before. */
    const char* r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, SRC ".dec"));
    CHECK(file_is(SRC ".dec", plain, sizeof plain));
    remove(SRC ".dec");

    /* PS3_EDAT_CACHE: "<cache>/<name>.<hash>.dec", the folder made, nothing beside the source. */
    set_env("PS3_EDAT_CACHE", CACHE);
    CHECK(!exists(CACHE));
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1));
    CHECK(file_is(c1, plain, sizeof plain));
    CHECK(!exists(SRC ".dec"));

    /* Opened again while the copy is newer: reused, not written again. */
    const long long src_time = mtime_of(SRC);
    set_mtime(c1, src_time + 100);
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1) && mtime_of(c1) == src_time + 100);

    /* A source newer than its copy: decrypted again. */
    set_mtime(c1, src_time - 100);
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1) && mtime_of(c1) != src_time - 100 && file_is(c1, plain, sizeof plain));

    /* A source of the same name in another folder: its own copy. */
    r = edat_resolve(SRC2, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c2) && strcmp(c1, c2) != 0 && file_is(c2, plain, sizeof plain));

    /* A path that does not fit the caller's buffer: the source itself. */
    char small[16];
    const char* src = SRC;
    CHECK(edat_resolve(src, small, sizeof small) == src);

    /* PS3_EDAT_PLAIN: the player's copy at the source's path below PS3_VFS_ROOT. Nothing is decrypted or written. The
     * root may end in a separator, and on Windows be written with backslashes and in another case. */
    remove(c1);
    CHECK(write_file(PLAIN_COPY, plain, sizeof plain));
    set_env("PS3_EDAT_PLAIN", PLAIN);
#ifdef _WIN32
    const char* roots[] = {TMP "/game", TMP "/game/", "TEST_EDAT_PATHS.TMP\\GAME\\"};
#else
    const char* roots[] = {TMP "/game", TMP "/game/"};
#endif
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        set_env("PS3_VFS_ROOT", roots[i]);
        r = edat_resolve(SRC, buf, sizeof buf);
        CHECK(r == buf && !strcmp(buf, PLAIN_COPY));
    }
    CHECK(!exists(c1) && !exists(SRC ".dec"));

    /* Not below PS3_VFS_ROOT, or no PS3_VFS_ROOT: no plain copy is looked for. */
    set_env("PS3_VFS_ROOT", TMP "/other");
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1));
    set_env("PS3_VFS_ROOT", TMP "/gam");
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1));
    set_env("PS3_VFS_ROOT", NULL);
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1));

    /* A copy of another size than the header gives is not this file's: the runtime decrypts into its cache. */
    remove(c1);
    set_env("PS3_VFS_ROOT", TMP "/game");
    CHECK(write_file(PLAIN_COPY, plain, sizeof plain - 1));
    r = edat_resolve(SRC, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, c1) && file_is(c1, plain, sizeof plain));

    if (g_fail == 0) {
        clean();
        printf("test_edat_paths: all passed\n");
        return 0;
    }
    printf("test_edat_paths: %d failed (files left in " TMP ")\n", g_fail);
    return 1;
}
