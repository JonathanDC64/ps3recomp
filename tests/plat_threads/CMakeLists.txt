# ps3recomp - tests/plat_threads
#
# runtime/platform/plat.h's threads, events and mutex, and the SPU async job
# pool built on them (runtime/spu/spu_async_pool.c). Standalone project with
# its own build dir, and also added from the top-level CMakeLists under
# PS3RECOMP_BUILD_TESTS.
#
#   cmake -S tests/plat_threads -B build-plat-threads -G Ninja
#   cmake --build build-plat-threads && ctest --test-dir build-plat-threads
#
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(plat_threads LANGUAGES C)
    enable_testing()
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PS3RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED PS3RECOMP_TREE_ROOT)
    set(PS3RECOMP_TREE_ROOT "${PS3RECOMP_ROOT}")
endif()

# The pool is compiled directly, as tests/lv2_time does with sys_timer.c, so
# the test needs no HLE, RSX or SDL2, and with a 1 s idle time and room for
# 16 idle workers, so its idle rules can be watched in a few seconds.
add_executable(test_plat_threads
    test_plat_threads.c
    "${PS3RECOMP_TREE_ROOT}/runtime/spu/spu_async_pool.c"
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/plat_posix.c"
)
target_compile_definitions(test_plat_threads PRIVATE
    SPU_ASYNC_POOL_IDLE_MS=1000
    SPU_ASYNC_POOL_MAX_IDLE=16
)
target_include_directories(test_plat_threads PRIVATE
    "${PS3RECOMP_TREE_ROOT}/runtime"
    "${PS3RECOMP_TREE_ROOT}/include"
)

if(MSVC)
    target_compile_options(test_plat_threads PRIVATE /W3 /wd4100)
    target_compile_definitions(test_plat_threads PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_plat_threads PRIVATE -Wall -Wextra -Wno-unused-parameter)
    find_package(Threads REQUIRED)
    target_link_libraries(test_plat_threads PRIVATE Threads::Threads)
endif()

add_test(NAME plat_threads COMMAND test_plat_threads)
set_tests_properties(plat_threads PROPERTIES
    LABELS "runtime;platform;threads"
    TIMEOUT 120
)
