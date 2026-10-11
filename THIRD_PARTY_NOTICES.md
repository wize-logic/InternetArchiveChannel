# Third-party software and assets

The source code written for the Internet Archive Channel is MIT (see `LICENSE`). The built app
and the channel WAD link NetSurf (GPL-2.0) and use a boot stub based on GPL-2.0-or-later code, so
the binaries are distributed under GPL-2.0 (`LICENSES/GPL-2.0.txt`). Anyone who redistributes a
binary must offer the source, which is this repository at the release tag. The components below
are used under licences compatible with that.

| Component | Version | Licence used | Where |
|---|---|---|---|
| NetSurf core and libraries | 3.11 | GPL-2.0 | Wayback viewer (`src/web/`, patches in `third_party/netsurf/`); CSS and messages in `assets/netsurf/` |
| FFmpeg (libavcodec, libavformat, libavutil, libswresample) | 8.1.3 | LGPL-2.1-or-later (built without `--enable-gpl`) | Audio and video playback |
| mbedTLS | 3.6.7 | GPL-2.0-or-later (it is dual-licensed Apache-2.0 / GPL-2.0-or-later) | HTTPS |
| FreeType | 2.14.3 | FreeType Licence (FTL) | Text rendering |
| libjpeg-turbo | 3.1.4.1 | IJG / BSD-3-Clause / zlib | JPEG decoding |
| libpng | 1.6.55 | libpng licence | PNG decoding |
| zlib | devkitPro portlibs | zlib licence | Compression |
| cJSON | bundled in `third_party/cjson/` | MIT | archive.org JSON |
| WiiLibs | submodule `third_party/WiiLibs` | MIT | Platform layer, texture and audio codecs |
| libogc, devkitPPC | devkitPro | libogc licence (BSD-style), GCC runtime exception | Wii system library and toolchain |
| Noto Sans | `assets/fonts/` | SIL OFL 1.1 (`assets/fonts/OFL.txt`) | UI font |
| Root certificates | `assets/cacert.pem` (seven roots extracted from the Mozilla list at curl.se/ca) | MPL-2.0 (Mozilla CA list) | Built-in trust roots |
| priiloader OpenDolBoot loader | reimplemented in `src/forwarder/nboot.S` | GPL-2.0-or-later | Channel NAND boot stub |

Source for every library above is available from its upstream project at the version listed.
`tools/build-deps.sh` downloads and builds them, and the NetSurf patches are in
`third_party/netsurf/patches/`. The corresponding source for any release is this repository at the
release tag. Because FFmpeg is statically linked under the LGPL, the build scripts and object
files needed to relink with a modified FFmpeg are reproducible with those scripts.

This is an unofficial client. "Internet Archive" and "Wayback Machine" are marks of their owner;
this project is not affiliated with or endorsed by the Internet Archive.
