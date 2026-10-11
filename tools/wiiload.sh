#!/usr/bin/env bash
# Sends a .dol to a Wii running the Homebrew Channel (it shows "waiting for connection").
# Usage: WIILOAD=tcp:192.168.x.y tools/wiiload.sh [build/wii/iachannel.dol]
set -euo pipefail
DOL="${1:-$(dirname "$0")/../build/wii/iachannel.dol}"
: "${WIILOAD:?set WIILOAD=tcp:<wii-ip> (the IP is in the Homebrew Channel's top-right corner)}"
export WIILOAD
exec "${DEVKITPRO:-/opt/devkitpro}/tools/bin/wiiload" "$DOL"
