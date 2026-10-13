#!/usr/bin/env python3
"""Regression test: the VMX merges pick their element size from mn[5].

vmrgh{b,h,w} / vmrgl{b,h,w} interleave bytes, halfwords or words. An older
handler chose the size with `"h" in mn[4:]`, which is also true for "hw", so
vmrghw lifted as a halfword merge -- half of the matrix-transpose idiom, every
transposed row came out as a halfword interleave. That handler had been
shadowed by the one that reads mn[5] and was removed; this keeps the six
forms pinned to the right lane type and the right half.
"""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]


def _load(name, rel_path):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel_path)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


L = _load("ppu_lifter", "tools/ppu_lifter.py")


def _lift_one(mnemonic, operands):
    insns = [SimpleNamespace(addr=0x1000, mnemonic=mnemonic, operands=operands),
             SimpleNamespace(addr=0x1004, mnemonic="blr", operands="")]
    func = L.PPULifter().lift_function(insns, 0x1000, 0x1008)
    return "\n".join(ln for ln in func.body_lines if "return" not in ln)


# mnemonic -> (lane type, first source element of the merged half)
CASES = {
    "vmrghb": ("uint8_t", 0), "vmrglb": ("uint8_t", 8),
    "vmrghh": ("uint16_t", 0), "vmrglh": ("uint16_t", 4),
    "vmrghw": ("uint32_t", 0), "vmrglw": ("uint32_t", 2),
}


def test_merge_element_size():
    for mn, (ty, first) in CASES.items():
        src = _lift_one(mn, "v1, v2, v3")
        assert f"{ty}* a=({ty}*)&ctx->vr[2]" in src, (mn, src)
        assert f"t[0]=a[{first}]; t[1]=b[{first}];" in src, (mn, src)
        n = {"uint8_t": 16, "uint16_t": 8, "uint32_t": 4}[ty]
        assert len(re.findall(r"t\[\d+\]=[ab]\[", src)) == n, (mn, src)


if __name__ == "__main__":
    test_merge_element_size()
    print("ok")
