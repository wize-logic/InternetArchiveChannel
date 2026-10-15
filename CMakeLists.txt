cmake_minimum_required(VERSION 3.20)
project(IAChannel C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NINTENDO_WII OR CMAKE_SYSTEM_NAME STREQUAL "NintendoWii")
	set(IAC_WII ON)
	set(IAC_DEPS_PREFIX "${CMAKE_SOURCE_DIR}/deps/prefix")
else()
	set(IAC_WII OFF)
	set(IAC_DEPS_PREFIX "${CMAKE_SOURCE_DIR}/deps/host-prefix")
endif()

if(NOT EXISTS "${IAC_DEPS_PREFIX}/lib/libmbedtls.a")
	message(FATAL_ERROR "mbedTLS missing in ${IAC_DEPS_PREFIX}: run tools/build-deps.sh mbedtls mbedtls_host")
endif()

add_library(mbedtls_deps INTERFACE)
target_include_directories(mbedtls_deps INTERFACE "${IAC_DEPS_PREFIX}/include")
target_compile_definitions(mbedtls_deps INTERFACE
	MBEDTLS_USER_CONFIG_FILE="iac_mbedtls_user_config.h")
target_link_libraries(mbedtls_deps INTERFACE
	"${IAC_DEPS_PREFIX}/lib/libmbedtls.a"
	"${IAC_DEPS_PREFIX}/lib/libmbedx509.a"
	"${IAC_DEPS_PREFIX}/lib/libmbedcrypto.a"
	"${IAC_DEPS_PREFIX}/lib/libp256m.a"
	"${IAC_DEPS_PREFIX}/lib/libeverest.a")

# Built-in CA roots (regenerate with tools/mk_ca_bundle.py).
set(CA_BUNDLE_C "${CMAKE_BINARY_DIR}/gen/ca_bundle.c")
add_custom_command(
	OUTPUT "${CA_BUNDLE_C}"
	COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/assets/cacert.pem
		-DOUT=${CA_BUNDLE_C} -DSYM=iac_ca_bundle_pem -P ${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake
	DEPENDS "${CMAKE_SOURCE_DIR}/assets/cacert.pem" "${CMAKE_SOURCE_DIR}/cmake/embed_text.cmake"
	VERBATIM)

# Portable core + network layer, shared by the app, spikes and host tests.
add_library(iac_net STATIC
	src/core/log.c
	src/core/platform.c
	src/net/entropy.c
	src/net/sock.c
	src/net/tls.c
	src/net/url.c
	src/net/http_parse.c
	src/net/http.c
	"${CA_BUNDLE_C}")
target_include_directories(iac_net PUBLIC src)
target_compile_options(iac_net PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(iac_net PUBLIC mbedtls_deps)
if(IAC_WII)
	target_link_libraries(iac_net PUBLIC fat wiiuse bte ogc m)
else()
	find_package(Threads REQUIRED)
	target_link_libraries(iac_net PUBLIC Threads::Threads)
endif()

add_executable(spike_tls src/spikes/tls_spike.c)
target_compile_options(spike_tls PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(spike_tls PRIVATE iac_net)
add_executable(spike_net src/spikes/net_spike.c)
target_compile_options(spike_net PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(spike_net PRIVATE iac_net)

if(IAC_WII)
	ogc_create_dol(spike_tls)
	ogc_create_dol(spike_net)

	# Channel forwarder (the WAD's main content): chain-loads apps/iachannel/boot.dol. Linked high
	# in MEM1, clear of the app's sections, with the NAND boot stub at 0x80003400.
	add_executable(forwarder src/forwarder/main.c src/forwarder/nboot.S)
	target_compile_options(forwarder PRIVATE -Wall -Wextra -Os)
	target_link_options(forwarder PRIVATE -Wl,--section-start,.init=0x81300000
		-Wl,--section-start,.stub=0x80003400)
	target_link_libraries(forwarder PRIVATE fat wiiuse bte ogc)
	ogc_create_dol(forwarder)

	# Video needs the FFmpeg portlib (tools/build-deps.sh ffmpeg); Wii only.
	if(EXISTS "${IAC_DEPS_PREFIX}/lib/libavcodec.a")
		add_library(iac_gfx STATIC src/gfx/gx.c src/gfx/yuv.c)
		target_include_directories(iac_gfx PUBLIC src)
		target_compile_options(iac_gfx PRIVATE -Wall -Wextra -Wno-unused-parameter)
		target_link_libraries(iac_gfx PUBLIC ogc m)

		add_library(ffmpeg_deps INTERFACE)
		target_include_directories(ffmpeg_deps INTERFACE "${IAC_DEPS_PREFIX}/include")
		target_link_libraries(ffmpeg_deps INTERFACE
			"${IAC_DEPS_PREFIX}/lib/libavformat.a"
			"${IAC_DEPS_PREFIX}/lib/libavcodec.a"
			"${IAC_DEPS_PREFIX}/lib/libswresample.a"
			"${IAC_DEPS_PREFIX}/lib/libavutil.a")

		add_executable(spike_video src/spikes/video_spike.c)
		target_compile_options(spike_video PRIVATE -Wall -Wextra -Wno-unused-parameter)
		target_link_libraries(spike_video PRIVATE iac_gfx iac_net ffmpeg_deps)
		ogc_create_dol(spike_video)
	endif()

	# WiiLibs platform layer: git submodule (git submodule update --init).
	set(IAC_WIILIBS_DIR "${CMAKE_SOURCE_DIR}/third_party/WiiLibs" CACHE PATH "WiiLibs source tree")
	if(EXISTS "${IAC_WIILIBS_DIR}/CMakeLists.txt")
		add_subdirectory("${IAC_WIILIBS_DIR}" wiilibs EXCLUDE_FROM_ALL)
		add_executable(spike_mem src/spikes/mem_spike.c)
		target_link_libraries(spike_mem PRIVATE WiiLibs::platform)
		ogc_create_dol(spike_mem)

		# The app (Phase 1+): WiiLibs platform layer, FreeType UI fonts embedded with bin2s.
		if(EXISTS "${IAC_DEPS_PREFIX}/lib/libfreetype.a")
			enable_language(ASM)
			include(dkp-embedded-binary)
			file(GLOB IAC_UI_ART CONFIGURE_DEPENDS assets/ui/*.png assets/ui/icons/*.png)
			dkp_add_embedded_binary_library(iac_fonts
				assets/fonts/NotoSans-Regular.ttf assets/fonts/NotoSans-Bold.ttf ${IAC_UI_ART})

			add_library(freetype_deps INTERFACE)
			target_include_directories(freetype_deps INTERFACE "${IAC_DEPS_PREFIX}/include/freetype2")
			target_link_libraries(freetype_deps INTERFACE "${IAC_DEPS_PREFIX}/lib/libfreetype.a")

			file(GLOB IAC_APP_SOURCES CONFIGURE_DEPENDS src/app/*.c src/ui/*.c src/media/*.c src/gfx/image.c
				src/gfx/yuv.c)
			add_executable(iachannel ${IAC_APP_SOURCES} src/core/settings.c src/ia/ia.c
				third_party/cjson/cJSON.c)
			target_include_directories(iachannel PRIVATE third_party/cjson)
			target_include_directories(iachannel PRIVATE src)
			target_compile_options(iachannel PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation)
			target_include_directories(iachannel PRIVATE "${IAC_DEPS_PREFIX}/include")
			target_link_libraries(iachannel PRIVATE iac_web iac_fonts freetype_deps iac_net ffmpeg_deps WiiLibs::platform
				WiiLibs::codec WiiLibs::base "${IAC_DEPS_PREFIX}/lib/libpng16.a" "${IAC_DEPS_PREFIX}/lib/libjpeg.a"
				"$ENV{DEVKITPRO}/portlibs/ppc/lib/libz.a")
			ogc_create_dol(iachannel)

			# Homebrew Channel package: build/wii/package/apps/iachannel/{boot.dol,meta.xml,icon.png}
			# and iachannel.zip (unzip onto the SD card root).
			set(PKG_DIR "${CMAKE_BINARY_DIR}/package")
			add_custom_target(package
				COMMAND ${CMAKE_COMMAND} -E make_directory "${PKG_DIR}/apps/iachannel"
				COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/iachannel.dol" "${PKG_DIR}/apps/iachannel/boot.dol"
				COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/dist/apps/iachannel/meta.xml"
					"${CMAKE_SOURCE_DIR}/dist/apps/iachannel/icon.png" "${PKG_DIR}/apps/iachannel/"
				COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/netsurf"
					"${PKG_DIR}/apps/iachannel/netsurf"
				COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf"
					"${CMAKE_SOURCE_DIR}/assets/fonts/NotoSans-Bold.ttf" "${CMAKE_SOURCE_DIR}/assets/fonts/OFL.txt"
					"${PKG_DIR}/apps/iachannel/netsurf/"
				COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/LICENSE"
					"${CMAKE_SOURCE_DIR}/LICENSES/GPL-2.0.txt" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
					"${PKG_DIR}/apps/iachannel/"
				COMMAND ${CMAKE_COMMAND} -E chdir "${PKG_DIR}" ${CMAKE_COMMAND} -E tar cf
					"${CMAKE_BINARY_DIR}/iachannel.zip" --format=zip apps
				DEPENDS iachannel
				COMMENT "Packaging the Homebrew Channel app")

			# Channel WAD (forwarder + banner): build/wii/iachannel.wad. Needs a Python with Pillow
			# and libWiiPy (IAC_PYTHON); the certificate chain is downloaded once into the build dir.
			set(IAC_PYTHON "python3" CACHE STRING "Python with Pillow and libWiiPy, for the wad target")
			add_custom_target(wad
				COMMAND ${IAC_PYTHON} "${CMAKE_SOURCE_DIR}/tools/mkbanner.py" "${CMAKE_BINARY_DIR}/opening.bnr"
				COMMAND ${IAC_PYTHON} "${CMAKE_SOURCE_DIR}/tools/mkwad.py" --banner "${CMAKE_BINARY_DIR}/opening.bnr"
					--dol "${CMAKE_BINARY_DIR}/forwarder.dol" --out "${CMAKE_BINARY_DIR}/iachannel.wad"
					--certs "${CMAKE_BINARY_DIR}/wad-certs.bin"
				DEPENDS forwarder
				COMMENT "Building the channel WAD")
		endif()
	endif()

	# Wayback renderer: NetSurf core (tools/build-deps.sh compat netsurf). Headers and generated
	# files come straight from the patched source/build tree.
	set(IAC_NETSURF_SRC "${CMAKE_SOURCE_DIR}/deps/src/netsurf-all-3.11/netsurf")
	if(EXISTS "${IAC_DEPS_PREFIX}/lib/libnetsurf.a" AND TARGET iac_gfx)
		add_library(netsurf_deps INTERFACE)
		target_include_directories(netsurf_deps INTERFACE
			"${IAC_NETSURF_SRC}" "${IAC_NETSURF_SRC}/include" "${IAC_NETSURF_SRC}/frontends"
			"${IAC_NETSURF_SRC}/build/Linux-wii" "${IAC_DEPS_PREFIX}/include"
			"${CMAKE_SOURCE_DIR}/third_party/compat")
		target_compile_definitions(netsurf_deps INTERFACE nsframebuffer small)
		set(NS_LIBS netsurf css dom hubbub parserutils wapcaplet nsgif nsbmp jpeg png16 nspsl nslog
			nsutils utf8proc iconv iaccompat)
		list(TRANSFORM NS_LIBS PREPEND "${IAC_DEPS_PREFIX}/lib/lib")
		list(TRANSFORM NS_LIBS APPEND ".a")
		# The NetSurf libraries reference each other in both directions.
		# libnsfb surfaces register themselves from static constructors, so nothing references
		# them; --whole-archive keeps them (the "ram" surface in particular).
		target_link_libraries(netsurf_deps INTERFACE
			-Wl,--whole-archive "${IAC_DEPS_PREFIX}/lib/libnsfb.a" -Wl,--no-whole-archive
			-Wl,--start-group ${NS_LIBS} -Wl,--end-group z)
		target_link_directories(netsurf_deps INTERFACE "$ENV{DEVKITPRO}/portlibs/ppc/lib")

		# The app's Wayback page view (only these files see NetSurf's headers and defines).
		add_library(iac_web STATIC src/web/webview.c src/web/wb_fetch.c)
		target_include_directories(iac_web PRIVATE src)
		target_compile_options(iac_web PRIVATE -Wall -Wno-unused-parameter)
		target_link_libraries(iac_web PRIVATE netsurf_deps iac_net WiiLibs::platform WiiLibs::base)

		add_executable(spike_web src/spikes/web_spike.c)
		target_compile_options(spike_web PRIVATE -Wall -Wno-unused-parameter)
		target_link_libraries(spike_web PRIVATE netsurf_deps iac_gfx iac_net freetype_deps)
		ogc_create_dol(spike_web)
	endif()
endif()

if(NOT IAC_WII)
	enable_testing()
	add_executable(test_http_date tests/test_http_date.c)
	target_link_libraries(test_http_date PRIVATE iac_net)
	add_test(NAME http_date COMMAND test_http_date)

	# Wii compat iconv, built for the host under renamed symbols so it can't clash with glibc's.
	add_executable(test_iconv tests/test_iconv.c third_party/compat/src/iconv.c)
	target_compile_definitions(test_iconv PRIVATE
		iconv_open=t_iconv_open iconv=t_iconv iconv_close=t_iconv_close)
	target_compile_options(test_iconv PRIVATE -include stddef.h)
	add_test(NAME iconv COMMAND test_iconv)

	add_executable(test_settings tests/test_settings.c src/core/settings.c)
	target_include_directories(test_settings PRIVATE src)
	add_test(NAME settings COMMAND test_settings)

	add_executable(test_url tests/test_url.c)
	target_link_libraries(test_url PRIVATE iac_net)
	add_test(NAME url COMMAND test_url)

	add_executable(test_http_parse tests/test_http_parse.c)
	target_link_libraries(test_http_parse PRIVATE iac_net)
	add_test(NAME http_parse COMMAND test_http_parse)

	add_executable(test_ia tests/test_ia.c src/ia/ia.c third_party/cjson/cJSON.c)
	target_include_directories(test_ia PRIVATE src third_party/cjson)
	target_link_libraries(test_ia PRIVATE iac_net)
	add_test(NAME ia COMMAND test_ia "${CMAKE_SOURCE_DIR}/tests/fixtures")

	add_executable(test_mp4idx tests/test_mp4idx.c src/media/mp4idx.c)
	target_include_directories(test_mp4idx PRIVATE src)
	target_compile_options(test_mp4idx PRIVATE -Wall -Wextra)
	add_test(NAME mp4idx COMMAND test_mp4idx)

	# Live network test, run by hand: build/host/http_live
	add_executable(http_live tests/http_live.c)
	target_link_libraries(http_live PRIVATE iac_net)
endif()
