/*
 * ps3recomp - trophy set reader (TROPHY.TRP + the SFM trophy list)
 *
 * Host-side helper for sceNpTrophy.c. Nothing here is exported to the guest:
 * the HLE NID generator only takes cell / sce / sys function names, and these
 * are all np_trophy_set_*.
 *
 * A title ships its trophy set as PS3_GAME/TROPDIR/<comm id>_<nn>/TROPHY.TRP.
 * The container is big-endian:
 *
 *   0x00  u32 magic 0xDCA24D00, u32 version, u64 file size, u32 entry count,
 *         u32 entry size, u32 dev flag, u8 sha1[20], padding up to 0x40
 *   0x40  entry count x entry size bytes: char name[32], u64 offset, u64 size,
 *         u32 unknown, padding
 *
 * The files are stored as they are. The trophy list is XML behind a signature
 * comment, in TROP_<lang>.SFM, else TROP.SFM. TROPCONF.SFM has the same
 * structure without the names and descriptions, so it is only the last resort.
 * Each <trophy id hidden ttype pid> holds a <name> and a <detail>; the root also
 * holds <title-name> and <title-detail>. Icons are ICON0.PNG (the set) and
 * TROPnnn.PNG (one per trophy id).
 */

#ifndef PS3RECOMP_NP_TROPHY_SET_H
#define PS3RECOMP_NP_TROPHY_SET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_TROPHY_SET_MAX_TROPHIES   128   /* trophy ids are 0..127 */
#define NP_TROPHY_SET_NAME_SIZE      128   /* SceNpTrophyDetails.name */
#define NP_TROPHY_SET_DETAIL_SIZE    1024  /* SceNpTrophyDetails.description */
#define NP_TROPHY_SET_TITLE_SIZE     128   /* SceNpTrophyGameDetails.title */
#define NP_TROPHY_SET_TDETAIL_SIZE   1024  /* SceNpTrophyGameDetails.description */

#define NP_TROPHY_SET_ERR_OPEN    (-1)  /* no such file */
#define NP_TROPHY_SET_ERR_FORMAT  (-2)  /* not a TRP container, or a damaged one */
#define NP_TROPHY_SET_ERR_NO_SFM  (-3)  /* the container has no trophy list */
#define NP_TROPHY_SET_ERR_PARSE   (-4)  /* the trophy list holds no trophies */
#define NP_TROPHY_SET_ERR_NOMEM   (-5)

typedef struct np_trophy_info {
    uint8_t  present;       /* 1 when the list has this id */
    uint8_t  hidden;
    uint8_t  grade;         /* 1 platinum, 2 gold, 3 silver, 4 bronze, 0 unknown */
    int32_t  pid;           /* the platinum this one counts toward, -1 for none */
    char     name[NP_TROPHY_SET_NAME_SIZE];       /* UTF-8, NUL-terminated */
    char     detail[NP_TROPHY_SET_DETAIL_SIZE];
    uint64_t icon_offset;   /* TROPnnn.PNG inside the container */
    uint64_t icon_size;     /* 0: no icon */
} np_trophy_info;

typedef struct np_trophy_set {
    char     trp_path[1024];                       /* host path it was read from */
    char     sfm[32];                              /* the list it came from */
    char     title[NP_TROPHY_SET_TITLE_SIZE];
    char     title_detail[NP_TROPHY_SET_TDETAIL_SIZE];
    uint32_t count;                                /* trophies in the list */
    uint32_t per_grade[5];                         /* indexed by grade */
    uint64_t icon0_offset;                         /* ICON0.PNG */
    uint64_t icon0_size;
    np_trophy_info trophy[NP_TROPHY_SET_MAX_TROPHIES];   /* indexed by trophy id */
} np_trophy_set;

/* Reads the container at a host path and parses its trophy list, preferring
 * TROP_<lang>.SFM (lang < 0 skips that step). Returns 0 or NP_TROPHY_SET_ERR_*. */
int np_trophy_set_load(np_trophy_set* set, const char* trp_path, int lang);

/* Parses one trophy list into a zeroed set (the loader's second half; tests
 * call it directly). Returns 0, or NP_TROPHY_SET_ERR_PARSE for no trophies. */
int np_trophy_set_parse_sfm(np_trophy_set* set, const char* xml, size_t len);

/* Reads size bytes at offset from the container into a malloc'd buffer the
 * caller frees. Returns 0 or NP_TROPHY_SET_ERR_*. */
int np_trophy_set_read_blob(const char* trp_path, uint64_t offset, uint64_t size,
                            void** out, size_t* out_size);

/* Picks the trophy folder in a host TROPDIR: the first name in byte order of
 * the form XXXXnnnnn_nn that holds a TROPHY.TRP. Returns 0 or -1. */
int np_trophy_set_find_folder(const char* host_tropdir, char* name, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_NP_TROPHY_SET_H */
