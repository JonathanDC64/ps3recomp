volk (https://github.com/zeux/volk), the Vulkan entry point loader, for the
portable renderer's Vulkan backend (libs/video/gpu/gpu_vk*).

ps3recomp: vendored from zeux/volk at commit
54fc0d7a72887cbfc5b030d26897b2b40ac53fb8 (tag vulkan-sdk-1.4.363.0, the same
pin as cmake/gpu_deps.cmake), git blobs:

  volk.c      44fcfa44942dd61695c03aa70901f34bc30b4e79
  volk.h      3997af6b4fb9ca4abde5e5b1aec04eadba33998e
  LICENSE.md  b310a6e5604bdcb41ff72f47aceb0c9cbbf43768

Unmodified. Built only with PS3RECOMP_RSX_HAL and PS3RECOMP_RSX_HAL_VULKAN,
against the pinned Vulkan-Headers of the same SDK release. The backend calls
the device through a VolkDeviceTable (volkLoadDeviceTable), so each draw's
calls go straight to the driver's functions.

Licence: MIT (LICENSE.md).
