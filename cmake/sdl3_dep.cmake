# SDL3, the host's input on every OS to come (and later its audio and window), built from a pinned release.
#
# Nothing in the runtime uses it yet. A project that wants it includes this file and calls
# ps3recomp_sdl3_available(), which makes the target SDL3::SDL3-static: tests/sdl3_smoke does, and so will
# the runtime and a port, so that both build the one pinned copy.
#
#   SDL3 3.4.20 (release-3.4.20, commit f00dc919ca35b9756316e86c4777392de442f9b2)    zlib
#
# Pinned by the release archive's SHA-256 (the one GitHub lists for the asset). 3.4.10 is the first release
# with the 2026 Steam Controller's driver complete (src/joystick/hidapi/SDL_hidapi_steam_triton.c: its input
# reports, touchpads and rumble, May 2026); SDL2 has no driver for it.
#
# Static, so there is nothing to ship beside the program. It takes the including project's
# CMAKE_MSVC_RUNTIME_LIBRARY. A build without a network, or with its own copy of the same release:
#
#   -DFETCHCONTENT_SOURCE_DIR_SDL3=<dir>
#
# SDL2 and SDL3 export many of the same symbol names (SDL_Init, SDL_PollEvent, ...), so one Linux process
# must not load both: whatever moves to SDL3 there moves together.

include_guard(GLOBAL)
include(FetchContent)

set(PS3RECOMP_SDL3_VERSION 3.4.20)

# FetchContent_Declare takes EXCLUDE_FROM_ALL from CMake 3.28 on; older ones would hand it to ExternalProject.
set(_ps3_sdl3_excl "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(_ps3_sdl3_excl EXCLUDE_FROM_ALL)
endif()

FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    ${_ps3_sdl3_excl})

# SDL's own options, as plain variables in this function's scope (SDL's CMake honours them, CMP0077), so
# they reach SDL and nothing else.
function(ps3recomp_sdl3_available)
    if(TARGET SDL3::SDL3-static)
        return()
    endif()
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    set(SDL_DISABLE_INSTALL ON)
    set(SDL_X11_XTEST OFF)        # synthesised input and global mouse warps: unused, and one less -dev package
    FetchContent_MakeAvailable(sdl3)
    if(NOT TARGET SDL3::SDL3-static)
        message(FATAL_ERROR "SDL3 ${PS3RECOMP_SDL3_VERSION}: no SDL3::SDL3-static target after the build was set up")
    endif()
    message(STATUS "  SDL3:         ${PS3RECOMP_SDL3_VERSION}, static (cmake/sdl3_dep.cmake)")
endfunction()
