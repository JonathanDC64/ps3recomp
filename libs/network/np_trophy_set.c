/*
 * ps3recomp - trophy set reader (TROPHY.TRP + the SFM trophy list)
 *
 * See np_trophy_set.h for the formats. The list parser is a small tag scanner,
 * not a general XML parser: it does not care about line breaks or attribute
 * order, decodes the five named entities and numeric character references,
 * passes CDATA through, and keeps UTF-8 as it is. Strings longer than the
 * firmware buffers are cut on a character boundary.
 */

#include "np_trophy_set.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define np_fseek _fseeki64
#define np_ftell _ftelli64
#else
#include <dirent.h>
#include <sys/stat.h>
#define np_fseek fseeko
#define np_ftell ftello
#endif

#define TRP_MAGIC          0xDCA24D00u
#define TRP_HEADER_SIZE    0x40
#define TRP_MAX_ENTRIES    4096
#define TRP_MAX_SFM        (8u << 20)
#define TRP_MAX_BLOB       (16u << 20)

/* ---------------------------------------------------------------------------
 * Small helpers
 * -----------------------------------------------------------------------*/

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t* p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static int starts_with(const char* p, const char* end, const char* lit)
{
    size_t n = strlen(lit);
    return (size_t)(end - p) >= n && memcmp(p, lit, n) == 0;
}

static const char* find_lit(const char* p, const char* end, const char* lit)
{
    size_t n = strlen(lit);
    for (; (size_t)(end - p) >= n; p++)
        if (memcmp(p, lit, n) == 0)
            return p;
    return NULL;
}

static int ascii_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.';
}

/* Copies n bytes of UTF-8 into dst as a NUL-terminated string of at most cap-1
 * bytes. A cut never lands inside a multi-byte character. */
static void utf8_copy(char* dst, size_t cap, const char* src, size_t n)
{
    if (!cap)
        return;
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Appends a code point as UTF-8. One that does not fit is dropped whole. */
static size_t put_utf8(char* buf, size_t pos, size_t cap, uint32_t cp)
{
    unsigned char t[4];
    size_t n;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        t[0] = (unsigned char)cp; n = 1;
    } else if (cp < 0x800) {
        t[0] = (unsigned char)(0xC0 | (cp >> 6));
        t[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        t[0] = (unsigned char)(0xE0 | (cp >> 12));
        t[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        t[0] = (unsigned char)(0xF0 | (cp >> 18));
        t[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (pos + n > cap)
        return pos;
    memcpy(buf + pos, t, n);
    return pos + n;
}

/* Decodes the character data in [s, e) into dst (see utf8_copy for the cut). */
static void decode_text(char* dst, size_t cap, const char* s, const char* e)
{
    char tmp[4096];
    size_t n = 0;

    while (s < e && n < sizeof tmp) {
        if (*s == '&') {
            size_t room = (size_t)(e - s) < 12 ? (size_t)(e - s) : 12;
            const char* semi = (const char*)memchr(s, ';', room);
            if (semi) {
                const char* nm = s + 1;
                size_t len = (size_t)(semi - nm);
                uint32_t cp = 0;
                int ok = 1;
                if (len >= 2 && nm[0] == '#') {
                    int hex = (nm[1] == 'x' || nm[1] == 'X');
                    const char* d = nm + (hex ? 2 : 1);
                    if (d == semi)
                        ok = 0;
                    for (; ok && d < semi; d++) {
                        int v;
                        if (*d >= '0' && *d <= '9') v = *d - '0';
                        else if (hex && *d >= 'a' && *d <= 'f') v = *d - 'a' + 10;
                        else if (hex && *d >= 'A' && *d <= 'F') v = *d - 'A' + 10;
                        else { ok = 0; break; }
                        cp = cp * (hex ? 16u : 10u) + (uint32_t)v;
                        if (cp > 0x10FFFF)
                            cp = 0x110000;   /* keeps the loop bounded; put_utf8 maps it to U+FFFD */
                    }
                } else if (len == 3 && memcmp(nm, "amp", 3) == 0) {
                    cp = '&';
                } else if (len == 2 && memcmp(nm, "lt", 2) == 0) {
                    cp = '<';
                } else if (len == 2 && memcmp(nm, "gt", 2) == 0) {
                    cp = '>';
                } else if (len == 4 && memcmp(nm, "quot", 4) == 0) {
                    cp = '"';
                } else if (len == 4 && memcmp(nm, "apos", 4) == 0) {
                    cp = '\'';
                } else {
                    ok = 0;
                }
                if (ok) {
                    n = put_utf8(tmp, n, sizeof tmp, cp);
                    s = semi + 1;
                    continue;
                }
            }
            tmp[n++] = *s++;   /* not an entity we know: keep it as written */
        } else if (*s == '<' && starts_with(s, e, "<![CDATA[")) {
            const char* c = s + 9;
            const char* ce = find_lit(c, e, "]]>");
            const char* stop = ce ? ce : e;
            while (c < stop && n < sizeof tmp)
                tmp[n++] = *c++;
            s = ce ? ce + 3 : e;
        } else {
            tmp[n++] = *s++;
        }
    }
    utf8_copy(dst, cap, tmp, n);
}

/* The end of an element's character data: the next '<' that does not open a
 * CDATA section. */
static const char* text_end(const char* p, const char* end)
{
    while (p < end) {
        if (*p == '<') {
            if (!starts_with(p, end, "<![CDATA["))
                return p;
            {
                const char* ce = find_lit(p + 9, end, "]]>");
                p = ce ? ce + 3 : end;
            }
            continue;
        }
        p++;
    }
    return end;
}

typedef struct {
    char name[32];
    char id[16];
    char hidden[8];
    char ttype[4];
    char pid[16];
    int  self_closing;
} sfm_tag;

/* p is at the '<' of a start tag. Returns the position after its '>'. */
static const char* parse_tag(const char* p, const char* end, sfm_tag* t)
{
    size_t n = 0;
    memset(t, 0, sizeof *t);
    p++;
    while (p < end && is_name_char(*p)) {
        if (n < sizeof t->name - 1)
            t->name[n++] = *p;
        p++;
    }
    for (;;) {
        char an[32];
        size_t al = 0;
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p >= end)
            return end;
        if (*p == '>')
            return p + 1;
        if (*p == '/') {
            t->self_closing = 1;
            p++;
            continue;
        }
        if (!is_name_char(*p)) {
            p++;
            continue;
        }
        while (p < end && is_name_char(*p)) {
            if (al < sizeof an - 1)
                an[al++] = *p;
            p++;
        }
        an[al] = '\0';
        while (p < end && isspace((unsigned char)*p))
            p++;
        if (p < end && *p == '=') {
            const char* vs;
            const char* ve;
            char* dst = NULL;
            size_t cap = 0;
            p++;
            while (p < end && isspace((unsigned char)*p))
                p++;
            if (p < end && (*p == '"' || *p == '\'')) {
                char q = *p++;
                vs = p;
                while (p < end && *p != q)
                    p++;
                ve = p;
                if (p < end)
                    p++;
            } else {
                vs = p;
                while (p < end && !isspace((unsigned char)*p) && *p != '>' && *p != '/')
                    p++;
                ve = p;
            }
            if (strcmp(an, "id") == 0)          { dst = t->id;     cap = sizeof t->id; }
            else if (strcmp(an, "hidden") == 0) { dst = t->hidden; cap = sizeof t->hidden; }
            else if (strcmp(an, "ttype") == 0)  { dst = t->ttype;  cap = sizeof t->ttype; }
            else if (strcmp(an, "pid") == 0)    { dst = t->pid;    cap = sizeof t->pid; }
            if (dst)
                decode_text(dst, cap, vs, ve);
        }
    }
}

static long parse_long(const char* s, long def)
{
    char* e;
    long v;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return def;
    v = strtol(s, &e, 10);
    return e == s ? def : v;
}

static uint8_t grade_of(char t)
{
    switch (toupper((unsigned char)t)) {
    case 'P': return 1;
    case 'G': return 2;
    case 'S': return 3;
    case 'B': return 4;
    default:  return 0;
    }
}

/* ---------------------------------------------------------------------------
 * The trophy list
 * -----------------------------------------------------------------------*/

int np_trophy_set_parse_sfm(np_trophy_set* set, const char* xml, size_t len)
{
    const char* p = xml;
    const char* end = xml + len;
    np_trophy_info* cur = NULL;
    int got_title = 0, got_tdetail = 0;
    uint32_t count = 0;

    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        p += 3;

    while (p < end) {
        const char* lt = (const char*)memchr(p, '<', (size_t)(end - p));
        const char* c;
        sfm_tag t;
        if (!lt)
            break;
        p = lt;
        if (starts_with(p, end, "<!--")) {
            c = find_lit(p + 4, end, "-->");
            p = c ? c + 3 : end;
            continue;
        }
        if (starts_with(p, end, "<![CDATA[")) {
            c = find_lit(p + 9, end, "]]>");
            p = c ? c + 3 : end;
            continue;
        }
        if (starts_with(p, end, "<?")) {
            c = find_lit(p + 2, end, "?>");
            p = c ? c + 2 : end;
            continue;
        }
        if (starts_with(p, end, "<!")) {
            c = (const char*)memchr(p, '>', (size_t)(end - p));
            p = c ? c + 1 : end;
            continue;
        }
        if (starts_with(p, end, "</")) {
            const char* q = p + 2;
            size_t n = 0;
            char nm[32];
            while (q < end && is_name_char(*q) && n < sizeof nm - 1)
                nm[n++] = *q++;
            nm[n] = '\0';
            if (strcmp(nm, "trophy") == 0)
                cur = NULL;
            c = (const char*)memchr(p, '>', (size_t)(end - p));
            p = c ? c + 1 : end;
            continue;
        }

        p = parse_tag(p, end, &t);
        if (strcmp(t.name, "trophy") == 0) {
            long id = parse_long(t.id, -1);
            cur = NULL;
            if (id >= 0 && id < NP_TROPHY_SET_MAX_TROPHIES) {
                long pid = parse_long(t.pid, -1);
                cur = &set->trophy[id];
                if (!cur->present)
                    count++;
                cur->present = 1;
                cur->hidden = ascii_ieq(t.hidden, "yes") ? 1 : 0;
                cur->grade = grade_of(t.ttype[0]);
                cur->pid = (pid >= 0 && pid < NP_TROPHY_SET_MAX_TROPHIES) ? (int32_t)pid : -1;
            }
            if (t.self_closing)
                cur = NULL;
        } else if (!t.self_closing) {
            const char* te = text_end(p, end);
            if (strcmp(t.name, "name") == 0) {
                if (cur)
                    decode_text(cur->name, sizeof cur->name, p, te);
            } else if (strcmp(t.name, "detail") == 0) {
                if (cur)
                    decode_text(cur->detail, sizeof cur->detail, p, te);
            } else if (strcmp(t.name, "title-name") == 0) {
                if (!cur && !got_title) {
                    decode_text(set->title, sizeof set->title, p, te);
                    got_title = 1;
                }
            } else if (strcmp(t.name, "title-detail") == 0) {
                if (!cur && !got_tdetail) {
                    decode_text(set->title_detail, sizeof set->title_detail, p, te);
                    got_tdetail = 1;
                }
            } else {
                continue;   /* an element we do not read: scan on from its start tag */
            }
            p = te;
        }
    }

    set->count = count;
    memset(set->per_grade, 0, sizeof set->per_grade);
    for (int i = 0; i < NP_TROPHY_SET_MAX_TROPHIES; i++)
        if (set->trophy[i].present)
            set->per_grade[set->trophy[i].grade]++;
    return count ? 0 : NP_TROPHY_SET_ERR_PARSE;
}

/* ---------------------------------------------------------------------------
 * The container
 * -----------------------------------------------------------------------*/

typedef struct {
    uint64_t offset, size;
    int      found;
} trp_entry_ref;

int np_trophy_set_load(np_trophy_set* set, const char* trp_path, int lang)
{
    uint8_t hdr[TRP_HEADER_SIZE];
    uint8_t* table = NULL;
    char* sfm = NULL;
    char want[32];
    trp_entry_ref e_lang = {0}, e_trop = {0}, e_conf = {0};
    const trp_entry_ref* pick;
    const char* pick_name;
    uint32_t count, esize;
    int64_t fsize;
    int rc = NP_TROPHY_SET_ERR_FORMAT;
    FILE* f;

    memset(set, 0, sizeof *set);
    f = fopen(trp_path, "rb");
    if (!f)
        return NP_TROPHY_SET_ERR_OPEN;

    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || be32(hdr) != TRP_MAGIC)
        goto out;
    count = be32(hdr + 16);
    esize = be32(hdr + 20);
    if (np_fseek(f, 0, SEEK_END) != 0 || (fsize = (int64_t)np_ftell(f)) < TRP_HEADER_SIZE)
        goto out;
    if (count == 0 || count > TRP_MAX_ENTRIES || esize < 52 || esize > 1024 ||
        TRP_HEADER_SIZE + (int64_t)count * esize > fsize)
        goto out;

    table = (uint8_t*)malloc((size_t)count * esize);
    if (!table) {
        rc = NP_TROPHY_SET_ERR_NOMEM;
        goto out;
    }
    if (np_fseek(f, TRP_HEADER_SIZE, SEEK_SET) != 0 ||
        fread(table, esize, count, f) != count)
        goto out;

    snprintf(want, sizeof want, "TROP_%02d.SFM", lang);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = table + (size_t)i * esize;
        char name[33];
        uint64_t off = be64(e + 32), size = be64(e + 40);
        memcpy(name, e, 32);
        name[32] = '\0';
        if (size == 0 || off > (uint64_t)fsize || size > (uint64_t)fsize - off)
            continue;   /* points outside the file */
        if (lang >= 0 && strcmp(name, want) == 0) {
            e_lang.offset = off; e_lang.size = size; e_lang.found = 1;
        } else if (strcmp(name, "TROP.SFM") == 0) {
            e_trop.offset = off; e_trop.size = size; e_trop.found = 1;
        } else if (strcmp(name, "TROPCONF.SFM") == 0) {
            e_conf.offset = off; e_conf.size = size; e_conf.found = 1;
        } else if (strcmp(name, "ICON0.PNG") == 0) {
            set->icon0_offset = off;
            set->icon0_size = size;
        } else if (strlen(name) == 11 && memcmp(name, "TROP", 4) == 0 &&
                   isdigit((unsigned char)name[4]) && isdigit((unsigned char)name[5]) &&
                   isdigit((unsigned char)name[6]) && strcmp(name + 7, ".PNG") == 0) {
            int id = (name[4] - '0') * 100 + (name[5] - '0') * 10 + (name[6] - '0');
            if (id < NP_TROPHY_SET_MAX_TROPHIES) {
                set->trophy[id].icon_offset = off;
                set->trophy[id].icon_size = size;
            }
        }
    }

    if (e_lang.found)      { pick = &e_lang; pick_name = want; }
    else if (e_trop.found) { pick = &e_trop; pick_name = "TROP.SFM"; }
    else if (e_conf.found) { pick = &e_conf; pick_name = "TROPCONF.SFM"; }
    else {
        rc = NP_TROPHY_SET_ERR_NO_SFM;
        goto out;
    }
    if (pick->size > TRP_MAX_SFM)
        goto out;
    sfm = (char*)malloc((size_t)pick->size + 1);
    if (!sfm) {
        rc = NP_TROPHY_SET_ERR_NOMEM;
        goto out;
    }
    if (np_fseek(f, (int64_t)pick->offset, SEEK_SET) != 0 ||
        fread(sfm, 1, (size_t)pick->size, f) != (size_t)pick->size)
        goto out;
    sfm[pick->size] = '\0';

    rc = np_trophy_set_parse_sfm(set, sfm, (size_t)pick->size);
    snprintf(set->sfm, sizeof set->sfm, "%s", pick_name);
    snprintf(set->trp_path, sizeof set->trp_path, "%s", trp_path);

out:
    free(sfm);
    free(table);
    fclose(f);
    return rc;
}

int np_trophy_set_read_blob(const char* trp_path, uint64_t offset, uint64_t size,
                            void** out, size_t* out_size)
{
    FILE* f;
    void* buf;
    *out = NULL;
    *out_size = 0;
    if (size == 0 || size > TRP_MAX_BLOB)
        return NP_TROPHY_SET_ERR_FORMAT;
    f = fopen(trp_path, "rb");
    if (!f)
        return NP_TROPHY_SET_ERR_OPEN;
    buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NP_TROPHY_SET_ERR_NOMEM;
    }
    if (np_fseek(f, (int64_t)offset, SEEK_SET) != 0 ||
        fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NP_TROPHY_SET_ERR_FORMAT;
    }
    fclose(f);
    *out = buf;
    *out_size = (size_t)size;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Finding the folder
 * -----------------------------------------------------------------------*/

static int folder_name_ok(const char* n)
{
    if (strlen(n) != 12 || n[9] != '_')
        return 0;
    for (int i = 0; i < 4; i++)
        if (!isupper((unsigned char)n[i]))
            return 0;
    for (int i = 4; i < 9; i++)
        if (!isdigit((unsigned char)n[i]))
            return 0;
    return isdigit((unsigned char)n[10]) && isdigit((unsigned char)n[11]);
}

static void consider_folder(const char* dir, const char* n, char* best)
{
    char path[1100];
    FILE* f;
    if (!folder_name_ok(n) || (best[0] && strcmp(n, best) >= 0))
        return;
    snprintf(path, sizeof path, "%s/%s/TROPHY.TRP", dir, n);
    f = fopen(path, "rb");
    if (!f)
        return;
    fclose(f);
    memcpy(best, n, 13);
}

int np_trophy_set_find_folder(const char* host_tropdir, char* name, size_t cap)
{
    char best[13] = "";
#ifdef _WIN32
    char pattern[1100];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", host_tropdir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            consider_folder(host_tropdir, fd.cFileName, best);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(host_tropdir);
    struct dirent* de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL)
        consider_folder(host_tropdir, de->d_name, best);
    closedir(d);
#endif
    if (!best[0] || cap < sizeof best)
        return -1;
    memcpy(name, best, sizeof best);
    return 0;
}
