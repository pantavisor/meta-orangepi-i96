/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RDA8810PL internal SPI ("ISPI") — the transport to the analog blocks.
 *
 * Two ISPI ports exist and the vendor code switches between them with
 * ispi_open(modemSpi):
 *   port 0 = AP SPI3   @ 0x20a40000 — the AP's own analog baseband (PLLs)
 *   port 1 = modem SPI2 @ 0x11a14000 — the PMU (all v_* LDOs, the DCDC bucks
 *                                      and the low-frequency clock outputs)
 *
 * At runtime under the vendor system the modem coprocessor owns port 1, which
 * is why Linux can only reach the PMU through mdcom/msys. In our modem-less
 * boot nothing else drives it: the vendor SPL already talks to the PMU over
 * this exact path (board/rda/rda8810/clock.c pmu_setup_init), so u-boot
 * stage-2 can re-open it and keep poking. See ../../../../../MODEM-WIFI-PORT.md
 * strategy A'.
 */
#ifndef __ASM_ARCH_RDA_ISPI_H
#define __ASM_ARCH_RDA_ISPI_H

#include <linux/types.h>

#define RDA_ISPI_PORT_ABB	0	/* AP SPI3: AP analog baseband */
#define RDA_ISPI_PORT_PMU	1	/* modem SPI2: the PMU */

/**
 * rda_ispi_open() - point the ISPI transport at one of the two ports
 * @port: %RDA_ISPI_PORT_ABB or %RDA_ISPI_PORT_PMU
 *
 * Vendor-exact: writes the same cfg/ctrl magic the SPL uses. Safe to call
 * repeatedly.
 */
void rda_ispi_open(int port);

/** rda_ispi_write() - write a 16-bit value to a 9-bit ISPI register index */
void rda_ispi_write(u32 reg, u32 val);

/**
 * rda_ispi_read() - read a 16-bit value from a 9-bit ISPI register index
 *
 * Returns the value, or 0xffffffff if the transfer timed out (the vendor code
 * spins forever here; we bound it so a register sweep cannot wedge u-boot).
 */
u32 rda_ispi_read(u32 reg);

/* Same three, but they select %RDA_ISPI_PORT_PMU first and restore
 * %RDA_ISPI_PORT_ABB afterwards — exactly what the vendor SPL does around
 * every PMU access. */
void rda_pmu_write(u32 reg, u32 val);
u32 rda_pmu_read(u32 reg);

/**
 * rda_pmu_update() - read/modify/write one PMU register
 * @reg: PMU register index
 * @clr: bits to clear
 * @set: bits to set
 *
 * Returns the value written, or 0xffffffff if the read failed.
 */
u32 rda_pmu_update(u32 reg, u32 clr, u32 set);

#endif /* __ASM_ARCH_RDA_ISPI_H */
