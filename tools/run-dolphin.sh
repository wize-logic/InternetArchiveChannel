#!/usr/bin/env bash
# Runs a .dol in the Windows Dolphin install from WSL, headless (Null video), with a throwaway
# user folder so the real Dolphin config/NAND is untouched. Prints the OSReport log, which is
# where log_printf() output goes (SYS_Report). Stops when the log contains $DONE_PATTERN.
#
# Usage: [EXTRA_LOGS="POWERPC IOS_NET"] [SD_DIR=dir] tools/run-dolphin.sh build/wii/spike_tls.dol [timeout_s]
# VIDEO_BACKEND="Software Renderer" renders on the CPU (no GPU load: use it for screenshots);
# VIDEO_BACKEND=D3D (or Vulkan/OGL) renders on the GPU; the default Null draws nothing, so EFB
# readbacks return zeros and dumped frames are black.
# DUMP_FRAMES=True writes every frame as PNG to <user dir>/Dump/Frames.
# SD_DIR's contents become the emulated SD card (Dolphin folder sync); without it the card is empty.
# Instead of a file, the first argument can be a 16-digit NAND title ID (0000000100000002 is the
# Wii Menu); use it with DOLPHIN_USER_DIR pointing at a user folder whose Wii/ holds a NAND.
set -euo pipefail

DOL="$1"
TIMEOUT="${2:-300}"
DONE_PATTERN="${DONE_PATTERN:-Press HOME to exit}"
# Fixed install location (C:\Users\bcolb\Documents\Emulators\Dolphin-x64); intentionally not overridable.
readonly DOLPHIN_DIR="/mnt/c/Users/bcolb/Documents/Emulators/Dolphin-x64"
[ -x "$DOLPHIN_DIR/Dolphin.exe" ] || { echo "Dolphin.exe not found in $DOLPHIN_DIR" >&2; exit 1; }
USER_DIR="${DOLPHIN_USER_DIR:-/mnt/c/Users/bcolb/AppData/Local/Temp/iac-dolphin}"

WIN_USER_DIR="$(wslpath -w "$USER_DIR")"
mkdir -p "$USER_DIR/Config" "$USER_DIR/Load"
if [[ "$DOL" =~ ^[0-9a-fA-F]{16}$ ]]; then
	BOOT=(-n "$DOL")
else
	cp "$DOL" "$USER_DIR/Load/"
	BOOT=(-e "$WIN_USER_DIR\\Load\\$(basename "$DOL")")
fi

cat > "$USER_DIR/Config/Logger.ini" <<'INI'
[Options]
Verbosity = 4
WriteToFile = True
WriteToConsole = False
WriteToWindow = False
[Logs]
OSREPORT = True
OSREPORT_HLE = True
INI
# Extra Dolphin log types for debugging, e.g. EXTRA_LOGS="POWERPC MEMMAP IOS_NET".
for t in ${EXTRA_LOGS:-}; do echo "$t = True" >> "$USER_DIR/Config/Logger.ini"; done
cat > "$USER_DIR/Config/Dolphin.ini" <<INI
[Core]
CPUThread = ${DOLPHIN_DUALCORE:-False}
WiiSDCard = True
WiiSDCardEnableFolderSync = True
[Movie]
DumpFrames = ${DUMP_FRAMES:-False}
[DSP]
Backend = No Audio Output
[Interface]
ConfirmStop = False
[Analytics]
PermissionAsked = True
Enabled = False
[AutoUpdate]
UpdateTrack =
INI

# Real EFB->RAM copies, so programs can read back what the GPU drew (Dolphin's default keeps them GPU-only).
cat > "$USER_DIR/Config/GFX.ini" <<'INI'
[Settings]
DumpFramesAsImages = True
# Hash every texel: the default "fast" cache samples a few, so in-place updates to a large
# texture (the glyph atlas) go unnoticed in Dolphin, though they work on hardware.
SafeTextureCacheColorSamples = 0
[Hacks]
EFBToTextureEnable = False
EFBAccessEnable = True
INI

# Dolphin builds the SD image from Load/WiiSDSync at boot and writes it back on clean shutdown.
SD_SYNC="$USER_DIR/Load/WiiSDSync"
rm -rf "$SD_SYNC" "$USER_DIR/Load/WiiSD.raw" "$USER_DIR/Dump/Frames"
mkdir -p "$SD_SYNC"
[ -n "${SD_DIR:-}" ] && cp -r "$SD_DIR"/. "$SD_SYNC"/

LOG="$USER_DIR/Logs/dolphin.log"
rm -f "$LOG"
# ASPECT=4:3 sets the emulated console's aspect-ratio setting (SYSCONF IPL.AR); default 16:9.
ARGS=()
[ "${ASPECT:-16:9}" = "4:3" ] && ARGS+=(-C SYSCONF.IPL.AR=0)
(cd "$DOLPHIN_DIR" && ./Dolphin.exe -u "$WIN_USER_DIR" -b -v "${VIDEO_BACKEND:-Null}" "${ARGS[@]}" "${BOOT[@]}" >/dev/null 2>&1 &)
# Stop only this run's Dolphin (its command line names our user folder): other sessions may be
# running their own Dolphin at the same time.
PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
kill_ours() {
	"$PS" -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='Dolphin.exe'\" | Where-Object { \$_.CommandLine -like '*$(basename "$USER_DIR")*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1 || true
}
trap kill_ours EXIT

for ((t = 0; t < TIMEOUT; t += 2)); do
	sleep 2
	grep -aq "$DONE_PATTERN" "$LOG" 2>/dev/null && break
done
if [ -n "${EXTRA_LOGS:-}" ]; then
	cat "$LOG" 2>/dev/null || echo "(no log)"
else
	grep -a "OSREPORT" "$LOG" 2>/dev/null | sed 's/^.*OSREPORT[^:]*: *//' || echo "(no OSReport output)"
fi
