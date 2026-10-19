#!/usr/bin/env python3
"""The lift is ASCII, so it is the same bytes on every PC, and a path outside stdout's encoding does not end a run.

Without an encoding, Python writes a file in the system code page: a non-ASCII comment in the lift came out as
different bytes on different PCs, and the Japanese and Korean code pages (cp932, cp949) cannot write U+2014 at all.
PYTHONIOENCODING stands in for a console or pipe whose code page lacks a character of the output path."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).parent
# li r3,0 ; tw 4,r3,r4 ; twi 0,r3,0 ; mtfsf 0xFF,f0 ; blr -- the trap and FPSCR comments, plus the chunk
# preamble's indirect-call one.
CODE = '38600000' '7c832008' '0c030000' 'fdfe058e' '4e800020'
ENV = dict(os.environ, PYTHONIOENCODING='cp1252:strict')
ENV.pop('PYTHONUTF8', None)


def run(args):
    p = subprocess.run([sys.executable] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=ENV)
    assert p.returncode == 0, p.stderr.decode('cp1252', 'replace')[-2000:]
    return p.stdout.decode('cp1252', 'replace')


def ascii_only(out):
    for f in out.iterdir():
        data = f.read_bytes()
        bad = next((i for i, b in enumerate(data) if b > 127), None)
        assert bad is None, f'{f.name}: non-ASCII byte 0x{data[bad]:02X} at offset {bad}'


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / 'code.bin').write_bytes(bytes.fromhex(CODE))
        (root / 'functions.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10014'}]))
        out = root / 'lift-テスト'   # katakana: outside cp1252
        lift = [str(TOOLS / 'ppu_lifter.py'), str(root / 'code.bin'), '--raw', '--base', '0x10000',
                '--functions', str(root / 'functions.json'), '--jobs', '1', '--output', str(out)]

        printed = run(lift)
        assert '\\u30c6' in printed, 'the output path was not printed escaped'
        ascii_only(out)
        chunk = (out / 'ppu_recomp_000.cpp').read_text(encoding='ascii')
        assert 'Indirect call dispatch (bctrl/bctr) -- implemented' in chunk
        assert '/* trap: tw 4, r3, r4 -- ignored */' in chunk
        assert '/* trap: twi 0, r3, 0 -- ignored */' in chunk
        assert '/* mtfsf: FPSCR update -- ignored for now */' in chunk

        # A chunk an older lifter wrote in cp1252 (0x97 = U+2014) is not UTF-8: it is replaced, not kept.
        (out / 'ppu_recomp_000.cpp').write_bytes(chunk.replace('--', '—').encode('cp1252'))
        run(lift)
        assert (out / 'ppu_recomp_000.cpp').read_text(encoding='ascii') == chunk

        run(lift + ['--single-file'])
        ascii_only(out)

        printed = run([str(TOOLS / 'gen_hle_nids.py'), '--all', '--out', str(out / 'hle_nid_table.cpp')])
        assert '\\u30c6' in printed, 'the NID table path was not printed escaped'
        ascii_only(out)
    print('PASS: the lift is ASCII; paths outside stdout\'s encoding are printed escaped')


if __name__ == '__main__':
    main()
