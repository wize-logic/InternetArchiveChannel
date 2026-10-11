# NetSurf frontend for the Internet Archive Channel (Nintendo Wii).
#
# This frontend only exists to build the NetSurf core plus the reusable framebuffer pieces
# (nsfb plotters, FreeType fonts, bitmaps, scheduler, fetch glue) for powerpc-eabi.
# tools/build-deps.sh archives the objects into libnetsurf.a; the real window/GX glue lives
# in the app (src/web/). wii_stub.c only provides main() so this tree links on its own.

ARCH_FLAGS := -mrvl -mcpu=750 -meabi -mhard-float
CFLAGS += -std=c99 $(ARCH_FLAGS) -DGEKKO -Dnsframebuffer -Dsmall \
	-ffunction-sections -fdata-sections \
	-I$(IAC_PREFIX)/include -I$(DEVKITPRO)/libogc/include -I$(DEVKITPRO)/portlibs/ppc/include \
	-I$(IAC_COMPAT) \
	-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE -D_BSD_SOURCE \
	'-DNETSURF_FB_RESPATH="$(NETSURF_FB_RESPATH)"' '-DNETSURF_FB_FONTPATH=""' \
	-I$(IAC_PREFIX)/include/freetype2 -DFB_USE_FREETYPE

# Page text: FreeType with the app's Noto Sans (sd:/apps/iachannel/netsurf/*.ttf, found through
# the resource path). Every family maps to it, as the Wii has no other fonts; scaled text (zoom)
# lays out and draws at the same size, which the built-in 8 px bitmap font can't do.
NS_SANS := NotoSans-Regular.ttf
NS_BOLD := NotoSans-Bold.ttf
CFLAGS += '-DNETSURF_FB_FONT_SANS_SERIF="$(NS_SANS)"' '-DNETSURF_FB_FONT_SANS_SERIF_BOLD="$(NS_BOLD)"' \
	'-DNETSURF_FB_FONT_SANS_SERIF_ITALIC="$(NS_SANS)"' '-DNETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD="$(NS_BOLD)"' \
	'-DNETSURF_FB_FONT_SERIF="$(NS_SANS)"' '-DNETSURF_FB_FONT_SERIF_BOLD="$(NS_BOLD)"' \
	'-DNETSURF_FB_FONT_MONOSPACE="$(NS_SANS)"' '-DNETSURF_FB_FONT_MONOSPACE_BOLD="$(NS_BOLD)"' \
	'-DNETSURF_FB_FONT_CURSIVE="$(NS_SANS)"' '-DNETSURF_FB_FONT_FANTASY="$(NS_SANS)"'

LDFLAGS += $(ARCH_FLAGS) -L$(DEVKITPRO)/libogc/lib/wii -L$(DEVKITPRO)/portlibs/ppc/lib -L$(IAC_PREFIX)/lib
$(eval $(call pkg_config_find_and_add,libnsfb,libnsfb))
LDFLAGS += -lfreetype -liaccompat -lz -lfat -logc -lm

MESSAGES_FILTER=fb
MESSAGES_TARGET=$(FRONTEND_RESOURCES_DIR)

S_FRONTEND := wii_stub.c \
	../framebuffer/framebuffer.c ../framebuffer/schedule.c ../framebuffer/bitmap.c \
	../framebuffer/fetch.c ../framebuffer/findfile.c ../framebuffer/font_freetype.c

SOURCES = $(S_COMMON) $(S_IMAGE) $(S_BROWSER) $(S_FRONTEND)
EXETARGET := nswii.elf
