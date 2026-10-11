#!/usr/bin/env bash
# Regenerates third_party/netsurf/patches/*.patch from a patched tree vs a pristine one.
# Usage: tools/ns-mkpatches.sh <pristine netsurf-all-3.11 dir> <patched netsurf-all-3.11 dir>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/netsurf/patches"
A="$1" B="$2"
mk() { # patch-name file...
	local name="$1"; shift
	(cd "$(dirname "$A")" && for f in "$@"; do
		diff -u "$(basename "$A")/$f" "$B/$f" | sed -e "1s|^--- [^\t]*|--- a/$f|" -e "2s|^+++ [^\t]*|+++ b/$f|" || true
	done) > "$OUT/$name"
}
mk 0001-wii-config.patch netsurf/utils/config.h
mk 0002-wii-target.patch netsurf/frontends/Makefile.hts
mk 0003-curl-optional.patch netsurf/content/fetch.c
mk 0004-linear-text-advances.patch netsurf/frontends/framebuffer/font_freetype.h \
	netsurf/frontends/framebuffer/font_freetype.c netsurf/frontends/framebuffer/framebuffer.c
