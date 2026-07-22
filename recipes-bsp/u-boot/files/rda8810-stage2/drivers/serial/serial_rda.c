// SPDX-License-Identifier: GPL-2.0+
/*
 * RDA Micro RDA8810PL UART — driver-model serial driver for modern u-boot.
 *
 * Hybrid forward-port (M2): this is the STAGE-2 u-boot loaded by the vendor SPL.
 * The SPL has already done DDR + clock init and configured the debug UART, so
 * setbrg is a no-op first cut (the DT uart_clk is a fixed 921600). Register map
 * mirrors the mainline Linux driver drivers/tty/serial/rda-uart.c.
 *
 * Console UART on the OrangePi i96 is uart3 @ 0x20a90000 (mainline DT
 * chosen/stdout-path = "serial2:921600n8", serial2 = &uart3). If there is no
 * output on the bench, try uart1 @ 0x20a00000 / uart2 @ 0x20a10000.
 */
#include <common.h>
#include <dm.h>
#include <debug_uart.h>
#include <errno.h>
#include <serial.h>
#include <asm/io.h>

#define RDA_UART_CTRL		0x00
#define RDA_UART_STATUS		0x04
#define RDA_UART_RXTX_BUFFER	0x08

#define RDA_UART_RX_FIFO_MASK	(0x7f << 0)	/* RX bytes available */
#define RDA_UART_TX_FIFO_MASK	(0x1f << 8)	/* free TX FIFO slots  */

struct rda_serial_priv {
	void __iomem *base;
};

static int rda_serial_putc(struct udevice *dev, const char ch)
{
	struct rda_serial_priv *priv = dev_get_priv(dev);

	/* No free TX slot -> tell the uclass to retry. */
	if (!(readl(priv->base + RDA_UART_STATUS) & RDA_UART_TX_FIFO_MASK))
		return -EAGAIN;

	writel((u8)ch, priv->base + RDA_UART_RXTX_BUFFER);
	return 0;
}

static int rda_serial_pending(struct udevice *dev, bool input)
{
	struct rda_serial_priv *priv = dev_get_priv(dev);
	u32 st = readl(priv->base + RDA_UART_STATUS);

	if (input)
		return (st & RDA_UART_RX_FIFO_MASK) ? 1 : 0;
	/* output "pending" == FIFO full (no free slots) */
	return (st & RDA_UART_TX_FIFO_MASK) ? 0 : 1;
}

static int rda_serial_getc(struct udevice *dev)
{
	struct rda_serial_priv *priv = dev_get_priv(dev);

	if (!(readl(priv->base + RDA_UART_STATUS) & RDA_UART_RX_FIFO_MASK))
		return -EAGAIN;

	return readl(priv->base + RDA_UART_RXTX_BUFFER) & 0xff;
}

static int rda_serial_setbrg(struct udevice *dev, int baudrate)
{
	/*
	 * SPL already programmed the divisor for the fixed 921600 console.
	 * TODO(hw): if a different baud is needed, program CTRL divisor here
	 * (RDA_UART_DIV_MODE BIT(20) + divisor from the UART source clock).
	 */
	return 0;
}

static int rda_serial_probe(struct udevice *dev)
{
	struct rda_serial_priv *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_ptr(dev);
	if (!priv->base)
		return -EINVAL;
	return 0;
}

static const struct dm_serial_ops rda_serial_ops = {
	.putc = rda_serial_putc,
	.pending = rda_serial_pending,
	.getc = rda_serial_getc,
	.setbrg = rda_serial_setbrg,
};

static const struct udevice_id rda_serial_ids[] = {
	{ .compatible = "rda,8810pl-uart" },
	{ }
};

U_BOOT_DRIVER(serial_rda) = {
	.name = "serial_rda",
	.id = UCLASS_SERIAL,
	.of_match = rda_serial_ids,
	.probe = rda_serial_probe,
	.priv_auto = sizeof(struct rda_serial_priv),
	.ops = &rda_serial_ops,
};

#ifdef CONFIG_DEBUG_UART_RDA
/*
 * Early debug UART: writes straight to uart3's FIFO from the first instruction
 * of u-boot (called from arch_cpu_init), bypassing driver-model. Used to prove
 * whether stage-2 executes at all after the vendor SPL jumps to it. The SPL has
 * already configured uart3, so init is a no-op.
 */
static inline void _debug_uart_init(void)
{
}

static inline void _debug_uart_putc(int ch)
{
	void __iomem *base = (void __iomem *)CONFIG_VAL(DEBUG_UART_BASE);

	while (!(readl(base + RDA_UART_STATUS) & RDA_UART_TX_FIFO_MASK))
		;
	writel((u8)ch, base + RDA_UART_RXTX_BUFFER);
}

DEBUG_UART_FUNCS
#endif
