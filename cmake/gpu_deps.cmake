# The portable renderer's third-party code (PS3RECOMP_RSX_HAL; CMakeLists.txt includes this file only with it on).
#
# Each is pinned by commit (a tag can be moved; a commit cannot), with its tag beside it. All six are at the same
# Vulkan SDK release, so they agree with one another and with an installed SDK of that version.
#
#   Vulkan-Headers   the Vulkan API's headers                      Apache-2.0 / MIT
#   volk             loads Vulkan's entry points at run time       MIT
#   SPIRV-Headers    SPIR-V's grammar, for the two below           MIT
#   SPIRV-Tools      SPIR-V's validator and optimizer              Apache-2.0
#   glslang          HLSL -> SPIR-V (the spv target)               BSD-3-Clause and others (its LICENSE.txt)
#   SPIRV-Cross      SPIR-V -> MSL (the Mac's plan B)              Apache-2.0
#
# Declared here, and downloaded only by the sub-options that use them: PS3RECOMP_RSX_HAL_VULKAN (below) fetches
# Vulkan-Headers; volk is vendored (third_party/volk), so its declaration is only the pin. The packages still to
# come add theirs the same way:
# for the shader compile service's spv and msl targets, spirv_headers, spirv_tools, glslang and spirv_cross
# (in that order: SPIRV-Tools builds on SPIRV-Headers, and glslang's optimizer on SPIRV-Tools). A build without a
# network, or with its own copies, names them with FetchContent's own variables, which FetchContent_MakeAvailable
# honours:
#
#   -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=<dir>   -DFETCHCONTENT_SOURCE_DIR_VOLK=<dir>
#   -DFETCHCONTENT_SOURCE_DIR_SPIRV_HEADERS=<dir>    -DFETCHCONTENT_SOURCE_DIR_SPIRV_TOOLS=<dir>
#   -DFETCHCONTENT_SOURCE_DIR_GLSLANG=<dir>          -DFETCHCONTENT_SOURCE_DIR_SPIRV_CROSS=<dir>
#
# (or FETCHCONTENT_FULLY_DISCONNECTED=ON with the sources already in the build directory).

include(FetchContent)

# FetchContent_Declare takes EXCLUDE_FROM_ALL from CMake 3.28 on; older ones would hand it to ExternalProject.
set(_ps3_gpu_dep_excl "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    set(_ps3_gpu_dep_excl EXCLUDE_FROM_ALL)
endif()

# By commit, so not shallow: a shallow clone works with tag names only.
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG 6802bb4733b63ed5efd3adb308a6c885ef180ea1   # vulkan-sdk-1.4.363.0
    ${_ps3_gpu_dep_excl})
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG 54fc0d7a72887cbfc5b030d26897b2b40ac53fb8   # vulkan-sdk-1.4.363.0
    ${_ps3_gpu_dep_excl})
FetchContent_Declare(spirv_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git
    GIT_TAG 496543121ce6419f23d6fa5d7194ba66c36212d2   # vulkan-sdk-1.4.363.0
    ${_ps3_gpu_dep_excl})
FetchContent_Declare(spirv_tools
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git
    GIT_TAG ef96ed763b43b59b33b31b362f09a02b729fa1c9   # vulkan-sdk-1.4.363.0
    ${_ps3_gpu_dep_excl})
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG e1b562a8bed273a02f30b59b66a5d499793cede5   # vulkan-sdk-1.4.363.0
    ${_ps3_gpu_dep_excl})
FetchContent_Declare(spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7   # vulkan-sdk-1.4.363.0
    ${_ps3_gpu_dep_excl})

# The shader compile service's spv target (libs/video/gpu/gpu_shader_spv*.cpp): HLSL -> SPIR-V through glslang's
# HLSL front end, with SPIRV-Tools' legalization passes and validator. On by default where the Vulkan backend is
# (Windows and Linux). SPIRV-Headers, SPIRV-Tools and glslang come from their pins above, built static, libraries
# only: no tests, tools or install rules. Their build runs Python 3 (grammar tables, glslang's build_info.h).
if(WIN32 OR (UNIX AND NOT APPLE))
    set(_ps3_gpu_spv_default ON)
else()
    set(_ps3_gpu_spv_default OFF)
endif()
option(PS3RECOMP_RSX_HAL_SPV "The shader compile service's spv target (glslang and SPIRV-Tools, fetched)"
       ${_ps3_gpu_spv_default})
if(PS3RECOMP_RSX_HAL_SPV AND (TARGET glslang::glslang OR TARGET SPIRV-Tools-opt))
    # A system glslang found earlier (CMakeLists.txt, the Metal path's MSL translation) defines the same target
    # names as the pinned one, so the two cannot be in one build.
    message(WARNING "PS3RECOMP_RSX_HAL_SPV: a system glslang or SPIRV-Tools is already loaded; the spv target is "
                    "not built. Configure without it, or with -DPS3RECOMP_RSX_HAL_SPV=OFF.")
    set(PS3RECOMP_RSX_HAL_SPV OFF)
endif()
if(PS3RECOMP_RSX_HAL_SPV)
    set(_ps3_gpu_spv_shared "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    set(SPIRV_HEADERS_ENABLE_TESTS OFF)
    set(SPIRV_HEADERS_ENABLE_INSTALL OFF)
    FetchContent_MakeAvailable(spirv_headers)
    set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}")
    set(SPIRV_SKIP_TESTS ON)
    set(SPIRV_SKIP_EXECUTABLES ON)
    set(SKIP_SPIRV_TOOLS_INSTALL ON)
    set(SPIRV_WERROR OFF)
    set(SPIRV_BUILD_FUZZER OFF)
    set(SPIRV_TOOLS_BUILD_STATIC ON)
    set(SPIRV_TOOLS_USE_MIMALLOC OFF)
    FetchContent_MakeAvailable(spirv_tools)
    set(GLSLANG_TESTS OFF)
    set(GLSLANG_ENABLE_INSTALL OFF)
    set(ENABLE_GLSLANG_BINARIES OFF)
    set(ENABLE_GLSLANG_JS OFF)
    set(ENABLE_HLSL ON)
    set(ENABLE_OPT ON)
    set(BUILD_EXTERNAL OFF)
    FetchContent_MakeAvailable(glslang)
    set(BUILD_SHARED_LIBS "${_ps3_gpu_spv_shared}")
    message(STATUS "  Renderer:     the compile service's spv target (glslang, SPIRV-Tools: fetched at their pins)")
endif()

message(STATUS "  Renderer:     GPU layer dependencies declared (Vulkan SDK 1.4.363.0 pins)")

# The Vulkan backend (libs/video/gpu/gpu_vk*): on by default on the systems with a Vulkan driver to run it today,
# Windows and Linux (the Mac's comes later). It builds against the pinned Vulkan-Headers, fetched here, and volk,
# vendored in third_party/volk at the same pin; it loads the Vulkan library at run time, so nothing is linked.
# -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=<a Vulkan-Headers checkout of that commit> builds without the download.
if(WIN32 OR (UNIX AND NOT APPLE))
    set(_ps3_gpu_vk_default ON)
else()
    set(_ps3_gpu_vk_default OFF)
endif()
option(PS3RECOMP_RSX_HAL_VULKAN "The GPU layer's Vulkan backend (libs/video/gpu/gpu_vk*)" ${_ps3_gpu_vk_default})
if(PS3RECOMP_RSX_HAL_VULKAN)
    # Only the C headers: not Vulkan-Hpp's C++ modules, tests or install rules.
    set(VULKAN_HEADERS_ENABLE_MODULE OFF)
    set(VULKAN_HEADERS_ENABLE_TESTS OFF)
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF)
    FetchContent_MakeAvailable(vulkan_headers)
    message(STATUS "  Renderer:     the GPU layer's Vulkan backend (Vulkan-Headers ${vulkan_headers_SOURCE_DIR})")
endif()
