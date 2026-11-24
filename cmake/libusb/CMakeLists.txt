# libusb-1.0 as a Windows DLL, built from a release's sources: cmake/libusb_dep.cmake configures and builds this
# project once, at configure time, for SDL3's Switch 2 controller support (SDL opens libusb-1.0.dll at run time).
#
# The sources and the defines are the release's own MSVC project's (msvc/libusb_dll.vcxproj, its Release
# configuration, which leaves Windows hotplug out: SDL looks for controllers itself). The C runtime is linked in
# (/MT), so the DLL needs nothing beside it.
cmake_minimum_required(VERSION 3.20)
project(ps3recomp_libusb C RC)

if(NOT LIBUSB_SRC OR NOT EXISTS "${LIBUSB_SRC}/libusb/libusb.h")
    message(FATAL_ERROR "LIBUSB_SRC must be an unpacked libusb release (got '${LIBUSB_SRC}')")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(_u "${LIBUSB_SRC}/libusb")
add_library(usb-1.0 SHARED
    "${_u}/core.c"
    "${_u}/descriptor.c"
    "${_u}/hotplug.c"
    "${_u}/io.c"
    "${_u}/strerror.c"
    "${_u}/sync.c"
    "${_u}/os/events_windows.c"
    "${_u}/os/threads_windows.c"
    "${_u}/os/windows_common.c"
    "${_u}/os/windows_usbdk.c"
    "${_u}/os/windows_winusb.c"
    "${_u}/libusb-1.0.def"
    "${_u}/libusb-1.0.rc")
set_target_properties(usb-1.0 PROPERTIES OUTPUT_NAME "libusb-1.0" PREFIX "")
target_include_directories(usb-1.0 PRIVATE "${LIBUSB_SRC}/msvc" "${_u}")
target_compile_definitions(usb-1.0 PRIVATE _WIN32_WINNT=_WIN32_WINNT_VISTA _CRT_SECURE_NO_WARNINGS NDEBUG)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(usb-1.0 PRIVATE -Wno-everything)   # their code, their warnings
endif()
