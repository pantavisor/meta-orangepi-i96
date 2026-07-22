// SPDX-License-Identifier: GPL-2.0+
/*
 * RDA Micro RDA8810PL SD/MMC host — driver-model driver for modern u-boot.
 *
 * Command/response path + IFC-DMA data path ported from the vendor non-DM driver
 * (OrangePiLibra/OrangePi_i96_uboot drivers/mmc/rda_mmc.c + reg_mmc.h + ifc.c).
 *
 * The controller has NO PIO/FIFO data mode: all block data moves through the
 * separate "IFC" DMA engine (@ 0x20af0000). The IFC channel start_addr is only
 * 26 bits, so the DMA can address only the first 64 MB of DRAM
 * (0x80000000..0x84000000). We therefore always DMA into a fixed low-DRAM
 * bounce buffer inside that window and memcpy() to/from the caller's buffer,
 * which decouples the DMA range limit from any load address.
 */
#include <common.h>
#include <cpu_func.h>
#include <dm.h>
#include <mmc.h>
#include <errno.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/system.h>

/* SDMMC controller register offsets (HWP_SDMMC_T) */
#define RDA_APBI_CTRL		0x000
#define RDA_FIFO_TXRX		0x008		/* APBI data FIFO */
#define RDA_CONFIG		0x800
#define RDA_STATUS		0x804
#define RDA_CMD_INDEX		0x808
#define RDA_CMD_ARG		0x80c
#define RDA_RESP_ARG3		0x814
#define RDA_RESP_ARG2		0x818
#define RDA_RESP_ARG1		0x81c
#define RDA_RESP_ARG0		0x820
#define RDA_DATA_WIDTH		0x824
#define RDA_BLOCK_SIZE		0x828
#define RDA_BLOCK_CNT		0x82c
#define RDA_INT_STATUS		0x830
#define RDA_INT_MASK		0x834
#define RDA_INT_CLEAR		0x838
#define RDA_TRANS_SPEED		0x83c

/* CONFIG (0x800) bits */
#define CFG_SENDCMD		BIT(0)
#define CFG_RSP_EN		BIT(4)
#define CFG_RSP_SEL_R2		(2 << 5)
#define CFG_RSP_SEL_R3		(1 << 5)
#define CFG_RSP_SEL_OTHER	(0 << 5)
#define CFG_RD_WT_EN		BIT(8)
#define CFG_RD_WT_SEL_WRITE	BIT(9)		/* read = 0 */
#define CFG_S_M_SEL_MULTIPLE	BIT(10)

/* STATUS (0x804) bits */
#define ST_NOT_OVER		BIT(0)		/* command in progress while set */
#define ST_RSP_ERROR		BIT(8)		/* response CRC error */
#define ST_NO_RSP_ERROR		BIT(9)		/* no response received */
#define ST_DATA_ERROR_MASK	(0xffU << 16)	/* per-line read data CRC (0 = ok) */

/* INT_STATUS / INT_CLEAR (0x830 / 0x838) bits */
#define INT_DAT_OVER		BIT(4)		/* data transfer done */

/* apbi_ctrl (0x000): SOFT_RST_L active-LOW (bit SET = out of reset), L_ENDIAN(1)
 * = byte reorder for the DMA FIFO. 0x09 is the "running" value the vendor uses. */
#define RDA_APBI_SOFT_RST_L	BIT(3)
#define APBI_FIFO_INIT		(RDA_APBI_SOFT_RST_L | 0x1)	/* SOFT_RST_L | L_ENDIAN(1) */

#define RDA_CMD_TIMEOUT_US	100000
#define RDA_DATA_TIMEOUT_US	2000000

/* SD controller source clock: APB2 = BUS PLL (800 MHz, SPL "pll freq BUS") / 4. */
#define RDA_APB2_CLOCK		200000000

/* IFC DMA engine (HWP_SYS_IFC_T @ RDA_IFC_BASE). 7 standard channels of 16 B. */
#define RDA_IFC_BASE		0x20af0000
#define IFC_GET_CH		0x000		/* [3:0] = next free channel */
#define IFC_STD_CH(n)		(0x010 + (n) * 0x10)
#define IFC_CH_CONTROL		0x00
#define IFC_CH_STATUS		0x04
#define IFC_CH_START_ADDR	0x08
#define IFC_CH_TC		0x0c
#define IFC_STD_CHAN_NB		7
#define IFC_CTRL_ENABLE		BIT(0)
#define IFC_CTRL_DISABLE	BIT(1)
#define IFC_CTRL_RD_HW_EXCH	BIT(2)
#define IFC_CTRL_AUTODISABLE	BIT(4)
#define IFC_CTRL_SIZE_WORD	(2 << 5)
#define IFC_CTRL_REQ_SRC(id)	(((id) & 0x1f) << 8)
#define IFC_CTRL_FLUSH		BIT(16)
#define IFC_STAT_FIFO_EMPTY	BIT(4)
#define IFC_REQ_SDMMC_TX	10		/* SD (dev 0) */
#define IFC_REQ_SDMMC_RX	11

/* Bounce buffer in the first 64 MB of DRAM (the IFC's addressable window).
 * Sits at 0x81000000, below every load address (loadaddr/addr_fit = 0x82000000
 * and up per boot.scr), inside the 1 MB uncached MMU section set up in probe.
 * Only used transiently per transfer. */
#define RDA_DMA_BOUNCE		0x81000000UL
/* Multi-block DMA: a CMD18 read streams all blocks continuously through the one
 * APBI FIFO into a single IFC transfer (one tc, no per-block FIFO reset), so the
 * RD_HW_EXCH residual is a single 2-word tail at the very end — one tail-stitch
 * for the whole chunk. With b_max=1 an 11 MB image needs 22528 per-block
 * stitches (one mis-stitch => image CRC fail); 64 KB chunks cut that ~130x. */
#define RDA_MMC_B_MAX		128			/* 64 KB per transfer */
#define RDA_DMA_BOUNCE_SIZE	(RDA_MMC_B_MAX * 512)

/* RD_HW_EXCH completion protocol (hard-won bench lesson): tc==0 is DMA
 * *accounting*, not "all words in DRAM" — the final exchange-buffer words are
 * committed by the channel DISABLE write itself. So the vendor-exact release
 * (INT_CLEAR, DISABLE, tc=0, apbi 0x09) must run BEFORE the CPU touches the
 * buffer, and the success path must not FLUSH, poll FIFO_EMPTY, pop the APBI
 * FIFO, or pulse SOFT_RST_L — each of those perturbs the pipeline and strands
 * 1-2 tail words in the APBI FIFO, rotating every subsequent read by +8. The
 * abort path (and only it) uses FLUSH + FIFO_EMPTY drain, mirroring the
 * vendor's hal_data_transfer_stop(). */

struct rda_mmc_plat {
	struct mmc_config cfg;
	struct mmc mmc;
};

struct rda_mmc_priv {
	void __iomem *base;
};

/* SD clock divider: ceil(apb2 / (2 * clock)) - 1, capped at 255. */
static u32 rda_sd_clkdiv(u32 clock)
{
	u32 div;

	if (!clock)
		return 0xff;
	div = (RDA_APB2_CLOCK + 2 * clock - 1) / (2 * clock);
	if (div)
		div--;
	if (div > 255)
		div = 255;
	return div;
}

static int rda_wait_clear(void __iomem *base, u32 reg, u32 mask)
{
	u32 t = RDA_CMD_TIMEOUT_US;

	while ((readl(base + reg) & mask) && t--)
		udelay(1);
	return (readl(base + reg) & mask) ? -ETIMEDOUT : 0;
}

/* ---- IFC DMA engine helpers ---- */

static int rda_ifc_start(u8 req, ulong addr, u32 size)
{
	void __iomem *ifc = (void __iomem *)RDA_IFC_BASE;
	u8 ch = readl(ifc + IFC_GET_CH) & 0xf;

	if (ch >= IFC_STD_CHAN_NB)
		return -EBUSY;
	writel((u32)addr, ifc + IFC_STD_CH(ch) + IFC_CH_START_ADDR);
	writel(size, ifc + IFC_STD_CH(ch) + IFC_CH_TC);
	/* Proven-working control word (no AUTODISABLE: it does NOT flush the
	 * 2-word RD_HW_EXCH residual — bench-tested, sector 0 came back rotated).
	 * The residual is recovered by the tail-stitch in the completion path. */
	writel(IFC_CTRL_REQ_SRC(req) | IFC_CTRL_SIZE_WORD |
	       IFC_CTRL_RD_HW_EXCH | IFC_CTRL_ENABLE,
	       ifc + IFC_STD_CH(ch) + IFC_CH_CONTROL);
	return ch;
}

/* Success wait = the vendor's exact condition and NOTHING else: data-over from
 * the SDMMC and tc==0 on the channel. Finalization happens in the release (see
 * the RD_HW_EXCH comment above). */
static int rda_ifc_wait(void __iomem *base, int ch)
{
	void __iomem *ifc = (void __iomem *)RDA_IFC_BASE;
	u32 t = RDA_DATA_TIMEOUT_US;

	while (t--) {
		if ((readl(base + RDA_INT_STATUS) & INT_DAT_OVER) &&
		    readl(ifc + IFC_STD_CH(ch) + IFC_CH_TC) == 0)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void rda_ifc_release(void __iomem *base, int ch, u8 req)
{
	void __iomem *ifc = (void __iomem *)RDA_IFC_BASE;

	writel(INT_DAT_OVER, base + RDA_INT_CLEAR);
	writel(IFC_CTRL_REQ_SRC(req) | IFC_CTRL_RD_HW_EXCH | IFC_CTRL_DISABLE,
	       ifc + IFC_STD_CH(ch) + IFC_CH_CONTROL);
	/* Vendor does a status readback between DISABLE and tc=0 — acts as a
	 * fence that serializes the posted DISABLE write. Keep it. */
	readl(ifc + IFC_STD_CH(ch) + IFC_CH_STATUS);
	writel(0, ifc + IFC_STD_CH(ch) + IFC_CH_TC);
	writel(APBI_FIFO_INIT, base + RDA_APBI_CTRL);	/* SDMMC FIFO to run state */
}

/* Abort cleanup for failed/timed-out transfers, mirroring the vendor's
 * hal_data_transfer_stop(): zero the block regs, FLUSH the still-enabled
 * channel, drain until FIFO_EMPTY, then release. Every failed transfer MUST
 * run this so no stale word survives into the next arm (a single leftover
 * rotates every later read). */
static void rda_ifc_abort(void __iomem *base, int ch, u8 req)
{
	void __iomem *ifc = (void __iomem *)RDA_IFC_BASE;
	u32 t = 100000;

	writel(0, base + RDA_BLOCK_CNT);
	writel(0, base + RDA_BLOCK_SIZE);
	writel(APBI_FIFO_INIT, base + RDA_APBI_CTRL);
	if (!(readl(ifc + IFC_STD_CH(ch) + IFC_CH_STATUS) & IFC_STAT_FIFO_EMPTY)) {
		u32 c = readl(ifc + IFC_STD_CH(ch) + IFC_CH_CONTROL);

		writel(c | IFC_CTRL_FLUSH, ifc + IFC_STD_CH(ch) + IFC_CH_CONTROL);
	}
	while (t-- && !(readl(ifc + IFC_STD_CH(ch) + IFC_CH_STATUS) &
			IFC_STAT_FIFO_EMPTY))
		udelay(1);
	rda_ifc_release(base, ch, req);
}

static int rda_mmc_send_cmd(struct udevice *dev, struct mmc_cmd *cmd,
			    struct mmc_data *data)
{
	struct rda_mmc_priv *priv = dev_get_priv(dev);
	void __iomem *base = priv->base;
	u32 cfg = CFG_SENDCMD;
	ulong bounce = RDA_DMA_BOUNCE;
	u32 len = 0;
	u8 req = 0;
	int ch = -1;
	int ret;

	writel(0, base + RDA_CONFIG);

	if (cmd->resp_type & MMC_RSP_PRESENT) {
		cfg |= CFG_RSP_EN;
		if (cmd->resp_type & MMC_RSP_136)
			cfg |= CFG_RSP_SEL_R2;
		else if (cmd->resp_type & MMC_RSP_CRC)
			cfg |= CFG_RSP_SEL_OTHER;
		else
			cfg |= CFG_RSP_SEL_R3;
	}

	if (data) {
		u32 bs = data->blocksize, lenexp = 0;

		len = data->blocks * data->blocksize;
		if (len > RDA_DMA_BOUNCE_SIZE)		/* bounded by b_max */
			return -EINVAL;
		while (bs > 1) {			/* SDMMC wants log2(blocksize) */
			bs >>= 1;
			lenexp++;
		}

		writel(data->blocks, base + RDA_BLOCK_CNT);
		writel(lenexp, base + RDA_BLOCK_SIZE);
		writel(APBI_FIFO_INIT, base + RDA_APBI_CTRL);

		req = (data->flags & MMC_DATA_READ) ?
			IFC_REQ_SDMMC_RX : IFC_REQ_SDMMC_TX;
		/* Copy through the low-DRAM bounce window (IFC start_addr is 26-bit). */
		if (data->flags & MMC_DATA_WRITE)
			memcpy((void *)bounce, data->src, len);

		ch = rda_ifc_start(req, bounce, len);	/* arm DMA before the cmd */
		if (ch < 0)
			return ch;

		cfg |= CFG_RD_WT_EN;
		if (data->flags & MMC_DATA_WRITE)
			cfg |= CFG_RD_WT_SEL_WRITE;
		if (data->blocks > 1)
			cfg |= CFG_S_M_SEL_MULTIPLE;
	}

	writel(cmd->cmdidx & 0x3f, base + RDA_CMD_INDEX);
	writel(cmd->cmdarg, base + RDA_CMD_ARG);
	writel(cfg, base + RDA_CONFIG);			/* trigger command (+ data) */

	/* wait for the command to finish (NOT_OVER clears) */
	ret = rda_wait_clear(base, RDA_STATUS, ST_NOT_OVER);
	if (ret) {
		debug("rda_mmc: cmd%d timeout\n", cmd->cmdidx);
		goto out;
	}

	if (cmd->resp_type & MMC_RSP_PRESENT) {
		ret = rda_wait_clear(base, RDA_STATUS, ST_NO_RSP_ERROR);
		if (ret)
			goto out;
		if (readl(base + RDA_STATUS) & ST_RSP_ERROR) {
			ret = -EILSEQ;
			goto out;
		}
		if (cmd->resp_type & MMC_RSP_136) {
			cmd->response[0] = readl(base + RDA_RESP_ARG3);
			cmd->response[1] = readl(base + RDA_RESP_ARG2);
			cmd->response[2] = readl(base + RDA_RESP_ARG1);
			cmd->response[3] = readl(base + RDA_RESP_ARG0) << 1;
		} else {
			cmd->response[0] = readl(base + RDA_RESP_ARG3);
		}
	}

	if (!data)
		return 0;

	/* data phase: wait for the IFC DMA + SDMMC data-over */
	ret = rda_ifc_wait(base, ch);
	if (ret) {
		rda_ifc_abort(base, ch, req);
		return ret;
	}
	/* Release FIRST: the DISABLE write is the RD_HW_EXCH finalization that
	 * commits the last exchange-buffer words to DRAM. Only then may the CPU
	 * look at the bounce. */
	rda_ifc_release(base, ch, req);
	ch = -1;
	dsb();
	if (data->flags & MMC_DATA_READ) {
		if (readl(base + RDA_STATUS) & ST_DATA_ERROR_MASK)
			return -EILSEQ;
		memcpy(data->dest, (void *)bounce, len);
	}
	return 0;

out:
	if (ch >= 0)
		rda_ifc_abort(base, ch, req);
	return ret;
}

static int rda_mmc_set_ios(struct udevice *dev)
{
	struct rda_mmc_priv *priv = dev_get_priv(dev);
	struct mmc *mmc = mmc_get_mmc_dev(dev);
	void __iomem *base = priv->base;

	/* data bus width (raw 1/4/8 into SDMMC_DATA_WIDTH) */
	writel(mmc->bus_width == 4 ? 0x4 : 0x1, base + RDA_DATA_WIDTH);
	if (mmc->clock)
		writel(rda_sd_clkdiv(mmc->clock), base + RDA_TRANS_SPEED);
	return 0;
}

static int rda_mmc_get_cd(struct udevice *dev)
{
	return 1;	/* no reliable CD on the i96 SD slot; assume present */
}

static const struct dm_mmc_ops rda_mmc_ops = {
	.send_cmd = rda_mmc_send_cmd,
	.set_ios = rda_mmc_set_ios,
	.get_cd = rda_mmc_get_cd,
};

static int rda_mmc_probe(struct udevice *dev)
{
	struct rda_mmc_plat *plat = dev_get_plat(dev);
	struct rda_mmc_priv *priv = dev_get_priv(dev);
	struct mmc_uclass_priv *upriv = dev_get_uclass_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	/* Map the DMA bounce region uncached (1 MB MMU section) so there is no
	 * cache-coherency race between the IFC DMA writes and the CPU memcpy. */
	mmu_set_region_dcache_behaviour(RDA_DMA_BOUNCE, SZ_1M, DCACHE_OFF);

	/* Vendor-exact init: INT_MASK=0 and the identification clock, NOTHING
	 * else. Critically, do NOT pulse SOFT_RST_L: the vendor never asserts
	 * the APBI reset, and the SPL hands over a proven-good FIFO/handshake
	 * state (it just loaded this u-boot over the same path). Bench data
	 * says every reset pulse leaves the APBI in a state where reads run as
	 * a 2-word delay line (tail stuck in the FIFO, stream rotated +8).
	 * MCLK_ADJUST / apbi_ctrl are likewise inherited from the SPL. */
	writel(0, priv->base + RDA_INT_MASK);
	writel(rda_sd_clkdiv(400000), priv->base + RDA_TRANS_SPEED);

	plat->cfg.name = dev->name;
	plat->cfg.voltages = MMC_VDD_32_33 | MMC_VDD_33_34;
	plat->cfg.host_caps = MMC_MODE_4BIT;
	plat->cfg.f_min = 400000;
	/* Conservative SD clock: the exact APB2 source rate is uncertain (200 vs
	 * 240 MHz), so an over-fast divider silently corrupts large reads. Cap low
	 * for reliable data first; can be raised once the source clock is pinned. */
	plat->cfg.f_max = 6250000;
	plat->cfg.b_max = RDA_MMC_B_MAX;

	plat->mmc.priv = priv;
	upriv->mmc = &plat->mmc;
	return 0;
}

static int rda_mmc_bind(struct udevice *dev)
{
	struct rda_mmc_plat *plat = dev_get_plat(dev);

	return mmc_bind(dev, &plat->mmc, &plat->cfg);
}

static const struct udevice_id rda_mmc_ids[] = {
	{ .compatible = "rda,8810-mmc" },
	{ }
};

U_BOOT_DRIVER(rda_mmc) = {
	.name = "rda_mmc",
	.id = UCLASS_MMC,
	.of_match = rda_mmc_ids,
	.bind = rda_mmc_bind,
	.probe = rda_mmc_probe,
	.ops = &rda_mmc_ops,
	.priv_auto = sizeof(struct rda_mmc_priv),
	.plat_auto = sizeof(struct rda_mmc_plat),
};
