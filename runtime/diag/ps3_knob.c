/* Live diagnostic knobs: see include/ps3emu/ps3_knob.h. */
#include <stdlib.h>
#include "ps3emu/ps3_knob.h"

void ps3_env_bump(void)
{
    ps3_env_gen++;   /* only the debug console writes it */
}

