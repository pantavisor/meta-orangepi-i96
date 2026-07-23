// SPDX-License-Identifier: GPL-2.0+
/*
 * RDA Micro RDA8810PL I2C master — driver-model driver for modern u-boot.
 *
 * Same byte-level engine as the Linux driver we forward-ported for 6.6
 * (recipes-kernel/linux/files/rda-mmc-15-i2c-add-rda8810pl-controller.patch):
 * one CMD register issues START/WRITE/READ/STOP/ACK phases and IRQ_STATUS is
 * polled for phase completion. The vendor never used the interrupt either.
 *
 * Why u-boot needs I2C at all: the RDA599x WiFi/BT combo chip's control
 * interface is on this bus, and turning that chip on is a PMU experiment that
 * has to be iterated (see board/rda/rda8810pl/rda_combo.c and
 * MODEM-WIFI-PORT.md). Being able to ask the chip "are you alive?" from the
 * u-boot prompt turns a multi-minute Linux boot per attempt into a sub-second
 * loop.
 *
 * There is no clock driver for this SoC in stage-2, so the APB1 rate that
 * feeds the prescaler is a constant here (the vendor u-boot hardcodes the same
 * 200 MHz in board/rda/common/i2c_test.c) and can be overridden per node with
 * "rda,apb-clock-hz" without a rebuild of anything but the DT.
 */
#include <common.h>
#include <dm.h>
#include <i2c.h>
#include <errno.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>

#define REG_CTRL		0x00
#define REG_STATUS		0x04
#define REG_TXRX_BUFFER		0x08
#define REG_CMD			0x0c
#define REG_IRQ_CLR		0x10

/* CTRL */
#define CTRL_EN			BIT(0)
#define CTRL_IRQ_MASK		BIT(8)
#define CTRL_PRESCALE(n)	(((n) & 0xffff) << 16)
#define CTRL_PRESCALE_MASK	(0xffffU << 16)

/* STATUS */
#define STATUS_IRQ_CAUSE	BIT(0)
#define STATUS_IRQ_STATUS	BIT(4)
#define STATUS_TIP		BIT(8)
#define STATUS_AL		BIT(12)
#define STATUS_BUSY		BIT(16)
#define STATUS_RXACK		BIT(20)

/* CMD */
#define CMD_ACK			BIT(0)
#define CMD_RD			BIT(4)
#define CMD_STO			BIT(8)
#define CMD_WR			BIT(12)
#define CMD_STA			BIT(16)

/* IRQ_CLR */
#define IRQ_CLR_BIT		BIT(0)

/* APB1 feeds the controller. The AP syscon divider that sets it is not modelled
 * in stage-2; 200 MHz is what the vendor u-boot assumes for the same block. */
#define RDA_I2C_DEFAULT_APB_HZ	200000000

#define RDA_I2C_TIMEOUT_US	100000
#define RDA_I2C_POLL_STEP_US	20

/*
 * RXACK is already valid by the time the phase-complete IRQ latches, so this
 * loop is pure belt-and-braces — but it is also the cost of every NAK, and
 * rdacombo's PMU sweep NAKs several hundred times in a row. 5 ms is ~500x an
 * ACK bit time at 100 kHz and keeps the sweep at a few seconds instead of a
 * minute.
 */
#define RDA_I2C_ACK_TIMEOUT_US	5000

struct rda_i2c_priv {
	void __iomem *base;
	u32 apb_hz;
	u32 speed;
};

/* Wait for the current phase to complete, then clear the latched IRQ. */
static int rda_i2c_wait_done(struct rda_i2c_priv *i2c)
{
	int us;

	for (us = 0; us < RDA_I2C_TIMEOUT_US; us += RDA_I2C_POLL_STEP_US) {
		if (readl(i2c->base + REG_STATUS) & STATUS_IRQ_STATUS)
			return 0;
		udelay(RDA_I2C_POLL_STEP_US);
	}

	return -ETIMEDOUT;
}

static int rda_i2c_stop(struct rda_i2c_priv *i2c)
{
	int ret;

	writel(CMD_STO, i2c->base + REG_CMD);
	ret = rda_i2c_wait_done(i2c);
	if (ret) {
		debug("rda_i2c: timeout waiting for STOP\n");
		return ret;
	}
	writel(IRQ_CLR_BIT, i2c->base + REG_IRQ_CLR);

	return 0;
}

static int rda_i2c_send_byte(struct rda_i2c_priv *i2c, u8 data, bool start,
			     bool stop)
{
	u32 cmd = CMD_WR;
	int ret, us;

	if (start)
		cmd |= CMD_STA;
	if (stop)
		cmd |= CMD_STO;

	writel(data, i2c->base + REG_TXRX_BUFFER);
	writel(cmd, i2c->base + REG_CMD);

	ret = rda_i2c_wait_done(i2c);
	if (ret) {
		writel(CMD_STO, i2c->base + REG_CMD);
		return ret;
	}
	writel(IRQ_CLR_BIT, i2c->base + REG_IRQ_CLR);

	/* RXACK clears once the slave has acknowledged. */
	for (us = 0; us < RDA_I2C_ACK_TIMEOUT_US; us += RDA_I2C_POLL_STEP_US) {
		if (!(readl(i2c->base + REG_STATUS) & STATUS_RXACK))
			return 0;
		udelay(RDA_I2C_POLL_STEP_US);
	}

	rda_i2c_stop(i2c);

	return -EREMOTEIO;	/* NAK */
}

static int rda_i2c_get_byte(struct rda_i2c_priv *i2c, u8 *data, bool stop)
{
	u32 cmd = CMD_RD;
	int ret;

	/* NAK the last byte of a read, then STOP. */
	if (stop)
		cmd |= CMD_ACK | CMD_STO;

	writel(cmd, i2c->base + REG_CMD);

	ret = rda_i2c_wait_done(i2c);
	if (ret) {
		writel(CMD_STO, i2c->base + REG_CMD);
		return ret;
	}
	writel(IRQ_CLR_BIT, i2c->base + REG_IRQ_CLR);

	*data = readl(i2c->base + REG_TXRX_BUFFER) & 0xff;

	return 0;
}

static int rda_i2c_xfer_msg(struct rda_i2c_priv *i2c, struct i2c_msg *msg,
			    bool stop)
{
	int i, ret;

	if (msg->len < 1)
		return -EINVAL;

	if (msg->flags & I2C_M_RD) {
		ret = rda_i2c_send_byte(i2c, (msg->addr << 1) | 0x01, true,
					false);
		if (ret)
			return ret;
		for (i = 0; i < msg->len; i++) {
			ret = rda_i2c_get_byte(i2c, &msg->buf[i],
					       stop && i == msg->len - 1);
			if (ret)
				return ret;
		}
	} else {
		ret = rda_i2c_send_byte(i2c, (msg->addr << 1) & 0xfe, true,
					false);
		if (ret)
			return ret;
		for (i = 0; i < msg->len; i++) {
			ret = rda_i2c_send_byte(i2c, msg->buf[i], false,
						stop && i == msg->len - 1);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int rda_i2c_xfer(struct udevice *bus, struct i2c_msg *msg, int nmsgs)
{
	struct rda_i2c_priv *i2c = dev_get_priv(bus);
	int i, ret;

	for (i = 0; i < nmsgs; i++) {
		ret = rda_i2c_xfer_msg(i2c, &msg[i], i == nmsgs - 1);
		if (ret) {
			rda_i2c_stop(i2c);
			return ret;
		}
	}

	return 0;
}

/* Address-only transfer: START + address byte + STOP, ACK decides. On a NAK or
 * a timeout rda_i2c_send_byte() has already issued the STOP. */
static int rda_i2c_probe_chip(struct udevice *bus, uint chip, uint chip_flags)
{
	struct rda_i2c_priv *i2c = dev_get_priv(bus);

	return rda_i2c_send_byte(i2c, (chip << 1) & 0xfe, true, true);
}

static int rda_i2c_set_bus_speed(struct udevice *bus, uint speed)
{
	struct rda_i2c_priv *i2c = dev_get_priv(bus);
	u32 div, ctrl;

	if (!speed)
		return -EINVAL;

	/* Vendor formula: prescale = ceil(apb / (5 * speed)) - 1 */
	div = i2c->apb_hz / (5 * speed);
	if (i2c->apb_hz % (5 * speed))
		div++;
	if (div)
		div--;
	if (div > 0xffff)
		div = 0xffff;

	/* The prescaler only latches out of reset, so drop EN across the write
	 * (this is the vendor i2c_config_clock sequence). */
	writel(0, i2c->base + REG_CTRL);
	ctrl = CTRL_EN | CTRL_PRESCALE(div);
	writel(ctrl, i2c->base + REG_CTRL);

	i2c->speed = speed;
	debug("rda_i2c: apb %u Hz, bus %u Hz, prescale %u\n", i2c->apb_hz,
	      speed, div);

	return 0;
}

static int rda_i2c_of_to_plat(struct udevice *bus)
{
	struct rda_i2c_priv *i2c = dev_get_priv(bus);

	i2c->base = dev_read_addr_ptr(bus);
	if (!i2c->base)
		return -EINVAL;

	i2c->apb_hz = dev_read_u32_default(bus, "rda,apb-clock-hz",
					   RDA_I2C_DEFAULT_APB_HZ);
	i2c->speed = dev_read_u32_default(bus, "clock-frequency",
					  I2C_SPEED_STANDARD_RATE);

	return 0;
}

static int rda_i2c_probe(struct udevice *bus)
{
	struct rda_i2c_priv *i2c = dev_get_priv(bus);

	return rda_i2c_set_bus_speed(bus, i2c->speed);
}

static const struct dm_i2c_ops rda_i2c_ops = {
	.xfer		= rda_i2c_xfer,
	.probe_chip	= rda_i2c_probe_chip,
	.set_bus_speed	= rda_i2c_set_bus_speed,
};

static const struct udevice_id rda_i2c_ids[] = {
	{ .compatible = "rda,8810pl-i2c" },
	{ }
};

U_BOOT_DRIVER(rda_i2c) = {
	.name		= "rda_i2c",
	.id		= UCLASS_I2C,
	.of_match	= rda_i2c_ids,
	.of_to_plat	= rda_i2c_of_to_plat,
	.probe		= rda_i2c_probe,
	.priv_auto	= sizeof(struct rda_i2c_priv),
	.ops		= &rda_i2c_ops,
};
