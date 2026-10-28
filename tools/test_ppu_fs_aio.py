#!/usr/bin/env python3
"""cellFsAioRead and cellFsAioWrite on an fd that plain reads and other threads use too.

An AIO read names its own offset and must leave the fd's position alone.
runtime/ppu/ppu_fs.cpp did a bare seek and read on the fd's FILE*, so the
next cellFsRead went on from where the AIO stopped, and another thread's
seek could land between the two. This pulls the AIO section (the HOST_*
macros through cellFsAioWrite) out of ppu_fs.cpp, builds it into a small
host program with stand-ins for the rest, and checks:

  - a plain read after an AIO read continues from where the plain reads
    were, and the AIO read got the bytes at its offset;
  - 4 threads x 2000 AIO reads on one fd each get their own bytes, and
    every request has its own id;
  - an AIO write puts its bytes at its offset and leaves the position too.

Run: python tools/test_ppu_fs_aio.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)


def aio_section():
    with open(os.path.join(ROOT, "runtime", "ppu", "ppu_fs.cpp"), encoding="utf-8") as f:
        src = f.read().replace("\r\n", "\n")
    i = src.index("#ifdef _WIN32\n#  define HOST_FSEEK64")
    j = src.index("static void cellFsAioWrite(ppu_context* ctx)", i)
    j = src.index("\n", j) + 1
    return src[i:j]


DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <thread>
#ifndef _WIN32
#include <sys/types.h>
#endif
struct ppu_context { uint64_t gpr[32]; };
#define CELL_OK         0
#define CELL_FS_ENOENT  (-2147418106)
#define CELL_FS_EIO     (-2147418111)
#define FS_MAX          64
static FILE* g_files[FS_MAX];
static uint8_t* vm_base;
static uint32_t ppu_vm_size = 0x01000000u;
static void vm_write32(uint32_t a, uint32_t v)
{
    vm_base[a] = (uint8_t)(v >> 24); vm_base[a + 1] = (uint8_t)(v >> 16);
    vm_base[a + 2] = (uint8_t)(v >> 8); vm_base[a + 3] = (uint8_t)v;
}
static void vm_write64(uint32_t a, uint64_t v) { vm_write32(a, (uint32_t)(v >> 32)); vm_write32(a + 4, (uint32_t)v); }
static inline void fs_prefault(uint32_t, uint64_t) {}
static void ps3_invoke_guest(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
%(aio)s

enum { FILE_BYTES = 1 << 20, FD = 5, THREADS = 4, READS = 2000, LEN = 64 };
static uint8_t pattern(uint32_t off) { return (uint8_t)(off * 2654435761u >> 13); }

/* One AIO read through the real handler; the request, id and buffer live in
 * this thread's own corner of guest memory. */
static size_t aio(uint32_t area, uint64_t off, uint32_t len, uint32_t* id)
{
    const uint32_t req = area, id_ea = area + 0x40, buf = area + 0x100;
    vm_write32(req + 0x00, FD); vm_write64(req + 0x08, off);
    vm_write32(req + 0x10, buf); vm_write64(req + 0x18, len);
    ppu_context ctx = {};
    ctx.gpr[3] = req; ctx.gpr[4] = id_ea; ctx.gpr[5] = 0;
    cellFsAioRead(&ctx);
    *id = ((uint32_t)vm_base[id_ea] << 24) | ((uint32_t)vm_base[id_ea + 1] << 16) |
          ((uint32_t)vm_base[id_ea + 2] << 8) | vm_base[id_ea + 3];
    for (uint32_t k = 0; k < len; k++)
        if (vm_base[buf + k] != pattern((uint32_t)off + k)) return 0;
    return len;
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    FILE* w = fopen(argv[1], "wb");
    for (uint32_t k = 0; k < FILE_BYTES; k++) fputc(pattern(k), w);
    fclose(w);
    vm_base = (uint8_t*)calloc(ppu_vm_size, 1);
    g_files[FD] = fopen(argv[1], "rb");
    if (!vm_base || !g_files[FD]) { printf("FAIL: setup\n"); return 1; }
    int fails = 0;

    /* The fd's position across an AIO read. */
    uint8_t head[100], next[16];
    uint32_t id;
    if (fread(head, 1, sizeof head, g_files[FD]) != sizeof head) fails++;
    const size_t got = aio(0x10000, 5000, LEN, &id);
    if (got != LEN) { printf("FAIL: the AIO read at 5000 got the wrong bytes\n"); fails++; }
    if (fread(next, 1, sizeof next, g_files[FD]) != sizeof next) fails++;
    int pos_ok = 1;
    for (uint32_t k = 0; k < sizeof next; k++) pos_ok &= next[k] == pattern(100 + k);
    if (!pos_ok) { printf("FAIL: the plain read after an AIO read did not go on from byte 100\n"); fails++; }
    else printf("ok: a plain read after an AIO read goes on from byte 100\n");

    /* AIO reads racing on one fd. */
    std::atomic<int> wrong{0};
    static uint32_t ids[THREADS][READS];
    std::thread th[THREADS];
    for (int t = 0; t < THREADS; t++)
        th[t] = std::thread([t, &wrong] {
            uint32_t seed = 0x9E3779B9u * (t + 1);
            for (int r = 0; r < READS; r++) {
                seed = seed * 1664525u + 1013904223u;
                const uint64_t off = (seed >> 8) %% (FILE_BYTES - LEN);
                if (aio(0x100000 + 0x1000 * t, off, LEN, &ids[t][r]) != LEN) wrong++;
            }
        });
    for (auto& x : th) x.join();
    if (wrong) { printf("FAIL: %%d of %%d racing AIO reads got another read's bytes\n", wrong.load(), THREADS * READS); fails++; }
    else printf("ok: %%d racing AIO reads on one fd each got their own bytes\n", THREADS * READS);
    static uint8_t seen[THREADS * READS + 2];
    int dup = 0;
    for (int t = 0; t < THREADS; t++)
        for (int r = 0; r < READS; r++) {
            const uint32_t v = ids[t][r];
            if (v < 2 || v >= 2 + THREADS * READS || seen[v]++) dup++;
        }
    if (dup) { printf("FAIL: %%d request ids repeated or out of range\n", dup); fails++; }
    else printf("ok: every request has its own id\n");
    fclose(g_files[FD]);

    /* An AIO write: its bytes land at its offset, the fd's position stays. */
    g_files[FD] = fopen(argv[1], "r+b");
    if (!g_files[FD] || fread(head, 1, sizeof head, g_files[FD]) != sizeof head) { printf("FAIL: reopen\n"); return 1; }
    const uint32_t req = 0x20000, buf = req + 0x100;
    for (uint32_t k = 0; k < LEN; k++) vm_base[buf + k] = (uint8_t)~pattern(7000 + k);
    vm_write32(req + 0x00, FD); vm_write64(req + 0x08, 7000);
    vm_write32(req + 0x10, buf); vm_write64(req + 0x18, LEN);
    ppu_context ctx = {};
    ctx.gpr[3] = req; ctx.gpr[4] = req + 0x40; ctx.gpr[5] = 0;
    cellFsAioWrite(&ctx);
    pos_ok = fread(next, 1, sizeof next, g_files[FD]) == sizeof next;
    for (uint32_t k = 0; pos_ok && k < sizeof next; k++) pos_ok &= next[k] == pattern(100 + k);
    if (!pos_ok) { printf("FAIL: the plain read after an AIO write did not go on from byte 100\n"); fails++; }
    fclose(g_files[FD]);
    FILE* r = fopen(argv[1], "rb");
    uint8_t back[LEN];
    int wrote = r && fseek(r, 7000, SEEK_SET) == 0 && fread(back, 1, LEN, r) == LEN;
    for (uint32_t k = 0; wrote && k < LEN; k++) wrote &= back[k] == (uint8_t)~pattern(7000 + k);
    if (r) fclose(r);
    if (!wrote) { printf("FAIL: the AIO write's bytes are not in the file at its offset\n"); fails++; }
    if (pos_ok && wrote) printf("ok: an AIO write lands at its offset and leaves the fd's position\n");
    return fails ? 1 : 0;
}
"""


def builds(tmp, cpp):
    name = os.path.join(tmp, "aio")
    if os.name == "nt":
        from msvc_env import environ
        env = environ()
        for tool in ("clang-cl", "cl"):
            exe = shutil.which(tool, path=env.get("PATH", ""))
            if exe:
                return [exe, "/nologo", "/w", "/O1", "/EHsc", "/std:c++17", "/Fe" + name + ".exe",
                        "/Fo" + name + ".obj", cpp], name + ".exe", env
        return None
    cxx = (os.environ.get("CXX") or shutil.which("c++")
           or shutil.which("clang++") or shutil.which("g++"))
    if not cxx:
        return None
    return [cxx, "-std=c++17", "-O1", "-w", "-pthread", "-o", name, cpp], name, None


def main():
    tmp = tempfile.mkdtemp(prefix="ppu_aio_")
    try:
        cpp = os.path.join(tmp, "aio.cpp")
        with open(cpp, "w") as f:
            f.write(DRIVER % {"aio": aio_section()})
        todo = builds(tmp, cpp)
        if not todo:
            print("SKIPPED: no C++ compiler found, so the AIO read was not built or run.")
            return 1 if os.environ.get("CI") else 0
        cmd, exe, env = todo
        cp = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True)
        if cp.returncode != 0:
            print(f"COMPILE FAILED:\n{cp.stdout}{cp.stderr}")
            return 1
        rp = subprocess.run([exe, os.path.join(tmp, "data.bin")], cwd=tmp, capture_output=True,
                            text=True, timeout=120)
        print(rp.stdout, end="")
        print("[ppu-fs-aio]", "ok" if rp.returncode == 0 else "FAILED")
        return rp.returncode
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
