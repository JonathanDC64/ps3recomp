#!/usr/bin/env python3
"""libs/video/rsx_smaa_src.h holds third_party/smaa/SMAA.hlsl, byte for byte.

The D3D12 backend's SMAA pass (RSX_AA=2) compiles that string at run time.
This checks:
  - the header is what tools/smaa_embed.py writes from SMAA.hlsl now (so an
    updated SMAA.hlsl was not left without its header);
  - reading the header's string literals back, with C's escape rules, gives
    SMAA.hlsl exactly (line ends as LF);
  - the string stays under 65535 bytes, the most one string may hold in
    MSVC, so the header builds with every compiler the runtime supports.

Run: python tools/test_smaa_embed.py
"""
import os
import re
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import smaa_embed  # noqa: E402

ESC = {"n": "\n", "t": "\t", "\\": "\\", '"': '"', "?": "?"}


def decode(header):
    """The bytes rsx_smaa_src[] holds, from the header's text."""
    body = header.split("rsx_smaa_src[] =", 1)[1]
    out = bytearray()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', body):
        i = 0
        while i < len(lit):
            c = lit[i]
            if c != "\\":
                out += c.encode("ascii")
                i += 1
                continue
            n = lit[i + 1]
            if n in "01234567":
                j = i + 1
                while j < len(lit) and j < i + 4 and lit[j] in "01234567":
                    j += 1
                out.append(int(lit[i + 1:j], 8))
                i = j
            else:
                out += ESC[n].encode("ascii")
                i += 2
    return bytes(out)


def main():
    src = open(smaa_embed.SRC, "rb").read().replace(b"\r\n", b"\n")
    header = open(smaa_embed.OUT, "r", encoding="ascii", newline="").read().replace("\r\n", "\n")
    fails = 0
    if header != smaa_embed.render():
        print("FAIL: rsx_smaa_src.h is out of date: run python tools/smaa_embed.py")
        fails += 1
    got = decode(header)
    if got != src:
        at = next((i for i in range(min(len(got), len(src))) if got[i] != src[i]), min(len(got), len(src)))
        print("FAIL: the string differs from SMAA.hlsl at byte %d (%d vs %d bytes)" % (at, len(got), len(src)))
        fails += 1
    if len(got) + 1 > 65535:
        print("FAIL: the string is %d bytes; MSVC takes at most 65535" % (len(got) + 1))
        fails += 1
    if fails:
        return 1
    print("ok: rsx_smaa_src.h holds SMAA.hlsl (%d bytes)" % len(src))
    return 0


if __name__ == "__main__":
    sys.exit(main())
