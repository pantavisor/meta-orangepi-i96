// SPDX-License-Identifier: GPL-2.0+
/*
 * RDA8810PL SoC glue for the modern u-boot stage-2.
 * DDR and clocks are already configured by the vendor SPL, so there is very
 * little to do here. Cache is enabled to speed up image copies.
 */
#include <common.h>
#include <cpu_func.h>
#include <debug_uart.h>
#include <stdio.h>
#include <asm/io.h>

int arch_cpu_init(void)
{
#ifdef CONFIG_DEBUG_UART
	/* Earliest sign of life: with DEBUG_UART_ANNOUNCE this emits
	 * "<debug_uart>" on uart3 before DM/console come up, proving stage-2
	 * actually executes after the vendor SPL hands off. */
	debug_uart_init();
#endif
	return 0;
}

/* Plain ARMv7 MMU/cache; no SoC-specific MMU table needed for stage-2. */
void enable_caches(void)
{
	dcache_enable();
}

int print_cpuinfo(void)
{
	puts("CPU:   RDA8810PL (Cortex-A5)\n");
	return 0;
}

/*
 * Whole-chip soft reset, from the always-on MD system controller. The AP can
 * reach it with the modem stopped, which is the only reason this works at
 * all -- the vendor resets this SoC from the modem coprocessor. Register
 * names are the vendor's (reg_md_sysctrl_rda8810.h); the kernel does the same
 * two writes from drivers/power/reset/rda8810pl-restart.c.
 */
#define RDA_MD_SYSCTRL_BASE		0x11a00000
#define RDA_SYSCTRL_REG_DBG		0x00
#define RDA_SYSCTRL_SYS_RST_SET		0x04
#define RDA_SYSCTRL_PROTECT_UNLOCK	0x00a50001
#define RDA_SYSCTRL_SOFT_RST		(1u << 31)

void reset_cpu(void)
{
	void __iomem *base = (void __iomem *)RDA_MD_SYSCTRL_BASE;

	writel(RDA_SYSCTRL_PROTECT_UNLOCK, base + RDA_SYSCTRL_REG_DBG);
	writel(RDA_SYSCTRL_SOFT_RST, base + RDA_SYSCTRL_SYS_RST_SET);

	while (1)
		;
}
