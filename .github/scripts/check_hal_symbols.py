#!/usr/bin/env python3
"""The portable renderer's export check.

    python .github/scripts/check_hal_symbols.py [--list] [--nm <nm>] <build dir | object>...

Every external symbol that an object of the new path defines (libs/video/gpu/,
libs/video/rsxcore/ and libs/video/rsx_out.c, as ps3recomp_runtime compiles
them) has to start with gpu_, rsxc_ or rsx_out_, so the new path never
collides with today's renderer (rsx_d3d12_*), the rest of the runtime or a
game linked with it. A C++ symbol passes when its name, class or namespace
starts with one. ALLOW below lists the few others, each with the reason it
cannot be ours.

A build directory is searched for ps3recomp_runtime's objects of those files
(CMakeFiles/ps3recomp_runtime.dir/...); an object named on the command line is
checked whatever its path. No object found is a failure: the files moved, or
the build is not the one with PS3RECOMP_RSX_HAL=ON.

nm: --nm, else $LLVM_NM, else llvm-nm on PATH, else CMAKE_NM from the build's
CMakeCache.txt, else llvm-nm beside its CMAKE_C_COMPILER, else nm. Written for
clang-cl's COFF objects (CI's Windows job); the patterns for GCC's and Clang's
ELF objects are there too, untried.

    --list   every symbol and why it passes, not only the ones that fail

Exit status: 0 every symbol passes; 1 a symbol outside the prefixes, or no
object found; 2 bad arguments or no nm.
"""
import os
import re
import shutil
import subprocess
import sys

# The objects of the new path, by path (forward slashes).
HAL_FILES = re.compile(r'(^|/)libs/video/(gpu|rsxcore)/[^/]+\.(c|cpp)\.(obj|o)$|(^|/)libs/video/rsx_out\.c\.(obj|o)$')
RUNTIME_DIR = re.compile(r'(^|/)CMakeFiles/ps3recomp_runtime\.dir/')

PREFIX = r'(gpu_|rsxc_|rsx_out_)'
OURS = [
    re.compile(r'^_?' + PREFIX),                         # C, and extern "C"
    re.compile(r'^\?' + PREFIX),                         # MSVC C++: a function or variable of ours
    re.compile(r'^\?[^@]+@(\w+@)*' + PREFIX + r'\w*@@'), # ... a member of our class or namespace
    re.compile(r'^\?\?_(?!C@)[0-9A-Z][^@]*@?(\w+@)*' + PREFIX), # ... our class's vtable, RTTI, special member (not a string literal)
    re.compile(r'^_?_Z[A-Z]*N?K?\d+' + PREFIX),          # Itanium C++: ours, or in our class or namespace
]

# (symbol pattern, object pattern or None for any, the reason it is allowed)
ALLOW = [
    (r'^\?\?_C@',
     None,
     "the compiler's string literals (MSVC ABI): COMDAT, named by their contents, merged across objects"),
    (r'^__(real|xmm|ymm|zmm)@[0-9a-f]+$',
     None,
     "the compiler's floating-point and vector constants: COMDAT, named by their value"),
    (r'^_fltused$|^@feat\.00$',
     None,
     "COFF markers every object carries: floating point is used; the object's features (/guard, SafeSEH)"),
    (r'^(__local_stdio_(printf|scanf)_options|_?v?(f|s|sn|w|fw|sw|snw)?(printf|scanf)(_s)?(_c)?(_p)?(_l)?)$'
     r'|^\?_OptionsStorage@\?1\?\?__local_stdio_(printf|scanf)_options@@',
     None,
     "the UCRT's stdio functions are inline in its headers (stdio.h, corecrt_stdio_config.h): defined, COMDAT, "
     "in every object that calls them; in C++ the options' function-local static (_OptionsStorage) is external too"),
    (r'^_Avx2WmemEnabledWeakValue$',
     None,
     "the UCRT's wchar.h (seen in Windows SDK 10.0.26100) defines it __declspec(selectany) in every object that "
     "includes it, as the fallback /alternatename of _Avx2WmemEnabled"),
    (r'^_?ps3_env_gen$',
     None,
     "include/ps3emu/ps3_knob.h defines it selectany (weak off Windows) in every object that includes the knob "
     "header, so a test that compiles a few files links without ps3_knob.c: the runtime's, not the new path's"),
    (r'^c_DwmMax(QueuedBuffers|Monitors|Adapters)$',
     None,
     "dwmapi.h defines these constants EXTERN_C __declspec(selectany) in every object that includes it"),
    (r'@std@@|@stdext@@|^\?\?_[0-9A-Z]\w*(type_info|exception|bad_alloc|bad_array_new_length)@@'
     r'|^_?_Z[A-Z]*N?K?St|^_?_ZT[VIS]N?St|^_?_ZN9__gnu_cxx|__cxxabiv1',
     re.compile(r'\.cpp\.(obj|o)$'),
     "the C++ standard library's templates and classes, instantiated in the C++ objects (COMDAT)"),
    (r'@glslang@@|@spv@@|^_?_Z[A-Z]*N?K?7glslang|^_?_Z[A-Z]*N?K?3spv|^_?_ZT[VIS]N7glslang|^_?_ZT[VIS]N3spv',
     re.compile(r'(^|/)gpu_shader_spv[^/]*\.cpp\.(obj|o)$'),
     "glslang's own classes: TShader::parse's short form makes a TShader::ForbidIncluder in the calling object "
     "(glslang/Public/ShaderLang.h) and GlslangToSpv takes our spv::SpvBuildLogger, so their inline virtual "
     "functions, destructors, vtables and RTTI are emitted, COMDAT, beside glslang's identical ones. Their "
     "linkage is glslang's: they cannot be internal while the file uses those classes"),
]
ALLOW = [(re.compile(s), o, why) for s, o, why in ALLOW]


def find_nm(nm, inputs):
    if nm:
        return nm
    if os.environ.get('LLVM_NM'):
        return os.environ['LLVM_NM']
    w = shutil.which('llvm-nm')
    if w:
        return w
    for a in inputs:   # the nearest CMakeCache.txt above an input
        d = os.path.abspath(a if os.path.isdir(a) else os.path.dirname(a))
        while True:
            cache = os.path.join(d, 'CMakeCache.txt')
            if os.path.isfile(cache):
                vals = {}
                with open(cache, encoding='utf-8', errors='replace') as f:
                    for line in f:
                        m = re.match(r'^(CMAKE_NM|CMAKE_C_COMPILER):[A-Z]*=(.*)$', line.strip())
                        if m:
                            vals[m.group(1)] = m.group(2)
                if vals.get('CMAKE_NM') and os.path.isfile(vals['CMAKE_NM']):
                    return vals['CMAKE_NM']
                cc = vals.get('CMAKE_C_COMPILER', '')
                for name in ('llvm-nm.exe', 'llvm-nm'):
                    cand = os.path.join(os.path.dirname(cc), name)
                    if cc and os.path.isfile(cand):
                        return cand
                break
            up = os.path.dirname(d)
            if up == d:
                break
            d = up
    return shutil.which('nm')


def objects(inputs):
    found = []
    for a in inputs:
        if os.path.isdir(a):
            for root, _, files in os.walk(a):
                for n in files:
                    p = os.path.join(root, n).replace('\\', '/')
                    if HAL_FILES.search(p) and RUNTIME_DIR.search(p):
                        found.append(p)
        else:
            found.append(a.replace('\\', '/'))
    return sorted(set(found))


def symbols(nm, objs):
    """(object, type, name) for each defined external symbol."""
    out = []
    for i in range(0, len(objs), 32):
        chunk = objs[i:i + 32]
        r = subprocess.run([nm, '--defined-only', '--extern-only'] + chunk, capture_output=True, text=True,
                           errors='replace')
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            raise SystemExit('check_hal_symbols: %s failed (%d)' % (nm, r.returncode))
        cur = chunk[0] if len(chunk) == 1 else None
        for line in r.stdout.splitlines():
            line = line.rstrip()
            if not line:
                continue
            if line.endswith(':') and len(chunk) > 1:
                cur = line[:-1].replace('\\', '/')
                continue
            parts = line.split(None, 2)
            if len(parts) == 3 and re.match(r'^[0-9A-Fa-f]+$', parts[0]):
                out.append((cur or '?', parts[1], parts[2]))
            elif len(parts) == 2:   # (an absolute or common symbol without a value)
                out.append((cur or '?', parts[0], parts[1]))
    return out


def verdict(obj, name):
    for r in OURS:
        if r.search(name):
            return 'ours'
    for s, o, why in ALLOW:
        if s.search(name) and (o is None or o.search(obj)):
            return why
    return None


def main(argv):
    args, nm, listing = [], None, False
    it = iter(argv)
    for a in it:
        if a == '--list':
            listing = True
        elif a == '--nm':
            nm = next(it, None)
        elif a in ('-h', '--help'):
            print(__doc__)
            return 0
        elif a.startswith('-'):
            print('check_hal_symbols: unknown option %s' % a)
            return 2
        else:
            args.append(a)
    if not args:
        print('usage: check_hal_symbols.py [--list] [--nm <nm>] <build dir | object>...')
        return 2
    for a in args:
        if not os.path.exists(a):
            print('check_hal_symbols: no file %s' % a)
            return 2
    objs = objects(args)
    if not objs:
        print('check_hal_symbols: no object of libs/video/gpu, libs/video/rsxcore or rsx_out.c found in '
              + ', '.join(args) + ' (a PS3RECOMP_RSX_HAL=ON build of ps3recomp_runtime?)')
        return 1
    nm = find_nm(nm, args)
    if not nm:
        print('check_hal_symbols: no llvm-nm or nm (set LLVM_NM)')
        return 2
    syms = symbols(nm, objs)
    bad, allowed, ours = [], {}, 0
    for obj, typ, name in syms:
        v = verdict(obj, name)
        if v is None:
            bad.append((obj, typ, name))
        elif v == 'ours':
            ours += 1
        else:
            allowed.setdefault(v, []).append((obj, typ, name))
        if listing:
            print('%-8s %s %-60s %s' % ('BAD' if v is None else 'ours' if v == 'ours' else 'allowed', typ, name,
                                        os.path.basename(obj)))
    nallowed = sum(len(v) for v in allowed.values())
    print('check_hal_symbols: %d objects, %d defined external symbols: %d ours, %d allowed, %d outside '
          'gpu_/rsxc_/rsx_out_' % (len(objs), len(syms), ours, nallowed, len(bad)))
    for _, _, why in ALLOW:
        if why in allowed:
            print('  allowed %4d: %s' % (len(allowed[why]), why))
    if bad:
        print('outside the prefixes (object, type, symbol):')
        for obj, typ, name in sorted(bad):
            print('  %s  %s %s' % (obj, typ, name))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
