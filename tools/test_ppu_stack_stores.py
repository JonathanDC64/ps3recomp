#!/usr/bin/env python3
"""Stores with base r1: the lifter writes them as vm_write*_stk, the header stores them straight.

The lifter writes a D-form store with base r1 (a frame save or a spill) as
vm_write*_stk(ctx->gpr[1] + ...), and only that: a store through r11 or r21
stays as it is, and the frame analyses still see the plain form in the body.
ps3emu/ppu_lift.h's vm_write*_stk stores an address in the stack region
straight away while ea - 0xD0000000 is below the stack limit (the word after
the SPU armed flag), and otherwise takes vm_write*_inl, which calls the
accessor while the store limit is 0. The second half compiles the header and
checks which path each address takes, with the limit on and off, and the
bytes a direct store leaves (big-endian, as the accessor would).

Run: python tools/test_ppu_stack_stores.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import ppu_disasm                       # noqa: E402
from ppu_lifter import PPULifter        # noqa: E402

BASE = 0x10000


def w_d(op, rs, ra, d):  return (op << 26) | (rs << 21) | (ra << 16) | (d & 0xFFFF)
def w_ds(op, rs, ra, ds, xo): return (op << 26) | (rs << 21) | (ra << 16) | (ds & 0xFFFC) | xo
def w_blr():             return (19 << 26) | (20 << 21) | (16 << 1)


def check_lifter():
    words = [
        w_ds(62, 1, 1, -0x40, 1),    # stdu r1,-0x40(r1)
        w_ds(62, 31, 1, 0x38, 0),    # std  r31,0x38(r1)
        w_d(36, 3, 1, 0x10),         # stw  r3,0x10(r1)
        w_d(44, 4, 1, 0x14),         # sth  r4,0x14(r1)
        w_d(38, 5, 1, 0x16),         # stb  r5,0x16(r1)
        w_d(36, 3, 11, 0x10),        # stw  r3,0x10(r11)
        w_d(36, 3, 21, 0x10),        # stw  r3,0x10(r21)
        w_d(36, 3, 5, 0),            # stw  r3,0(r5)
        w_ds(58, 31, 1, 0x38, 0),    # ld   r31,0x38(r1)
        w_d(14, 1, 1, 0x40),         # addi r1,r1,0x40
        w_blr(),
    ]
    insns = [ppu_disasm.decode(w, BASE + 4 * i) for i, w in enumerate(words)]
    lf = PPULifter()
    func = lf.lift_function(insns, BASE, BASE + 4 * len(words))
    plain = "\n".join(func.body_lines)
    out = "\n".join(lf._function_def_lines(func, {BASE: func}, [BASE], {BASE: 0}))
    for w in (64, 32, 16, 8):
        assert f"vm_write{w}(ctx->gpr[1] + " in plain, f"the body lost its plain {w}-bit r1 store:\n{plain}"
        assert f"vm_write{w}_stk(ctx->gpr[1] + " in out, f"no vm_write{w}_stk in the function:\n{out}"
        assert f"vm_write{w}(ctx->gpr[1] + " not in out, f"a plain {w}-bit r1 store was left:\n{out}"
    assert "_stk" not in plain, f"the body itself was rewritten:\n{plain}"
    assert out.count("_stk(") == 5, (
        f"want 5 stack stores (the stdu back chain is one), got {out.count('_stk(')}:\n{out}")
    for r in (11, 21, 5):
        assert f"_stk(ctx->gpr[{r}]" not in out, f"a store through r{r} became a stack store:\n{out}"
        assert f"vm_write32(ctx->gpr[{r}]" in out, f"the store through r{r} is missing:\n{out}"
    # The four stores after the stdu are one run: bytes 0x10..0x3F of the frame.
    for want in ("{ uint8_t* const _sk = vm_stk_run(ctx->gpr[1] + 0x10, 48);",
                 "vm_run_put64(_sk + 40, ctx->gpr[31]);", "vm_run_put32(_sk + 0, ctx->gpr[3]);",
                 "vm_run_put16(_sk + 4, ctx->gpr[4]);", "vm_run_put8(_sk + 6, ctx->gpr[5]);"):
        assert want in out, f"the run is missing {want!r}:\n{out}"
    assert out.count("vm_stk_run(") == 1, f"want one run:\n{out}"
    print("ok: r1 stores are written as vm_write*_stk, the body and other bases are not")


def check_runs():
    """_stack_store_runs on lines: what makes a run and what ends one."""
    from ppu_lifter import _stack_store_runs
    s = ["    vm_write64_stk(ctx->gpr[1] + 0x70, ctx->gpr[16]);",
         "    vm_write64_stk(ctx->gpr[1] + 0x78, ctx->gpr[17]);",
         "    vm_write32_stk(ctx->gpr[1] + -0x8, ctx->gpr[18]);"]
    out = _stack_store_runs(s)
    assert out[0] == "    { uint8_t* const _sk = vm_stk_run(ctx->gpr[1] + -0x8, 136);", out
    assert out[1] == "      if (__builtin_expect(_sk != 0, 1)) {", out
    assert out[2:5] == ["        vm_run_put64(_sk + 120, ctx->gpr[16]);",
                        "        vm_run_put64(_sk + 128, ctx->gpr[17]);",
                        "        vm_run_put32(_sk + 0, ctx->gpr[18]);"], out
    assert out[5] == "      } else {" and out[-1] == "      } }", out
    assert [l.strip() for l in out[6:9]] == [l.strip() for l in s], out
    # Two in a row stay as they are; a label, another line or a store of
    # something else than a GPR ends a run; so does a change of indentation.
    two = s[:2]
    assert _stack_store_runs(two) == two
    cut = [s[0], s[1], "loc_00010010:", s[2]]
    assert _stack_store_runs(cut) == cut
    cut = [s[0], "    ctx->gpr[1] += -0x40;", s[1], s[2]]
    assert _stack_store_runs(cut) == cut
    cut = [s[0], s[1], "    vm_write64_stk(ctx->gpr[1] + 0x80, tmp);"]
    assert _stack_store_runs(cut) == cut
    cut = [s[0], s[1], "        " + s[2].strip()]
    assert _stack_store_runs(cut) == cut
    # Never as the body of a control line with no braces.
    for ctl in ("    if (x)", "    else"):
        assert _stack_store_runs([ctl] + s) == [ctl] + s
    # A run of three after other lines, and the lines after it, kept in order.
    out = _stack_store_runs(["    a;"] + s + ["    b;"])
    assert out[0] == "    a;" and out[-1] == "    b;" and out.count("      } }") == 1, out
    # Stores through another base register: vm_st_run, at most 256 bytes.
    t = ["    vm_write32(ctx->gpr[11] + 0x8, ctx->gpr[0]);",
         "    vm_write32(ctx->gpr[11] + 0x4, ctx->gpr[9]);",
         "    vm_write16(ctx->gpr[11] + 0xC, ctx->gpr[11]);"]
    out = _stack_store_runs(t)
    assert out[0] == "    { uint8_t* const _sr = vm_st_run(ctx->gpr[11] + 0x4, 10);", out
    assert out[2:5] == ["        vm_run_put32(_sr + 4, ctx->gpr[0]);",
                        "        vm_run_put32(_sr + 0, ctx->gpr[9]);",
                        "        vm_run_put16(_sr + 8, ctx->gpr[11]);"], out
    assert [l.strip() for l in out[6:9]] == [l.strip() for l in t], out
    wide = t[:2] + ["    vm_write32(ctx->gpr[11] + 0x104, ctx->gpr[3]);"]   # 0x4 .. 0x107: 260 bytes
    assert _stack_store_runs(wide) == wide
    mixed = [t[0], t[1], "    vm_write32(ctx->gpr[12] + 0xC, ctx->gpr[3]);"]
    assert _stack_store_runs(mixed) == mixed
    # A stack store and a store through r11 never share a run, and a plain
    # store with base r1 (none after the _stk rewrite) is not a run's.
    assert _stack_store_runs([s[0], s[1], t[0]]) == [s[0], s[1], t[0]]
    plain = [l.replace("_stk(", "(") for l in s]
    assert _stack_store_runs(plain) == plain
    print("ok: runs of three or more stores through one base share one test; anything else ends a run")


DRIVER = r"""
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER) && !defined(__clang__)
#  define __builtin_expect(x, y) (x)
#  define __builtin_bswap16 _byteswap_ushort
#  define __builtin_bswap32 _byteswap_ulong
#  define __builtin_bswap64 _byteswap_uint64
#endif
#define PPU_VM_INLINE 1
#include "ps3emu/ppu_lift.h"

static uint8_t g_mem[0x10000];   /* guest 0xD0000000 .. 0xD000FFFF */
static int g_slow, g_fail, g_pass;
extern "C" {
uint8_t* vm_base;   /* set in main: macOS's linker cannot relocate a pointer that far outside g_mem */
unsigned g_vm_inl_rd_lim, g_vm_inl_wr_lim, g_vm_inl_rd_hole;   /* 0: every other store calls */
unsigned char g_spu_coh_bitmap[256];
#ifndef PPU_VM_NO_INLINE
struct spu_coh_armed_line_t g_spu_coh_armed_line = { 1, 0 };
#endif
uint8_t  (vm_read8)(uint64_t)  { return 0; }
uint16_t (vm_read16)(uint64_t) { return 0; }
uint32_t (vm_read32)(uint64_t) { return 0; }
uint64_t (vm_read64)(uint64_t) { return 0; }
void (vm_write8)(uint64_t, uint8_t)   { g_slow++; }
void (vm_write16)(uint64_t, uint16_t) { g_slow++; }
void (vm_write32)(uint64_t, uint32_t) { g_slow++; }
void (vm_write64)(uint64_t, uint64_t) { g_slow++; }
}
static void store(int w, uint64_t a, uint64_t v) {
    switch (w) {
    case 8:  vm_write8_stk(a, (uint8_t)v); break;
    case 16: vm_write16_stk(a, (uint16_t)v); break;
    case 32: vm_write32_stk(a, (uint32_t)v); break;
    default: vm_write64_stk(a, v); break;
    }
}
int main(void) {
    vm_base = (uint8_t*)((uintptr_t)g_mem - 0xD0000000u);
    const unsigned LIM = 0x8000u;   /* any limit: the runtime's is 0x0FFF0000 */
    /* address, direct with the limit on */
    const struct { uint64_t a; int direct; } at[] = {
        { 0xD0000000u, 1 }, { 0xD0001230u, 1 }, { 0xD0007FF0u, 1 },
        { 0xD0008000u, 0 }, { 0xD000F000u, 0 }, { 0xCFFFFFF0u, 0 },
        { 0x30001000u, 0 }, { 0x00010000u, 0 }, { 0x1D0004000ull, 1 },   /* the lift keeps the low word */
    };
    const int widths[] = { 8, 16, 32, 64 };
    for (int on = 0; on < 2; on++) {
#ifndef PPU_VM_NO_INLINE
        g_spu_coh_armed_line.stk_lim = on ? LIM : 0u;
#endif
        for (unsigned i = 0; i < sizeof at / sizeof at[0]; i++)
            for (int k = 0; k < 4; k++) {
                const int w = widths[k];
                const uint64_t v = 0x0102030405060708ull;
#ifdef PPU_VM_NO_INLINE
                const int want = 0;
#else
                const int want = on && at[i].direct;
#endif
                memset(g_mem, 0xCC, sizeof g_mem); g_slow = 0;
                store(w, at[i].a, v);
                int ok = (g_slow == !want);
                const uint32_t ea = (uint32_t)at[i].a;
                if (ok && want) {   /* big-endian, the low w bits of v, and nothing else */
                    uint8_t be[8]; for (int b = 0; b < w / 8; b++) be[b] = (uint8_t)(v >> (w - 8 - 8 * b));
                    const uint32_t off = ea - 0xD0000000u;
                    ok = memcmp(g_mem + off, be, (size_t)(w / 8)) == 0 &&
                         (off == 0 || g_mem[off - 1] == 0xCC) && g_mem[off + w / 8] == 0xCC;
                }
                if (ok) g_pass++;
                else { g_fail++;
                       printf("FAIL %d-bit store at 0x%llX, limit %s: %s\n", w, (unsigned long long)at[i].a,
                              on ? "on" : "off", g_slow ? "called the accessor" : "stored directly"); }
            }
    }
    /* A run's one test (vm_stk_run): the host address of its first byte when
     * all of [ea, ea + span) is below the run limit, else 0. The stack limit
     * alone does not do it: PPU_STK_RUN=0 leaves the run limit 0. */
    const struct { uint64_t a; unsigned span; int direct; } run[] = {
        { 0xD0000000u, 16, 1 }, { 0xD0007FF0u, 16, 1 }, { 0xD0007FF8u, 16, 0 },
        { 0xD0007FF8u, 8, 1 }, { 0xCFFFFFF8u, 16, 0 }, { 0xD0008000u, 1, 0 }, { 0x1D0000100ull, 64, 1 },
    };
    for (int on = 0; on < 4; on++) {
#ifndef PPU_VM_NO_INLINE
        g_spu_coh_armed_line.stk_lim = (on & 1) ? LIM : 0u;
        g_spu_coh_armed_line.stk_run = (on & 2) ? LIM : 0u;
#endif
        for (unsigned i = 0; i < sizeof run / sizeof run[0]; i++) {
#ifdef PPU_VM_NO_INLINE
            const int want = 0;
#else
            const int want = (on & 2) && run[i].direct;
#endif
            uint8_t* p = vm_stk_run(run[i].a, run[i].span);
            const int ok = want ? p == g_mem + ((uint32_t)run[i].a - 0xD0000000u) : p == 0;
            if (ok) g_pass++;
            else { g_fail++;
                   printf("FAIL run of %u at 0x%llX, stack limit %s, run limit %s: %s\n", run[i].span,
                          (unsigned long long)run[i].a, (on & 1) ? "on" : "off", (on & 2) ? "on" : "off",
                          p ? "direct" : "store by store"); }
        }
    }
    /* A run through another base register (vm_st_run): below the store limit,
     * and no line it touches reserved while the armed flag is up. */
    {
#ifndef PPU_VM_NO_INLINE
        g_vm_inl_wr_lim = 0x10000u;   /* guest 0x10000 .. 0x1FFFF */
        g_spu_coh_armed_line.st_run = 1u;
        g_spu_coh_bitmap[0x201 >> 3] |= (unsigned char)(1u << (0x201 & 7));   /* line 0x10080 */
#endif
        const struct { uint64_t a; unsigned span; int armed, knob, lim, direct; } st[] = {
            { 0x10000u, 16, 1, 1, 1, 1 }, { 0x1FFF0u, 16, 1, 1, 1, 1 }, { 0x1FFF8u, 16, 1, 1, 1, 0 },
            { 0xFFF8u, 16, 1, 1, 1, 0 }, { 0x10000u, 0x80, 1, 1, 1, 1 }, { 0x10000u, 0x81, 1, 1, 1, 0 },
            { 0x10070u, 0x20, 1, 1, 1, 0 }, { 0x10100u, 16, 1, 1, 1, 1 }, { 0x10070u, 0x20, 0, 1, 1, 1 },
            { 0x10000u, 16, 1, 0, 1, 0 }, { 0x10000u, 16, 1, 1, 0, 0 }, { 0x110000u, 16, 1, 1, 1, 1 },
        };
        for (unsigned i = 0; i < sizeof st / sizeof st[0]; i++) {
#ifdef PPU_VM_NO_INLINE
            const int want = 0;
#else
            g_spu_coh_armed_line.v = st[i].armed;
            g_spu_coh_armed_line.st_run = (unsigned)st[i].knob;
            g_vm_inl_wr_lim = st[i].lim ? 0x10000u : 0u;
            const int want = st[i].direct && (uint32_t)st[i].a < 0x20000u;
#endif
            uint8_t* p = vm_st_run(st[i].a, st[i].span);
            const int ok = want ? p == vm_base + (uint32_t)st[i].a : p == 0;
            if (ok) g_pass++;
            else { g_fail++;
                   printf("FAIL store run of %u at 0x%llX (armed %d, knob %d, limit %d): %s\n", st[i].span,
                          (unsigned long long)st[i].a, st[i].armed, st[i].knob, st[i].lim,
                          p ? "direct" : "store by store"); }
        }
#ifndef PPU_VM_NO_INLINE
        g_vm_inl_wr_lim = 0u; g_spu_coh_armed_line.v = 1;
#endif
    }
    /* The run's stores: big-endian, the low w bits, as the direct store. */
    {
        memset(g_mem, 0xCC, sizeof g_mem);
        vm_run_put64(g_mem + 0, 0x0102030405060708ull); vm_run_put32(g_mem + 8, 0x1112131415161718ull);
        vm_run_put16(g_mem + 12, 0x2122u); vm_run_put8(g_mem + 14, 0x31u);
        static const uint8_t want[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 0x15, 0x16, 0x17, 0x18, 0x21, 0x22, 0x31, 0xCC };
        if (memcmp(g_mem, want, 16) == 0) g_pass++;
        else { g_fail++; printf("FAIL the run's stores left the wrong bytes\n"); }
    }
    printf("[ppu-stack-stores] %d checks passed, %d FAILED\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
"""


def builds(tmp, cpp):
    inc = os.path.join(ROOT, "include")
    out = []
    if os.name == "nt":
        from msvc_env import environ
        env = environ()
        for tool in ("clang-cl", "cl"):
            exe = shutil.which(tool, path=env.get("PATH", ""))
            if not exe:
                continue
            for tag, flags in (("inline", []), ("no-inline", ["/DPPU_VM_NO_INLINE"])):
                name = os.path.join(tmp, f"stk_{tool}_{tag}")
                out.append((f"{tool}, {tag}", [exe, "/nologo", "/O2", "/EHsc", "/std:c++17", "/I", inc, *flags,
                                                "/Fe" + name + ".exe", "/Fo" + name + ".obj", cpp],
                            name + ".exe", env))
            break   # clang-cl, the game's compiler, when it is there
    else:
        cxx = (os.environ.get("CXX") or shutil.which("c++")
               or shutil.which("clang++") or shutil.which("g++"))
        if cxx:
            for tag, flags in (("inline", []), ("no-inline", ["-DPPU_VM_NO_INLINE"])):
                name = os.path.join(tmp, f"stk_{tag}")
                out.append((f"{os.path.basename(cxx)}, {tag}",
                            [cxx, "-std=c++17", "-O2", "-w", "-I", inc, *flags, "-o", name, cpp], name, None))
    return out


def check_header():
    tmp = tempfile.mkdtemp(prefix="ppu_stk_")
    try:
        cpp = os.path.join(tmp, "stk.cpp")
        with open(cpp, "w") as f:
            f.write(DRIVER)
        todo = builds(tmp, cpp)
        if not todo:
            print("SKIPPED: no C++ compiler found, so the header half was not compiled or run.")
            return not os.environ.get("CI")
        ok = True
        for label, cmd, exe, env in todo:
            cp = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True)
            if cp.returncode != 0:
                print(f"[{label}] COMPILE FAILED:\n{cp.stdout}{cp.stderr}")
                ok = False
                continue
            rp = subprocess.run([exe], cwd=tmp, capture_output=True, text=True)
            for ln in rp.stdout.splitlines():
                print(f"[{label}] {ln}")
            ok = ok and rp.returncode == 0
        return ok
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    check_lifter()
    check_runs()
    ok = check_header()
    print("[ppu-stack-stores]", "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
