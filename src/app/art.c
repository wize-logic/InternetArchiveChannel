#include "app/app.h"

#include "about_png.h"
#include "audio_png.h"
#include "books_png.h"
#include "collections_png.h"
#include "favourite_png.h"
#include "images_png.h"
#include "logo_png.h"
#include "movies_png.h"
#include "pointer_grab_png.h"
#include "pointer_png.h"
#include "pointer_tap_0_png.h"
#include "pointer_tap_1_png.h"
#include "pointer_tap_2_png.h"
#include "pointer_tap_3_png.h"
#include "pointer_tap_4_png.h"
#include "pointer_wait_png.h"
#include "search_png.h"
#include "settings_png.h"
#include "wayback_png.h"

/* Pointer hotspot: the index fingertip of the upright glove (assets/ui/pointer.png, which is the
 * first tap frame, so the finger rests in the same place). The fist and the clock point from
 * their middle. */
#define POINTER_HOT_X 14
#define POINTER_HOT_Y 1
#define POINTER_TAP_MS 110

struct app_art art;

static void load(struct image *img, const void *data, size_t size, const char *name)
{
	if (image_load_png(img, data, size, 1) != 0)
		WL_LOG_W("ART", "could not load %s", name);
}

void art_load(void)
{
	static const struct {
		const uint8_t *data;
		const uint32_t *size;
		const char *name;
	} icons[ICON_COUNT] = {
		{movies_png, &movies_png_size, "movies"},
		{audio_png, &audio_png_size, "audio"},
		{books_png, &books_png_size, "books"},
		{images_png, &images_png_size, "images"},
		{wayback_png, &wayback_png_size, "wayback"},
		{search_png, &search_png_size, "search"},
		{settings_png, &settings_png_size, "settings"},
		{about_png, &about_png_size, "about"},
		{favourite_png, &favourite_png_size, "favourite"},
		{collections_png, &collections_png_size, "collections"},
	};
	for (int i = 0; i < ICON_COUNT; i++)
		load(&art.icons[i], icons[i].data, *icons[i].size, icons[i].name);
	load(&art.logo, logo_png, logo_png_size, "logo");
	load(&art.pointer, pointer_png, pointer_png_size, "pointer");
	static const struct {
		const uint8_t *data;
		const uint32_t *size;
	} tap[POINTER_TAP_FRAMES] = {
		{pointer_tap_0_png, &pointer_tap_0_png_size}, {pointer_tap_1_png, &pointer_tap_1_png_size},
		{pointer_tap_2_png, &pointer_tap_2_png_size}, {pointer_tap_3_png, &pointer_tap_3_png_size},
		{pointer_tap_4_png, &pointer_tap_4_png_size},
	};
	for (int i = 0; i < POINTER_TAP_FRAMES; i++)
		load(&art.pointer_tap[i], tap[i].data, *tap[i].size, "pointer_tap");
	load(&art.pointer_grab, pointer_grab_png, pointer_grab_png_size, "pointer_grab");
	load(&art.pointer_wait, pointer_wait_png, pointer_wait_png_size, "pointer_wait");
	ui_set_cursor_image(UI_CURSOR_NORMAL, &art.pointer, POINTER_HOT_X, POINTER_HOT_Y);
	ui_set_cursor_frames(UI_CURSOR_HAND, art.pointer_tap, POINTER_TAP_FRAMES, POINTER_TAP_MS, POINTER_HOT_X,
		POINTER_HOT_Y);
	ui_set_cursor_image(UI_CURSOR_GRAB, &art.pointer_grab, art.pointer_grab.w / 2, art.pointer_grab.h / 2);
	ui_set_cursor_image(UI_CURSOR_WAIT, &art.pointer_wait, art.pointer_wait.w / 2, art.pointer_wait.h / 2);
}

const struct image *art_icon(int which)
{
	return which >= 0 && which < ICON_COUNT && art.icons[which].w ? &art.icons[which] : NULL;
}
