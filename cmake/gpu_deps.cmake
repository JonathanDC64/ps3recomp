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
# Only declared here: nothing is downloaded, so a HAL configure fetches nothing that phase 0 does not use. The
# packages that need them each add a sub-option here and make theirs available under it, for example
#
#   option(PS3RECOMP_RSX_HAL_VULKAN "The Vulkan backend (libs/video/gpu/gpu_vk*)" OFF)
#   if(PS3RECOMP_RSX_HAL_VULKAN)
#       FetchContent_MakeAvailable(vulkan_headers volk)
#   endif()
#
# and, for the shader compile service's spv and msl targets, spirv_headers, spirv_tools, glslang and spirv_cross
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

message(STATUS "  Renderer:     GPU layer dependencies declared (Vulkan SDK 1.4.363.0 pins); none fetched yet")
