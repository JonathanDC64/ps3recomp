sse2neon (https://github.com/DLTcollab/sse2neon), the SSE intrinsics written
on NEON, so the runtime's and the lift's SSE paths build on arm64.

ps3recomp: vendored from DLTcollab/sse2neon at commit
92f6de174717aef09033ad21568d5bb9e5470404 (tag v1.9.1), git blobs:

  sse2neon.h  16b6e8341aa9b5d5caf07af700abc4f6d318f543
  LICENSE     c1aa1af5495ab76c5ae49502d82c743b5947eb9c

Unmodified. Included only through include/ps3emu/ps3_sse.h, and only when
compiling for arm64; x86 builds never read it. Its precision options are left
at their defaults (all 0): on AArch64 _mm_div_ps and _mm_sqrt_ps are then the
exact vdivq_f32 and vsqrtq_f32, and the one min/max user (the PPU lift's
vmaxfp/vminfp) handles NaNs and equal operands itself.

Licence: MIT (LICENSE).
