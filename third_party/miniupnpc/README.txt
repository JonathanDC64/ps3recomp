miniupnpc 2.3.3 (https://miniupnp.tuxfamily.org/, https://github.com/miniupnp/miniupnp),
the UPnP IGD client library, for asking the player's router to forward a port
(libs/network/net_upnp.c).

ps3recomp: vendored from the release miniupnpc-2.3.3.tar.gz on GitHub
(miniupnp/miniupnp, release miniupnpc_2_3_3, published 2025-05-26):

  SHA-256 d52a0afa614ad6c088cc9ddff1ae7d29c8c595ac5fdd321170a05f41e634bd1a

Its files match the git tag miniupnpc_2_3_3 (commit
bf4215a7574f88aa55859db9db00e3ae58cf42d6) apart from the $Id$ lines.

Copied unchanged: LICENSE, include/*.h, and the library's own sources in src/
(the MINIUPNPC_SOURCES list of upstream's CMakeLists.txt, with the private
headers they include). Left out: the programs (upnpc, listdevices), the tests
and their data, the Python and Java bindings, minihttptestserver, minixmlvalid,
and upstream's build files (CMake, Makefiles, MSVC projects, scripts).

miniupnpcstrings.h is ours. Upstream's build writes it at configure time from
miniupnpcstrings.h.cmake; this one sets the same macros, with OS_STRING taken
from the compiler's platform macros (_WIN32, __APPLE__, __linux__), so a plain
compile line needs no configure step.

The build compiles src/*.c into the runtime when PS3RECOMP_UPNP is on (the
default; CMakeLists.txt), with MINIUPNP_STATICLIB and upstream's own
definitions, and warnings off (upstream code). Windows links ws2_32 and
iphlpapi.

Updating: replace include/ and src/ from a newer release (check upstream's
MINIUPNPC_SOURCES for new files), update the version in miniupnpcstrings.h,
and change the version and hash here.

Licence: BSD-3-Clause (LICENSE).
