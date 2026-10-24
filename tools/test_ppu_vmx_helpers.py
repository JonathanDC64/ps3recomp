#!/usr/bin/env python3
"""The lifter's VMX helpers against the loops they replaced.

vperm, vsldoi, vmaxfp, vminfp, vrsqrtefp, lvlx and lvrx used to be lifted as
byte or lane loops. They now call helpers in the lifter's preamble: SSE4.1 on
x86 (vperm was about 100 x86 instructions, vsldoi a store-forwarding stall),
the same loops elsewhere. This lifts each instruction with the real lifter,
puts the statement it emits next to the statement the lifter used to emit, and
runs both on random and special inputs -- NaNs (quiet and signalling), signed
zeros, infinities, denormals, equal operands, every vsldoi shift, every
lvlx/lvrx offset, and a destination that is also a source. The two must agree
bit for bit across the whole context, so a stray write shows too.

On x86 the driver is built once as the game builds it (the SSE4.1 path) and
once with PPU_VMX_NO_SSE41 (the loops), and with clang-cl as well when it is
there (the game's compiler). Elsewhere only the loops exist.

Run: python tools/test_ppu_vmx_helpers.py
"""
import os
import platform
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)

import ppu_disasm                                                     # noqa: E402
from ppu_lifter import PPULifter, LiftedFunction, FENCE_PREAMBLE, _xea  # noqa: E402

ITERS = 20000   # random inputs per lifted statement

# ---------------------------------------------------------------------------
# Encoders (big-endian instruction words)
# ---------------------------------------------------------------------------

def va_form(xo6, vd, va, vb, vc):   # vperm (43), vsldoi (44: vc is the shift)
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | (vc << 6) | xo6

def vx_form(xo, vd, va, vb):        # vmaxfp (1034), vminfp (1098), vrsqrtefp (330)
    return (4 << 26) | (vd << 21) | (va << 16) | (vb << 11) | xo

def x_form(xo, rt, ra, rb):         # lvlx (519), lvlxl (775), lvrx (551)
    return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1)

# ---------------------------------------------------------------------------
# The statements the lifter emitted before the helpers, as it emitted them.
# ---------------------------------------------------------------------------

def old_vperm(vd, va, vb, vc):
    return (f"{{ uint8_t* a = (uint8_t*)&ctx->vr[{va}]; "
            f"uint8_t* b = (uint8_t*)&ctx->vr[{vb}]; "
            f"uint8_t* c = (uint8_t*)&ctx->vr[{vc}]; "
            f"uint8_t* d = (uint8_t*)&ctx->vr[{vd}]; "
            f"uint8_t tmp[16]; "
            f"for (int i = 0; i < 16; i++) {{ "
            f"uint8_t sel = c[i] & 0x1F; "
            f"tmp[i] = (sel < 16) ? a[sel] : b[sel - 16]; }} "
            f"memcpy(d, tmp, 16); }}")

def old_vsldoi(vd, va, vb, sh):
    return (f"{{ uint8_t tmp[32]; "
            f"memcpy(tmp, &ctx->vr[{va}], 16); memcpy(tmp+16, &ctx->vr[{vb}], 16); "
            f"memcpy(&ctx->vr[{vd}], tmp + {sh}, 16); }}")

def old_maxmin(mn, vd, va, vb):
    if mn == "vmaxfp":
        pick = "(a[i]>b[i]) ? a[i] : (b[i]>a[i]) ? b[i] : (signbit(a[i]) ? b[i] : a[i])"
    else:
        pick = "(a[i]<b[i]) ? a[i] : (b[i]<a[i]) ? b[i] : (signbit(a[i]) ? a[i] : b[i])"
    return (f"{{ float a[4],b[4],d[4]; ppu_vldf4(&ctx->vr[{va}],a); ppu_vldf4(&ctx->vr[{vb}],b); "
            f"for(int i=0;i<4;i++) d[i] = (a[i]!=a[i]) ? a[i]+a[i] : (b[i]!=b[i]) ? b[i]+b[i] : ({pick}); "
            f"ppu_vstf4(&ctx->vr[{vd}],d); }}")

def old_vrsqrtefp(vd, vb):
    return (f"{{ float b[4],d[4]; ppu_vldf4(&ctx->vr[{vb}],b); "
            f"for(int i=0;i<4;i++) d[i]=1.0f/sqrtf(b[i]); ppu_vstf4(&ctx->vr[{vd}],d); }}")

def old_lvlx(vd, ra, rb):
    return (f"{{ uint64_t ea = {_xea(ra, rb)}; "
            f"uint32_t sh = (uint32_t)(ea & 0xF); "
            f"uint8_t* m = vm_base + (uint32_t)(ea & ~0xFULL); "
            f"uint8_t* d = (uint8_t*)&ctx->vr[{vd}]; "
            f"for (int i = 0; i < 16; i++) d[i] = (sh + (uint32_t)i < 16) ? m[sh + i] : 0; }}")

def old_lvrx(vd, ra, rb):
    return (f"{{ uint64_t ea = {_xea(ra, rb)}; "
            f"uint32_t sh = (uint32_t)(ea & 0xF); "
            f"uint8_t* m = vm_base + (uint32_t)(ea & ~0xFULL); "
            f"uint8_t* d = (uint8_t*)&ctx->vr[{vd}]; "
            f"for (int i = 0; i < 16; i++) d[i] = ((uint32_t)i >= 16u - sh) ? m[i - (int)(16u - sh)] : 0; }}")

# (word, mnemonic, old statement, input kind, the call the new statement must make)
CASES = []

def _cases():
    # destination apart from the sources, and the same as each source in turn
    for vd, va, vb, vc in ((3, 4, 5, 6), (4, 4, 5, 6), (5, 4, 5, 6), (6, 4, 5, 6), (7, 7, 7, 7)):
        CASES.append((va_form(43, vd, va, vb, vc), "vperm", old_vperm(vd, va, vb, vc), "bytes", "ppu_vperm("))
    for vd, va, vb in ((3, 4, 5), (4, 4, 5), (5, 4, 5), (6, 6, 6)):
        for sh in range(16):
            CASES.append((va_form(44, vd, va, vb, sh), "vsldoi", old_vsldoi(vd, va, vb, sh), "bytes",
                          "PPU_VSLDOI("))
    for mn, xo in (("vmaxfp", 1034), ("vminfp", 1098)):
        for vd, va, vb in ((3, 4, 5), (4, 4, 5), (5, 4, 5), (6, 6, 6)):
            CASES.append((vx_form(xo, vd, va, vb), mn, old_maxmin(mn, vd, va, vb), "floats", f"ppu_{mn}("))
    for vd, vb in ((3, 5), (5, 5)):
        CASES.append((vx_form(330, vd, 0, vb), "vrsqrtefp", old_vrsqrtefp(vd, vb), "floats", "ppu_vrsqrtefp("))
    for mn, xo, old, call in (("lvlx", 519, old_lvlx, "ppu_lvlx("), ("lvlxl", 775, old_lvlx, "ppu_lvlx("),
                              ("lvrx", 551, old_lvrx, "ppu_lvrx(")):
        for ra, rb in ((0, 4), (3, 4)):   # rA = 0 is the literal 0, not r0
            CASES.append((x_form(xo, 2, ra, rb), mn, old(2, ra, rb), "mem" if ra else "mem0", call))

_cases()

DRIVER = r"""
#include <stdio.h>
static ppu_context g_in, g_new, g_old;
static uint8_t g_mem[65536 + 16];
extern "C" { uint8_t* vm_base = g_mem; }
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 32); }
/* lane values: signed zeros, ones, infinities, denormals, the normal limits,
 * quiet and signalling NaNs with and without payloads, values around 1 */
static const uint32_t k_special[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u,
    0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu, 0x00400000u, 0x80400000u,
    0x00800000u, 0x80800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x01000000u, 0x7E800000u,
    0x7FC00000u, 0xFFC00000u, 0x7FC12345u, 0xFFFFFFFFu, 0x7FFFFFFFu,
    0x7F800001u, 0xFF800001u, 0x7FA00000u, 0x7FBFFFFFu, 0xFFA5A5A5u,
    0x3F7FFFFFu, 0x3F800001u, 0x40490FDBu, 0xC0490FDBu, 0x4B800000u, 0x34000000u,
    0x5F800000u, 0x1F800000u, 0x3E800000u, 0x40800000u,
};
#define N_SPECIAL (sizeof(k_special) / sizeof(k_special[0]))
static void set_lane(ppu_context* c, int r, int k, uint32_t v) {
    uint32_t be = ppu_vbswap32(v); memcpy((uint8_t*)&c->vr[r] + 4 * k, &be, 4);
}
static void fill(ppu_context* c, int kind) {
    for (int r = 0; r < 32; r++)
        for (int k = 0; k < 16; k++) ((uint8_t*)&c->vr[r])[k] = (uint8_t)rnd();
    for (int r = 0; r < 32; r++) c->gpr[r] = ((uint64_t)rnd() << 32) | rnd();
    if (kind == 1) {
        for (int r = 0; r < 32; r++)
            for (int k = 0; k < 4; k++) {
                uint32_t v = (rnd() & 3) ? k_special[rnd() % N_SPECIAL] : rnd();
                set_lane(c, r, k, v);
            }
        /* equal operands and +0/-0 pairs: the operands are always v4 and v5 */
        for (int k = 0; k < 4; k++) {
            uint32_t x = rnd() & 7, v;
            memcpy(&v, (uint8_t*)&c->vr[4] + 4 * k, 4); v = ppu_vbswap32(v);
            if (x == 0) set_lane(c, 5, k, v);
            else if (x == 1) set_lane(c, 5, k, v ^ 0x80000000u);
        }
    } else {
        /* the address (r3 + r4, or r4 alone for rA = 0) is in g_mem, with any
         * high word: the lift keeps the low 32 bits */
        const uint64_t ea = ((uint64_t)rnd() << 32) | (rnd() % 65536u);
        if (kind == 2) c->gpr[4] = ea - c->gpr[3]; else c->gpr[4] = ea;
        for (int k = 0; k < 64; k++) g_mem[rnd() % sizeof(g_mem)] = (uint8_t)rnd();
    }
}
static int g_fail = 0;
static long g_pass = 0;
static void dump(const char* what, const ppu_context* c, int r) {
    printf("    %s v%d =", what, r);
    for (int k = 0; k < 16; k++) printf(" %02X", ((const uint8_t*)&c->vr[r])[k]);
    printf("\n");
}
static void run(const char* name, int kind, int vd, void (*fnew)(ppu_context*), void (*fold)(ppu_context*)) {
    int shown = 0;
    for (int it = 0; it < ITERS; it++) {
        fill(&g_in, kind);
        memcpy(&g_new, &g_in, sizeof g_in); memcpy(&g_old, &g_in, sizeof g_in);
        fnew(&g_new); fold(&g_old);
        if (memcmp(&g_new, &g_old, sizeof g_new) == 0) { g_pass++; continue; }
        g_fail++;
        if (shown++ < 3) {
            printf("FAIL %s: iteration %d differs from the old loop\n", name, it);
            for (int r = 0; r < 8; r++) dump("in ", &g_in, r);
            dump("new", &g_new, vd); dump("old", &g_old, vd);
        }
    }
}
"""


def emit(path):
    """Write the driver; returns the number of statement pairs, or -1."""
    lifter = PPULifter()
    dummy = LiftedFunction(name="vmx", start_addr=0, end_addr=0x10000)
    pre = lifter._preamble_lines()
    # the context header in place of the generated one, as test_ppu_lift.py does
    pre[0] = pre[0].replace('#include "ppu_recomp.h"',
                            '#include "ppu_context.h"\n#include <stdint.h>\n' + FENCE_PREAMBLE)
    out = ["/* Auto-generated by tools/test_ppu_vmx_helpers.py */",
           "#define _CRT_SECURE_NO_WARNINGS", f"#define ITERS {ITERS}"]
    out.extend(pre)
    out.append(DRIVER)
    runs, bad = [], 0
    for i, (word, mn, old, kind, call) in enumerate(CASES):
        insn = ppu_disasm.decode(word, 0x10000 + i * 4)
        if insn.mnemonic != mn:
            print(f"ENCODING: {word:#010x} decoded as {insn.mnemonic} {insn.operands}, want {mn}")
            bad += 1
            continue
        new = lifter._translate(insn, dummy)
        if call not in new:
            print(f"FAIL {mn} {insn.operands}: the lifter emits {new!r}, not a {call[:-1]} call")
            bad += 1
            continue
        out.append(f"static void new_{i}(ppu_context* ctx) {{ {new} }}")
        out.append(f"static void old_{i}(ppu_context* ctx) {{ {old} }}")
        vd = int(insn.operands.split(",")[0].strip()[1:])
        k = {"bytes": 0, "floats": 1, "mem": 2, "mem0": 3}[kind]
        label = f"{insn.mnemonic} {insn.operands}"
        runs.append(f'    run("{label}", {k}, {vd}, new_{i}, old_{i});')
    out.append("int main(void) {")
    out.append("#ifdef PPU_VMX_SSE41")
    out.append('    printf("path: sse41\\n");')
    out.append("#else")
    out.append('    printf("path: loops\\n");')
    out.append("#endif")
    out.extend(runs)
    out.append(f'    printf("[ppu-vmx-helpers] {len(runs)} statements x %d inputs: %ld agree, %d FAILED\\n", '
               f'ITERS, g_pass, g_fail);')
    out.append("    return g_fail ? 1 : 0;")
    out.append("}")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    return -1 if bad else len(runs)


def builds(tmp, cpp):
    """(label, command, expected path) for each compiler this host has."""
    x86 = platform.machine().lower() in ("amd64", "x86_64", "x64")
    inc = os.path.join(ROOT, "runtime", "ppu")
    out = []
    if os.name == "nt":
        from msvc_env import environ
        env = environ()
        path = env.get("PATH", "")
        cl = shutil.which("cl", path=path)
        clang_cl = shutil.which("clang-cl", path=path)
        def msvc(tool, tag, flags):
            exe = os.path.join(tmp, f"vmx_{tag}.exe")
            return ([tool, "/nologo", "/O2", "/W3", "/EHsc", "/I", inc, *flags,
                     "/Fe" + exe, "/Fo" + os.path.join(tmp, tag + ".obj"), cpp], exe, env)
        if cl:
            if x86:
                out.append(("cl, SSE4.1", *msvc(cl, "cl_sse", []), "sse41"))
            out.append(("cl, loops", *msvc(cl, "cl_loops", ["/DPPU_VMX_NO_SSE41"]), "loops"))
        if clang_cl and x86:
            out.append(("clang-cl, SSE4.1", *msvc(clang_cl, "clang_sse", ["/clang:-msse4.1"]), "sse41"))
    else:
        cxx = (os.environ.get("CXX") or shutil.which("c++")
               or shutil.which("clang++") or shutil.which("g++"))
        if cxx:
            def gcc(tag, flags):
                exe = os.path.join(tmp, f"vmx_{tag}")
                return ([cxx, "-std=c++17", "-O2", "-w", "-I", inc, *flags, "-o", exe, cpp],
                        exe, None)
            if x86:
                out.append((f"{os.path.basename(cxx)}, SSE4.1", *gcc("sse", ["-msse4.1"]), "sse41"))
            out.append((f"{os.path.basename(cxx)}, loops", *gcc("loops", ["-DPPU_VMX_NO_SSE41"]), "loops"))
    return out


def main():
    tmp = tempfile.mkdtemp(prefix="ppu_vmx_")
    try:
        cpp = os.path.join(tmp, "vmx_helpers.cpp")
        n = emit(cpp)
        if n < 0:
            print("[ppu-vmx-helpers] FAILED before compiling (see above)")
            return 1
        todo = builds(tmp, cpp)
        if not todo:
            print("SKIPPED: no C++ compiler found, so nothing was compiled or run.")
            if os.environ.get("CI"):
                print("...and this is CI, where that is a failure.")
                return 1
            return 0
        ok = True
        for label, cmd, exe, env, want in todo:
            cp = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True)
            if cp.returncode != 0:
                print(f"[{label}] COMPILE FAILED:\n{cp.stdout}{cp.stderr}")
                ok = False
                continue
            rp = subprocess.run([exe], cwd=tmp, capture_output=True, text=True)
            lines = rp.stdout.splitlines()
            took = lines[0].split(": ", 1)[1] if lines and lines[0].startswith("path: ") else "?"
            for ln in lines[1:]:
                print(f"[{label}] {ln}")
            if took != want:
                print(f"[{label}] FAIL: built the {took} path, meant to test the {want} path")
                ok = False
            if rp.returncode != 0:
                ok = False
        print("[ppu-vmx-helpers]", "ok" if ok else "FAILED")
        return 0 if ok else 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
