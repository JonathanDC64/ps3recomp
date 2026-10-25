/*
 * test_vfs_path - the checks every guest path passes before it becomes a host
 * path (include/ps3emu/vfs_path.h): ".." stays inside the virtual file system,
 * and ':', control characters and Windows device names are refused.
 *
 *   clang-cl /I include /Fe:test_vfs_path.exe libs/filesystem/tests/test_vfs_path.c
 *   cc -std=gnu17 -I include -o test_vfs_path libs/filesystem/tests/test_vfs_path.c
 */
#include "ps3emu/vfs_path.h"

#include <assert.h>
#include <stdio.h>

/* The cleaned path, or NULL if it is refused. */
static const char* clean(const char* in)
{
    static char out[256];
    return ps3_vfs_clean_path(in, out, sizeof out) ? out : NULL;
}

static int same(const char* in, const char* want)
{
    const char* got = clean(in);
    if (!got || strcmp(got, want)) {
        printf("  %s -> %s, want %s\n", in, got ? got : "(refused)", want);
        return 0;
    }
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Paths titles use come through as they are. */
    assert(same("/dev_hdd0/game/BLUS30443/USRDIR/data.bin", "/dev_hdd0/game/BLUS30443/USRDIR/data.bin"));
    assert(same("/dev_bdvd", "/dev_bdvd"));
    assert(same("/dev_hdd1/", "/dev_hdd1/"));
    assert(same("/", "/"));
    assert(same("", ""));
    assert(same("Data/Resources/first.xml", "Data/Resources/first.xml"));   /* relative */
    assert(same("e:/app_home/x.bin", "e:/app_home/x.bin"));
    assert(same("/dev_hdd0/My Save (1)/a-b_c.dat", "/dev_hdd0/My Save (1)/a-b_c.dat"));
    assert(same("/dev_hdd0/.hidden/x", "/dev_hdd0/.hidden/x"));

    /* Separators, "." and ".." inside the tree. */
    assert(same("/dev_hdd0//game/./x", "/dev_hdd0/game/x"));
    assert(same("\\dev_hdd0\\game\\x", "/dev_hdd0/game/x"));
    assert(same("/dev_hdd0/game/../tmp/x", "/dev_hdd0/tmp/x"));
    assert(same("/dev_hdd0/..", "/"));
    assert(same("/dev_hdd0/../dev_bdvd/x/", "/dev_bdvd/x/"));
    assert(same("a/b/../../c", "c"));

    /* Above the root, from any start. */
    assert(!clean("/.."));
    assert(!clean("/dev_hdd0/../../x"));
    assert(!clean("/dev_hdd0/game/USRDIR/../../../../x"));
    assert(!clean("..\\x"));
    assert(!clean("e:/../x"));
    assert(!clean("/dev_hdd0/a/..\\..\\..\\x"));

    /* ':' (streams, drives past the first part), control characters, names
     * made of dots and spaces. */
    assert(!clean("/dev_hdd0/x.dat:stream"));
    assert(!clean("/dev_hdd0/c:/x"));
    assert(!clean("/c:/x"));
    assert(!clean("/dev_hdd0/a\nb"));
    assert(!clean("/dev_hdd0/a\x7f"));
    assert(!clean("/dev_hdd0/.../x"));
    assert(!clean("/dev_hdd0/. ./x"));
    assert(!clean("/dev_hdd0/ /x"));

    /* Windows device names, any case, with an extension or trailing spaces. */
    assert(!clean("/dev_hdd0/CON"));
    assert(!clean("/dev_hdd0/nul.txt"));
    assert(!clean("/dev_hdd0/Com1/x"));
    assert(!clean("/dev_hdd0/lpt9.log"));
    assert(!clean("/dev_hdd0/aux .bin"));
    assert(!clean("/dev_hdd0/conout$"));
    assert(same("/dev_hdd0/CONFIG", "/dev_hdd0/CONFIG"));
    assert(same("/dev_hdd0/com10", "/dev_hdd0/com10"));
    assert(same("/dev_hdd0/console.txt", "/dev_hdd0/console.txt"));
    assert(same("/dev_hdd0/nul_x", "/dev_hdd0/nul_x"));

    /* What does not fit is refused, not cut. */
    {
        char longp[300], out[64];
        memset(longp, 'a', sizeof longp - 1);
        longp[0] = '/';
        longp[sizeof longp - 1] = 0;
        assert(!ps3_vfs_clean_path(longp, out, sizeof out));
        assert(ps3_vfs_clean_path("/abc", out, 5) && !strcmp(out, "/abc"));
        assert(!ps3_vfs_clean_path("/abcd", out, 5));
    }

    /* Save names: one component. */
    assert(ps3_vfs_name_ok("BLUS30443-AUTOSAVE") && ps3_vfs_name_ok("ICON0.PNG"));
    assert(!ps3_vfs_name_ok("") && !ps3_vfs_name_ok(".") && !ps3_vfs_name_ok(".."));
    assert(!ps3_vfs_name_ok("a/b") && !ps3_vfs_name_ok("a\\b") && !ps3_vfs_name_ok("a:b"));
    assert(!ps3_vfs_name_ok("PRN") && !ps3_vfs_name_ok(NULL));

    /* A refusal goes to the log once, the guest's bytes shown safely. */
    {
        char out[64];
        assert(!ps3_vfs_guest_path("/dev_hdd0/../../x\x01", out, sizeof out, "test"));
        assert(ps3_vfs_guest_path("/dev_hdd0/x", out, sizeof out, "test") && !strcmp(out, "/dev_hdd0/x"));
    }

    printf("test_vfs_path: all passed\n");
    return 0;
}
