/* Phase 1 probe: where does libogc's malloc get memory once MEM1 runs out, with and without
 * WiiLibs' wl_mem claiming the top of MEM1? Decides whether wl_mem/wl_gx can coexist with
 * FFmpeg and NetSurf, which allocate through plain malloc. */
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <gccore.h>

#include "wl/platform/mem.h"

static void probe(const char *label)
{
	enum { CHUNK = 1024 * 1024, MAX = 64 };
	void *blocks[MAX];
	int n = 0, mem1 = 0, mem2 = 0;
	uintptr_t lo1 = UINTPTR_MAX, hi1 = 0, lo2 = UINTPTR_MAX, hi2 = 0;
	while (n < MAX && (blocks[n] = malloc(CHUNK)) != NULL) {
		uintptr_t a = (uintptr_t)blocks[n];
		if (a < 0x90000000u) {
			mem1++;
			lo1 = a < lo1 ? a : lo1;
			hi1 = a > hi1 ? a : hi1;
		} else {
			mem2++;
			lo2 = a < lo2 ? a : lo2;
			hi2 = a > hi2 ? a : hi2;
		}
		n++;
	}
	SYS_Report("%s: malloc'd %d MiB before failing: %d in MEM1 (%08lx-%08lx), %d in MEM2 (%08lx-%08lx)\n",
		label, n, mem1, (unsigned long)lo1, (unsigned long)hi1, mem2, (unsigned long)lo2,
		(unsigned long)hi2);
	SYS_Report("  arena1 %p-%p arena2 %p-%p\n", SYS_GetArena1Lo(), SYS_GetArena1Hi(),
		SYS_GetArena2Lo(), SYS_GetArena2Hi());
	while (n)
		free(blocks[--n]);
}

int main(void)
{
	VIDEO_Init();
	SYS_Report("mem probe start\n");

	/* WiiLibs with only MEM1 tags in use (GX FIFO etc.), as the app would run it. */
	WlMemConfig cfg;
	wl_mem_config_default(&cfg);
	static WlMemTagDef tags[WL_MEM_MAX_TAGS];
	for (uint32_t i = 0; i < cfg.tag_count; i++) {
		tags[i] = cfg.tags[i];
		if (tags[i].region == WL_MEM2)
			tags[i].budget = 0;
	}
	cfg.tags = tags;
	cfg.claim[WL_MEM1] = WL_MEM_CLAIM_NEEDED; /* only the budgets: the rest of MEM1 stays with malloc */
	WlResult r = wl_mem_init(&cfg);
	SYS_Report("wl_mem_init (MEM2 tags disabled): %d\n", (int)r);
	WlMemRow rows[WL_MEM_MAX_ROWS];
	uint32_t nrows = wl_mem_rows(rows, WL_MEM_MAX_ROWS);
	size_t mem1_budget = 0;
	for (uint32_t i = 0; i < nrows; i++)
		if (rows[i].region == WL_MEM1)
			mem1_budget += rows[i].budget;
	SYS_Report("wl_mem MEM1 budgets total %u KiB\n", (unsigned)(mem1_budget / 1024));
	probe("after wl_mem_init");
	SYS_Report("mem probe done\n");
	for (;;)
		VIDEO_WaitVSync();
}
