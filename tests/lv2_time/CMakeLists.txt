# ps3recomp - tests/lv2_time
#
# The guest system-time clock, lv2_system_time_usec (runtime/syscalls/
# sys_timer.c): what sys_time_get_system_time returns and what
# cellGcmGetLastFlipTime reports. Standalone project with its own build dir,
# and also added from the top-level CMakeLists under PS3RECOMP_BUILD_TESTS.
#
#   cmake -S tests/lv2_time -B build-lv2time -G Ninja
#   cmake --build build-lv2time && ./build-lv2time/test_system_time
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lv2_time LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

# sys_timer.c is compiled directly, as tests/sync_stress does with the sync
# sources, so the test needs no HLE, RSX or SDL2.
add_executable(test_system_time
    test_system_time.c
    "${PS3RECOMP_TREE_ROOT}/runtime/syscalls/sys_timer.c"
)
if(NOT WIN32)
    # The Win32 handle and wait API sys_timer.c uses for its timer threads.
    target_sources(test_system_time PRIVATE
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/win32_compat.c")
endif()

target_include_directories(test_system_time PRIVATE
    "${PS3RECOMP_TREE_ROOT}"
    "${PS3RECOMP_TREE_ROOT}/runtime"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(test_system_time PRIVATE /W3 /wd4100 /wd4244 /wd4267)
    target_compile_definitions(test_system_time PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_system_time PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_package(Threads REQUIRED)
    target_link_libraries(test_system_time PRIVATE Threads::Threads)
endif()

add_test(NAME lv2_system_time COMMAND test_system_time)
set_tests_properties(lv2_system_time PROPERTIES
    LABELS "runtime;time"
    TIMEOUT 30
)
