#!/usr/bin/env python3
"""A re-lift rewrites only the files whose text changed, and insn-hook declarations live in
the chunks that call the hooks, not in the header (so a hook change does not touch every
chunk's input)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

LIFTER = str(Path(__file__).with_name('ppu_lifter.py'))
HOOK_DECL = 'extern "C" void host_hook(ppu_context* ctx, uint32_t addr);'
OLD = 10 ** 18   # ns since 1970: September 2001


def lift(root, out, hooks=None, single=False):
    command = [sys.executable, LIFTER, str(root / 'code.bin'), '--raw', '--base', '0x10000',
               '--functions', str(root / 'functions.json'), '--jobs', '1', '--output', str(out)]
    if hooks:
        command += ['--insn-hooks', hooks]
    if single:
        command.append('--single-file')
    subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def stamps(out):
    return {p.name: (p.stat().st_mtime_ns, p.read_bytes()) for p in out.iterdir()}


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / 'code.bin').write_bytes(bytes.fromhex('386000004e800020'))  # li r3,0 ; blr
        (root / 'functions.json').write_text(json.dumps([{'start': '0x10000', 'end': '0x10008'}]))
        out = root / 'out'

        lift(root, out, hooks='0x10000=host_hook')
        header = (out / 'ppu_recomp.h').read_text()
        chunk = (out / 'ppu_recomp_000.cpp').read_text()
        assert 'host_hook' not in header, 'insn-hook declaration still in the header'
        assert HOOK_DECL in chunk, 'insn-hook declaration missing from the chunk that calls it'
        assert 'host_hook(ctx, 0x00010000u);' in chunk, 'insn-hook call missing'
        assert chunk.index(HOOK_DECL) < chunk.index('void func_00010000(ppu_context* ctx) {')

        # Same input again: nothing is rewritten. Back-date both files (a multiple of 100 ns, which
        # NTFS stores exactly); a rewrite would move their times to now.
        before = stamps(out)
        for name in ('ppu_recomp.h', 'ppu_recomp_000.cpp'):
            os.utime(out / name, ns=(OLD, OLD))
        lift(root, out, hooks='0x10000=host_hook')
        after = stamps(out)
        assert after['ppu_recomp.h'][0] == OLD, 'the unchanged header was rewritten'
        assert after['ppu_recomp_000.cpp'][0] == OLD, 'the unchanged chunk was rewritten'
        assert {k: v[1] for k, v in after.items()} == {k: v[1] for k, v in before.items()}

        # Hook removed: the chunk changes, the header does not.
        lift(root, out)
        after = stamps(out)
        assert after['ppu_recomp.h'][0] == OLD, 'the header was rewritten for a hook change'
        assert after['ppu_recomp_000.cpp'][0] != OLD, 'the changed chunk was not rewritten'
        assert 'host_hook' not in (out / 'ppu_recomp_000.cpp').read_text()

        # Stale chunks past the last one, and a stale single-file source, are removed; a single-file
        # lift removes the chunks.
        (out / 'ppu_recomp_007.cpp').write_text('stale')
        (out / 'ppu_recomp.c').write_text('stale')
        lift(root, out)
        names = sorted(p.name for p in out.iterdir())
        assert names == ['ppu_recomp.h', 'ppu_recomp_000.cpp'], names
        lift(root, out, hooks='0x10000=host_hook', single=True)
        names = sorted(p.name for p in out.iterdir())
        assert names == ['ppu_recomp.c', 'ppu_recomp.h'], names
        single = (out / 'ppu_recomp.c').read_text()
        assert HOOK_DECL in single and 'host_hook(ctx, 0x00010000u);' in single
    print('PASS: re-lift rewrites only changed files; insn-hook declarations in the calling chunk')


if __name__ == '__main__':
    main()
