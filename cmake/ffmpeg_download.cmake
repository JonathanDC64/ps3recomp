# The FFmpeg download for Windows builds: FFmpeg's MPEG-2 video decoder (libavcodec, libavutil) as DLLs, with the
# headers the runtime builds against. .github/workflows/ffmpeg-archive.yml makes it and attaches it to a release of
# this repository; its URL and SHA-256 are pinned here.
#
# A Windows configure without PS3RECOMP_FFMPEG_RUNTIME downloads it once into <build>/ffmpeg-win64, checks the
# SHA-256 and turns the run-time FFmpeg on, so the movies decode through FFmpeg rather than Windows' own decoder
# (which needs the Microsoft Store's MPEG-2 Video Extension). Because a changed CMakeLists.txt makes the next build
# configure again, an existing build directory gets it on its next build after a pull.
#
# The DLLs stay in <build>/ffmpeg-win64/bin: the runtime opens them by name, so a game build copies them beside its
# executable (DemonsSoulsRecomp's port/CMakeLists.txt does). With no network, or a file that does not match, the
# build goes on without FFmpeg and says so.
#
#   -DPS3RECOMP_FFMPEG_DOWNLOAD=OFF        never download (the old behaviour: Windows' own decoder)
#   -DPS3RECOMP_FFMPEG_URL=<url>           another file than the pinned one (a mirror, or file:///... offline);
#   -DPS3RECOMP_FFMPEG_SHA256=<hex>        it must have this SHA-256. Empty (the default) means the pin, so a
#                                          new pin reaches existing build directories.

set(PS3RECOMP_FFMPEG_PIN_URL "")
set(PS3RECOMP_FFMPEG_PIN_SHA256 "")

option(PS3RECOMP_FFMPEG_DOWNLOAD "Windows: download the pinned FFmpeg build for the movies when no FFmpeg is given" ON)
set(PS3RECOMP_FFMPEG_URL "" CACHE STRING "Another FFmpeg download than the pinned one (empty: the pin)")
set(PS3RECOMP_FFMPEG_SHA256 "" CACHE STRING "The SHA-256 of PS3RECOMP_FFMPEG_URL's file (empty: the pin)")

# Sets <out_include> to the headers' folder and <out_bin> to the DLLs' folder, or both to "" when there is no
# usable download.
function(ps3recomp_ffmpeg_download out_include out_bin)
    set(${out_include} "" PARENT_SCOPE)
    set(${out_bin} "" PARENT_SCOPE)
    set(url "${PS3RECOMP_FFMPEG_PIN_URL}")
    set(sha "${PS3RECOMP_FFMPEG_PIN_SHA256}")
    if(PS3RECOMP_FFMPEG_URL)
        set(url "${PS3RECOMP_FFMPEG_URL}")
        set(sha "${PS3RECOMP_FFMPEG_SHA256}")
    endif()
    string(TOLOWER "${sha}" sha)
    string(LENGTH "${sha}" sha_len)
    if(NOT url OR NOT sha MATCHES "^[0-9a-f]+$" OR NOT sha_len EQUAL 64)
        message(STATUS "  FFmpeg:       no download pinned; the movies use Windows' own decoder")
        return()
    endif()

    set(dir "${CMAKE_BINARY_DIR}/ffmpeg-win64")
    set(stamp "${dir}/.sha256")
    set(have "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" have)
        string(STRIP "${have}" have)
    endif()
    if(NOT have STREQUAL sha OR NOT EXISTS "${dir}/include/libavcodec/avcodec.h")
        get_filename_component(name "${url}" NAME)
        set(zip "${CMAKE_BINARY_DIR}/_downloads/${name}")
        set(got "")
        if(EXISTS "${zip}")
            file(SHA256 "${zip}" got)
        endif()
        if(NOT got STREQUAL sha)
            message(STATUS "  FFmpeg:       downloading ${url}")
            file(DOWNLOAD "${url}" "${zip}" STATUS status TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
            list(GET status 0 code)
            set(got "")
            if(code EQUAL 0)
                file(SHA256 "${zip}" got)
            endif()
            if(NOT got STREQUAL sha)
                file(REMOVE "${zip}")
                if(code EQUAL 0)
                    set(why "it does not have the pinned SHA-256 (it has ${got})")
                else()
                    list(GET status 1 msg)
                    set(why "${msg}")
                endif()
                message(WARNING "FFmpeg could not be downloaded from ${url}: ${why}. This build goes on "
                    "without it, so its movies use Windows' own decoder, which needs the Microsoft Store's MPEG-2 "
                    "Video Extension. Configure this build again with a network connection to retry, or set "
                    "-DPS3RECOMP_FFMPEG_DOWNLOAD=OFF to stop trying.")
                return()
            endif()
        endif()
        file(REMOVE_RECURSE "${dir}")
        file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${dir}")
        if(NOT EXISTS "${dir}/include/libavcodec/avcodec.h")
            message(WARNING "FFmpeg: ${name} has no include/libavcodec/avcodec.h; this build goes on without it.")
            file(REMOVE_RECURSE "${dir}")
            return()
        endif()
        file(WRITE "${stamp}" "${sha}\n")
    endif()

    file(GLOB dlls "${dir}/bin/avcodec-*.dll" "${dir}/bin/avutil-*.dll")
    list(LENGTH dlls n)
    if(NOT n EQUAL 2)
        message(WARNING "FFmpeg: ${dir}/bin should hold avcodec-*.dll and avutil-*.dll; this build goes on without it.")
        return()
    endif()
    set(${out_include} "${dir}/include" PARENT_SCOPE)
    set(${out_bin} "${dir}/bin" PARENT_SCOPE)
endfunction()
