# ps3recomp - tests/sdl3_smoke
#
# The pinned SDL3 (cmake/sdl3_dep.cmake) builds on this host and starts its gamepad subsystem. A standalone
# project with its own build directory, as tests/sync_stress is; CI runs it on Windows and Linux.
#
#   cmake -S tests/sdl3_smoke -B build-sdl3 -G Ninja && cmake --build build-sdl3 && ctest --test-dir build-sdl3
#
cmake_minimum_required(VERSION 3.20)
project(sdl3_smoke LANGUAGES C)
enable_testing()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/sdl3_dep.cmake")
ps3recomp_sdl3_available()

add_executable(sdl3_smoke main.c)
target_link_libraries(sdl3_smoke PRIVATE SDL3::SDL3-static)
target_compile_definitions(sdl3_smoke PRIVATE PS3RECOMP_SDL3_VERSION="${PS3RECOMP_SDL3_VERSION}")
# Windows: SDL was built to open libusb-1.0.dll (cmake/libusb_dep.cmake), and the DLL beside the test loads and
# is the pinned version.
if(WIN32 AND PS3RECOMP_SDL3_LIBUSB)
    # (SDL writes its config in two steps; at configure time the first one's file is there)
    FetchContent_GetProperties(sdl3)
    set(_dyn "")
    set(_cfg "${sdl3_BINARY_DIR}/CMakeFiles/SDL_build_config.h.intermediate")
    if(EXISTS "${_cfg}")
        file(STRINGS "${_cfg}" _dyn REGEX "^#define SDL_LIBUSB_DYNAMIC ")
    endif()
    if(NOT _dyn MATCHES "libusb-1\\.0\\.dll" OR NOT PS3RECOMP_LIBUSB_DIR)
        message(FATAL_ERROR "SDL3 was not built to open libusb-1.0.dll (SDL_build_config.h: '${_dyn}')")
    endif()
    add_custom_command(TARGET sdl3_smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PS3RECOMP_LIBUSB_DIR}/libusb-1.0.dll" "$<TARGET_FILE_DIR:sdl3_smoke>"
        VERBATIM)
    target_compile_definitions(sdl3_smoke PRIVATE PS3RECOMP_LIBUSB_VERSION="${PS3RECOMP_LIBUSB_VERSION}")
endif()
add_test(NAME sdl3_smoke COMMAND sdl3_smoke)
set_tests_properties(sdl3_smoke PROPERTIES TIMEOUT 60)
