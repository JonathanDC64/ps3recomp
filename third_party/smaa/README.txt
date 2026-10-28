SMAA, Enhanced Subpixel Morphological Antialiasing (https://www.iryoku.com/smaa/),
by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego
Gutierrez, for the D3D12 backend's SMAA 1x pass (RSX_AA=2,
libs/video/rsx_smaa.h).

ps3recomp: vendored from the reference implementation on GitHub, iryoku/smaa
at commit 71c806a838bdd7d517df19192a20f0c61b3ca29d:

  LICENSE.txt   LICENSE.txt           SHA-256 2b848b67ba50ac40b53e11cf1ccdaee2a190ca83b4652cba695a5f1ce81f52ae
  SMAA.hlsl     SMAA.hlsl             SHA-256 06b71714c2c2f8f1fd6ec8747a9700d6fbfd6d807ece87f0fb874d24b64e9cbc
  AreaTex.h     Textures/AreaTex.h    SHA-256 1933bf43ce86ba71f7ed5f0fa94b148610c634dc3395b9bc67a817d51ee73af2
  SearchTex.h   Textures/SearchTex.h  SHA-256 1fac315ab5f87b60083c9b8387af17f056204d3d1bf21500f2acc5b3e8c1a37a

Unmodified, CRLF line ends and all (.gitattributes keeps git from changing
them). AreaTex.h and SearchTex.h are the two precomputed lookup textures,
which the backend uploads as they are. SMAA.hlsl goes into the program as a
string, libs/video/rsx_smaa_src.h, which tools/smaa_embed.py writes from it;
tools/test_smaa_embed.py checks the two match. SMAA's notes say to update the
shader and the textures together.

Licence: MIT (LICENSE.txt).
