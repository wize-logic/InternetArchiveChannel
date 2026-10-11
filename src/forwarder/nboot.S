/* NAND boot stub. IOS starts a channel's main content at physical 0x3400 with the MMU off and
 * the caches in whatever state the System Menu left them, so this sets Broadway up the way a
 * libogc program expects (caches on, BATs for MEM1, MEM2 and the uncached views, translation
 * on) and then jumps to the normal libogc entry point. The libogc linker script puts the .stub
 * section at 0x3400; CMakeLists.txt moves it to 0x80003400 so loaders that only accept cached
 * addresses take it too. The setup sequence follows the open-source NAND loader in DacoTaco's
 * OpenDolBoot (priiloader, GPL-2.0-or-later). */

#define HID0    1008
#define IBAT0U  528
#define IBAT0L  529
#define IBAT1U  530
#define IBAT2U  532
#define IBAT3U  534
#define IBAT3L  535
#define DBAT0U  536
#define DBAT0L  537
#define DBAT1U  538
#define DBAT1L  539
#define DBAT2U  540
#define DBAT3U  542
#define DBAT3L  543
#define IBAT4U  560
#define IBAT4L  561
#define IBAT5U  562
#define DBAT4U  568
#define DBAT4L  569
#define DBAT5U  570
#define DBAT5L  571

	.section .stub, "ax"
	.globl	__nboot
__nboot:
	b	1f
	/* Same parameter block layout as OpenDolBoot's loader, so tools that inspect it recognise it. */
	.long	0xC4951034	/* magic */
	.long	2		/* version */
	.long	__app_start	/* entry */
	.long	0		/* application version */
1:
	/* HID0: caches off and invalidated, branch target cache and history table on. */
	lis	3, 0x0011
	ori	3, 3, 0x0C64
	mtspr	HID0, 3
	isync
	li	4, 0x2000	/* MSR: FP on, translation off */
	mtmsr	4
	ori	3, 3, 0xC000	/* caches on */
	mtspr	HID0, 3
	isync

	li	4, 0
	mtspr	DBAT0U, 4
	mtspr	DBAT1U, 4
	mtspr	DBAT2U, 4
	mtspr	DBAT3U, 4
	mtspr	DBAT4U, 4
	mtspr	DBAT5U, 4
	mtspr	IBAT0U, 4
	mtspr	IBAT1U, 4
	mtspr	IBAT2U, 4
	mtspr	IBAT3U, 4
	mtspr	IBAT4U, 4
	mtspr	IBAT5U, 4
	isync

	lis	4, 0x8000	/* segment registers: all direct-store off */
	mtsr	0, 4
	mtsr	1, 4
	mtsr	2, 4
	mtsr	3, 4
	mtsr	4, 4
	mtsr	5, 4
	mtsr	6, 4
	mtsr	7, 4
	mtsr	8, 4
	mtsr	9, 4
	mtsr	10, 4
	mtsr	11, 4
	mtsr	12, 4
	mtsr	13, 4
	mtsr	14, 4
	mtsr	15, 4
	isync

	/* [ID]BAT0: 256 MB at 0x80000000 -> 0, cached (MEM1). */
	li	3, 0x0002
	lis	4, 0x8000
	ori	4, 4, 0x1FFF
	mtspr	DBAT0L, 3
	mtspr	DBAT0U, 4
	mtspr	IBAT0L, 3
	mtspr	IBAT0U, 4
	isync
	/* DBAT1: 256 MB at 0xC0000000 -> 0, uncached (MEM1 and hardware registers). */
	li	4, 0x002A
	lis	3, 0xC000
	ori	3, 3, 0x1FFF
	mtspr	DBAT1L, 4
	mtspr	DBAT1U, 3
	isync
	/* [ID]BAT4: 256 MB at 0x90000000 -> 0x10000000, cached (MEM2). */
	lis	4, 0x1000
	ori	4, 4, 0x0002
	lis	3, 0x9000
	ori	3, 3, 0x1FFF
	mtspr	DBAT4L, 4
	mtspr	DBAT4U, 3
	mtspr	IBAT4L, 4
	mtspr	IBAT4U, 3
	isync
	/* DBAT5: 256 MB at 0xD0000000 -> 0x10000000, uncached (MEM2). */
	lis	4, 0x1000
	ori	4, 4, 0x002A
	lis	3, 0xD000
	ori	3, 3, 0x1FFF
	mtspr	DBAT5L, 4
	mtspr	DBAT5U, 3
	isync
	/* [ID]BAT3: identity map of low memory, so this code keeps running once translation is on. */
	li	4, 0x0001
	li	3, 0x001F
	mtspr	DBAT3L, 4
	mtspr	DBAT3U, 3
	mtspr	IBAT3L, 4
	mtspr	IBAT3U, 3
	isync

	mfmsr	4
	ori	4, 4, 0x0030	/* instruction and data translation on */
	mtmsr	4
	isync

	/* No bi2.bin (disc boot info) for a channel, then off to libogc's entry. */
	lis	3, 0x8000
	li	4, 0
	stw	4, 0xF4(3)
	lis	4, __app_start@h
	ori	4, 4, __app_start@l
	mtlr	4
	blr
