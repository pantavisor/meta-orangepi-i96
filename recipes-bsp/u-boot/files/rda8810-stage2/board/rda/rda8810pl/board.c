// SPDX-License-Identifier: GPL-2.0+
/*
 * OrangePi i96 (RDA8810PL) board file for the modern u-boot stage-2.
 * DRAM is already initialised by the vendor SPL; we only report its size.
 */
#include <common.h>
#include <init.h>
#include <asm/global_data.h>
#include <linux/sizes.h>
#include "rda_combo.h"

DECLARE_GLOBAL_DATA_PTR;

#define RDA_DRAM_BASE	0x80000000
#define RDA_DRAM_SIZE	(256 * SZ_1M)	/* OrangePi i96 = 256 MB LPDDR2 */

int dram_init(void)
{
	gd->ram_size = RDA_DRAM_SIZE;
	return 0;
}

int dram_init_banksize(void)
{
	gd->bd->bi_dram[0].start = RDA_DRAM_BASE;
	gd->bd->bi_dram[0].size = RDA_DRAM_SIZE;
	return 0;
}

int board_init(void)
{
	/* boot params / FDT default address inside DRAM */
	gd->bd->bi_boot_params = RDA_DRAM_BASE + 0x100;

	/* Switch the WiFi/BT combo chip's PMU rail on and leave it latched, so
	 * Linux inherits a powered chip instead of needing the modem stack.
	 * Off by default until the bench pins the PMU bit — see rda_combo.c. */
	if (IS_ENABLED(CONFIG_RDA_COMBO_POWER_AUTO))
		rda_combo_power_latch(false);

	return 0;
}
