/*
 * ps3recomp - <TargetConditionals.h> for a freestanding game module
 * (PS3_GAME_FREESTANDING, ps3emu/game_freestanding.h).
 *
 * For sse2neon (ps3emu/ps3_sse.h) on macOS, which reads TARGET_OS_IOS. The
 * module is built for macOS only, so these are a Mac's values.
 */
#ifndef PS3_GAME_LIBC_TARGETCONDITIONALS_H
#define PS3_GAME_LIBC_TARGETCONDITIONALS_H

#define TARGET_OS_MAC       1
#define TARGET_OS_OSX       1
#define TARGET_OS_IPHONE    0
#define TARGET_OS_IOS       0
#define TARGET_OS_TV        0
#define TARGET_OS_WATCH     0
#define TARGET_OS_VISION    0
#define TARGET_OS_SIMULATOR 0

#endif /* PS3_GAME_LIBC_TARGETCONDITIONALS_H */
