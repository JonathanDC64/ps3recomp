/* ps3emu/sys_lang.h: the PS3's system language, as a title is told it.
 *
 * cellSysutilGetSystemParamInt(CELL_SYSUTIL_SYSTEMPARAM_ID_LANG) gives it, and
 * the trophy names are read in it (TROP_<nn>.SFM, sceNpTrophy.c). A title with
 * several languages on its disc shows its text in this one, as on a PS3 set to it.
 *
 * PS3_SYS_LANG: a CELL_SYSUTIL_LANG_* number (0-19) or its name (below; a
 * language code such as "fr" or "en-gb" too). Unset, empty or unknown: English
 * (US), 1, as before this knob. Read at each call, so a port may set it before
 * the title asks. Header-only, so code that tests alone (libs/network/tests)
 * links nothing more. */
#ifndef PS3EMU_SYS_LANG_H
#define PS3EMU_SYS_LANG_H

#include <stdlib.h>
#include <string.h>

#define PS3_SYS_LANG_ENGLISH_US 1

/* The language for a PS3_SYS_LANG value; -1 when it names none. */
static inline int ps3_sys_lang_parse(const char* v)
{
    static const struct { const char* name; const char* code; } k_langs[] = {
        {"japanese", "ja"},    {"english", "en"},       {"french", "fr"},     {"spanish", "es"},
        {"german", "de"},      {"italian", "it"},       {"dutch", "nl"},      {"portuguese", "pt"},
        {"russian", "ru"},     {"korean", "ko"},        {"chinese-t", "zh-tw"}, {"chinese-s", "zh-cn"},
        {"finnish", "fi"},     {"swedish", "sv"},       {"danish", "da"},     {"norwegian", "no"},
        {"polish", "pl"},      {"portuguese-br", "pt-br"}, {"english-gb", "en-gb"}, {"turkish", "tr"},
    };
    if (!v || !*v)
        return -1;
    if (v[0] >= '0' && v[0] <= '9') {
        char* end;
        const long n = strtol(v, &end, 10);
        return *end == 0 && n >= 0 && n < (long)(sizeof k_langs / sizeof k_langs[0]) ? (int)n : -1;
    }
    for (int i = 0; i < (int)(sizeof k_langs / sizeof k_langs[0]); i++) {
        const char* a = k_langs[i].name;
        const char* b = k_langs[i].code;
        int same_a = 1, same_b = 1;
        size_t k = 0;
        for (; v[k]; k++) {
            const char c = (char)(v[k] >= 'A' && v[k] <= 'Z' ? v[k] - 'A' + 'a' : v[k] == '_' ? '-' : v[k]);
            same_a = same_a && a[k] == c;
            same_b = same_b && b[k] == c;
            if (!same_a && !same_b)
                break;
        }
        if ((same_a && !v[k] && !a[k]) || (same_b && !v[k] && !b[k]))
            return i;
    }
    return -1;
}

/* The system language now: PS3_SYS_LANG, else English (US). */
static inline int ps3_sys_lang(void)
{
    const int lang = ps3_sys_lang_parse(getenv("PS3_SYS_LANG"));
    return lang < 0 ? PS3_SYS_LANG_ENGLISH_US : lang;
}

#endif
