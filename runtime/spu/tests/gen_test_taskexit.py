#!/usr/bin/env python3
"""SPURS leaf-task EXIT test.

A leaf task signals completion to the SPURS kernel via `stop 0`
(CELL_SPURS_TASK_SYSCALL_EXIT), leaving its exit code in r3. The runtime helper
spu_spurs_task_write_exit_code() must detect the EXIT (stop code 0) and write the
code into the registered exit-code container. This pins that mechanism (the
foundation of func_00A31158's task-exit-code completion path).
"""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
from wrap_spu_elf import wrap

def w(v): return struct.pack(">I", v & 0xFFFFFFFF)
def ri16(op9, i16, rt): return w(((op9 & 0x1FF) << 23) | ((i16 & 0xFFFF) << 7) | (rt & 0x7F))

IL, STOP = 0x81, 0x000

b = b""
b += ri16(IL, 0x1234, 3)          # il r3, 0x1234   -> exit code in r3 (preferred slot)
b += w(0x00000000)                # stop 0          -> CELL_SPURS_TASK_SYSCALL_EXIT

elf = wrap(b, base=0, entry=0, symbols=[{"name": "main", "addr": 0, "size": len(b)}])
open(os.path.join(HERE, "test_taskexit.elf"), "wb").write(elf)
print(f"Wrote test_taskexit.elf ({len(b)} bytes code)")
