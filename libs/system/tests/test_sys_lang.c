/* clang -std=gnu17 -I include libs/system/tests/test_sys_lang.c -o /tmp/test_sys_lang && /tmp/test_sys_lang
 *
 * PS3_SYS_LANG (ps3emu/sys_lang.h): a number or a name gives that language; unset, empty or unknown gives English
 * (US), as before the knob. */
#include "ps3emu/sys_lang.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
static void set(const char* v) { _putenv_s("PS3_SYS_LANG", v ? v : ""); }
#else
static void set(const char* v) { if (v) setenv("PS3_SYS_LANG", v, 1); else unsetenv("PS3_SYS_LANG"); }
#endif

int main(void)
{
    set(NULL);
    assert(ps3_sys_lang() == 1);
    set("");
    assert(ps3_sys_lang() == 1);
    set("2");
    assert(ps3_sys_lang() == 2);
    set("18");
    assert(ps3_sys_lang() == 18);
    set("19");
    assert(ps3_sys_lang() == 19);
    set("20");
    assert(ps3_sys_lang() == 1);
    set("4x");
    assert(ps3_sys_lang() == 1);
    set("French");
    assert(ps3_sys_lang() == 2);
    set("de");
    assert(ps3_sys_lang() == 4);
    set("IT");
    assert(ps3_sys_lang() == 5);
    set("es");
    assert(ps3_sys_lang() == 3);
    set("en_GB");
    assert(ps3_sys_lang() == 18);
    set("english-gb");
    assert(ps3_sys_lang() == 18);
    set("english");
    assert(ps3_sys_lang() == 1);
    set("en");
    assert(ps3_sys_lang() == 1);
    set("eng");
    assert(ps3_sys_lang() == 1);
    set("frenchy");
    assert(ps3_sys_lang() == 1);
    set("ja");
    assert(ps3_sys_lang() == 0);
    assert(ps3_sys_lang_parse("klingon") == -1 && ps3_sys_lang_parse(NULL) == -1);
    puts("PS3_SYS_LANG: numbers and names, English (US) otherwise");
    return 0;
}
