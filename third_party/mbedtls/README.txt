Mbed TLS 3.6.7 (https://www.trustedfirmware.org/projects/mbed-tls/), for TLS
on the links to the online server (libs/network/ps3_tls.c).

ps3recomp: vendored from the release mbedtls-3.6.7.tar.bz2 on GitHub
(Mbed-TLS/mbedtls, release mbedtls-3.6.7), checked against the
mbedtls-3.6.7-sha256sum.txt published beside it:

  SHA-256 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

Unmodified, with its default configuration (include/mbedtls/mbedtls_config.h).
Only include/ and library/ are kept, with LICENSE. The build compiles
library/*.c into the runtime (CMakeLists.txt). The default configuration has
no threading, so ps3_tls.c makes every call into it under one lock.

3.6 is a long-term support branch, with security fixes until about March 2027.
Updating: replace include/ and library/ from a newer 3.6.x release and change
the version and hash here.

Licence: Apache-2.0 OR GPL-2.0-or-later, at the user's choice (LICENSE);
ps3recomp takes it under Apache-2.0.
