#!/usr/bin/env python3
"""Per-thread TLS blocks: how many fit, that they stay apart, and that a freed one comes back fresh.

Builds the real block allocator (s_tls_used, s_tls_mine, ppu_tls_block_bytes,
ppu_tls_block_count, ppu_tls_new_block and ppu_tls_thread_done, pulled out of
runtime/ppu/ppu_loader.cpp) into a small host program. For a few TLS image
sizes it takes blocks until the window is full and checks:

  - a small image (Demon's Souls' 0x434 bytes) gets at least 200 blocks; a
    block that also counted the 0x7000 r13 bias gave 31, and every thread
    after that shared the main thread's TLS;
  - no block overlaps another one or the main block's image, and .tdata was
    copied and .tbss zeroed in each;
  - a block a finished thread gives back is handed out again, set up as a new
    one even after its last owner scribbled over it.

Run: python tools/test_ppu_tls_blocks.py
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)


def pull(src, head):
    """The definition that starts with `head`, through its closing brace."""
    i = src.index(head)
    j = src.index("{", i) + 1
    depth = 1
    while depth:
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        j += 1
    return src[i:j]


def allocator():
    with open(os.path.join(ROOT, "runtime", "ppu", "ppu_loader.cpp"), encoding="utf-8") as f:
        src = f.read().replace("\r\n", "\n")
    used = re.search(r"^static uint8_t s_tls_used\[\d+\];", src, re.M).group(0)
    mine = re.search(r"^static PPU_THREAD_LOCAL uint32_t s_tls_mine = 0;", src, re.M).group(0)
    img = re.search(r"^#define PPU_TLS_IMG\s+(\S+)", src, re.M).group(1)
    funcs = [pull(src, h) for h in ("static uint32_t ppu_tls_block_bytes(void)",
                                    "static uint32_t ppu_tls_block_count(uint32_t total)",
                                    "static uint32_t ppu_tls_new_block(void)",
                                    'extern "C" void ppu_tls_thread_done(void)')]
    return img, "\n".join([used, mine] + funcs)


DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PPU_THREAD_LOCAL thread_local
#define PPU_TLS_IMG %(img)s
static uint8_t* vm_base;
static uint32_t ppu_vm_size = 0;
static uint32_t g_tls_vaddr = 0x10000, g_tls_filesz = 0, g_tls_memsz = 0;
%(alloc)s

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int fresh(uint32_t base)
{
    if (memcmp(vm_base + base, vm_base + g_tls_vaddr, g_tls_filesz)) return 0;
    for (uint32_t k = g_tls_filesz; k < g_tls_memsz; k++)
        if (vm_base[base + k]) return 0;
    return 1;
}

static void run(uint32_t filesz, uint32_t memsz, uint32_t at_least)
{
    g_tls_filesz = filesz; g_tls_memsz = memsz;
    memset(s_tls_used, 0, sizeof s_tls_used);
    for (uint32_t k = 0; k < filesz; k++) vm_base[g_tls_vaddr + k] = (uint8_t)(k * 7 + 1);
    memset(vm_base + PPU_TLS_IMG, 0xEE, 0x11000000u - PPU_TLS_IMG);   /* stale bytes everywhere */

    static uint32_t blk[1024];
    uint32_t n = 0, tp;
    while (n < 1024 && (tp = ppu_tls_new_block()) != 0) blk[n++] = tp - 0x7000u;
    printf("image 0x%%X+0x%%X: %%u blocks of 0x%%X\n", filesz, memsz - filesz, n, ppu_tls_block_bytes());
    CHECK(n >= at_least, "image 0x%%X: only %%u blocks, want at least %%u", memsz, n, at_least);
    CHECK(n == ppu_tls_block_count(ppu_tls_block_bytes()), "image 0x%%X: %%u blocks, count says %%u",
          memsz, n, ppu_tls_block_count(ppu_tls_block_bytes()));
    for (uint32_t i = 0; i < n; i++) {
        CHECK(blk[i] >= PPU_TLS_IMG + memsz, "block %%u at 0x%%X overlaps the main image", i, blk[i]);
        CHECK(blk[i] + memsz <= 0x11000000u, "block %%u at 0x%%X runs past the window", i, blk[i]);
        CHECK(fresh(blk[i]), "block %%u at 0x%%X: .tdata/.tbss not set up", i, blk[i]);
        for (uint32_t j = 0; j < i; j++)
            CHECK(blk[i] >= blk[j] + memsz || blk[j] >= blk[i] + memsz,
                  "blocks %%u and %%u overlap (0x%%X, 0x%%X)", j, i, blk[j], blk[i]);
    }
    if (n < 2) return;
    /* A thread on block 1 scribbles over its TLS and ends; the next thread
     * gets that block back, as a new one. */
    memset(vm_base + blk[1], 0x5A, memsz);
    s_tls_mine = blk[1] + 0x7000u;
    ppu_tls_thread_done();
    CHECK(s_tls_mine == 0, "thread_done kept the block");
    tp = ppu_tls_new_block();
    CHECK(tp == blk[1] + 0x7000u, "freed block 0x%%X not reused (got r13 0x%%X)", blk[1], tp);
    CHECK(tp && fresh(tp - 0x7000u), "reused block 0x%%X not set up again", blk[1]);
    CHECK(ppu_tls_new_block() == 0, "a block appeared from nowhere");
}

int main()
{
    vm_base = (uint8_t*)calloc(0x11000000u, 1);
    if (!vm_base) { printf("FAIL: no memory\n"); return 1; }
    run(0x4, 0x434, 200);       /* Demon's Souls */
    run(0x100, 0x240, 200);     /* a small title */
    run(0x800, 0x1000, 200);    /* exactly a page */
    run(0x2000, 0x9000, 20);    /* bigger than the bias */
    printf("%%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
"""


def builds(tmp, cpp):
    if os.name == "nt":
        # clang-cl only: the allocator uses __atomic builtins, as the loader
        # does, and plain cl has none.
        from msvc_env import environ
        env = environ()
        exe = shutil.which("clang-cl", path=env.get("PATH", ""))
        if not exe:
            return None
        name = os.path.join(tmp, "tls")
        return [exe, "/nologo", "/O1", "/EHsc", "/std:c++17", "/Fe" + name + ".exe",
                "/Fo" + name + ".obj", cpp], name + ".exe", env
    cxx = (os.environ.get("CXX") or shutil.which("c++")
           or shutil.which("clang++") or shutil.which("g++"))
    if not cxx:
        return None
    name = os.path.join(tmp, "tls")
    return [cxx, "-std=c++17", "-O1", "-w", "-o", name, cpp], name, None


def main():
    img, alloc = allocator()
    tmp = tempfile.mkdtemp(prefix="ppu_tls_")
    try:
        cpp = os.path.join(tmp, "tls.cpp")
        with open(cpp, "w") as f:
            f.write(DRIVER % {"img": img, "alloc": alloc})
        todo = builds(tmp, cpp)
        if not todo:
            print("SKIPPED: no C++ compiler found, so the allocator was not built or run.")
            return 1 if os.environ.get("CI") else 0
        cmd, exe, env = todo
        cp = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True)
        if cp.returncode != 0:
            print(f"COMPILE FAILED:\n{cp.stdout}{cp.stderr}")
            return 1
        rp = subprocess.run([exe], cwd=tmp, capture_output=True, text=True, timeout=60)
        print(rp.stdout, end="")
        print("[ppu-tls-blocks]", "ok" if rp.returncode == 0 else "FAILED")
        return rp.returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
