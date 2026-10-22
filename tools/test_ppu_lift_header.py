#!/usr/bin/env python3
"""The fixed part of a lift's header lives in include/ps3emu/ppu_lift.h, and only there.

A lift's ppu_recomp.h includes it; a --symbol-prefix lift copies it inline, cut out of the file at
its marker lines, with the function table renamed. So the file has to keep its shape: every
declaration inside a part, each marker line once. And the two forms of a lift's header have to
say the same thing: the included one, expanded, is the inline one apart from the guard and where
the shadow-stack define sits."""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ppu_lifter as L                                    # noqa: E402
from ppu_lifter import LiftedFunction, PPULifter          # noqa: E402

GUARD = ("#ifndef PS3EMU_PPU_LIFT_H", "#define PS3EMU_PPU_LIFT_H", "#endif /* PS3EMU_PPU_LIFT_H */")
INCLUDE = '#include "ps3emu/ppu_lift.h"'


def code_lines(text):
    """The lines that are not comments or blank: what a compiler sees, near enough."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return [line.rstrip() for line in text.split("\n") if line.strip()]


def lifter(prefix="", shadow=False):
    lf = PPULifter(prefix=prefix)
    lf.shadow_stack = shadow
    lf.functions = [LiftedFunction(name=f"{lf.prefix}func_00010000", start_addr=0x10000, end_addr=0x10008)]
    lf.call_targets = {0x10200}
    lf.override_syms = {"host_override"}
    return lf


def check_fixed_header():
    text = L.LIFT_HEADER_TEXT
    assert all(ord(c) < 128 for c in text), "ppu_lift.h is not ASCII (the lift must be, test_lifter_output_ascii)"
    lines = [line.strip() for line in text.split("\n")]
    code = code_lines(text)
    assert tuple(code[:2]) == GUARD[:2], f"ppu_lift.h does not open with its guard: {code[:2]}"
    assert lines[-1] == "" and [l for l in lines if l][-1] == GUARD[2], "ppu_lift.h does not end with its guard"
    at = {}
    for part in ("preamble", "fence", "shadow_stack"):
        for end in ("begin", "end"):
            mark = f"/* ppu_lifter:{part}:{end} */"
            assert lines.count(mark) == 1, f"want one {mark!r} line, found {lines.count(mark)}"
            at[part, end] = lines.index(mark)
    assert at["preamble", "begin"] < at["fence", "begin"] < at["fence", "end"] < at["preamble", "end"]
    assert at["preamble", "end"] < at["shadow_stack", "begin"] < at["shadow_stack", "end"]

    # Nothing outside the parts but comments: a declaration there would be missing from a
    # --symbol-prefix lift's inline copy.
    raw = text.split("\n")
    outside = raw[:at["preamble", "begin"]] + raw[at["preamble", "end"] + 1:at["shadow_stack", "begin"]] \
        + raw[at["shadow_stack", "end"] + 1:]
    stray = code_lines("\n".join(outside))
    assert stray == code_lines("\n".join(GUARD)), f"ppu_lift.h has code outside its parts: {stray}"


def check_constants():
    assert L.HEADER_PREAMBLE.startswith(L.HEADER_BANNER)
    for name in ("HEADER_PREAMBLE", "FENCE_PREAMBLE", "SHADOW_STACK_BLOCK"):
        assert "ppu_lifter:" not in getattr(L, name), f"{name} kept a marker line"
    assert L.FENCE_PREAMBLE.startswith("/* Guest memory barriers") and "#ifndef PPU_FENCE" in L.FENCE_PREAMBLE
    assert L.FENCE_PREAMBLE in L.HEADER_PREAMBLE
    for want in ("typedef struct ppu_context", "void lv2_syscall(ppu_context* ctx);",
                 "extern const func_entry function_table[];", "#ifndef PPU_THREAD_LOCAL"):
        assert want in L.HEADER_PREAMBLE, f"HEADER_PREAMBLE is missing {want!r}"
    assert "DS_FRAME" not in L.HEADER_PREAMBLE, "the shadow stack is a part of its own"
    assert L.SHADOW_STACK_BLOCK.startswith("#ifdef DS_SHADOW_STACK") and L.SHADOW_STACK_BLOCK.endswith("#endif")


def check_included_form():
    for shadow in (False, True):
        header = lifter(shadow=shadow).emit_header()
        assert header.startswith(L.HEADER_BANNER), header[:120]
        assert header.count(INCLUDE) == 1, "the lift's header does not include the fixed part"
        assert "typedef struct ppu_context" not in header and "DsFrame" not in header, \
            "the lift's header repeats the fixed part"
        assert "void func_00010000(ppu_context* ctx);" in header
        assert 'extern "C" void host_override(ppu_context* ctx);' in header
        if shadow:
            # Ahead of the include: the fixed part's shadow-stack part tests it.
            assert 0 <= header.find(L.SHADOW_STACK_DEFINE) < header.find(INCLUDE)
        else:
            assert "DS_SHADOW_STACK" not in header

        # Expanded, it is the inline form a lift wrote before the split.
        inline = "\n".join([L.HEADER_PREAMBLE] + ([L.SHADOW_STACK_DEFINE] if shadow else [])
                           + [L.SHADOW_STACK_BLOCK] + header.split(INCLUDE + "\n", 1)[1].split("\n"))
        guarded = L.LIFT_HEADER_TEXT.split(GUARD[1] + "\n", 1)[1].rsplit(GUARD[2], 1)[0]
        expanded = code_lines(header.replace(INCLUDE, guarded))
        if shadow:
            define = code_lines(L.SHADOW_STACK_DEFINE)
            i = expanded.index(define[0])
            del expanded[i:i + len(define)]
            j = expanded.index("#ifdef DS_SHADOW_STACK")
            expanded[j:j] = define
        assert expanded == code_lines(inline), "the included form says something the inline form does not"


def check_prefixed_form():
    for shadow in (False, True):
        header = lifter(prefix="libsre_", shadow=shadow).emit_header()
        assert INCLUDE not in header, "a --symbol-prefix lift's header must stand alone"
        assert header.startswith(L.HEADER_PREAMBLE.split("extern const func_entry")[0])
        assert "extern const func_entry libsre_function_table[];" in header
        assert "extern const uint64_t  libsre_function_table_count;" in header
        assert not re.search(r"(?<![\w])function_table", header), "an unprefixed function_table is left"
        assert "void libsre_func_00010000(ppu_context* ctx);" in header
        assert "void libsre_func_00010200(ppu_context* ctx); /* external */" in header
        block = header.find(L.SHADOW_STACK_BLOCK)
        assert block > header.find("libsre_function_table_count")
        if shadow:
            assert header.find("libsre_function_table_count") < header.find(L.SHADOW_STACK_DEFINE) < block
        else:
            assert L.SHADOW_STACK_DEFINE not in header


def check_stand_alone_headers():
    """The scaffold check's stub and the smoke title's header are the lifter's own."""
    for script, fn in (("check_ppu_scaffold.py", "stub_header"), ("make_smoke_elf.py", "build_header")):
        spec = importlib.util.spec_from_file_location(script[:-3], TOOLS / script)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        text = getattr(mod, fn)()
        assert text.startswith(PPULifter().emit_header()), f"{script}: not the lifter's header"
        assert INCLUDE in text and "typedef struct ppu_context" not in text, script


def check_a_lift():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "code.bin").write_bytes(bytes.fromhex("4e800020"))  # blr
        (root / "functions.json").write_text(json.dumps([{"start": "0x10000", "end": "0x10004"}]))
        out = root / "out"
        subprocess.run([sys.executable, str(TOOLS / "ppu_lifter.py"), str(root / "code.bin"), "--raw",
                        "--base", "0x10000", "--functions", str(root / "functions.json"), "--jobs", "1",
                        "--shadow-stack", "--output", str(out)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        header = (out / "ppu_recomp.h").read_text(encoding="ascii")
        assert header.index(L.SHADOW_STACK_DEFINE) < header.index(INCLUDE), header[:400]
        assert "void func_00010000(ppu_context* ctx);" in header
        chunk = (out / "ppu_recomp_000.cpp").read_text(encoding="ascii")
        assert '#include "ppu_recomp.h"' in chunk and "DS_FRAME(0x00010000);" in chunk


def main():
    check_fixed_header()
    check_constants()
    check_included_form()
    check_prefixed_form()
    check_stand_alone_headers()
    check_a_lift()
    print("PASS: ppu_lift.h is the one copy of the fixed header; included and inline forms agree")


if __name__ == "__main__":
    main()
