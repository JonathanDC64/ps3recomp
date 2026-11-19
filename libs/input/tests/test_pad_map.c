/* PAD_MAP's rules (pad_map.h): a host side's names, several joined by '|', and "none"; the game's buttons and
 * triggers from a host pad's buttons, the extra ones (paddles, misc, touchpad) included. No pad, OS or SDL needed.
 * cc -std=c17 -I libs/input libs/input/tests/test_pad_map.c -o test_pad_map
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pad_map.h"

#define SELECT   (1u << 0)
#define L2       (1u << 8)
#define R2       (1u << 9)
#define L1       (1u << 10)
#define R1       (1u << 11)
#define TRIANGLE (1u << 12)
#define CROSS    (1u << 14)
#define SQUARE   (1u << 15)
#define PADDLE1  (PAD_EXTRA_PADDLE1 << 16)
#define PADDLE4  (PAD_EXTRA_PADDLE4 << 16)
#define MISC     (PAD_EXTRA_MISC << 16)
#define TOUCHPAD (PAD_EXTRA_TOUCHPAD << 16)

static uint32_t value(const char* v, int* ok) { return pad_map_value(v, strlen(v), ok); }

int main(void)
{
    int ok = 0;
    /* Names: the 16, then the extra ones, in their bits; any case. */
    assert(pad_map_name("select", 6) == 0 && pad_map_name("square", 6) == 15);
    assert(pad_map_name("paddle1", 7) == 16 && pad_map_name("PADDLE4", 7) == 19);
    assert(pad_map_name("misc", 4) == 20 && pad_map_name("touchpad", 8) == 21);
    assert(pad_map_name("paddle5", 7) < 0 && pad_map_name("kick", 4) < 0 && pad_map_name("", 0) < 0);

    /* Values: one name, several, none; anything else is left out whole. */
    assert(value("r1", &ok) == R1 && ok);
    assert(value("r1|paddle1", &ok) == (R1 | PADDLE1) && ok);
    assert(value("Square|misc|touchpad", &ok) == (SQUARE | MISC | TOUCHPAD) && ok);
    assert(value("none", &ok) == 0 && ok);
    assert(value("NONE", &ok) == 0 && ok);
    assert(value("r1|kick", &ok) == 0 && !ok);
    assert(value("r1|", &ok) == 0 && !ok);
    assert(value("", &ok) == 0 && !ok);

    /* As written back: the names in bit order. */
    char text[64];
    pad_map_text(R1 | PADDLE1, text, sizeof text);
    assert(!strcmp(text, "r1|paddle1"));
    pad_map_text(0, text, sizeof text);
    assert(!strcmp(text, "none"));

    uint32_t src[PAD_MAP_GAME];
    uint8_t l2 = 0, r2 = 0;
    pad_map_identity(src);

    /* The identity: each button itself, the extra ones press nothing, the triggers keep their travel. */
    assert(pad_map_apply(src, CROSS | L2 | PADDLE1 | MISC, 90, 7, &l2, &r2) == (CROSS | L2));
    assert(l2 == 90 && r2 == 7);

    /* A kick on a paddle: R1 is R1 or paddle1. */
    src[11] = R1 | PADDLE1;
    assert(pad_map_apply(src, PADDLE1, 0, 0, &l2, &r2) == R1);
    assert(pad_map_apply(src, R1, 0, 0, &l2, &r2) == R1);
    assert(pad_map_apply(src, PADDLE4, 0, 0, &l2, &r2) == 0);

    /* A kick on Square: Square presses R1, and nothing presses the game's Square. */
    pad_map_identity(src);
    src[11] = R1 | SQUARE;
    src[15] = 0;
    assert(pad_map_apply(src, SQUARE, 0, 0, &l2, &r2) == R1);
    assert(pad_map_apply(src, SQUARE | CROSS, 0, 0, &l2, &r2) == (R1 | CROSS));

    /* Swapped buttons, as the Controller page writes them, still swap. */
    pad_map_identity(src);
    src[14] = TRIANGLE;
    src[12] = CROSS;
    assert(pad_map_apply(src, CROSS, 0, 0, &l2, &r2) == TRIANGLE);
    assert(pad_map_apply(src, TRIANGLE | SELECT, 0, 0, &l2, &r2) == (CROSS | SELECT));

    /* Triggers: from a host trigger its travel, from another button 0 or 255, from both the most. */
    pad_map_identity(src);
    src[8] = R2;   /* the game's L2 reads the host's R2 */
    src[9] = L1;   /* the game's R2 reads L1 */
    assert(pad_map_apply(src, R2, 0, 200, &l2, &r2) == L2 && l2 == 200 && r2 == 0);
    assert(pad_map_apply(src, L1, 40, 0, &l2, &r2) == (R2 | L1) && l2 == 0 && r2 == 255);   /* (L1 still itself) */
    assert(pad_map_apply(src, 0, 40, 20, &l2, &r2) == 0 && l2 == 20 && r2 == 0);   /* a trigger short of its bit */
    src[8] = L2 | PADDLE4;
    assert(pad_map_apply(src, PADDLE4, 30, 0, &l2, &r2) == L2 && l2 == 255);
    assert(pad_map_apply(src, L2, 130, 0, &l2, &r2) == L2 && l2 == 130);
    src[8] = 0;
    assert(pad_map_apply(src, L2, 130, 0, &l2, &r2) == 0 && l2 == 0);

    printf("test_pad_map: ok\n");
    return 0;
}
