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
add_test(NAME sdl3_smoke COMMAND sdl3_smoke)
set_tests_properties(sdl3_smoke PROPERTIES TIMEOUT 60)
