/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Board config header for the OrangePi i96 (RDA8810PL) modern u-boot stage-2.
 * Most settings live in the defconfig / Kconfig; this only carries what still
 * needs a header in modern u-boot.
 */
#ifndef __RDA8810PL_H
#define __RDA8810PL_H

#define CFG_SYS_SDRAM_BASE		0x80000000

/* Early malloc/stack live in low DRAM (already inited by the vendor SPL), in
 * the 32 KB gap below u-boot's TEXT_BASE (0x80008000). Must be big enough for
 * the pre-reloc stack + global_data + early DM malloc (SYS_MALLOC_F_LEN, 0x2000
 * with driver-model). The old 0x1000 (4 KB) was too small: board_init_f reserves
 * SYS_MALLOC_F_LEN below the init SP, which underflowed past the 0x80000000 DRAM
 * base and faulted in board_init_f_init_reserve — before arch_cpu_init, so the
 * console/DEBUG_UART never came up and the SPL->u-boot handoff looked dead. */
#define CFG_SYS_INIT_RAM_ADDR		0x80000000
#define CFG_SYS_INIT_RAM_SIZE		0x8000

/* Pantavisor-convention default environment: the generic pantavisor boot.scr
 * (boot.cmd.pvgeneric) composes bootargs from ${console}/${baudrate} and loads
 * via ${kernel_addr_r}/${ramdisk_addr_r}/${fdt_addr_r}, so the BOARD env must
 * provide them. Mainline serial2 alias = uart3 (the console UART) and the
 * rda-uart driver registers ttyRDA<alias#> => console=ttyRDA2.
 * Memory map (256 MB @ 0x80000000): u-boot @0x80008000, DMA bounce @0x81000000,
 * scripts/env @loadaddr 0x82000000, fdt @0x83000000, kernel @0x84000000,
 * ramdisk @0x85000000 (initramfs Load Address, clears the ~12 MB zImage).
 * mmcdev: the SD card is mmc 0 here. boot.scr falls back to mmc 1 when neither
 * ${devnum} nor ${mmcdev} is set, and the bootcmd sets neither. */
#define CFG_EXTRA_ENV_SETTINGS \
	"loadaddr=0x82000000\0" \
	"mmcdev=0\0" \
	"console=ttyRDA2\0" \
	"fdtfile=rda8810pl-orangepi-i96.dtb\0" \
	"kernel_addr_r=0x84000000\0" \
	"fdt_addr_r=0x83000000\0" \
	"ramdisk_addr_r=0x85000000\0" \
	"scriptaddr=0x82000000\0"

#endif /* __RDA8810PL_H */
