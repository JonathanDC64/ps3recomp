# ps3recomp

### *Because the Cell processor deserves a second life*

> Static recompilation runtime libraries for PlayStation 3 titles.
> Turn PS3 binaries into native executables. No emulator required.

**[Join the sp00nznet recomp Discord](https://discord.gg/CRpzGWZFcu)** — the
community hub for sp00nznet's recomp projects, where ps3recomp development
happens in the open. Good place to ask questions, show a port you are working
on, or find out what people are stuck on before you duplicate the effort.

---

## What Is This?

**ps3recomp** is an open-source toolkit that provides the runtime libraries, system stubs, and analysis tools needed to **statically recompile PlayStation 3 games into native PC executables**.

Instead of interpreting or dynamically recompiling PowerPC/SPU instructions at runtime (what emulators like RPCS3 do brilliantly), we take the opposite approach: **translate everything ahead of time** into C/C++ that compiles with your favorite compiler on any modern platform.

This is the same philosophy behind:
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) (N64 -> native)
- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) (Xbox 360 -> native)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) (PS2 -> native)
- [burnout3](https://github.com/sp00nznet/burnout3) (Original Xbox -> native)
- [ReXGlue](https://github.com/rexglue/rexglue-sdk) (Xbox 360 -> native)
- [AnyPS5](https://github.com/boykopovar/AnyPS5) (PS5 -> native)

...but for the PS3's glorious, terrifying **Cell Broadband Engine**.

## Why PS3?

The PlayStation 3 has over **3,000 titles** and some of the most beloved games ever made. RPCS3 has done heroic work making ~70% of the library playable through emulation, but static recompilation offers something different:

- **Native performance** — no runtime translation overhead
- **Lower hardware requirements** — runs on mid-range PCs
- **Moddability** — recompiled C/C++ is infinitely more hackable than PPC binaries
- **Preservation** — native ports survive long after emulators stop being maintained
- **Portability** — the output is C/C++, so it can target any platform a compiler does (see [platform support](#platform-support) for where the runtime actually stands today)

## Platform support

CI builds and tests every row on every pull request: Windows (clang-cl), macOS
arm64, Linux (GCC and Clang), and Linux with the Vulkan backend on lavapipe
under the validation layer. A green check means the runtime built and the tests
passed. It does not mean a game ran; the last row is the only one that claims
that.

| | Windows | macOS (arm64) | Linux |
|---|---|---|---|
| Runtime library builds | yes | yes | yes |
| Lifter test suites + C unit tests | yes | yes | yes |
| Render backend | D3D12 | Metal | Vulkan (opt-in); null (headless, CI) |
| Runs a recompiled game, in this tree | **yes** | **no** | **no** |

**What stops macOS and Linux running a title is the PPU boot scaffold.**
`ppu_loader.cpp`, `boot_main.cpp` and the HLE dispatch are compiled per game,
not into the library. They compile on POSIX with zero errors (a CI ratchet
keeps it that way) and the frame clock runs there, but no title has yet been
booted to gameplay from this tree on either OS.

**The renderers are further along than that.** `ps3recomp_host` drives cellGcm
→ RSX → backend end to end with no game. With guest programs on, Metal and
Vulkan both run the title's own shaders on the shared register-file draw
engine. Every `ps3recomp_host` scene passes on both, including render-to-texture,
MRT, depth textures, cube maps and stencil. Linux's default stays the headless
null backend, a CPU triangle filler that CI draws with.

**Downstream, people are already past this.** Twisted Metal's menus run on
Linux/Vulkan, inFamous on macOS, and Killzone 3 under Proton (see
[In the community](#in-the-community)). Those ports carry patches that are
still on their way upstream, which is why the table still says "no".

## The Challenge

The PS3's Cell processor is a beast unlike anything else:

| Component | What It Is | Why It's Hard |
|-----------|-----------|---------------|
| **PPU** | PowerPC 64-bit main CPU | Custom ISA extensions, VMX/AltiVec SIMD |
| **6x SPU** | 128-bit SIMD vector processors | Proprietary ISA, 256KB local store, explicit DMA |
| **Memory** | 256MB XDR RAM | 128-byte alignment requirements, split bus |
| **LV2 Kernel** | Proprietary OS layer | ~300+ system calls, complex threading model |
| **PRX Modules** | Dynamic libraries | 100+ system libraries with intricate interdependencies |

We don't shy away from hard problems. We just break them into smaller ones.

## Architecture

```
ps3recomp/
├── tools/                    # Binary analysis & recompilation pipeline
│   ├── elf_parser.py         # Parse PS3 ELF/SELF/PRX binaries
│   ├── ppu_disasm.py         # PowerPC disassembler with PS3 extensions
│   ├── ppu_lifter.py         # PPU assembly -> C code lifter
│   ├── spu_disasm.py         # SPU instruction set disassembler
│   ├── spu_lifter.py         # SPU assembly -> C code lifter
│   ├── extract_spu_images.py # Extract embedded SPU images from PRX/ELF
│   ├── find_spu_functions.py # SPU function boundary detection
│   ├── wrap_spu_elf.py       # Wrap a raw SPU blob as a loadable ELF
│   ├── prx_analyzer.py       # Analyze PRX imports/exports and NIDs
│   ├── nid_database.py       # Function Name ID (NID) resolver
│   ├── find_functions.py     # Function boundary detection
│   └── generate_stubs.py     # Generate HLE stub skeletons from a NID list
│
├── runtime/                  # Core runtime for recompiled executables
│   ├── ppu/                  # PPU execution context
│   │   ├── ppu_context.h     # Register file, CR, LR, CTR, FPSCR
│   │   ├── ppu_memory.h      # Memory access macros (loads, stores, byte-swap)
│   │   └── ppu_ops.h         # Instruction operation macros
│   ├── spu/                  # SPU execution context
│   │   ├── spu_context.h     # 128×128-bit register file, 256KB local store, image_id
│   │   ├── spu_channels.c    # Channel ops + per-image function-table dispatch
│   │   ├── spu_helpers.h     # SPU intrinsics (incl. float conversions)
│   │   ├── spu_dma.h         # DMA (MFC) transfer engine
│   │   └── tests/            # Self-contained SPU runtime tests
│   ├── memory/               # Memory subsystem
│   │   └── vm.h              # Virtual memory manager (256MB address space)
│   └── syscalls/             # LV2 kernel system call implementations
│       ├── sys_ppu_thread.h  # Thread creation/management
│       ├── sys_mutex.h       # Mutex/cond synchronization
│       ├── sys_memory.h      # Memory allocation/mapping
│       ├── sys_fs.h          # Filesystem operations
│       └── lv2_syscall_table.h # Full syscall dispatch table
│
├── libs/                     # HLE library implementations
│   ├── audio/                # cellAudio, cellAdec, libmixer, libsnd3, libsynth2
│   ├── video/                # cellGcmSys, cellResc, cellVideoOut, cellVpost, cellVdec
│   ├── input/                # cellPad, cellKb, cellMouse
│   ├── hardware/             # cellGem, cellCamera, cellUsbd
│   ├── network/              # cellNetCtl, cellHttp, cellHttps, cellSsl, sceNp*
│   ├── filesystem/           # cellFs, cellGame, cellSaveData, cellStorage
│   ├── system/               # cellSysutil, cellSysmodule, cellRtc, sysPrxForUser
│   ├── spurs/                # cellSpurs, cellFiber, cellSpursJq, cellDaisy (SPU task management)
│   ├── sync/                 # cellSync, cellSync2 (cross-PPU/SPU synchronization)
│   ├── codec/                # cellJpgDec, cellPngDec, cellGifDec, cellPamf, cellDmux, cellAdecAtrac3p, cellAdecCelp8, cellVdecDivx
│   ├── font/                 # cellFont, cellFontFT, cellFreeType, cellL10n
│   └── misc/                 # cellScreenshot, cellSubdisplay, cellImeJp, cellVideoExport, cellMusicExport, cellGameRecording, etc.
│
├── include/ps3emu/           # Public API headers
│   ├── ps3types.h            # Fundamental PS3 types (u8-u128, s8-s64, be_t<>)
│   ├── endian.h              # Big-endian <-> little-endian conversion
│   ├── nid.h                 # NID hashing and lookup
│   ├── module.h              # Module registration framework
│   └── error_codes.h         # PS3 system error codes (CELL_OK, CELL_E*)
│
├── templates/project/        # Starter template for new game ports
│   ├── CMakeLists.txt        # Build system
│   ├── main.cpp              # Application entry point
│   ├── stubs.cpp             # Game-specific function overrides
│   └── config.toml           # Recompiler configuration
│
├── config/                   # Reference configurations
│   └── example.toml          # Example recompilation config
│
└── docs/                     # See the Documentation table below
```

## How It Works

The pipeline follows the same proven approach as our [burnout3](https://github.com/sp00nznet/burnout3) project, adapted for the Cell architecture:

```
   PS3 Game Disc / PKG
         │
         ▼
   ┌─────────────┐
   │  SELF/EBOOT  │  Encrypted PS3 executable
   │  Decryption  │  (requires keys / fSELF)
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  ELF Parser  │  Extract sections, segments, relocations
   │  + PRX Scan  │  Identify imported NIDs & library deps
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  PPU Disasm  │  Disassemble PowerPC 64-bit + VMX
   │  + Analysis  │  Function boundaries, jump tables, ABI
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  PPU Lifter  │  PowerPC → C code generation
   │  + SPU Lift  │  SPU programs → C code generation
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  Link with   │  ps3recomp runtime + HLE libs
   │  Runtime     │  Provides all PS3 OS services
   └──────┬──────┘
          │
          ▼
   ┌─────────────┐
   │  Compile &   │  MSVC / GCC / Clang
   │  Ship!       │  Native x86-64 executable
   └─────────────┘
```

## SPU Strategy

The SPU is the elephant in the room. Each SPU has its own instruction set, its own 256KB memory, and communicates with the PPU via DMA and mailbox channels. Our approach:

1. **SPU programs are self-contained** — they live in ELF segments loaded to local store (`extract_spu_images.py` pulls them out of PRX/ELF)
2. **Recompile each SPU program separately** — dedicated `spu_disasm.py` + `spu_lifter.py` handle the SPU ISA
3. **SPU local store becomes an array** — 256KB per "virtual SPU"
4. **DMA operations become memcpy** — via the MFC engine (`spu_dma.h`)
5. **Channels become cross-thread message queues** — preserving ordering guarantees
6. **Per-image function-table dispatch** — `image_id` lets multiple SPU programs (e.g. a SPURS kernel, a policy module, and a job) coexist at overlapping local-store addresses

This mirrors how RPCS3 handles SPU but at compile time rather than runtime. The
disassembler, lifter, and runtime are functional and covered by self-contained
tests (`runtime/spu/tests/`: sum, shufb, DMA, brsl-return).

## Module Status

HLE implementations of the PS3 system libraries, written for this project. RPCS3 is a behavioural reference, not a code source. **About 93 modules complete and 5 partial** (media decode, SPURS), in roughly 260 files and 100,000 lines under `libs/`.

| Category | Modules | Status |
|----------|---------|--------|
| **Kernel Threading** | sys_ppu_thread, sys_mutex, sys_cond, sys_semaphore, sys_rwlock | ✅ Complete |
| **Kernel Events** | sys_event (queues, ports, flags), sys_timer | ✅ Complete |
| **Kernel Memory** | sys_memory, sys_mmapper (containers, shared mem) | ✅ Complete |
| **Kernel Filesystem** | sys_fs (path mapping, real I/O) | ✅ Complete |
| **Filesystem** | cellFs (real file I/O, dir ops, stat, path translation) | ✅ Complete |
| **Save System** | cellSaveData (callback-driven, PARAM.SFO), cellGame | ✅ Complete |
| **Input** | cellPad (XInput/SDL2), cellKb, cellMouse | ✅ Complete |
| **Audio** | cellAudio (WASAPI/SDL2, mixing thread, multi-port) | ✅ Complete |
| **Video Output** | cellVideoOut (resolution config, 720p default) | ✅ Complete |
| **Codecs** | cellPngDec, cellJpgDec, cellGifDec (stb_image) | ✅ Complete |
| **Font** | cellFont (stb_truetype backend + fallback metrics) | ✅ Complete |
| **Network** | sys_net (BSD sockets, P2P), cellNet, cellNetCtl, cellHttpUtil, cellSsl, sceNp*, sceNpTrophy, sceNpMatching2 + Score over [psnr](docs/ONLINE.md) | ✅ Complete |
| **Hardware** | cellUsbd (USB), cellCamera (PS Eye), cellGem (PS Move) — stub, no devices | ✅ Complete |
| **Sync** | cellSync (atomic spinlocks, LF queue), cellSync2 (OS-backed) | ✅ Complete |
| **System** | cellRtc, cellMsgDialog, cellOskDialog, cellUserInfo, cellGameExec | ✅ Complete |
| **Localization** | cellL10n (UTF-8/16/32/UCS-2, ISO-8859-1, generic converter) | ✅ Complete |
| **Resolution** | cellResc (display modes, scaling, interlace, aspect ratio) | ✅ Complete |
| **Fibers** | cellFiber (PPU fibers via Windows Fibers/ucontext) | ✅ Complete |
| **AV Config** | cellAvconfExt (audio output info, gamma, sound availability) | ✅ Complete |
| **Input Util** | cellKey2char (HID scancode → Unicode, US-101 layout) | ✅ Complete |
| **Graphics** | cellGcmSys (cmd buffer, IO mapping, tile/zcull, flip handlers, timestamps, FIFO walker + ring recycle), RSX command processor (NV47xx parsing, state tracking, backend callbacks), **live D3D12 backend** (executes the title's real NV4097 vertex/fragment programs — indexed draws, per-draw VP constants + textures, render-to-texture, MRT, FP predication), null backend (Win32 window) | ✅ Complete |
| **SPURS** | cellSpurs (management APIs, event flags, task/workload tracking), cellSpursJq (job queues with wait/signal), cellDaisy (real ring buffer FIFO) | ✅ Complete |
| **Core Runtime** | cellSysutil (BGM, cache, disc), cellSysmodule, sysPrxForUser (real lwmutex/lwcond/threads) | ✅ Complete |
| **Media Pipeline** | cellPamf (PAMF header + stream descriptors), cellDmux (callback sequencing), cellVdec (H.264/MPEG-2 through FFmpeg loaded at run time, opt-in), cellAtrac (ATRAC3plus, all-in-memory), cellAdec (callbacks, no decode yet), cellSail (state machine) | 🔨 Partial |
| **HTTP/HTTPS** | cellHttp (real HTTP/1.1 via native sockets), cellHttps (TLS stub with cert management) | ✅ Complete |
| **Misc/Export** | cellSubdisplay, cellImeJp, cellVideoExport, cellMusicExport, cellPhotoExport/Import, cellGameRecording, cellPrint, cellRemotePlay | ✅ Complete |

Full status tracking: [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md)

## Documentation

We've written extensive docs covering every aspect of the project. Whether you're contributing, porting a game, or just trying to understand how PS3 static recompilation works, there's a doc for you.

| Document | What It Covers |
|----------|---------------|
| **[Getting Started](docs/GETTING_STARTED.md)** | Prerequisites, installation, first recompilation walkthrough |
| **[Architecture](docs/ARCHITECTURE.md)** | Cell processor overview, recomp pipeline stages, memory model, comparison with RPCS3 |
| **[Building](docs/BUILDING.md)** | CMake build system, compiler support, platform-specific notes, troubleshooting |
| **[Game Porting Guide](docs/GAME_PORTING_GUIDE.md)** | Full 12-phase walkthrough for porting a PS3 game — with [flOw case study](docs/GAME_PORTING_GUIDE.md#case-study-flow) |
| **[Custom Modules](docs/CUSTOM_MODULES.md)** | Step-by-step tutorial for writing new HLE modules — NID system, calling convention bridge, patterns |
| **[FAQ & Troubleshooting](docs/FAQ.md)** | Common questions, build errors, runtime crashes, lifter issues, performance tips |
| **[Contributing](CONTRIBUTING.md)** | Development setup, code style, PR guidelines, ways to contribute |
| **[Runtime Reference](docs/RUNTIME.md)** | Virtual memory manager, PPU/SPU execution contexts, type system, endianness, syscall dispatch, DMA engine |
| **[Syscall Reference](docs/SYSCALLS.md)** | All LV2 kernel syscalls: threading, sync, events, timers, memory, filesystem |
| **[NID System](docs/NID_SYSTEM.md)** | How PS3 function linking works, NID computation, module registration framework |
| **[Module Reference](docs/MODULES_REFERENCE.md)** | Detailed documentation for every HLE module — what they do and how they're implemented |
| **[Module Status](docs/MODULE_STATUS.md)** | Quick-reference status table for all modules |
| **[RSX Graphics](docs/RSX_GRAPHICS.md)** | RSX GPU translation architecture: command processor, D3D12 and Metal backends, shader strategy |
| **[Tools Reference](docs/TOOLS.md)** | Every recompiler pipeline tool documented: ELF parser, disassembler, lifter, NID database |
| **[SPU Lifter](docs/SPU_LIFTER.md)** | SPU ISA decoding/lifting, the runtime model (local store, channels, DMA, per-image dispatch), and the SPU tests |
| **[Platform Abstraction](docs/PLATFORM_ABSTRACTION.md)** | How we handle Win32 vs POSIX: threading, sockets, timers, audio, memory, fibers |
| **[SPU Fallback](docs/SPU_FALLBACK.md)** | The PPU-side fallback registry — a hand-written replacement for an SPU job, or a bring-up shim before an image is lifted |
| **[Debug Console](docs/DEBUG_CONSOLE.md)** | Ask a title that is already running what it is doing, without a rebuild |
| **[Runtime Diagnostics](docs/DIAGNOSTICS.md)** | Generated index of every diagnostic environment variable the runtime reads |
| **[Prototype & Debug Builds](docs/PROTO_BUILDS.md)** | Why unencrypted, symbol-bearing prototype builds are the pipeline's ground truth |
| **[Online Play](docs/ONLINE.md)** | Matchmaking, leaderboards and P2P play through a [psnr](https://github.com/sp00nznet/psnr) server — `--psnr host --username NAME` |
| **[Regression Gate](docs/REGRESSION_GATE.md)** | How five ports are checked against recorded goldens before a shared-runtime change lands — and how to tell a real red from the four kinds of measurement artefact |

## Getting Started

**Downloads:** each [release](https://github.com/sp00nznet/ps3recomp/releases/latest)
includes a prebuilt SDK bundle per platform (`ps3recomp-<version>-win-x64.zip`,
`-mac-arm64.zip`, `-linux-x64.zip`) plus `SHA256SUMS.txt`. A bundle contains the
runtime library, its headers and the Python pipeline. `VERSION.txt` inside it
says what that platform can and cannot do yet.

> **Prerequisites (building from source)**: Python 3.10+, CMake 3.20+, a C17/C++20 compiler (MSVC 19.x / clang-cl, GCC 12+, or Clang 15+)

```bash
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp
pip install -r requirements.txt

# Stage 1: image, OPD function table, TOC and firmware imports
python tools/ppu_loader.py /path/to/EBOOT.ELF -o out/

# Stage 2: lift to C++. --hle-stubs turns each firmware import stub into
# ps3_hle_call(nid); leave it out and the first import call crashes the port.
python tools/ppu_lifter.py /path/to/EBOOT.ELF \
    --functions out/EBOOT.functions.json \
    --hle-stubs out/EBOOT.imports.json \
    --output templates/project/recompiled/

# Stage 3: the NID -> HLE handler table, next to the lifted code
python tools/gen_hle_nids.py --all --out templates/project/recompiled/ppu_hle_nids.cpp

# Build the port from the template (RECOMP_DIR defaults to ./recompiled)
cd templates/project
cmake -B build -G Ninja
cmake --build build
```

See [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) for the full walkthrough.

## Game Ports Using ps3recomp

*Status as of 2026-10-12. Each port's own README carries the live detail and the run recipe.*

### In game

| Game | Title ID | Status | Repo |
|------|----------|--------|------|
| **The Simpsons Arcade Game** (Konami) | NPUB30563 | **Playable, and playable online.** Boots through the Konami intro and character select into Stage 1, Downtown Springfield, with its ATRAC3plus music (#186) and correct flashes and glyphs (#185). **Two players meet over [psnr](https://github.com/sp00nznet/psnr)** and play a match together: lobby, character select, the host's game setup, then Stage 1 in sync on both machines with each player's input moving their character on both screens — including with one player behind a NAT, through psnr's relay (#200, see [docs/ONLINE.md](docs/ONLINE.md)). A Konami arcade *emulator* EBOOT: the 1991 coin-op ROM lives in `SIMPSONS.SR` and the binary emulates the hardware around it | [sp00nznet/simpsonsarcade-ps3](https://github.com/sp00nznet/simpsonsarcade-ps3) |
| **Tornado Outbreak** (Loose Cannon / Konami) | BLUS30371 | **In game, rendering correctly with no per-title overrides.** Logos and Bink intro, title, main menu, New Game (the save is created), the story intro and dialogue, then the first level under stick control at ~10–16 fps. The runtime fixes it needed — raw-SPU proxy DMA, vertex-program flow control, GPU resolve copies — are shared (#189) | [sp00nznet/tornado](https://github.com/sp00nznet/tornado) |

<p align="center">
  <img src="https://raw.githubusercontent.com/sp00nznet/simpsonsarcade-ps3/main/docs/media/06-stage1.png" alt="The Simpsons Arcade Game, Stage 1" width="48%">
  <img src="https://raw.githubusercontent.com/sp00nznet/tornado/main/docs/media/hero.gif" alt="Tornado Outbreak in the farmers' market" width="48%">
</p>

### Earlier ports

Not under active development; they are where much of the runtime was first
proven, and several stay in the [regression gate](docs/REGRESSION_GATE.md).

| Game | Title ID | Where it got to | Repo |
|------|----------|-----------------|------|
| **Virtua Fighter 5** (Sega AM2) | BLUS30020 | Renders its NOW LOADING, CRIWARE and AM2 screens; zero SPURS and zero PSN imports | [sp00nznet/vf5](https://github.com/sp00nznet/vf5) |
| **Tokyo Jungle** (Crispy's/SCE Japan) | NPUA80523 | Data install, SPURS jobs, title screen and the main menus | [sp00nznet/tokyojungle](https://github.com/sp00nznet/tokyojungle) |
| **You Don't Know Jack** (Jellyvision/THQ) | BLUS30569 | Menus and answerable quiz questions; movie-backed screens still draw noise | [sp00nznet/youdontknowjack](https://github.com/sp00nznet/youdontknowjack) |
| **Twisted Metal** (Eat Sleep Play) | BCUS98106 | Drove the live NV4097 engine (#113); boots real game code and its asset install | [sp00nznet/twistedmetal](https://github.com/sp00nznet/twistedmetal) |
| **flOw** (thatgamecompany) | NPUA80001 | First draws with the real `libsre.prx`; assets load | [sp00nznet/flow](https://github.com/sp00nznet/flow) |
| **Rubber Ducky** (Sony E3 2006 demo) | NPEA00003 | A debug build with full symbols — the lifter's ground truth, not a game target | [sp00nznet/rubberducky](https://github.com/sp00nznet/rubberducky) |

### In the community

Ports other people are running on ps3recomp, whose fixes come back upstream:

- **inFamous** ([@NoahBPeterson](https://github.com/NoahBPeterson)): in game on
  macOS/arm64 at around 13 fps, booting on Sony's own liblv2 and libsre (LLE).
  The LLE and conformance work is on its way upstream (#213).
- **Twisted Metal** ([@Kurtisone](https://github.com/Kurtisone)): menus on
  Linux/Vulkan, the deferred renderer's lighting and post-FX included. Most of
  v0.14.0's draw-engine and Vulkan work came out of this port.
- **Killzone 3** ([@undecuple](https://github.com/undecuple)): reaches the
  first level with geometry, lighting, shadows and depth of field (Windows
  build, run under Proton). About 60 general fixes are queued from it (#214).
- *LittleBigPlanet* ([@sagemono](https://github.com/sagemono)), *Yakuza: Dead
  Souls* ([@canersaka](https://github.com/canersaka)), *God of War II HD* and
  *Ben 10 Omniverse* ([@andrebrumdev](https://github.com/andrebrumdev)), and
  *DBZ Budokai HD* ([@gnome41](https://github.com/gnome41/ps3recomp-dbz-budokai-hd)).

Ask in the [Discord](https://discord.gg/CRpzGWZFcu) to get yours listed.

Want to port a game? Start with the [Getting Started](#getting-started) section, check [docs/MODULE_STATUS.md](docs/MODULE_STATUS.md) for system library coverage, and see the [Game Porting Guide](docs/GAME_PORTING_GUIDE.md) for a real-world walkthrough.

## Relationship to Other Projects

| Project | Role |
|---------|------|
| **[RPCS3](https://github.com/RPCS3/rpcs3)** | Our primary reference for HLE module behavior and system call semantics. Standing on the shoulders of giants. |
| **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** | PowerPC recompiler for Xbox 360. Same ISA family and the same approach; our lifter is our own. |
| **[N64Recomp](https://github.com/N64Recomp/N64Recomp)** | Pioneered the modern static recomp approach. Our architecture follows the same "recompile to C, link with runtime" philosophy. |
| **[PS2Recomp](https://github.com/ran-j/PS2Recomp)** | Sibling project for PS2. Different ISA (MIPS vs PowerPC) but same spirit. |
| **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** | The closest sibling this project has: a static recompilation SDK for the Xbox 360, turning PowerPC into portable C++ compiled ahead of time rather than interpreted. Same shape as ps3recomp — a toolkit plus a runtime, with game ports built on top — and a generation of Xenon hardware away from Cell. Their release engineering covers five platforms from one reusable build workflow, with nightlies and tagged releases that publish artifacts. |
| **[pcrecomp](https://github.com/sp00nznet/pcrecomp)** | Our PC recompilation toolkit. Shared philosophy of "analyze → disassemble → lift → link → ship". |

## Building Blocks We Leverage

- **RPCS3**: a behavioural reference for HLE modules and syscalls. We read what it does and then write our own. No RPCS3 (GPL) code belongs in this tree.
- **Direct3D 12 / Metal / Vulkan**: RSX graphics translation. D3D12 is the backend that runs games in this tree today. Metal (macOS) and Vulkan (Linux, opt-in, see [docs/BUILDING.md](docs/BUILDING.md#the-vulkan-backend-linux-opt-in)) run guest shaders on the shared draw engine. CI draws with the headless null backend.
- **SDL2**: input and audio off Windows. `cellPad` and `cellAudio` always use their SDL2 backends on non-Windows hosts, so SDL2 is required there; on Windows they use XInput and WASAPI. SDL2 does no windowing here; each graphics backend makes its own.
- **FFmpeg** (optional, loaded at run time): video decode for cellVdec and ATRAC3plus for cellAtrac. It is never linked and never vendored.

## Contributing

This is a massive undertaking and we need help. Here's how to get involved:

- **HLE Module Authors** — Fill in the partial modules (media decode, SPURS edge cases) from documented behaviour
- **PPU/SPU Experts** — Improve the disassembler and lifter accuracy
- **Graphics Engineers** — Bring Metal and Vulkan to D3D12 parity, then run a title on them
- **Game Testers** — Try recompiling titles and report what breaks
- **Documentation** — Help us map out the PS3's system library landscape

See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

Not sure where to start, or want to sanity-check an idea first? Ask in the
[Discord](https://discord.gg/CRpzGWZFcu) — several of the people working on ports
and on the lifter are there.

## Legal

This project does not contain any proprietary Sony code, firmware, encryption keys, or copyrighted material ([docs/FIRMWARE_LLE.md](docs/FIRMWARE_LLE.md) explains how LLE works without shipping firmware). It provides clean-room implementations of system library interfaces based on publicly documented behavior. You must provide your own legally obtained PS3 game files.

Contributors are credited in **[CONTRIBUTORS.md](CONTRIBUTORS.md)**.

## License

MIT License. See [LICENSE](LICENSE) for details.


## Changelog

### v0.14.0 — *"Outside In"* (October 2026)

*This release is mostly community work: 28 pull requests from five
contributors. Most of it came out of the Twisted Metal port on Linux, which put
the shared draw engine and the Vulkan backend under a real deferred renderer.*

**Draw engine and Vulkan**, from *[@Kurtisone](https://github.com/Kurtisone)*:
- **Surfaces read in another format** now go through guest memory: a depth
  buffer read as colour, a Z16 shadow map, a float packed into RGBA8. Each
  colour and depth write carries a serial, so the engine knows which one is
  newer (#236).
- Surfaces stay coherent around NV3089 transfers (#224), and cellGcmSys hands
  the engine its display buffers and flips (#223).
- Texel-addressed texture coordinates are scaled (#221), B8 presents its one
  channel as R, G and B (#220), and a Z16 zeta clears to its own far plane
  (#235).
- Vulkan gains RGBA16F colour targets (#222), cube maps (#229), images sized by
  the device's limit (#227) and readbacks from cached memory (#245).
- New diagnostics: `RSX_ENGINE_PASS_DUMP` dumps a frame pass by pass, with
  each pass's targets, depth and programs (#237).
  `PS3RECOMP_VK_DUMP_EVERY` dumps every nth presented frame (#219).
- **Vulkan stencil** (*[@gabryboy12-ai](https://github.com/gabryboy12-ai)*,
  #231): the depth format carries stencil, so the engine's stencil state now
  takes effect.
- **D3D12:** fragment programs whose HLSL ran past 32 KiB no longer fail to
  compile (*[@robbe1912](https://github.com/robbe1912)*, #230).

**Runtime and SPU**, from *[@Kurtisone](https://github.com/Kurtisone)* unless noted:
- **SPU-to-SPU task signals.** A SPURS task signalled from another SPU wakes
  immediately. Before, it woke only when its 1 s timed wait ran out, which made
  one frame of Twisted Metal's post-FX take 4.1 s (#242).
- **SPURS:** the guest's taskset no longer has native fields written into it
  (#240). `cellSpursBarrierInitialize` and `cellSpursGetTasksetInfo` are
  implemented (#241).
- **cellGcmSys:** a main-memory offset now resolves through the IO table first
  (#238). `ps3_ms_now` gets a clock off Windows (#244).
- **POSIX:** lwmutex and lwcond lock for real (#225), and stack scans skip
  unreadable guest pages (#217).
- **SPU disassembler:** decodes `orbi`, `xorhi` and `xorbi` (#239).
- **SPU interpreter:** `ILH` replicates its halfword (*[@tekulvw](https://github.com/tekulvw)*, #234).
- **Lifter:** `--chunk-lines` sets the size of the source chunks (#216).
- **Diagnostics:** `PS3_NULL_READ_STACK` names the guest code behind a NULL
  read (#226). The main thread's context is published as `g_ppu_main_ctx`
  (#228).

**Media**: cellVdec decodes H.264 and MPEG-2 through FFmpeg loaded at run time
(*[@undecuple](https://github.com/undecuple)*, #215). It is opt-in
(`-DPS3RECOMP_FFMPEG_RUNTIME=ON`) and FFmpeg is never linked. Without it,
cellVdec behaves as before.

**Housekeeping**:
- `fw_spu/` is no longer tracked, and `*.prx` is ignored. The tree had been
  carrying decrypted firmware PRXs and C lifted from them, against our own
  firmware policy. Thanks to *[@deucebucket](https://github.com/deucebucket)*
  for reporting it (#210, #212).
- README audit: the Getting Started commands now produce a buildable project,
  the platform table matches CI, and the provenance wording is corrected (RPCS3
  is a reference, not a code source). Changelogs before v0.14.0 moved to
  CHANGELOG.md.

Older releases: **[CHANGELOG.md](CHANGELOG.md)**.
