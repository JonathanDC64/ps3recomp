#!/usr/bin/env python3
"""The guest setjmp and longjmp in a lift (ppu_lifter.py, SJLJ_SUPPORT).

A lifted longjmp restores the guest registers from the buffer, but a C return
goes back to its own caller, not to the function that called setjmp: before
SJLJ_SUPPORT a guest longjmp went on running the code after it with the
stack pointer of the setjmp. Demon's Souls raises a Lua script error that way
(luaD_throw to luaD_rawrunprotected), and a script error the game catches on a
PS3 crashed the port a moment later.

Checked here:
  * find_guest_setjmp finds the SDK's two functions by their first words, and
    nothing else;
  * a call of setjmp becomes PS3_GUEST_SETJMP with a slot and a scope in the
    caller, longjmp's return becomes ps3_guest_longjmp, and only the source
    files with either get SJLJ_SUPPORT;
  * SJLJ_SUPPORT itself, compiled and run with functions written as the lift
    writes them: a longjmp from two frames down, a nested pair, a slot armed
    again in a loop, a value of 0, and a longjmp with no live setjmp.

Usage:  python tools/test_ppu_setjmp.py   (CI=1: a missing compiler fails)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from ppu_disasm import Instruction                                  # noqa: E402
from ppu_lifter import (PPULifter, LiftedFunction, SJLJ_SUPPORT,     # noqa: E402
                        find_guest_setjmp, _SETJMP_WORDS, _LONGJMP_WORDS)


def check_detect():
    base = 0x10000
    words = [0x60000000] * 64
    words[0:5] = _SETJMP_WORDS
    words[16:21] = _LONGJMP_WORDS
    words[32:37] = _SETJMP_WORDS[:4] + (0xF9C30018,)   # a near miss
    blob = b"".join(struct.pack(">I", w) for w in words)
    starts = [base, base + 16 * 4, base + 32 * 4, base + 48 * 4]
    s, l = find_guest_setjmp([(base, blob)], starts)
    assert s == {base} and l == {base + 64}, (s, l)
    # Only function starts count.
    s, l = find_guest_setjmp([(base, blob)], [base + 4])
    assert not s and not l, (s, l)


def lift(lifter, insns, start, end):
    return lifter.lift_function(insns, start, end)


def check_emit():
    lf = PPULifter(prefix="")
    lf.setjmp_addrs = {0x2000}
    lf.longjmp_addrs = {0x3000}
    caller = lift(lf, [Instruction(0x1000, 0, "bl", "0x2000"),
                       Instruction(0x1004, 0, "bl", "0x2000"),
                       Instruction(0x1008, 0, "blr", "")], 0x1000, 0x100C)
    body = [b.strip() for b in caller.body_lines]
    assert body[0] == "ps3_sjlj_scope _sjs;", body
    assert body[1] == "ps3_sjlj_slot _sj0, _sj1;", body
    assert body[2] == "ctx->lr = 0x00001004; PS3_GUEST_SETJMP(ctx, _sj0, func_00002000(ctx));", body
    assert body[3] == "ctx->lr = 0x00001008; PS3_GUEST_SETJMP(ctx, _sj1, func_00002000(ctx));", body
    assert body[4] == "return;", body

    lj = lift(lf, [Instruction(0x3000, 0, "addi", "r3, r3, 15"),
                   Instruction(0x3004, 0, "blr", "")], 0x3000, 0x3008)
    body = [b.strip() for b in lj.body_lines]
    assert body[0] == "const uint32_t _sj_buf = PS3_SJLJ_BUF(ctx->gpr[3]);", body
    assert body[-1] == "ps3_guest_longjmp(ctx, _sj_buf); return;", body

    other = lift(lf, [Instruction(0x4000, 0, "bl", "0x5000"),
                      Instruction(0x4004, 0, "blr", "")], 0x4000, 0x4008)
    assert not any("sjlj" in b or "SETJMP" in b for b in other.body_lines), other.body_lines

    assert lf._sjlj_decl_lines([caller]) == SJLJ_SUPPORT.split("\n")
    assert lf._sjlj_decl_lines([lj]) == SJLJ_SUPPORT.split("\n")
    assert lf._sjlj_decl_lines([other]) == []
    # A lift with neither changes nothing.
    plain = PPULifter(prefix="")
    f = lift(plain, [Instruction(0x1000, 0, "bl", "0x2000"), Instruction(0x1004, 0, "blr", "")],
             0x1000, 0x1008)
    assert [b.strip() for b in f.body_lines][0].endswith("func_00002000(ctx); DRAIN_TRAMPOLINE(ctx);")
    assert plain._sjlj_decl_lines([f]) == []
    assert all(ord(c) < 128 for c in SJLJ_SUPPORT)


# Functions written as the lift writes them, for the registers used here: r1
# (the stack pointer the guest setjmp saves), r14 (a callee-saved register it
# saves) and lr. func_setjmp and func_longjmp are the guest's; the rest call
# them the way luaD_rawrunprotected and luaD_throw do.
PROGRAM = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_MSC_VER)
#define PPU_THREAD_LOCAL __declspec(thread)
#else
#define PPU_THREAD_LOCAL __thread
#endif
typedef struct ppu_context { uint64_t gpr[32]; uint64_t lr; } ppu_context;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
#define DRAIN_TRAMPOLINE(ctx) do { \
    while (g_trampoline_fn) { void (*_tf)(void*) = g_trampoline_fn; g_trampoline_fn = 0; _tf((void*)(ctx)); } \
} while (0)
@SJLJ@
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*) = 0;
extern "C" PPU_THREAD_LOCAL ps3_sjlj_slot* g_ps3_sjlj_top = 0;

static uint64_t g_mem[512];                      /* guest memory: the buffers */
static uint64_t* gm(uint32_t a) { return &g_mem[(a & 0xFFF) / 8]; }
static int g_bad, g_fails;

static void func_setjmp(ppu_context* ctx) {
    const uint32_t b = PS3_SJLJ_BUF(ctx->gpr[3]);
    *gm(b) = ctx->gpr[1]; *gm(b + 8) = ctx->gpr[14]; *gm(b + 16) = ctx->lr;
    ctx->gpr[3] = 0;
}
static void func_longjmp(ppu_context* ctx) {
    const uint32_t _sj_buf = PS3_SJLJ_BUF(ctx->gpr[3]);
    ctx->gpr[1] = *gm(_sj_buf); ctx->gpr[14] = *gm(_sj_buf + 8); ctx->lr = *gm(_sj_buf + 16);
    ctx->gpr[3] = ctx->gpr[4] ? ctx->gpr[4] : 1;
    ps3_guest_longjmp(ctx, _sj_buf); return;
}
/* longjmp(r24, r21), two frames below whoever calls thrower1 */
static void thrower2(ppu_context* ctx) {
    ctx->gpr[1] -= 0x30; ctx->gpr[14] = 0xDEAD;
    ctx->gpr[3] = ctx->gpr[24]; ctx->gpr[4] = ctx->gpr[21];
    ctx->lr = 0x2004; func_longjmp(ctx);
    g_bad++; ctx->gpr[1] += 0x30;
}
static void thrower1(ppu_context* ctx) { ctx->gpr[1] -= 0x40; thrower2(ctx); g_bad++; ctx->gpr[1] += 0x40; }
static void protected_call(ppu_context* ctx);
/* setjmp(r20); if it returned 0: r22 == 1 throws, r22 == 2 runs a protected
 * call on r25 that throws to r24 (r26 = its r23 when it returns); r23 = what
 * setjmp returned */
static void protected_call(ppu_context* ctx) {
    ps3_sjlj_scope _sjs;
    ps3_sjlj_slot _sj0;
    const uint64_t _cs_20 = ctx->gpr[20];
    ctx->gpr[1] -= 0x80; ctx->gpr[14] = 0x14;
    ctx->gpr[3] = ctx->gpr[20];
    ctx->lr = 0x1234; PS3_GUEST_SETJMP(ctx, _sj0, func_setjmp(ctx));
    if (ctx->gpr[3] == 0 && ctx->gpr[22] == 1) { thrower1(ctx); g_bad++; }
    if (ctx->gpr[3] == 0 && ctx->gpr[22] == 2) {
        ctx->gpr[20] = ctx->gpr[25]; ctx->gpr[22] = 1;
        protected_call(ctx);
        ctx->gpr[26] = ctx->gpr[23];
        ctx->gpr[3] = 0;
    }
    ctx->gpr[23] = ctx->gpr[3];
    ctx->gpr[20] = _cs_20;
    ctx->gpr[1] += 0x80;
}
/* setjmp(r20) twice in a loop; the second round throws */
static void loop_call(ppu_context* ctx) {
    ps3_sjlj_scope _sjs;
    ps3_sjlj_slot _sj0;
    uint64_t round = 0;
    ctx->gpr[1] -= 0x60;
loc_top:
    ctx->gpr[3] = ctx->gpr[20];
    ctx->lr = 0x3000; PS3_GUEST_SETJMP(ctx, _sj0, func_setjmp(ctx));
    if (ctx->gpr[3] == 0) {
        if (++round == 2) { thrower1(ctx); g_bad++; }
        goto loc_top;
    }
    ctx->gpr[23] = ctx->gpr[3];
    ctx->gpr[1] += 0x60;
}
static void check(const char* what, int ok) { if (!ok) { printf("FAIL %s\n", what); g_fails++; } }

int main(void) {
    ppu_context c;
    for (int i = 0; i < 2000; i++) {
        memset(&c, 0, sizeof c);
        c.gpr[1] = 0x10000; c.gpr[20] = 0x101; c.gpr[24] = 0x101; c.gpr[21] = 9;

        c.gpr[22] = 0; protected_call(&c);
        check("no throw", c.gpr[23] == 0 && c.gpr[1] == 0x10000 && !g_ps3_sjlj_top);

        c.gpr[22] = 1; protected_call(&c);
        check("throw", c.gpr[23] == 9 && c.gpr[14] == 0x14 && c.gpr[1] == 0x10000 && !g_ps3_sjlj_top);

        c.gpr[21] = 0; c.gpr[22] = 1; protected_call(&c);
        check("value 0 is 1", c.gpr[23] == 1 && !g_ps3_sjlj_top);

        c.gpr[21] = 7; c.gpr[22] = 2; c.gpr[25] = 0x201; c.gpr[24] = 0x101; c.gpr[26] = 0; protected_call(&c);
        check("nested, to the outer", c.gpr[23] == 7 && c.gpr[26] == 0 && c.gpr[1] == 0x10000 &&
              c.gpr[20] == 0x101 && !g_ps3_sjlj_top);

        c.gpr[21] = 5; c.gpr[22] = 2; c.gpr[25] = 0x201; c.gpr[24] = 0x201; c.gpr[26] = 0; protected_call(&c);
        check("nested, to the inner", c.gpr[23] == 0 && c.gpr[26] == 5 && c.gpr[1] == 0x10000 &&
              c.gpr[20] == 0x101 && !g_ps3_sjlj_top);

        c.gpr[21] = 3; c.gpr[24] = 0x101; loop_call(&c);
        check("armed again in a loop", c.gpr[23] == 3 && c.gpr[1] == 0x10000 && !g_ps3_sjlj_top);
    }
    /* No live setjmp: it returns to its caller, as before. */
    memset(&c, 0, sizeof c);
    c.gpr[1] = 0x10000; c.gpr[3] = 0x301; c.gpr[4] = 1;
    func_longjmp(&c);
    check("no live setjmp returns", c.gpr[3] == 1 && !g_ps3_sjlj_top);
    check("nothing ran past a longjmp", g_bad == 0);
    printf("%s\n", g_fails ? "FAILED" : "ok");
    return g_fails ? 1 : 0;
}
"""


def _compilers():
    """(argv for one optimisation level, output flag style) to try, best first."""
    if os.name == "nt":
        found = shutil.which("clang-cl")
        llvm = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "LLVM", "bin", "clang-cl.exe")
        if not found and os.path.exists(llvm):
            found = llvm
        out = []
        if found:
            out.append([found, "/nologo", "/EHsc", "/W3"])
        out.append(["cl", "/nologo", "/EHsc", "/W3"])
        return out
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    return [[cxx, "-std=c++17", "-Wall"]] if cxx else []


def check_run() -> bool:
    """True if it compiled and ran; False if no compiler could be run at all."""
    src_text = PROGRAM.replace("@SJLJ@", SJLJ_SUPPORT)
    tmp = tempfile.mkdtemp(prefix="sjlj_")
    src = os.path.join(tmp, "t.cpp")
    with open(src, "w") as f:
        f.write(src_text)
    env = None
    if os.name == "nt":
        from msvc_env import environ
        env = environ()
    ran = False
    for base in _compilers():
        for opt in (("/O2", "/Od") if os.name == "nt" else ("-O2", "-O0")):
            exe = os.path.join(tmp, f"t{opt.strip('/-')}.exe")
            cmd = base + [opt, src] + ([f"/Fe{exe}", f"/Fo{tmp}{os.sep}"] if os.name == "nt" else ["-o", exe])
            try:
                cp = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True)
            except OSError:
                break                          # this compiler is not here: the next one
            if cp.returncode != 0:
                print(cp.stdout[-3000:], cp.stderr[-3000:])
                raise AssertionError(f"SJLJ_SUPPORT does not compile with {base[0]} {opt}")
            rp = subprocess.run([exe], capture_output=True, text=True)
            if rp.returncode != 0 or "ok" not in rp.stdout:
                print(rp.stdout[-3000:], rp.stderr[-3000:])
                raise AssertionError(f"SJLJ_SUPPORT run failed ({base[0]} {opt}, exit {rp.returncode:#x})")
            print(f"  ran with {os.path.basename(base[0])} {opt}")
            ran = True
        if ran:
            break
    shutil.rmtree(tmp, ignore_errors=True)
    return ran


def main() -> int:
    check_detect()
    check_emit()
    if not check_run():
        print("SKIPPED: no C++ compiler, so SJLJ_SUPPORT was not compiled or run")
        if os.environ.get("CI"):
            print("...and this is CI, where that is a failure")
            return 1
    print("test_ppu_setjmp: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
