/*
 * ps3recomp - tests for edat_resolve in a build with no decryptor (PS3RECOMP_NO_DECRYPT)
 *
 * Build and run directly, no framework, from the repository's root:
 *   cc -std=gnu17 -I include -I libs/filesystem -o /tmp/t libs/filesystem/tests/test_edat_no_decrypt.c && /tmp/t
 *   clang-cl /nologo /D_CRT_SECURE_NO_WARNINGS /I include /I libs/filesystem /Fe:t.exe libs/filesystem/tests/test_edat_no_decrypt.c && t.exe
 *
 * Only edat.c is built, so this also shows that it needs nothing from edat_decrypt.c. An NPD file here is only a header:
 * nothing decrypts it. The test works in ./test_edat_no_decrypt.tmp and removes what it made when every check passes.
 */
#define PS3RECOMP_NO_DECRYPT 1
#include "../edat.c"

#ifdef _WIN32
#define rmdir _rmdir
#else
#include <unistd.h>
#endif

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do { if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
                        g_fail++; } } while (0)

#define TMP "test_edat_no_decrypt.tmp"
#define GAME TMP "/game"
#define USRDIR GAME "/PS3_GAME/USRDIR"
#define SRC USRDIR "/data.sdat"
#define NOT_NPD USRDIR "/plain.bin"
#define CACHE TMP "/cache"
#define PLAIN TMP "/plain"
#define PLAIN_DIR PLAIN "/PS3_GAME/USRDIR"
#define PLAIN_COPY PLAIN_DIR "/data.sdat.unedat"

enum { PLAIN_SIZE = 32 };

static void set_env(const char* name, const char* value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

static int exists(const char* p) { struct stat s; return stat(p, &s) == 0; }

static int write_file(const char* path, const uint8_t* data, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

static void clean(void)
{
    const char* files[] = {SRC, SRC ".dec", NOT_NPD, PLAIN_COPY};
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) remove(files[i]);
    const char* dirs[] = {USRDIR, GAME "/PS3_GAME", GAME, PLAIN_DIR, PLAIN "/PS3_GAME", PLAIN, CACHE, TMP};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) rmdir(dirs[i]);
}

int main(void)
{
    clean();
    const char* dirs[] = {TMP, GAME, GAME "/PS3_GAME", USRDIR, PLAIN, PLAIN "/PS3_GAME", PLAIN_DIR};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
#ifdef _WIN32
        _mkdir(dirs[i]);
#else
        mkdir(dirs[i], 0777);
#endif
    }

    uint8_t hdr[0x90] = {'N', 'P', 'D', 0};
    hdr[0x8F] = PLAIN_SIZE;   /* the plaintext's size, big-endian at 0x88 */
    CHECK(write_file(SRC, hdr, sizeof hdr));
    CHECK(write_file(NOT_NPD, (const uint8_t*)"not npd", 7));
    set_env("PS3_EDAT", NULL);
    set_env("PS3_VFS_ROOT", GAME);
    set_env("PS3_VFS_UPDATE", NULL);
    set_env("PS3_EDAT_PLAIN", PLAIN);
    set_env("PS3_EDAT_CACHE", CACHE);

    char buf[512];
    const char* path = SRC;
    const char* other = NOT_NPD;
    CHECK(edat_is_npd(SRC) && !edat_is_npd(NOT_NPD));

    /* A file that is not NPD: opened as it is. */
    CHECK(edat_resolve(other, buf, sizeof buf) == other);

    /* An NPD file with no copy: the file itself, and nothing written anywhere. */
    CHECK(edat_resolve(path, buf, sizeof buf) == path);
    CHECK(!exists(CACHE) && !exists(SRC ".dec"));

    /* A copy of another size is not this file's. */
    uint8_t plain[PLAIN_SIZE + 1];
    memset(plain, 0x5A, sizeof plain);
    CHECK(write_file(PLAIN_COPY, plain, PLAIN_SIZE + 1));
    CHECK(edat_resolve(path, buf, sizeof buf) == path);

    /* The player's decrypted copy, of the size the header gives: opened in its place. */
    CHECK(write_file(PLAIN_COPY, plain, PLAIN_SIZE));
    const char* r = edat_resolve(path, buf, sizeof buf);
    CHECK(r == buf && !strcmp(buf, PLAIN_COPY));
    CHECK(!exists(CACHE) && !exists(SRC ".dec"));

    if (!g_fail) {
        clean();
        printf("test_edat_no_decrypt: all passed\n");
        return 0;
    }
    printf("test_edat_no_decrypt: %d failed (files left in " TMP ")\n", g_fail);
    return 1;
}
