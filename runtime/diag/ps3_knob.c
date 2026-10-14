/* Live diagnostic knobs: see include/ps3emu/ps3_knob.h. */
#include <stdlib.h>
#include "ps3emu/ps3_knob.h"

volatile unsigned ps3_env_gen = 1;

void ps3_env_bump(void)
{
    ps3_env_gen++;   /* only the debug console writes it */
}

int ps3_env_int(const char* name, int def)
{
    const char* e = getenv(name);
    return e ? atoi(e) : def;
}
