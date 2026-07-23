/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * OrangePi i96 — RDA599x WiFi/BT combo power bring-up. See rda_combo.c.
 */
#ifndef __BOARD_RDA8810PL_RDA_COMBO_H
#define __BOARD_RDA8810PL_RDA_COMBO_H

#include <linux/types.h>

/**
 * rda_combo_power_latch() - switch the combo chip's PMU rail on and leave it on
 * @verbose: print each PMU register transition
 */
#ifdef CONFIG_RDA_COMBO_POWER
void rda_combo_power_latch(bool verbose);
#else
static inline void rda_combo_power_latch(bool verbose) { }
#endif

#endif /* __BOARD_RDA8810PL_RDA_COMBO_H */
