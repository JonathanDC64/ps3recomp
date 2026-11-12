/* ps3recomp: not an upstream file. miniupnpc's own build writes this header at
 * configure time from miniupnpcstrings.h.cmake, with OS_STRING set to CMake's
 * system name. This one sets the same macros, with OS_STRING picked from the
 * compiler's platform macros, so any build (CMake or a plain compile line)
 * works without a configure step. The strings go in the User-Agent of
 * miniupnpc's HTTP requests to the router. */
#ifndef MINIUPNPCSTRINGS_H_INCLUDED
#define MINIUPNPCSTRINGS_H_INCLUDED

#if defined(_WIN32)
#define OS_STRING "Windows"
#elif defined(__APPLE__)
#define OS_STRING "Darwin"
#elif defined(__linux__)
#define OS_STRING "Linux"
#elif defined(__FreeBSD__)
#define OS_STRING "FreeBSD"
#else
#define OS_STRING "Unknown"
#endif
#define MINIUPNPC_VERSION_STRING "2.3.3"

/* according to "UPnP Device Architecture 1.1", as upstream's template */
#define UPNP_VERSION_MAJOR 1
#define UPNP_VERSION_MINOR 1
#define UPNP_VERSION_STRING "UPnP/1.1"

#endif
