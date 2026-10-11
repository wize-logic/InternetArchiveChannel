# Internet Archive Channel

A native Nintendo Wii browser for the [Internet Archive](https://archive.org): search,
collections, audio, books, images, video and the Wayback Machine. It runs as a Homebrew Channel
app, plus an optional Wii Menu channel (a forwarder WAD) that starts it.

**Version 1.0.0**, the first public release ([CHANGELOG.md](CHANGELOG.md)). Verified in Dolphin and on a
Wii running System Menu 4.3U. Not yet tested: vWii (Wii U) and other System Menu regions.

## Installing

You need a Wii with the Homebrew Channel, an SD card (FAT32), and an internet connection set up in
Wii Settings.

**The app (required):**

1. Unzip `iachannel.zip` to the root of the SD card. You should end up with
   `sd:/apps/iachannel/boot.dol`, `meta.xml`, `icon.png` and a `netsurf/` folder (needed for the
   Wayback Machine).
2. Put the card in the Wii and start **Internet Archive Channel** from the Homebrew Channel.

The app keeps its settings, logs and caches in `sd:/apps/iachannel/`.

**The channel (optional):** a Wii Menu icon that starts the app from the SD card. It needs a WAD
manager that installs fakesigned titles (such as YAWM ModMii Edition) run with a patched IOS.

1. Copy `iachannel.wad` to `sd:/wad/` and install it with your WAD manager.
2. **Internet Archive** appears on the Wii Menu. Starting it loads `sd:/apps/iachannel/boot.dol`
   (or `usb:/apps/iachannel/boot.dol` if the SD card has none), so step 1 above is still needed.
   Updating the app means replacing that folder; the channel never needs reinstalling. Leaving
   the app returns to the Wii Menu.

Title ID `00010001-UIAC`, IOS 58, about 0.6 MB (5 blocks). To remove it, delete it in Wii Settings →
Data Management → Channels. The WAD only adds a channel and doesn't touch the System Menu or any
IOS, but as with any WAD install, have Priiloader or BootMii backup protection in place first.

**Troubleshooting**

- *"Couldn't find sd:/apps/iachannel/boot.dol"* when starting the channel: the app folder is
  missing or misnamed. Press HOME or A to return to the Wii Menu.
- *No network:* check the connection test in Wii Settings → Internet. Press 1 in the app to show
  the network state on the overlay.
- *Wayback pages say "429 Too Many Requests":* web.archive.org is rate-limiting; wait a minute.
- Logs for bug reports are in `sd:/apps/iachannel/logs/`.

## Building

Requirements: devkitPro with `devkitPPC` and `libogc`, plus CMake ≥ 3.20 and Python 3.

```sh
# 1. Third-party libraries not shipped by devkitPro (installed into deps/, no sudo needed)
tools/build-deps.sh mbedtls mbedtls_host ffmpeg compat images freetype netsurf

# 2. Wii build -> build/wii/*.dol
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
cmake -S . -B build/wii -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/wii                      # spikes + iachannel.dol
cmake --build build/wii --target package     # build/wii/iachannel.zip for the SD card
cmake --build build/wii --target wad         # build/wii/iachannel.wad (the channel; needs
                                             # -DIAC_PYTHON=<python with Pillow and libWiiPy>)

# 3. Host build (same net code against BSD sockets) + unit tests
cmake -S . -B build/host && cmake --build build/host && (cd build/host && ctest)
```

## Running the app

- **Homebrew Channel:** unzip `build/wii/iachannel.zip` onto the SD card, or
  `WIILOAD=tcp:<wii-ip> tools/wiiload.sh`.
- **Dolphin:** `VIDEO_BACKEND=D3D SD_DIR=<dir> tools/run-dolphin.sh build/wii/iachannel.dol`. Put
  `tests/autotest/ui_walkthrough.txt` at `<dir>/apps/iachannel/autotest.txt` to drive it unattended, and add
  `DUMP_FRAMES=True` for screenshots.
- **Channel in Dolphin:** `tools/mkwad.py ... --nand-dir <user dir>/Wii` installs the title into a
  Dolphin NAND, and `DOLPHIN_USER_DIR=<user dir> tools/run-dolphin.sh 0001000155494143` boots it.

## Running the TLS spike

- **Real Wii:** `wiiload build/wii/spike_tls.dol` (set `WIILOAD=tcp:<wii-ip>` and open the Homebrew
  Channel). Results print on screen and go to `sd:/apps/iachannel/spike_tls.log`.
- **Dolphin (from WSL):** `tools/run-dolphin.sh build/wii/spike_tls.dol` runs the Windows
  Dolphin install headless, in a throwaway user folder, and prints the OSReport log.
- **Host:** `build/host/spike_tls`.

## Spikes on the SD card

| HBC app | Program | Log |
|---|---|---|
| IA Channel - TLS test | `spike_tls.dol` | `sd:/apps/iachannel/spike_tls.log` |
| IA Channel - Network test | `spike_net.dol` | `sd:/apps/iachannel/spike_net.log` |
| IA Channel - Video test | `spike_video.dol` (needs `sd:/apps/iachannel/media/*.mp4`) | `sd:/apps/iachannel/spike_video.log` |

The video spike also needs `tools/build-deps.sh ffmpeg` before building.

## Layout

| Path | Contents |
|---|---|
| `src/app/` | Screens (home, results, item, players, viewer, Wayback), main loop |
| `src/ui/` | Immediate-mode widgets, fonts, input, on-screen keyboard, autotest scripts |
| `src/core/` | Platform shims (time, mutex), logging, settings |
| `src/net/` | Socket shim (libogc `net_*` / BSD), entropy, mbedTLS, HTTP client |
| `src/ia/` | archive.org API: URLs, JSON parsing, file choice (host-tested) |
| `src/media/` | Range streaming, FFmpeg player, audio ring |
| `src/gfx/` | Images (PNG/JPEG), YUV→RGB video textures (TEV) |
| `src/web/` | NetSurf front end and Wayback fetcher |
| `src/forwarder/` | Channel forwarder and NAND boot stub |
| `tools/` | Dependency builds, Dolphin runner, banner and WAD tools |
| `src/spikes/` | Phase 0 throwaway feasibility programs |
| `third_party/mbedtls_config/` | mbedTLS overrides for the Wii |
| `assets/cacert.pem` | Built-in root CAs (`tools/mk_ca_bundle.py`) |

## License

The source code is MIT ([LICENSE](LICENSE)). The built app and the channel WAD link NetSurf
(GPLv2, used for the Wayback viewer), and the NAND boot stub follows priiloader's OpenDolBoot loader
(GPL-2.0-or-later), so those binaries are distributed under the GPL-2.0 ([LICENSES/GPL-2.0.txt](LICENSES/GPL-2.0.txt)).
The UI font is Noto Sans (SIL OFL 1.1, `assets/fonts/OFL.txt`).

This is an unofficial client, not affiliated with or endorsed by the Internet Archive.

All bundled and linked components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

WiiLibs (the platform layer, `third_party/WiiLibs`) is MIT.
