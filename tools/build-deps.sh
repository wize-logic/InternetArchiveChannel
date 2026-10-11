#!/usr/bin/env bash
# Builds third-party libraries that are not in devkitPro pacman into deps/prefix.
# Usage: tools/build-deps.sh [mbedtls] [mbedtls_host] [ffmpeg] [compat] [images] [freetype] [netsurf]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
PREFIX="$ROOT/deps/prefix"
DL="$ROOT/deps/dl"
SRC="$ROOT/deps/src"
mkdir -p "$PREFIX" "$DL" "$SRC"

fetch() { # url sha256 file
	local url="$1" sum="$2" out="$DL/$3"
	[ -f "$out" ] || curl -fL --retry 3 -o "$out" "$url"
	echo "$sum  $out" | sha256sum -c -
}

MBEDTLS_VER=3.6.7
MBEDTLS_SHA=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

build_mbedtls() {
	local ver=$MBEDTLS_VER
	fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$ver/mbedtls-$ver.tar.bz2" \
		"$MBEDTLS_SHA" "mbedtls-$ver.tar.bz2"
	rm -rf "$SRC/mbedtls-$ver" && tar -xjf "$DL/mbedtls-$ver.tar.bz2" -C "$SRC"
	cp "$ROOT/third_party/mbedtls_config/threading_alt.h" "$SRC/mbedtls-$ver/include/"
	cmake -S "$SRC/mbedtls-$ver" -B "$SRC/mbedtls-$ver/build-wii" \
		-DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Wii.cmake" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DMBEDTLS_USER_CONFIG_FILE="$ROOT/third_party/mbedtls_config/iac_mbedtls_user_config.h" \
		-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
		-DMBEDTLS_FATAL_WARNINGS=OFF
	cmake --build "$SRC/mbedtls-$ver/build-wii" -j"$(nproc)"
	cmake --install "$SRC/mbedtls-$ver/build-wii"
	# Installed headers must see the same config as the library.
	cp "$ROOT/third_party/mbedtls_config/iac_mbedtls_user_config.h" \
		"$ROOT/third_party/mbedtls_config/threading_alt.h" "$PREFIX/include/"
}

# Native build of the same mbedTLS config, for host unit tests (deps/host-prefix).
build_mbedtls_host() {
	local ver=$MBEDTLS_VER hp="$ROOT/deps/host-prefix"
	fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$ver/mbedtls-$ver.tar.bz2" \
		"$MBEDTLS_SHA" "mbedtls-$ver.tar.bz2"
	[ -d "$SRC/mbedtls-$ver" ] || tar -xjf "$DL/mbedtls-$ver.tar.bz2" -C "$SRC"
	cp "$ROOT/third_party/mbedtls_config/threading_alt.h" "$SRC/mbedtls-$ver/include/"
	cmake -S "$SRC/mbedtls-$ver" -B "$SRC/mbedtls-$ver/build-host" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$hp" \
		-DMBEDTLS_USER_CONFIG_FILE="$ROOT/third_party/mbedtls_config/iac_mbedtls_user_config.h" \
		-DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF \
		-DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
		-DMBEDTLS_FATAL_WARNINGS=OFF
	cmake --build "$SRC/mbedtls-$ver/build-host" -j"$(nproc)"
	cmake --install "$SRC/mbedtls-$ver/build-host"
	cp "$ROOT/third_party/mbedtls_config/iac_mbedtls_user_config.h" \
		"$ROOT/third_party/mbedtls_config/threading_alt.h" "$hp/include/"
}

FFMPEG_VER=8.1.3
# Pinned 2026-10-09 after verifying the GPG signature (FFmpeg release key FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8).
FFMPEG_SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3

# Minimal FFmpeg for the Wii: plain C (no AltiVec on Broadway), no threads, no network,
# no swscale (YUV planes go straight to GX textures), only the codecs IA derivatives use.
build_ffmpeg() {
	local ver=$FFMPEG_VER src="$SRC/ffmpeg-$FFMPEG_VER"
	fetch "https://ffmpeg.org/releases/ffmpeg-$ver.tar.xz" "$FFMPEG_SHA" "ffmpeg-$ver.tar.xz"
	rm -rf "$src" && tar -xJf "$DL/ffmpeg-$ver.tar.xz" -C "$SRC"
	local arch="-mrvl -mcpu=750 -meabi -mhard-float"
	(cd "$src" && ./configure \
		--prefix="$PREFIX" \
		--enable-cross-compile --target-os=none --arch=ppc --cpu=750 \
		--cross-prefix="$DEVKITPPC/bin/powerpc-eabi-" \
		--pkg-config=false \
		--extra-cflags="$arch -O2 -ffunction-sections -fdata-sections -DGEKKO -I$DEVKITPRO/libogc/include" \
		--extra-ldflags="$arch -L$DEVKITPRO/libogc/lib/wii" \
		--extra-libs="-logc -lm" \
		--disable-everything --disable-autodetect \
		--disable-programs --disable-doc --disable-debug \
		--disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
		--disable-avdevice --disable-avfilter --disable-swscale \
		--disable-altivec --disable-vsx --disable-power8 \
		--enable-static --disable-shared \
		--enable-decoder=mpeg4,h264,theora,vp8,mpeg1video,mpeg2video,mp3float,aac,vorbis \
		--enable-demuxer=mov,ogg,matroska,avi,mpegps,mp3 \
		--enable-parser=h264,mpeg4video,mpegvideo,mpegaudio,aac,vorbis,vp8 \
		--enable-swresample)
	make -C "$src" -j"$(nproc)"
	make -C "$src" install
}

ARCH_CFLAGS="-mrvl -mcpu=750 -meabi -mhard-float -O2 -ffunction-sections -fdata-sections -DGEKKO"

# newlib declares iconv() and scandir() but devkitPPC ships no implementation.
build_compat() {
	local out="$SRC/compat-build"
	rm -rf "$out" && mkdir -p "$out"
	for f in iconv scandir; do
		"$DEVKITPPC/bin/powerpc-eabi-gcc" $ARCH_CFLAGS -std=c11 -Wall -Wextra -Werror \
			-c "$ROOT/third_party/compat/src/$f.c" -o "$out/$f.o"
	done
	mkdir -p "$PREFIX/lib"
	"$DEVKITPPC/bin/powerpc-eabi-ar" rcs "$PREFIX/lib/libiconv.a" "$out/iconv.o"
	"$DEVKITPPC/bin/powerpc-eabi-ar" rcs "$PREFIX/lib/libiaccompat.a" "$out/scandir.o"
}

LJT_VER=3.1.4.1
# GitHub release over HTTPS, pinned 2026-10-09. The release is GPG-signed (key 7EC2DBB6F4DBF434) but
# no keyserver was reachable to verify it; re-check with `gpg --verify` when possible.
LJT_SHA=ecae8008e2cc9ade2f2c1bb9d5e6d4fb73e7c433866a056bd82980741571a022
PNG_VER=1.6.55
# GitHub tag archive over HTTPS, pinned 2026-10-09.
PNG_SHA=71a2c5b1218f60c4c6d2f1954c7eb20132156cae90bdb90b566c24db002782a6

wii_cmake() { # src build [cmake args...]
	local s="$1" b="$2"
	shift 2
	cmake -S "$s" -B "$b" -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Wii.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" "$@"
	cmake --build "$b" -j"$(nproc)"
	cmake --install "$b"
}

# JPEG and PNG for NetSurf (devkitPro ships them as pacman packages, which need sudo).
build_images() {
	fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LJT_VER/libjpeg-turbo-$LJT_VER.tar.gz" \
		"$LJT_SHA" "libjpeg-turbo-$LJT_VER.tar.gz"
	rm -rf "$SRC/libjpeg-turbo-$LJT_VER" && tar -xzf "$DL/libjpeg-turbo-$LJT_VER.tar.gz" -C "$SRC"
	# No AltiVec on Broadway, so plain C.
	wii_cmake "$SRC/libjpeg-turbo-$LJT_VER" "$SRC/libjpeg-turbo-$LJT_VER/build-wii" \
		-DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_SIMD=OFF -DWITH_TURBOJPEG=OFF \
		-DCMAKE_INSTALL_LIBDIR=lib

	fetch "https://github.com/pnggroup/libpng/archive/refs/tags/v$PNG_VER.tar.gz" \
		"$PNG_SHA" "libpng-$PNG_VER.tar.gz"
	rm -rf "$SRC/libpng-$PNG_VER" && tar -xzf "$DL/libpng-$PNG_VER.tar.gz" -C "$SRC"
	wii_cmake "$SRC/libpng-$PNG_VER" "$SRC/libpng-$PNG_VER/build-wii" \
		-DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
		-DPNG_HARDWARE_OPTIMIZATIONS=OFF -DCMAKE_INSTALL_LIBDIR=lib \
		-DZLIB_INCLUDE_DIR="$DEVKITPRO/portlibs/ppc/include" \
		-DZLIB_LIBRARY="$DEVKITPRO/portlibs/ppc/lib/libz.a"
	# libpng16.pc says -lpng16, but the static CMake build installs libpng.a.
	cp "$PREFIX/lib/libpng.a" "$PREFIX/lib/libpng16.a"
}

FT_VER=2.14.3
# Verified 2026-10-09: good GPG signature from Werner Lemberg (E306 7470 7856 409F F194 8010 BE6C 3AAC 63AD 8E3F).
FT_SHA=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f

# FreeType for the UI font renderer (TrueType/CFF + autohinter; no zlib/bzip2/png/harfbuzz/brotli).
build_freetype() {
	fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$FT_VER.tar.xz" "$FT_SHA" \
		"freetype-$FT_VER.tar.xz"
	rm -rf "$SRC/freetype-$FT_VER" && tar -xJf "$DL/freetype-$FT_VER.tar.xz" -C "$SRC"
	wii_cmake "$SRC/freetype-$FT_VER" "$SRC/freetype-$FT_VER/build-wii" \
		-DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
		-DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DCMAKE_INSTALL_LIBDIR=lib
}

NETSURF_VER=3.11
# Pinned 2026-10-09 from download.netsurf-browser.org over HTTPS (no published checksum/signature).
NETSURF_SHA=4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b

# NetSurf (GPLv2) for the Wayback viewer: support libraries via NetSurf's buildsystem, then the
# core + reusable framebuffer pieces via our "wii" frontend (src/web/nsfront), archived into
# libnetsurf.a. Needs host perl, flex, bison, gperf, pkg-config. Run build_compat first.
build_netsurf() {
	local ver=$NETSURF_VER src="$SRC/netsurf-all-$NETSURF_VER"
	fetch "https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-$ver.tar.gz" \
		"$NETSURF_SHA" "netsurf-all-$ver.tar.gz"
	rm -rf "$src" && tar -xzf "$DL/netsurf-all-$ver.tar.gz" -C "$SRC"
	for p in "$ROOT"/third_party/netsurf/patches/*.patch; do
		patch -d "$src" -p1 --quiet < "$p"
	done

	local ns_make=(PREFIX="$PREFIX" HOST=powerpc-eabi NSSHARED="$PREFIX/share/netsurf-buildsystem"
		COMPONENT_TYPE=lib-static)
	(
		export PATH="$PATH:$DEVKITPPC/bin" PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=
		export CFLAGS="$ARCH_CFLAGS -I$DEVKITPRO/libogc/include -I$PREFIX/include -I$ROOT/third_party/compat -Wno-error"
		make -C "$src/buildsystem" install PREFIX="$PREFIX"
		for lib in libwapcaplet libparserutils libhubbub libcss libnsutils libnslog \
			libutf8proc libnsgif libnsbmp libnspsl libnsfb; do
			make -C "$src/$lib" install "${ns_make[@]}"
		done
		make -C "$src/libdom" install "${ns_make[@]}" \
			WITH_EXPAT_BINDING=no WITH_LIBXML_BINDING=no WITH_HUBBUB_BINDING=yes
	)

	local fe="$src/netsurf/frontends/wii"
	mkdir -p "$fe"
	cp "$ROOT"/src/web/nsfront/* "$fe/"
	ln -sfn ../framebuffer/res "$fe/res"
	(
		export PATH="$PATH:$DEVKITPPC/bin"
		unset CFLAGS
		make -C "$src/netsurf" TARGET=wii IAC_PREFIX="$PREFIX" IAC_COMPAT="$ROOT/third_party/compat" \
			NSSHARED="$PREFIX/share/netsurf-buildsystem" -j"$(nproc)"
	)

	# Everything except the placeholder main() goes into libnetsurf.a; headers and generated
	# files are used straight from the source/build tree (see CMakeLists.txt).
	local objs="$src/netsurf/build/Linux-wii"
	rm -f "$PREFIX/lib/libnetsurf.a"
	find "$objs" -name '*.o' ! -name 'frontends_wii_wii_stub.o' -print0 |
		xargs -0 "$DEVKITPPC/bin/powerpc-eabi-ar" rcs "$PREFIX/lib/libnetsurf.a"
}

targets=("${@:-mbedtls}")
for t in "${targets[@]}"; do "build_$t"; done
