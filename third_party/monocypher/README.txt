Monocypher 4.0.3 (https://monocypher.org), for Ed25519 in the network sign-in
(libs/network/ps3_net_signin.c).

ps3recomp: vendored from the release monocypher-4.0.3.tar.gz, which is the same
file on GitHub (LoupVaillant/Monocypher, release 4.0.3) and monocypher.org:

  SHA-256 8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66
  SHA-512 40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965
          (as monocypher.org publishes it)

Unmodified. src/monocypher.{c,h} and src/optional/monocypher-ed25519.{c,h}
sit in this one directory, so monocypher-ed25519.h finds "monocypher.h"
beside it. 4.0.3 is the release that fixed a timing leak in EdDSA/Ed25519
signing.

Licence: BSD-2-Clause or CC0-1.0, at the user's choice (LICENCE.md).
