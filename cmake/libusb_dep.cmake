# libusb-1.0 for SDL3 on Windows, built from a pinned release: the Switch 2 controllers (Pro Controller, Joy-Con 2,
# the GameCube controller) need it. Over USB, SDL sends their start-up sequence on a bulk interface that only
# libusb reaches; without it SDL sees the Pro Controller as a plain joystick with no mapping.
#
#   libusb 1.0.30 (v1.0.30, 2026-05-17)    LGPL 2.1 or later
#
# Pinned by the release archive's SHA-256. ps3recomp_libusb_available() unpacks it, builds libusb-1.0.dll with
# cmake/libusb/CMakeLists.txt at configure time (SDL's own configure checks link against it), and points SDL's
# libusb lookup (its cmake/FindLibUSB.cmake) at the result. SDL then opens libusb-1.0.dll at run time
# (SDL_HIDAPI_LIBUSB_SHARED), and only for the devices it lists as needing libusb (SDL_hidapi.c,
# SDL_libusb_required): every other controller keeps Windows' own HID path. Without the DLL beside the program,
# SDL goes on without libusb.
#
# The DLL, its import library and libusb's COPYING end up in <build>/libusb-win64; a game build copies the DLL
# beside its executable, and a release ships it with the licence. Dynamic linking keeps the LGPL's terms simple:
# anyone can swap the DLL.
#
#   -DPS3RECOMP_SDL3_LIBUSB=OFF           no libusb (SDL3 without the Switch 2 controllers over USB)
#   -DFETCHCONTENT_SOURCE_DIR_LIBUSB=<dir>   an unpacked copy of the same release, for a build without a network

include_guard(GLOBAL)
include(FetchContent)

set(PS3RECOMP_LIBUSB_VERSION 1.0.30)

# libusb's release has no CMakeLists.txt, so FetchContent_MakeAvailable only unpacks it.
FetchContent_Declare(libusb
    URL https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
    URL_HASH SHA256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Sets PS3RECOMP_LIBUSB_DIR (cache) to <build>/libusb-win64 and SDL's LibUSB_LIBRARY and LibUSB_INCLUDE_PATH, or,
# when the build fails, says so and turns SDL's libusb off.
function(ps3recomp_libusb_available)
    if(NOT WIN32)
        return()
    endif()
    set(out "${CMAKE_BINARY_DIR}/libusb-win64")
    set(stamp "${out}/version.txt")
    FetchContent_MakeAvailable(libusb)
    set(src "${libusb_SOURCE_DIR}")
    set(want "${PS3RECOMP_LIBUSB_VERSION} ${CMAKE_C_COMPILER}")
    set(have "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" have)
    endif()
    if(NOT have STREQUAL want OR NOT EXISTS "${out}/libusb-1.0.dll" OR NOT EXISTS "${out}/libusb-1.0.lib")
        set(bld "${CMAKE_BINARY_DIR}/libusb-build")
        file(REMOVE_RECURSE "${bld}" "${out}")
        set(gen -G "${CMAKE_GENERATOR}")
        if(CMAKE_GENERATOR_PLATFORM)
            list(APPEND gen -A "${CMAKE_GENERATOR_PLATFORM}")
        endif()
        if(CMAKE_MAKE_PROGRAM)
            list(APPEND gen "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
        endif()
        if(CMAKE_RC_COMPILER)
            list(APPEND gen "-DCMAKE_RC_COMPILER=${CMAKE_RC_COMPILER}")
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/libusb" -B "${bld}" ${gen}
                    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" -DCMAKE_BUILD_TYPE=Release "-DLIBUSB_SRC=${src}"
            RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
        if(rc EQUAL 0)
            execute_process(COMMAND "${CMAKE_COMMAND}" --build "${bld}" --config Release
                RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
        endif()
        file(GLOB_RECURSE dll "${bld}/libusb-1.0.dll")
        file(GLOB_RECURSE lib "${bld}/libusb-1.0.lib")
        if(NOT rc EQUAL 0 OR NOT dll OR NOT lib)
            message(STATUS "  libusb:       ${PS3RECOMP_LIBUSB_VERSION} did not build; SDL3 goes on without it "
                           "(no Switch 2 controllers over USB):\n${log}")
            set(SDL_HIDAPI_LIBUSB OFF PARENT_SCOPE)
            return()
        endif()
        list(GET dll 0 dll)
        list(GET lib 0 lib)
        file(COPY "${dll}" "${lib}" "${src}/COPYING" DESTINATION "${out}")
        file(WRITE "${stamp}" "${want}")
    endif()
    set(PS3RECOMP_LIBUSB_DIR "${out}" CACHE INTERNAL "libusb-1.0.dll and its licence, for a game build to ship")
    set(PS3RECOMP_LIBUSB_VERSION "${PS3RECOMP_LIBUSB_VERSION}" CACHE INTERNAL "the pinned libusb (cmake/libusb_dep.cmake)")
    # SDL's FindLibUSB.cmake reads these; the configure check that follows is cached, so it runs again here
    # (a build folder from before had found no libusb).
    set(LibUSB_LIBRARY "${out}/libusb-1.0.lib" CACHE FILEPATH "libusb-1.0's import library (cmake/libusb_dep.cmake)" FORCE)
    set(LibUSB_INCLUDE_PATH "${src}/libusb" CACHE PATH "libusb.h's folder (cmake/libusb_dep.cmake)" FORCE)
    unset(HAVE_LIBUSB_H CACHE)
    message(STATUS "  libusb:       ${PS3RECOMP_LIBUSB_VERSION}, libusb-1.0.dll for SDL3 (cmake/libusb_dep.cmake)")
endfunction()
