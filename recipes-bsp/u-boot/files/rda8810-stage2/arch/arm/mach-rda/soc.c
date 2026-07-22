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

/* TODO(hw): reset via the RDA watchdog/reset register; stub for now. */
void reset_cpu(void)
{
	while (1)
		;
}
