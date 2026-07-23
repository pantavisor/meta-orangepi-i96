// SPDX-License-Identifier: GPL-2.0+
/*
 * RDA8810PL internal SPI ("ISPI") transport.
 *
 * Straight port of the vendor u-boot arch/arm/cpu/armv7/rda/ispi.c (which is
 * itself identical to the vendor kernel's arch/arm/plat-rda/ispi.c) to modern
 * u-boot: same cfg/ctrl magic, same 26-bit command word, same FIFO handshake.
 * The only deliberate difference is that every spin loop here is bounded — the
 * vendor loops are infinite, and a PMU register sweep that hits a
 * non-responding index would otherwise hang the board.
 *
 * The PMU (all v_* LDOs, the DCDC bucks, the low-frequency clock outputs) is
 * behind port 1. Nothing else drives it in a modem-less boot, so stage-2 owns
 * it; see asm/arch/rda_ispi.h and MODEM-WIFI-PORT.md strategy A'.
 */
#include <common.h>
#include <asm/arch/rda_ispi.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define RDA_SPI3_BASE		0x20a40000	/* AP SPI3   — analog baseband */
#define RDA_MODEM_SPI2_BASE	0x11a14000	/* modem SPI2 — the PMU */

/* HWP_SPI_T register offsets (vendor asm/arch-rda/reg_spi.h) */
#define SPI_CTRL		0x00
#define SPI_STATUS		0x04
#define SPI_RXTX_BUFFER		0x08
#define SPI_CFG			0x0c
#define SPI_IRQ			0x1c

/* status */
#define SPI_ACTIVE_STATUS	BIT(0)
#define SPI_TX_SPACE_SHIFT	16
#define SPI_TX_SPACE_MASK	(0x1f << SPI_TX_SPACE_SHIFT)
#define SPI_RX_LEVEL_SHIFT	24
#define SPI_RX_LEVEL_MASK	(0x1f << SPI_RX_LEVEL_SHIFT)
#define SPI_TX_FIFO_SIZE	16

/* rxtx_buffer */
#define SPI_CS_SHIFT		29
#define SPI_CS_MASK		(3 << SPI_CS_SHIFT)
#define SPI_READ_ENA		BIT(31)

/*
 * Vendor magic from ispi_open(): cfg 0x130003 puts the divider at 0x13, i.e.
 * spi_clk = APB2 / ((0x13 + 1) * 2) ~ 5 MHz (the block tops out around 7-8
 * MHz); ctrl 0x2019d821 is the frame/delay setup. Copied verbatim — this is
 * the same sequence the SPL already ran against the PMU.
 */
#define RDA_ISPI_CFG		0x00130003
#define RDA_ISPI_CTRL		0x2019d821

/* Bound every handshake. 10k iterations is ~1000x the worst case for a 5 MHz
 * 32-bit frame and still returns instantly on a dead port. */
#define RDA_ISPI_SPINS		10000

static void __iomem *rda_ispi_base = (void __iomem *)RDA_SPI3_BASE;

void rda_ispi_open(int port)
{
	rda_ispi_base = (void __iomem *)(port == RDA_ISPI_PORT_PMU ?
					 RDA_MODEM_SPI2_BASE : RDA_SPI3_BASE);

	/* Activate the ISPI. */
	writel(RDA_ISPI_CFG, rda_ispi_base + SPI_CFG);
	writel(RDA_ISPI_CTRL, rda_ispi_base + SPI_CTRL);

	/* No IRQ: everything here is polled. */
	writel(0, rda_ispi_base + SPI_IRQ);
}

static u32 rda_ispi_tx_space(void)
{
	return (readl(rda_ispi_base + SPI_STATUS) & SPI_TX_SPACE_MASK) >>
		SPI_TX_SPACE_SHIFT;
}

static u32 rda_ispi_rx_level(void)
{
	return (readl(rda_ispi_base + SPI_STATUS) & SPI_RX_LEVEL_MASK) >>
		SPI_RX_LEVEL_SHIFT;
}

/* The transfer is done once the FSM is idle *and* the TX FIFO has drained
 * back to its full size. */
static bool rda_ispi_tx_finished(void)
{
	u32 status = readl(rda_ispi_base + SPI_STATUS);

	return !(status & SPI_ACTIVE_STATUS) &&
	       ((status & SPI_TX_SPACE_MASK) >> SPI_TX_SPACE_SHIFT) ==
			SPI_TX_FIFO_SIZE;
}

/* Push one command word; returns false if the TX FIFO never freed up. */
static bool rda_ispi_send(u32 data, bool read)
{
	u32 reg = data & ~(SPI_CS_MASK | SPI_READ_ENA);
	int spins;

	/* CS0 plus the read-mode bit. */
	if (read)
		reg |= SPI_READ_ENA;

	for (spins = RDA_ISPI_SPINS; spins > 0; spins--) {
		if (rda_ispi_tx_space() > 0) {
			writel(reg, rda_ispi_base + SPI_RXTX_BUFFER);
			break;
		}
	}
	if (!spins)
		return false;

	for (spins = RDA_ISPI_SPINS; spins > 0; spins--)
		if (rda_ispi_tx_finished())
			return true;

	return false;
}

void rda_ispi_write(u32 reg, u32 val)
{
	u32 cmd = (0 << 25) | ((reg & 0x1ff) << 16) | (val & 0xffff);

	if (!rda_ispi_send(cmd, false))
		printf("rda_ispi: write 0x%03x timed out\n", reg);
}

u32 rda_ispi_read(u32 reg)
{
	u32 cmd = (1 << 25) | ((reg & 0x1ff) << 16);

	if (!rda_ispi_send(cmd, true)) {
		printf("rda_ispi: read 0x%03x timed out\n", reg);
		return 0xffffffff;
	}

	if (rda_ispi_rx_level() < 1)
		return 0xffffffff;

	return readl(rda_ispi_base + SPI_RXTX_BUFFER) & 0xffff;
}

void rda_pmu_write(u32 reg, u32 val)
{
	rda_ispi_open(RDA_ISPI_PORT_PMU);
	rda_ispi_write(reg, val);
	rda_ispi_open(RDA_ISPI_PORT_ABB);
}

u32 rda_pmu_read(u32 reg)
{
	u32 val;

	rda_ispi_open(RDA_ISPI_PORT_PMU);
	val = rda_ispi_read(reg);
	rda_ispi_open(RDA_ISPI_PORT_ABB);

	return val;
}

u32 rda_pmu_update(u32 reg, u32 clr, u32 set)
{
	u32 val;

	rda_ispi_open(RDA_ISPI_PORT_PMU);
	val = rda_ispi_read(reg);
	if (val != 0xffffffff) {
		val = (val & ~clr) | set;
		rda_ispi_write(reg, val);
	}
	rda_ispi_open(RDA_ISPI_PORT_ABB);

	return val;
}
