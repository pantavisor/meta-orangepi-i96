// SPDX-License-Identifier: GPL-2.0+
/*
 * RDA Micro RDA8810PL SoC timer — driver-model timer for modern u-boot.
 *
 * The RDA8810PL Cortex-A5 has NO ARM generic timer, so u-boot must not use
 * CONFIG_SYS_ARCH_TIMER (reading its dead counter makes every udelay()/get_timer()
 * spin forever). Instead use the SoC's HWTIMER: a free-running 64-bit up-counter
 * at 2 MHz, always running (the mainline Linux clocksource just reads it without
 * starting it — drivers/clocksource/timer-rda.c). We only need get_count().
 *
 * Register map + rate from the mainline Linux driver (rda,8810pl-timer).
 */
#include <common.h>
#include <dm.h>
#include <timer.h>
#include <asm/io.h>

#define RDA_HWTIMER_LOCKVAL_L	0x024
#define RDA_HWTIMER_LOCKVAL_H	0x028

#define RDA_TIMER_RATE		2000000	/* 2 MHz free-running HWTIMER */

struct rda_timer_priv {
	void __iomem *base;
};

static u64 rda_timer_get_count(struct udevice *dev)
{
	struct rda_timer_priv *priv = dev_get_priv(dev);
	u32 lo, hi;

	/* 64-bit counter: read low first, then high, and re-read high until it
	 * is stable to guard against a rollover between the two reads. */
	do {
		lo = readl(priv->base + RDA_HWTIMER_LOCKVAL_L);
		hi = readl(priv->base + RDA_HWTIMER_LOCKVAL_H);
	} while (hi != readl(priv->base + RDA_HWTIMER_LOCKVAL_H));

	return ((u64)hi << 32) | lo;
}

static int rda_timer_probe(struct udevice *dev)
{
	struct timer_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	struct rda_timer_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;

	/* No clock-frequency in the mainline DT node; the HWTIMER runs at 2 MHz. */
	uc_priv->clock_rate = RDA_TIMER_RATE;

	return 0;
}

static const struct timer_ops rda_timer_ops = {
	.get_count = rda_timer_get_count,
};

static const struct udevice_id rda_timer_ids[] = {
	{ .compatible = "rda,8810pl-timer" },
	{ }
};

U_BOOT_DRIVER(rda_timer) = {
	.name = "rda_timer",
	.id = UCLASS_TIMER,
	.of_match = rda_timer_ids,
	.probe = rda_timer_probe,
	.ops = &rda_timer_ops,
	.priv_auto = sizeof(struct rda_timer_priv),
};
