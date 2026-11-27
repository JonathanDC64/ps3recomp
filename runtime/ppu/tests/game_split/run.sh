#!/usr/bin/env bash
# ps3recomp - the split test (ps3emu/game_module.h): builds a small game module the way a split build's is built,
# freestanding (no C library, clang's own headers and include/ps3emu/game_libc/ only), and a host that loads it
# through runtime/ppu/game_host.c, then checks the module's exports and imports and runs the host (host.c says
# what it checks). x86-64 Linux (an ELF .so, ld.lld) and arm64 macOS (a Mach-O .dylib, ld64.lld).
#
# Usage: run.sh [out folder]. MODULE_CC: a clang whose driver finds lld (on a Mac, Homebrew's llvm with its lld,
# as setup's toolchain would be). HOST_CC: the host's compiler (default cc).
set -eo pipefail
root=$(cd "$(dirname "$0")/../../../.." && pwd)
out=${1:-${TMPDIR:-/tmp}/game_split}
mcc=${MODULE_CC:-clang}
hcc=${HOST_CC:-cc}
mkdir -p "$out"
rm -f "$out"/*.o "$out"/game.* "$out"/host

case "$(uname -s)" in
    Darwin)
        mod=game.dylib
        mextra=""
        link="-Wl,-install_name,@rpath/game.dylib $root/runtime/ppu/game_libc_system.tbd"
        hlibs="" ;;
    *)
        mod=game.so
        # floor, trunc, ceil and nearbyint are one instruction with SSE4.1 and C library calls without it, as in
        # a split build's module (DES_PPU_OPT).
        mextra="-ftls-model=initial-exec -msse4.1"
        link="-Wl,-z,defs -Wl,-z,now -Wl,-soname,game.so"
        hlibs="-ldl -lpthread -lm" ;;
esac

for s in runtime/ppu/game_module.c runtime/ppu/game_libc.c runtime/ppu/tests/game_split/module.c; do
    # shellcheck disable=SC2086
    "$mcc" -std=gnu17 -O2 -fPIC -fwrapv -fno-strict-aliasing -ffp-contract=off -nostdlibinc -fvisibility=hidden \
        $mextra -fno-stack-protector -DPS3_GAME_FREESTANDING -DPS3_GAME_MODULE \
        -I"$root/include/ps3emu/game_libc" -I"$root/include" -I"$root/runtime/ppu" \
        -include "$root/include/ps3emu/game_freestanding.h" \
        -c "$root/$s" -o "$out/$(basename "$s" .c).o"
done
# shellcheck disable=SC2086
"$mcc" -nostdlib -fuse-ld=lld -shared $link -o "$out/$mod" "$out/game_module.o" "$out/game_libc.o" "$out/module.o"

# What the module exports: its init alone. What it imports: nothing on Linux, dyld's __tlv_bootstrap on a Mac
# (game_libc.c), from libSystem alone. A name the compiler started calling on its own shows up here.
fail=0
if [ "$(uname -s)" = Darwin ]; then
    exports=$(nm -gU "$out/$mod" | awk '{print $NF}' | sort | tr '\n' ' ')
    imports=$(nm -u "$out/$mod" | awk '{print $NF}' | sort | tr '\n' ' ')
    libs=$(otool -L "$out/$mod" | tail -n +2 | awk '{print $1}' | grep -v '@rpath/game.dylib' | tr '\n' ' ')
    want_exports="_game_init "
    want_imports="__tlv_bootstrap "
    [ "$libs" = "/usr/lib/libSystem.B.dylib " ] || { echo "FAIL: the module links $libs"; fail=1; }
else
    exports=$(nm -D --defined-only "$out/$mod" | awk '{print $NF}' | sort | tr '\n' ' ')
    imports=$(nm -D --undefined-only "$out/$mod" | awk '{print $NF}' | sort | tr '\n' ' ')
    needed=$(readelf -d "$out/$mod" | grep NEEDED || true)
    want_exports="game_init "
    want_imports=""
    [ -z "$needed" ] || { echo "FAIL: the module needs $needed"; fail=1; }
fi
[ "$exports" = "$want_exports" ] || { echo "FAIL: the module exports '$exports', not '$want_exports'"; fail=1; }
[ "$imports" = "$want_imports" ] || { echo "FAIL: the module imports '$imports', not '$want_imports'"; fail=1; }
[ "$fail" = 0 ] || exit 1
echo "module: exports $exports; imports ${imports:-nothing}"

# shellcheck disable=SC2086
"$hcc" -std=gnu17 -O1 -D_GNU_SOURCE -DPS3_GAME_SPLIT -include "$root/include/ps3emu/game_split.h" \
    -I"$root/include" -I"$root/runtime/ppu" \
    "$root/runtime/ppu/game_host.c" "$root/runtime/ppu/tests/game_split/host.c" -o "$out/host" $hlibs
"$out/host"
