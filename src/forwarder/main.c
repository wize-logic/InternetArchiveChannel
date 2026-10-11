/* Channel forwarder: loads apps/iachannel/boot.dol from the SD card (or a USB drive) and jumps to
 * it, passing argv the way the Homebrew Channel does. It is linked high in MEM1 (see
 * CMakeLists.txt) so the app's sections can be copied over the low memory it doesn't use, and
 * nboot.S makes it bootable as a channel's main content. */
#include <fat.h>
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <wiiuse/wpad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define APP_PATH "/apps/iachannel/boot.dol"
/* The forwarder's own base (CMakeLists.txt, .init); the app must fit below it. */
#define LOAD_LIMIT 0x01300000u

struct dol_header {
	u32 text_off[7], data_off[11];
	u32 text_addr[7], data_addr[11];
	u32 text_size[7], data_size[11];
	u32 bss_addr, bss_size, entry;
	u32 pad[7];
};

static void *g_xfb;

/* A console is only brought up when something goes wrong, so a normal boot shows no flash of text. */
static void fail(const char *msg)
{
	if (!g_xfb) {
		VIDEO_Init();
		GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
		g_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
		console_init(g_xfb, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
		VIDEO_Configure(mode);
		VIDEO_SetNextFramebuffer(g_xfb);
		VIDEO_SetBlack(false);
		VIDEO_Flush();
		VIDEO_WaitVSync();
		WPAD_Init();
	}
	printf("\n\n  Internet Archive Channel\n\n  %s\n\n", msg);
	printf("  Copy the apps/iachannel folder from the release zip to the root of\n");
	printf("  your SD card, then try again.\n\n  Press HOME or A to return to the Wii Menu.\n");
	for (;;) {
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & (WPAD_BUTTON_HOME | WPAD_BUTTON_A))
			SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
		VIDEO_WaitVSync();
	}
}

/* Reads the whole file into MEM2, which the app's sections never overlap. */
static u8 *read_dol(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8 *buf = (u8 *)SYS_GetArena2Lo();
	if (n < (long)sizeof(struct dol_header) || (u8 *)buf + n > (u8 *)SYS_GetArena2Hi() ||
		fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

static int section_ok(u32 off, u32 addr, u32 size, size_t file_size)
{
	if (!size)
		return 1;
	u32 lo = addr & 0x0FFFFFFF;
	/* Into MEM1 below this forwarder only, and within the file. */
	return off + size <= file_size && lo >= 0x3000 && lo + size <= LOAD_LIMIT;
}

int main(void)
{
	static const struct {
		const char *mount;
		const DISC_INTERFACE *dev;
	} devices[] = {{"sd", &__io_wiisd}, {"usb", &__io_usbstorage}};

	char path[64] = "";
	u8 *dol = NULL;
	size_t dol_size = 0;
	for (unsigned i = 0; i < sizeof devices / sizeof devices[0] && !dol; i++) {
		if (!devices[i].dev->startup() || !fatMount(devices[i].mount, devices[i].dev, 0, 4, 64))
			continue;
		snprintf(path, sizeof path, "%s:%s", devices[i].mount, APP_PATH);
		dol = read_dol(path, &dol_size);
		fatUnmount(devices[i].mount);
	}
	if (!dol)
		fail("Couldn't find sd:/apps/iachannel/boot.dol.");

	const struct dol_header *h = (const struct dol_header *)dol;
	for (int i = 0; i < 7; i++)
		if (!section_ok(h->text_off[i], h->text_addr[i], h->text_size[i], dol_size))
			fail("boot.dol is damaged or too large.");
	for (int i = 0; i < 11; i++)
		if (!section_ok(h->data_off[i], h->data_addr[i], h->data_size[i], dol_size))
			fail("boot.dol is damaged or too large.");

	/* argv for the app: its own path, like the Homebrew Channel passes, plus "--channel" so it
	 * returns to the Wii Menu on exit (there's no Homebrew Channel stub to go back to). The
	 * strings live in MEM2 above the file buffer; the app's libogc startup reads them early. */
	char *cmdline = (char *)dol + ((dol_size + 31) & ~31u);
	size_t len = (size_t)snprintf(cmdline, 128, "%s%c--channel", path, '\0') + 1;
	cmdline[len] = '\0';

	SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
	u32 level;
	_CPU_ISR_Disable(level);
	(void)level;

	for (int i = 0; i < 7; i++)
		if (h->text_size[i]) {
			memcpy((void *)(0x80000000 | h->text_addr[i]), dol + h->text_off[i], h->text_size[i]);
			DCFlushRange((void *)(0x80000000 | h->text_addr[i]), h->text_size[i]);
			ICInvalidateRange((void *)(0x80000000 | h->text_addr[i]), h->text_size[i]);
		}
	for (int i = 0; i < 11; i++)
		if (h->data_size[i]) {
			memcpy((void *)(0x80000000 | h->data_addr[i]), dol + h->data_off[i], h->data_size[i]);
			DCFlushRange((void *)(0x80000000 | h->data_addr[i]), h->data_size[i]);
		}
	if (h->bss_size && section_ok(0, h->bss_addr, h->bss_size, ~0u)) {
		memset((void *)(0x80000000 | h->bss_addr), 0, h->bss_size);
		DCFlushRange((void *)(0x80000000 | h->bss_addr), h->bss_size);
	}

	u32 entry = 0x80000000 | h->entry;
	struct __argv *args = (struct __argv *)(entry + 8);
	if (*(u32 *)(entry + 4) == ARGV_MAGIC) {
		args->argvMagic = ARGV_MAGIC;
		args->commandLine = cmdline;
		args->length = (int)len + 1;
		DCFlushRange(args, sizeof *args);
	}
	DCFlushRange(cmdline, 128);

	((void (*)(void))entry)();
	return 0;
}
