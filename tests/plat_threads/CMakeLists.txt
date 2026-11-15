# ps3recomp - tests/plat_threads
#
# runtime/platform/plat.h's threads, events and mutex, and the SPU async job
# pool built on them (runtime/spu/spu_async_pool.c); its time and sleep
# (test_plat_time: the monotonic clock and the precise sleep); its file
# mapped read-only (test_plat_file); its core kept for one thread
# (test_plat_core); and the process's threads as the kernel counts them
# (test_plat_thread_times). Standalone project with
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

# Time and sleep: plat.h alone (and plat_posix.c off Windows).
add_executable(test_plat_time
    test_plat_time.c
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/plat_posix.c"
)
target_include_directories(test_plat_time PRIVATE "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_options(test_plat_time PRIVATE /W3)
    target_compile_definitions(test_plat_time PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_plat_time PRIVATE -Wall -Wextra)
    target_link_libraries(test_plat_time PRIVATE Threads::Threads)
endif()

add_test(NAME plat_time COMMAND test_plat_time)
set_tests_properties(plat_time PROPERTIES
    LABELS "runtime;platform;time"
    TIMEOUT 60
)

# A file mapped read-only, and replaced by a rename while mapped: plat.h alone
# (and plat_posix.c off Windows).
add_executable(test_plat_file
    test_plat_file.c
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/plat_posix.c"
)
target_include_directories(test_plat_file PRIVATE "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_options(test_plat_file PRIVATE /W3)
    target_compile_definitions(test_plat_file PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_plat_file PRIVATE -Wall -Wextra)
    target_link_libraries(test_plat_file PRIVATE Threads::Threads)
endif()

add_test(NAME plat_file COMMAND test_plat_file)
set_tests_properties(plat_file PROPERTIES
    LABELS "runtime;platform;file"
    TIMEOUT 60
)

# A core kept for one thread (RSX_CORE): plat.h alone (and plat_posix.c off
# Windows). Skips its checks on a machine with no core to spare.
add_executable(test_plat_core
    test_plat_core.c
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/plat_posix.c"
)
target_include_directories(test_plat_core PRIVATE "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_options(test_plat_core PRIVATE /W3)
    target_compile_definitions(test_plat_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_plat_core PRIVATE -Wall -Wextra)
    target_link_libraries(test_plat_core PRIVATE Threads::Threads)
endif()

add_test(NAME plat_core COMMAND test_plat_core)
set_tests_properties(plat_core PROPERTIES
    LABELS "runtime;platform;threads"
    TIMEOUT 60
)

# The process's threads as the kernel counts them (the problem report's CPU
# table on Linux): plat.h alone (and plat_posix.c off Windows). On Linux a
# second build reads the stat file alone, the path for a kernel without
# schedstat.
add_executable(test_plat_thread_times
    test_plat_thread_times.c
    "${PS3RECOMP_TREE_ROOT}/runtime/platform/plat_posix.c"
)
target_include_directories(test_plat_thread_times PRIVATE "${PS3RECOMP_TREE_ROOT}/runtime")
if(MSVC)
    target_compile_options(test_plat_thread_times PRIVATE /W3)
    target_compile_definitions(test_plat_thread_times PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_plat_thread_times PRIVATE -Wall -Wextra)
    target_link_libraries(test_plat_thread_times PRIVATE Threads::Threads)
endif()
add_test(NAME plat_thread_times COMMAND test_plat_thread_times)
set_tests_properties(plat_thread_times PROPERTIES
    LABELS "runtime;platform;threads"
    TIMEOUT 60
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_plat_thread_times_stat
        test_plat_thread_times.c
        "${PS3RECOMP_TREE_ROOT}/runtime/platform/plat_posix.c"
    )
    target_include_directories(test_plat_thread_times_stat PRIVATE "${PS3RECOMP_TREE_ROOT}/runtime")
    target_compile_definitions(test_plat_thread_times_stat PRIVATE PLAT_THREAD_TIMES_NO_SCHEDSTAT)
    target_compile_options(test_plat_thread_times_stat PRIVATE -Wall -Wextra)
    target_link_libraries(test_plat_thread_times_stat PRIVATE Threads::Threads)
    add_test(NAME plat_thread_times_stat COMMAND test_plat_thread_times_stat)
    set_tests_properties(plat_thread_times_stat PROPERTIES
        LABELS "runtime;platform;threads"
        TIMEOUT 60
    )
endif()
