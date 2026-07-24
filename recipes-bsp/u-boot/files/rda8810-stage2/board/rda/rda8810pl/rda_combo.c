// SPDX-License-Identifier: GPL-2.0+
/*
 * OrangePi i96 — RDA599x WiFi/BT combo power bring-up from u-boot.
 *
 * This is the implementation of strategy A' in
 * recipes-kernel/linux/files/MODEM-WIFI-PORT.md.
 *
 * The problem: the RDA5991_G combo chip NAKs every I2C transfer under our
 * modem-less pantavisor image, because its supply (the PMU "v_bt" LDO) and its
 * low-frequency clocks are switched by the RDA8810PL *modem* coprocessor, which
 * we never boot. On the vendor Debian image the chip answers I2C at t=1.2 s —
 * right after the modem's msys layer registers the regulators and before any
 * wifi_power_on runs — so all that is missing is "something turned the LDO on".
 *
 * The bet: the PMU is not actually modem-only hardware. It hangs off ISPI port
 * 1 (modem SPI2 @ 0x11a14000) and the vendor *SPL* — which is the SPL we boot —
 * already programs it directly over that port (board/rda/rda8810/clock.c
 * pmu_setup_init). With no modem running, nothing contends for it, so stage-2
 * can set the LDO enable and simply leave it latched: Linux then never needs
 * mdcom/msys at all, and the already-working rda-i2c + rda_combo + SDIO stack
 * takes over.
 *
 * The gap: which PMU register and bit is v_bt. RDA never published a PMU map
 * and the AP-side sources only ever name LDOs by an opaque msys "pm_id"
 * (v_bt = 10). The one leak is the vendor u-boot's board/rda/common/i2c_test.c
 * touch_sensor_power_init(), which enables an LDO with:
 *
 *	0x07 bit13 : clear = "select vol > 2V"  (set = < 2V)
 *	0x28 bit13 : enable in normal mode
 *	0x29 bit13 : enable in low-power mode
 *
 * i.e. three PMU registers indexed by *the same bit position*, and the LDO it
 * powers (the touch sensor's, on I2C) is v_i2c, whose msys pm_id is 13. So the
 * working hypothesis is: bit position == pm_id, and v_bt is bit 10 of the same
 * three registers, at the < 2V range (the vendor log reports v_bt at 1800 mV).
 *
 * THAT WHOLE PREMISE WAS WRONG, and the bench settled it in one command.
 *
 * The chip was powered the entire time. I2C1's SCL/SDA pads come out of reset
 * muxed to the GPIO block, so the controller clocked address bytes into
 * unconnected pins and every transfer NAKed -- which, from the AP side, is
 * indistinguishable from a chip with no supply. Clearing two bits in
 * AP_GPIO_B_Mode makes it answer instantly:
 *
 *	=> mw.l 0x11a09010 0x3fffffff
 *	=> i2c probe
 *	Valid chip addresses: 14 16
 *	=> rdacombo id
 *	rdacombo: project_id 0x5991 chip_version 0x0047  (RDA5991_G)
 *
 * matching the vendor Debian log's "read project_id:5991 version:47" exactly.
 * No modem, no mdcom/msys, no PMU write. See rda_combo_pinmux() below.
 *
 * The lesson worth keeping: an earlier "rdacombo scan" swept ~500 live PMU bits
 * and reported no hit. That negative was worthless, because the detection path
 * itself was broken -- a search with no positive control cannot produce a valid
 * negative. Confirm the bus works before believing a scan.
 *
 * What remains here:
 *
 *	rdapmu   read/write/dump   raw PMU access over ISPI. Still the only
 *	                           window into an undocumented PMU, and it
 *	                           validated against the vendor SPL's writes
 *	                           register for register, so the map is sound.
 *	rdacombo id                ask the chip for project_id/chip_version
 *	rdacombo on                apply the fix, then ask
 *	rdacombo scan [first last] brute-force a PMU enable bit -- kept for the
 *	                           stage-3/4 RF and BT rails, which may still
 *	                           need one
 */
#include <common.h>
#include <command.h>
#include <console.h>
#include <dm.h>
#include <i2c.h>
#include <errno.h>
#include <asm/arch/rda_ispi.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <vsprintf.h>
#include "rda_combo.h"

/* Combo control interface, from the vendor board files (and our kernel DT,
 * rda-mmc-18-arm-dts-orangepi-i96-add-combo-clients.patch). */
#define RDA_COMBO_I2C_BUS	0
#define RDA_WIFI_CORE_ADDR	0x13
#define RDA_WIFI_RF_ADDR	0x14	/* the one that reports the chip id */
#define RDA_BT_CORE_ADDR	0x15
#define RDA_BT_RF_ADDR		0x16

/* wifi_rf register map used by the vendor's wlan_read_version_from_chip() */
#define RF_REG_PAGE		0x3f
#define RF_PAGE_ID		0x0001
#define RF_PAGE_NORMAL		0x0000
#define RF_REG_PROJECT_ID	0x20
#define RF_REG_CHIP_VERSION	0x21

/*
 * PMU registers the vendor SPL programs for the core/DDR/charger rails. Writing
 * these can brown the board out, so the scan refuses to touch them and stops at
 * 0x40 (everything above is DDR timing / PLL / efuse territory).
 */
#define RDA_PMU_SCAN_LAST	0x3f
static const u8 rda_pmu_denylist[] = {
	0x03,	/* vibrator + misc enables */
	0x05,	/* touched around the vcore change */
	0x0d,	/* DDR PWM mode */
	0x0f,	/* bandgap chopper */
	0x12,	/* AC throttling */
	0x13,	/* charger current */
	0x2a,	/* vBuck3 (DDR) / vBuck4 */
	0x2d,	/* vcore DCDC frequency */
	0x2e,	/* vcore DCDC frequency */
	0x2f,	/* vBuck1 = vcore */
	0x36,	/* buck low-voltage range select */
};

static bool rda_pmu_reg_denied(u32 reg)
{
	int i;

	if (reg > RDA_PMU_SCAN_LAST)
		return true;

	for (i = 0; i < ARRAY_SIZE(rda_pmu_denylist); i++)
		if (rda_pmu_denylist[i] == reg)
			return true;

	return false;
}

/*
 * THE FIX (confirmed on hardware 2026-07-22).
 *
 * The chip was never unpowered. I2C1's SCL/SDA pads come out of reset muxed to
 * the GPIO block, so the controller was clocking address bytes into nothing and
 * every transfer NAKed -- indistinguishable, from the AP side, from a dead chip.
 *
 * Vendor board file tgt_gpio_setting.h:
 *      #define AS_ALT_FUNC 0            // 0 = alternate function
 *      #define AS_GPIO     1            // 1 = plain GPIO
 *      // GPIO(30) // I2C1_SCL:nil-nil-nil
 *      #define TGT_AP_HAL_GPIO_B_30_USED AS_ALT_FUNC
 *      // GPIO(31) // I2C1_SDA:nil-nil-nil
 *      #define TGT_AP_HAL_GPIO_B_31_USED AS_ALT_FUNC
 *
 * AP_GPIO_B_Mode reads 0xffffffff on our boot; clearing bits 30/31 makes the
 * combo chip answer immediately with project_id 0x5991, chip_version 0x47 -- the
 * exact values the vendor Debian image reports.
 *
 * The same polarity is proven by something that already works: the SD card is on
 * GPIO_C 9..14 and BB_GPIO_Mode has those bits *clear*, which is why u-boot can
 * load a kernel at all.
 *
 * Note this also explains the original Linux failure: our 6.6 rda-i2c driver has
 * no pinctrl and mainline has no RDA pinctrl driver, so /dev/i2c-0 was equally
 * disconnected there. Setting the mux here persists into Linux.
 */
#define RDA_CFG_REGS_BASE	0x11a09000
#define CFG_REGS_BB_GPIO_MODE	0x08
#define CFG_REGS_AP_GPIO_A_MODE	0x0c
#define CFG_REGS_AP_GPIO_B_MODE	0x10
#define AP_GPIO_B_I2C1_SCL	BIT(30)
#define AP_GPIO_B_I2C1_SDA	BIT(31)

/*
 * The SDIO data path needs far more than the SDMMC2 pads themselves: bench
 * bisection on 2026-07-24 (register diff against the running vendor system,
 * see recipes-kernel/linux/files/MODEM-WIFI-PORT.md) showed the combo chip
 * stays electrically silent until AP_GPIO_A_Mode and AP_GPIO_B_Mode carry the
 * vendor kernel's full mux, not just the two I2C1 bits.
 *
 * With only the I2C1/SDMMC2 bits cleared, every SDIO command (CMD52/8/5/55/1)
 * returns NO_RSP. Writing the vendor's values makes CMD5 answer and the card
 * enumerate as "mmc1: new SDIO card at address 4829" -- the same address the
 * vendor Debian reports. Restoring either register to our old value breaks it
 * again, in two distinct ways: AP_GPIO_B back to 0x3fffffff kills CMD5
 * outright; AP_GPIO_A back to 0xffffffff leaves CMD5 answering but fails the
 * SDIO init at -110, i.e. B carries the clock/command path and A the data.
 *
 * These are whole-register values, read from a vendor system with wlan0 up.
 * The per-pad meaning is undocumented; the vendor kernel programs them from
 * its own board files. Do not "simplify" them into single-bit clears without
 * re-running the bisection.
 */
#define AP_GPIO_A_MODE_VENDOR	0x000210fc
#define AP_GPIO_B_MODE_VENDOR	0x3f00033f

/*
 * The combo chip's SDIO data path (SDMMC2 @ 0x20a60000, "mmc@60000") has the
 * same problem: its pads come out of reset in GPIO mode. Same board file, the
 * modem-side bank this time -- GPIO_C 15..20 are the second SD interface's
 * CLK/CMD/D0-3, all AS_ALT_FUNC:
 *
 *	// GPIO(15) // SD CLK  ... // GPIO(20) // SD data3
 *	#define TGT_HAL_GPIO_C_15_USED AS_ALT_FUNC   (..._20_USED likewise)
 *
 * BB_GPIO_Mode reads 0x7fff81ff, i.e. bits 9..14 clear (SDMMC1 -- the boot card,
 * which is why we can load a kernel) but bits 15..20 set. Clear them here so
 * stage 3 does not repeat the I2C1 hunt: without this the SDIO host would just
 * report no card, indistinguishable from a chip that is not there.
 */
#define BB_GPIO_SDMMC2_PADS	(0x3f << 15)	/* CLK, CMD, D0..D3 */

/*
 * Low-frequency clocks the vendor combo driver enables via msys
 * (enable_32k_rtc -> RDA_CLK_OUT, enable_26m_rtc -> RDA_CLK_AUX). Plain MMIO in
 * the modem sysctrl block, reachable with no modem running. Cfg_Clk_Out is a
 * protected register and needs the REG_DBG unlock; Cfg_Clk_Auxclk is not.
 *
 * The chip answers I2C without these, so they are not needed for the id read.
 * They are kept because the RF blocks will want the reference once we get to
 * stage 3/4, and enabling them is harmless.
 */
#define RDA_MD_SYSCTRL_BASE	0x11a00000
#define MD_REG_DBG		0x00
#define MD_CFG_CLK_OUT		0x54
#define MD_CFG_CLK_AUXCLK	0x5c
#define MD_PROTECT_UNLOCK	0x00a50001
#define MD_PROTECT_LOCK		0x00a50000
#define MD_AUXCLK_EN		BIT(0)
#define MD_CLKOUT_SEL_DIVIDER	(2 << 8)

/* ------------------------------------------------------------------ I2C --- */

static int rda_combo_bus(struct udevice **bus)
{
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_I2C, RDA_COMBO_I2C_BUS, bus);
	if (ret)
		printf("rdacombo: no I2C bus %d (%d)\n", RDA_COMBO_I2C_BUS,
		       ret);

	return ret;
}

/* Cheapest possible "is the chip alive": one address byte, ACK or not. */
static int rda_combo_ping(void)
{
	struct udevice *bus, *dev;
	int ret;

	ret = rda_combo_bus(&bus);
	if (ret)
		return ret;

	return dm_i2c_probe(bus, RDA_WIFI_RF_ADDR, 0, &dev);
}

/* Vendor i2c_write_1_addr_2_data(): one 3-byte write, big-endian data. */
static int rda_combo_rf_write(struct udevice *dev, u8 reg, u16 val)
{
	u8 buf[3] = { reg, val >> 8, val & 0xff };
	struct i2c_msg msg = {
		.addr = RDA_WIFI_RF_ADDR,
		.flags = 0,
		.len = sizeof(buf),
		.buf = buf,
	};

	return dm_i2c_xfer(dev, &msg, 1);
}

/*
 * Vendor i2c_read_1_addr_2_data(): a 1-byte write *terminated by STOP*, then a
 * separate 2-byte read. Deliberately two transfers rather than one repeated-
 * start pair, because that is what the vendor driver does and the chip's
 * autoincrement behaviour on a repeated start is untested.
 */
static int rda_combo_rf_read(struct udevice *dev, u8 reg, u16 *val)
{
	u8 buf[2];
	struct i2c_msg wr = {
		.addr = RDA_WIFI_RF_ADDR,
		.flags = 0,
		.len = 1,
		.buf = &reg,
	};
	struct i2c_msg rd = {
		.addr = RDA_WIFI_RF_ADDR,
		.flags = I2C_M_RD,
		.len = sizeof(buf),
		.buf = buf,
	};
	int ret;

	ret = dm_i2c_xfer(dev, &wr, 1);
	if (ret)
		return ret;

	ret = dm_i2c_xfer(dev, &rd, 1);
	if (ret)
		return ret;

	*val = (buf[0] << 8) | buf[1];

	return 0;
}

/**
 * rda_combo_read_id() - read project_id / chip_version off the combo chip
 *
 * Same sequence as the vendor's wlan_read_version_from_chip(): switch the RF
 * block to the id page, read 0x21 then 0x20, switch back. A working RDA5991_G
 * answers project_id 0x5991, chip_version 0x47.
 */
static int rda_combo_read_id(u16 *project_id, u16 *chip_version)
{
	struct udevice *bus, *dev;
	int ret;

	ret = rda_combo_bus(&bus);
	if (ret)
		return ret;

	ret = i2c_get_chip(bus, RDA_WIFI_RF_ADDR, 0, &dev);
	if (ret)
		return ret;

	ret = rda_combo_rf_write(dev, RF_REG_PAGE, RF_PAGE_ID);
	if (ret)
		return ret;

	ret = rda_combo_rf_read(dev, RF_REG_CHIP_VERSION, chip_version);
	if (!ret)
		ret = rda_combo_rf_read(dev, RF_REG_PROJECT_ID, project_id);

	/* Always leave the RF block back on the normal page. */
	rda_combo_rf_write(dev, RF_REG_PAGE, RF_PAGE_NORMAL);

	return ret;
}

static void rda_combo_report_id(void)
{
	u16 project_id = 0, chip_version = 0;
	int ret;

	ret = rda_combo_read_id(&project_id, &chip_version);
	if (ret) {
		printf("rdacombo: chip @0x%02x does not answer (%d) - still unpowered\n",
		       RDA_WIFI_RF_ADDR, ret);
		return;
	}

	printf("rdacombo: project_id 0x%04x chip_version 0x%04x%s\n",
	       project_id, chip_version,
	       (project_id == 0x5991 && chip_version == 0x47) ?
			"  (RDA5991_G - expected part)" : "");
}

/* ------------------------------------------------------------- the latch --- */

/* Clear one pad-mux register's bits (0 = alternate function, 1 = GPIO). */
static void rda_pinmux_to_alt(u32 off, u32 pads, const char *name, bool verbose)
{
	void __iomem *reg = (void __iomem *)(RDA_CFG_REGS_BASE + off);
	u32 before = readl(reg);
	u32 want = before & ~pads;
	u32 got;

	writel(want, reg);
	got = readl(reg);

	if (verbose || got != want)
		printf("rdacombo: %s 0x%08x -> 0x%08x readback 0x%08x%s\n",
		       name, before, want, got,
		       got == want ? "" : "  *** WRITE REJECTED ***");
}

/* Write a pad-mux register outright (used for the vendor's whole-register map). */
static void rda_pinmux_set(u32 off, u32 want, const char *name, bool verbose)
{
	void __iomem *reg = (void __iomem *)(RDA_CFG_REGS_BASE + off);
	u32 before = readl(reg);
	u32 got;

	writel(want, reg);
	got = readl(reg);

	if (verbose || got != want)
		printf("rdacombo: %s 0x%08x -> 0x%08x readback 0x%08x%s\n",
		       name, before, want, got,
		       got == want ? "" : "  *** WRITE REJECTED ***");
}

/* Mux the combo chip's two buses away from the GPIO block. This is the fix. */
static void rda_combo_pinmux(bool verbose)
{
	/* I2C1 SCL/SDA -- control interface. Confirmed on hardware. */
	rda_pinmux_to_alt(CFG_REGS_AP_GPIO_B_MODE,
			  AP_GPIO_B_I2C1_SCL | AP_GPIO_B_I2C1_SDA,
			  "AP_GPIO_B_Mode (I2C1 SCL/SDA)", verbose);

	/* SDMMC2 CLK/CMD/D0-3 -- SDIO data path for stage 3. */
	rda_pinmux_to_alt(CFG_REGS_BB_GPIO_MODE, BB_GPIO_SDMMC2_PADS,
			  "BB_GPIO_Mode (SDMMC2)", verbose);

	/*
	 * The rest of the vendor pad map. Required for the combo chip's SDIO
	 * block to answer at all -- see the comment on AP_GPIO_A_MODE_VENDOR.
	 * Written whole rather than masked: these registers come out of reset
	 * all-GPIO and the vendor value is the known-good target.
	 */
	rda_pinmux_set(CFG_REGS_AP_GPIO_A_MODE, AP_GPIO_A_MODE_VENDOR,
		       "AP_GPIO_A_Mode (vendor map)", verbose);
	rda_pinmux_set(CFG_REGS_AP_GPIO_B_MODE, AP_GPIO_B_MODE_VENDOR,
		       "AP_GPIO_B_Mode (vendor map)", verbose);
}

/*
 * Enable the 32 kHz and 26 MHz low-frequency outputs the vendor combo driver
 * asks the modem for. Not required for the chip to answer I2C; done here so the
 * RF blocks have their reference for stage 3/4.
 */
static void rda_combo_clocks(bool verbose)
{
	void __iomem *base = (void __iomem *)RDA_MD_SYSCTRL_BASE;

	writel(MD_AUXCLK_EN, base + MD_CFG_CLK_AUXCLK);

	/* Cfg_Clk_Out is protected; REG_DBG gates writes to it. */
	writel(MD_PROTECT_UNLOCK, base + MD_REG_DBG);
	writel(MD_CLKOUT_SEL_DIVIDER, base + MD_CFG_CLK_OUT);
	writel(MD_PROTECT_LOCK, base + MD_REG_DBG);

	if (verbose)
		printf("rdacombo: Cfg_Clk_Auxclk 0x%08x, Cfg_Clk_Out 0x%08x\n",
		       readl(base + MD_CFG_CLK_AUXCLK),
		       readl(base + MD_CFG_CLK_OUT));
}

/**
 * rda_combo_power_latch() - bring the combo chip's control interface up
 * @verbose: print each register transition
 *
 * Nothing turns this back off: Linux inherits a working I2C1 and a clocked
 * combo chip, and never has to reach the modem.
 */
void rda_combo_power_latch(bool verbose)
{
	rda_combo_pinmux(verbose);
	rda_combo_clocks(verbose);

	/* The vendor's regulator bring-up settles well before its first I2C
	 * transfer; 2 ms is generous for an LDO ramp. */
	udelay(2000);
}

/* ---------------------------------------------------------------- rdapmu --- */

static int do_rdapmu_dump(u32 first, u32 count)
{
	u32 reg;

	for (reg = first; reg < first + count; reg++) {
		u32 val;

		if (ctrlc()) {
			puts("\nabort\n");
			return CMD_RET_FAILURE;
		}

		if ((reg % 8) == 0)
			printf("\n0x%03x:", reg);

		val = rda_ispi_read(reg);
		if (val == 0xffffffff)
			printf("  ----");
		else
			printf("  %04x", val);
	}
	puts("\n");

	return CMD_RET_SUCCESS;
}

static int do_rdapmu(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	u32 reg, val;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "dump")) {
		u32 first = (argc > 2) ? hextoul(argv[2], NULL) : 0;
		u32 count = (argc > 3) ? hextoul(argv[3], NULL) : 0x40;
		int ret;

		/* One open for the whole sweep: 128 port switches would take
		 * longer than the reads. */
		rda_ispi_open(RDA_ISPI_PORT_PMU);
		ret = do_rdapmu_dump(first, count);
		rda_ispi_open(RDA_ISPI_PORT_ABB);

		return ret;
	}

	if (!strcmp(argv[1], "read")) {
		if (argc != 3)
			return CMD_RET_USAGE;
		reg = hextoul(argv[2], NULL);
		val = rda_pmu_read(reg);
		if (val == 0xffffffff) {
			printf("pmu[0x%03x] read failed\n", reg);
			return CMD_RET_FAILURE;
		}
		printf("pmu[0x%03x] = 0x%04x\n", reg, val);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "write")) {
		if (argc != 4)
			return CMD_RET_USAGE;
		reg = hextoul(argv[2], NULL);
		val = hextoul(argv[3], NULL);
		if (rda_pmu_reg_denied(reg))
			printf("pmu[0x%03x]: WARNING, core/DDR/charger rail - writing anyway\n",
			       reg);
		rda_pmu_write(reg, val);
		printf("pmu[0x%03x] = 0x%04x (readback 0x%04x)\n", reg, val,
		       rda_pmu_read(reg));
		return CMD_RET_SUCCESS;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(rdapmu, 4, 0, do_rdapmu,
	   "raw RDA8810PL PMU access over ISPI",
	   "dump [first [count]]  - dump PMU registers (default 0x00, 0x40)\n"
	   "rdapmu read <reg>            - read one PMU register\n"
	   "rdapmu write <reg> <val>     - write one PMU register\n"
	   "\n"
	   "The PMU holds every v_* LDO and the low-frequency clock outputs. It\n"
	   "is reachable because no modem is running to contend for ISPI port 1.\n"
	   "Registers 0x03/0x05/0x0d/0x0f/0x12/0x13/0x2a/0x2d/0x2e/0x2f/0x36 are\n"
	   "the core, DDR and charger rails - writing them can brown out the board.");

/* -------------------------------------------------------------- rdacombo --- */

/*
 * The search. For every allowed PMU register, set one currently-clear bit,
 * ping the combo chip, and put the register straight back. The first bit that
 * makes the chip ACK is the answer.
 *
 * Only clear->set is tried: an LDO enable that is already 1 is not what is
 * keeping the chip dark.
 */
static int do_rdacombo_scan(u32 first, u32 last)
{
	struct udevice *bus;
	u32 reg;

	/* Resolve the bus once: the loop below pings hundreds of times and
	 * rda_combo_bus() complains on every failure. */
	if (rda_combo_bus(&bus))
		return CMD_RET_FAILURE;

	if (rda_combo_ping() == 0) {
		printf("rdacombo: chip already answers - nothing to search for\n");
		return CMD_RET_SUCCESS;
	}

	printf("rdacombo: scanning PMU 0x%02x..0x%02x (Ctrl-C aborts)\n",
	       first, last);

	for (reg = first; reg <= last; reg++) {
		u32 base, try;
		int bit;

		if (rda_pmu_reg_denied(reg))
			continue;

		base = rda_pmu_read(reg);
		if (base == 0xffffffff)
			continue;

		for (bit = 0; bit < 16; bit++) {
			if (base & BIT(bit))
				continue;

			if (ctrlc()) {
				puts("\nabort\n");
				return CMD_RET_FAILURE;
			}

			try = base | (u32)BIT(bit);
			rda_pmu_write(reg, try);

			/* A PMU that drops writes would let this loop run to
			 * completion and report "no bit found" -- a false
			 * negative that looks exactly like a real answer. Bail
			 * loudly on the first write that does not take. */
			if (rda_pmu_read(reg) != try) {
				printf("\nrdacombo: pmu[0x%02x] write rejected (wrote 0x%04x, read 0x%04x)\n",
				       reg, try, rda_pmu_read(reg));
				printf("rdacombo: PMU writes are not landing - a scan cannot conclude anything.\n");
				rda_pmu_write(reg, base);
				return CMD_RET_FAILURE;
			}

			udelay(2000);

			if (rda_combo_ping() == 0) {
				printf("\nrdacombo: HIT - pmu[0x%02x] bit %d (0x%04x -> 0x%04x)\n",
				       reg, bit, base, try);
				rda_combo_report_id();
				printf("rdacombo: left set; fold it into rda_combo_power_latch()\n");
				return CMD_RET_SUCCESS;
			}

			/* Put it back before moving on. */
			rda_pmu_write(reg, base);
		}
		printf(".");
	}

	printf("\nrdacombo: no single PMU bit woke the chip in 0x%02x..0x%02x\n",
	       first, last);
	printf("rdacombo: before believing that, check the bus is really connected -\n"
	       "          a scan with no positive control cannot produce a valid negative\n");

	return CMD_RET_FAILURE;
}

static int do_rdacombo(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "id")) {
		rda_combo_report_id();
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "on")) {
		rda_combo_power_latch(true);
		rda_combo_report_id();
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "scan")) {
		u32 first = (argc > 2) ? hextoul(argv[2], NULL) : 0x00;
		u32 last = (argc > 3) ? hextoul(argv[3], NULL) :
					RDA_PMU_SCAN_LAST;

		if (last > RDA_PMU_SCAN_LAST)
			last = RDA_PMU_SCAN_LAST;

		return do_rdacombo_scan(first, last);
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(rdacombo, 4, 0, do_rdacombo,
	   "RDA599x WiFi/BT combo bring-up (see MODEM-WIFI-PORT.md)",
	   "id                    - read project_id/chip_version over I2C\n"
	   "rdacombo on                    - mux I2C1 to its pads + enable 32k/26M, then read the id\n"
	   "rdacombo scan [first [last]]   - hunt a PMU enable bit (default 0x00..0x3f)\n"
	   "\n"
	   "A working RDA5991_G answers project_id 0x5991, chip_version 0x47.\n"
	   "board_init() already runs \"on\" when CONFIG_RDA_COMBO_POWER_AUTO is set.");
