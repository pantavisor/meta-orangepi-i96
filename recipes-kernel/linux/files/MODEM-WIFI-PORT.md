# Orange Pi i96 (RDA8810PL) — Modem / WiFi bring-up port plan

Status: **stages 2 and 3 DONE — both were pinmux. No modem needed. See §15.**

- **Stage 2 (I2C control) — DONE.** Two pinmux bits. See §10.
- **Stage 3 (SDIO data) — DONE (2026-07-24).** Also pinmux: the *five* pad
  registers in §15, not the two we had. On our own u-boot, with no modem
  anywhere, `mmc1: new SDIO card at address 4829` — the same address the
  vendor Debian reports. **The modem is NOT required for WiFi**, so
  `mdcom_loadm` never needs porting and `modem.bin`'s proprietary licensing
  is a non-issue. §11's "the modem gates the 26 MHz" conclusion was wrong.
- **Stage 4 (`rdawlan` cfg80211 port) — driver builds, see §16.** Not yet run on hardware.

Original (now partly superseded) status: **SOLVED — and it was not the modem.** The RDA5991_G answers I2C from
u-boot with `project_id 0x5991, chip_version 0x47`, matching the vendor image
exactly. The blocker was never power, the PMU, or the modem coprocessor: I2C1's
SCL/SDA pads come out of reset muxed to the GPIO block, so every transfer NAKed.
Two bits in `AP_GPIO_B_Mode` fix it. See [§10](#10-resolution-it-was-pinmux).

Sections 1–5 below are preserved as written *before* that was known. They are
wrong about the cause, and deliberately left in place: the reasoning that led to
the wrong answer is the most useful part of this document.

Sibling memories: `i96-wifi-bringup-state`, `rda8810-linux-mmc-backport`,
`i96-bsp-layer-extraction`.

---

## 1. The problem (confirmed on hardware, 2026-07-22)

The RDA5991_G WiFi/BT combo chip on the i96 does **not** answer I2C in our
pantavisor image (`rda_combo 0-0014: i2c write reg 0x3f failed: -6`). It is
unpowered/unclocked. Booting the **vendor Debian 3.10 image on the same board**
proves the chip works there, and pins the cause exactly:

```
[0.084] rda_md: mdcom initialized              ← modem comm channel up
[0.37 ] <rda_msys>: ... v_bt: ... at 1800 mV   ← modem-backed regulators registered
[1.227] read project_id:5991 version:47 wlan_version:6   ← chip ACKs I2C HERE
[4.02 ] power_on write pmu_setting succeed!!    ← only now do the I2C power tables run
```

The chip ACKs at 1.2 s — **before** any `wifi_power_on`, right after the modem
(`rda_md`/`rda_msys`) is up. The only thing different from our system at that
instant is the **modem coprocessor is running**.

**Root cause:** the RDA5991's supply (`v_bt`/`v_wifi` LDO) and its 26 MHz enable
are controlled by the **RDA8810PL modem coprocessor ("BB xcpu")**, which the AP
asks over the `mdcom` mailbox using the `msys` command protocol. Our modemless
image never starts that coprocessor, so nothing turns the chip on. No AP-side
`devmem` poke substitutes (verified: the AP clock/pad registers are already at
sane defaults; the PMU that holds the LDO is reached only via ISPI, which the
modem owns at runtime).

Chip identity is confirmed: **RDA5991_G** (`project_id 0x5991`, `chip_version
0x47`, `wlan_version 6`) — so the combo power driver must use the
`rda_5991g_*` tables (already included in patch 17).

---

## 2. Architecture

RDA8810PL is a phone SoC: a **Cortex-A5 "AP"** (what we run Linux on) plus a
**baseband "xcpu" modem coprocessor**, sharing DRAM, with a hardware mailbox
(`comreg0`) between them. The PMIC (all the `v_*` LDOs) and the low-frequency
clocks (26 MHz aux, 32 kHz) sit on the PMU, reached over an internal SPI (ISPI)
that the **modem** drives at runtime.

```
  AP (Linux)                         BB xcpu (modem firmware)          PMU
  ----------                         ------------------------          ---
  regulator_enable("v_bt") ─┐
  clk_enable(aux 26M)       ─┼─ msys cmd ─► mdcom mailbox ─► modem ─ISPI─► LDO/clk on
  rda-i2c / rda_combo       ─┘                                              │
  rda-mmc(SDIO) / rdawlan  ◄──────────── RDA5991 now powered+clocked ◄──────┘
```

- **mdcom** — mailbox + shared-memory IPC between AP and modem (`interface ver
  0x00010001`).
- **msys** — command protocol over mdcom. Modules seen in the vendor code:
  `SYS_PM_MOD` (regulators: `SYS_PM_CMD_EN`, `SYS_PM_CMD_SET_LEVEL`),
  `SYS_GEN_MOD` (clocks: `SYS_GEN_CMD_CLK_OUT`, `SYS_GEN_CMD_AUX_CLK`).
- The modem firmware itself is a ~2 MB blob loaded by **u-boot** into shared
  DRAM before Linux starts (the vendor u-boot prints `## Init mdcom channels …
  Done`).

---

## 3. Where the modem firmware lives

Found in the vendor SD image gap (mkimage-wrapped, before partition 1):

| Offset (SD) | mkimage name          | Size       | Purpose                        |
|-------------|-----------------------|------------|--------------------------------|
| `0x020000`  | `u-boot-spl`          | 43032      | vendor SPL                     |
| `0x032000`  | `U-Boot 2012.04…`     | 452104     | vendor u-boot                  |
| `0x284c00`  | `Modem raminit codes` | 2064       | modem RAM init stub            |
| `0x285450`  | **`Modem work codes`**| **2003744**| **the modem firmware (~2 MB)** |
| `0x288c00`  | `uInitrd`             | 3091255    | vendor initrd                  |

Extract with (image in scratchpad or re-copy from `~/Desktop/OrangePi_i96_…tar.gz`):
`dd if=<vendor.img> bs=1 skip=$((0x285450)) count=$((2003744+64)) of=modem-work.img`
(keeps the 64-byte mkimage header; `image_get_load`/`image_get_data` give the
modem load address and payload).

Calibration data ("RF: Not calibrated" in the log, yet WiFi still worked) lives
in a `factorydata` partition and is likely **optional** for basic WiFi — treat
as a later refinement, not a blocker.

**Licensing:** the modem blob is proprietary RDA firmware. Redistribution terms
are unknown — it must NOT be committed into meta-pantavisor/meta-orangepi-i96
without clearance. Plan for a fetch-from-vendor-image recipe or a user-supplied
blob, mirroring how other BSPs handle closed firmware.

---

## 4. Source inventory (vendor tree, for the port)

Repo: `OrangePiLibra/OrangePi_i96_kernel` (Linux 3.10) and
`OrangePiLibra/OrangePi_i96_uboot` (u-boot 2012.04).

### Kernel (3.10) — the modem stack
| File | Size | Role |
|------|------|------|
| `arch/arm/plat-rda/md.c` | 28774 | mdcom core driver (`rda_md`) |
| `arch/arm/plat-rda/md_sys.c` | 31486 | **msys** command layer (`rda_msys`) — sends SYS_PM/SYS_GEN |
| `arch/arm/plat-rda/modem_xcpu.c` | 5004 | modem coprocessor boot/reset |
| `arch/arm/plat-rda/comreg0_misc.c` | 7061 | comreg0 mailbox |
| `arch/arm/plat-rda/smd.c` | 4375 | shared-memory driver |
| `arch/arm/mach-rda/regulator.c` | 14532 | regulator_ops → `rda_msys_send_cmd(SYS_PM_CMD_EN)` |
| `arch/arm/plat-rda/ap_clk.c` | 37250 | `apsys_enable_aux_clk`/`clk_out` → `SYS_GEN_CMD_*` |
| `drivers/misc/rda/modemcore.c` | 11612 | modem core misc device |
| `drivers/misc/rda/rda_calib.c` | 17834 | RF/audio calibration loader |
| `drivers/tty/serial/rda_md_tty.c` | 12920 | modem AT/data TTY (not needed for WiFi) |
| headers | — | `plat/rda_md.h`, `plat/md_sys.h`, `plat/reg_comregs.h`, `plat/reg_md.h`, `plat/reg_modem_xcpu.h`, `mach/regulator.h` |

### u-boot (2012.04) — the modem loader
| File | Size | Role |
|------|------|------|
| `arch/arm/cpu/armv7/rda/mdcom.c` | 22439 | u-boot mdcom (channel init) |
| `common/cmd_mdcom.c` | 33373 | `rda_modem_image_load()`, calib save, syscmd handling |
| `board/rda/rda8810/clock.c` | 48603 | **direct ISPI→PMU access** (`ispi_open`, `pmu_reg_write`) |
| `arch/arm/cpu/armv7/rda/ispi.c` | 7096 | ISPI transport to the PMU |
| headers | — | `arch-rda/mdcom.h`, `arch-rda/defs_mdcom.h`, `arch-rda/reg_mdcom.h`, `arch-rda/factory.h`, `arch-rda/ispi.h` |

Key AP↔modem register bases (from the u-boot iomap): modem mailbox
`0x00200000`, modem sysctrl `0x11A00000`, AP sysctrl `0x20900000`.

---

## 5. Strategies (cheapest first — verify A′ before committing to B)

### A′. u-boot ISPI shortcut — *implemented, see §9*
**Hypothesis:** we own our u-boot completely and the vendor u-boot already
talks to the PMU **directly over ISPI** (no modem needed at boot). If we, in our
u-boot board init, use `ispi_open`/`pmu_reg_write` to (1) enable the `v_bt`/WiFi
LDO and (2) enable the 26 MHz aux clock, and simply **leave them on**, the
RDA5991 gets power+clock permanently. Linux never needs the modem: our
already-working `rda-i2c` + `rda_combo` + SDIO + (future) `rdawlan` take over,
and the Linux `regulator`/`msys` layer is replaced by "u-boot latched it on".
- **Risk:** the 26 MHz for the *combo chip* is its own crystal X900 gated by
  `XEN_IN` (net `BT_RF_CLKEN`) — need to confirm whether that gate is a PMU/ISPI
  bit or a SoC pad the modem drives. If it's a plain SoC pad, even easier (GPIO
  in u-boot). If it is only reachable through a modem msys command, A′ fails and
  we fall to A/B.
  - *Update:* re-reading `rda_combo_power_main.c` weakens this risk a lot.
    `rda_combo_power_ctrl_init()` calls `wlan_read_version_from_chip()` with
    **no** `regulator_enable()` and **no** `clk_prepare_enable()` — the chip
    ACKs at 1.2 s purely because something already switched it on. The only
    "something" in the vendor log is the msys regulator bring-up at 0.37 s
    (`v_bt … 1800 mV`). So the I2C-ACK milestone probably needs **only the
    supply**; the 26 MHz XEN question belongs to stage 3/4 (RF), not here.
- **First experiment:** port just the ISPI helpers + the vendor's `v_bt` LDO
  enable and `26M` enable into our u-boot `board_init`, boot our normal
  pantavisor image, check for `rda_combo … chip version 0x47`. — **done, plus an
  in-u-boot I2C path so the check no longer costs a Linux boot; see §9.**

### A. Load modem fw + minimal msys client — *medium, ~3–5 days*
Port only what is needed to (1) load the `Modem work codes` blob into reserved
shared DRAM and start the xcpu in our u-boot (`rda_modem_image_load` +
`mdcom.c`), and (2) a minimal Linux mdcom+msys client that sends the handful of
`SYS_PM_CMD_EN`(v_bt/v_wifi/v_sdmmc) and `SYS_GEN_CMD_*`(26M/32k) commands. Skip
the AT TTY, calibration, PM/DDR. Reserve the modem DRAM region in DT (like we
did for the DMA bounce/CMA).

### B. Full modem stack port — *large, multi-week*
Forward-port `md.c` + `md_sys.c` + `modem_xcpu.c` + `comreg0`/`smd` +
`regulator.c` + `ap_clk` low-freq clocks + `modemcore`/`rda_calib` to 6.6 as
proper DT drivers (ideally rpmsg/mailbox-framework based). Unlocks WiFi **and**
BT, FM, RF calibration, and modem-assisted PM. This is the "correct" answer and
the natural upstream story, but it is its own subsystem campaign on the scale of
the SD/MMC bring-up.

---

## 6. What's already done (AP side — runs the instant the chip powers up)
- `rda-i2c` controller (patches 15/16) — `/dev/i2c-0` verified on hardware.
- `rda_combo` power driver (patches 17/18) — 4 DT I2C clients @0x13/14/15/16,
  verbatim `rda_5991g` power tables, probes cleanly; only the chip ACK is missing.
- SDIO host: `mmc@60000` node exists in DT (disabled); board params 20 MHz,
  `mclk-inv`, `mclk-adj=3` (from `tgt_ap_board_config.h`). Enable + point the
  `rdawlan` driver at it in **stage 3**.
- Debug tooling: busybox `devmem` added (`recipes-core/busybox/`).

## 7. Remaining stages after the chip powers up
- **Stage 3:** enable SDIO on `mmc@60000`, confirm `mmc1: new SDIO card` +
  `Chipid: 0x6(RDA5991_G)` (exactly what the vendor log shows).
- **Stage 4:** port the `rdawlan` cfg80211 SDIO driver
  (`drivers/net/wireless/rdaw80211/rdawlan/`, ~180 KB `wland_cfg80211.c` + bus
  layer) to 6.6. This is the second-largest piece after the modem.

## 8. Definition of done (WiFi)
`ip link` shows a `wlan0`; `iw dev wlan0 scan` returns APs; a WPA2 association +
DHCP succeeds — matching the vendor log's `wland_cfg80211_up: dongle up`.

Vendor-system reference (confirms the stage-3/4 targets):
```
networkctl status wlan0
● 3: wlan0
            Type: wlan
            Path: platform-rda-mmc.1      ← SDIO on rda-mmc.1 == our mmc@60000 / SDMMC2
          Driver: rdawfmac               ← the rdawlan cfg80211 driver's netdev
      HW Address: 82:6b:41:3a:0a:48      (random — nvram has no MAC; see log)
```

---

## 9. What A′ is now (strategy A′ implementation)

All of this lives in the u-boot stage-2 port,
`recipes-bsp/u-boot/files/rda8810-stage2/`. It builds clean against u-boot
2024.01 with `arm-linux-gnueabi-gcc 13.2` in three configurations (commands
only, `RDA_COMBO_POWER_AUTO=y`, and `RDA_COMBO_POWER=n`), with no warnings.
**None of it has run on hardware yet.**

| File | What it is |
|------|-----------|
| `arch/arm/include/asm/arch-rda/rda_ispi.h` | ISPI API: `rda_ispi_open/read/write`, `rda_pmu_read/write/update` |
| `arch/arm/mach-rda/rda_ispi.c` | Vendor-exact ISPI transport (`CONFIG_RDA_ISPI`), all spin loops bounded |
| `drivers/i2c/rda_i2c.c` | DM I2C master (`CONFIG_RDA_I2C`), same engine as our Linux `rda-i2c` |
| `board/rda/rda8810pl/rda_combo.c` | `rdapmu` + `rdacombo` commands and the latch (`CONFIG_RDA_COMBO_POWER`) |
| DT | `i2c1/2/3` nodes in `rda8810pl.dtsi`; `i2c1` enabled with the four combo clients on the i96 |

### Why the PMU is reachable at all
`RDA_MODEM_SPI2_BASE = 0x11a14000` is inside the modem register window but is
AP-addressable, and the **vendor SPL we already boot** programs the PMU through
it (`board/rda/rda8810/clock.c pmu_setup_init` → `ispi_open(1)` →
`pmu_reg_write`). With no modem firmware running, nothing contends for the port,
so stage-2 (and, if it ever helps, Linux) can keep using it. The vendor Linux
`arch/arm/plat-rda/ispi.c` only ever opens port 0 (AP analog) — that is the
whole reason the kernel has to go through mdcom/msys.

### The one unknown, and how the search closes it
RDA never published a PMU register map; the AP sources name LDOs only by an
opaque msys `pm_id` (`v_bt` = 10, from `regulator-devices.c`). The single leak is
the vendor u-boot's `board/rda/common/i2c_test.c touch_sensor_power_init()`,
which switches an LDO on with:

```c
0x07 &= ~(1<<13);   /* select vol > 2V  (setting it selects < 2V) */
0x28 |=  (1<<13);   /* enable power in normal mode */
0x29 |=  (1<<13);   /* enable power in LP mode */
```

Three registers, **the same bit position** in each — and the LDO it powers is the
touch sensor's, i.e. `v_i2c`, whose `pm_id` is **13**. Hence the working
hypothesis encoded in `rda_combo_power_seq[]`: bit position == `pm_id`, so `v_bt`
is bit **10** of `0x07`/`0x28`/`0x29`, at the `< 2V` range (the vendor log reports
`v_bt` at 1800 mV). *This is a guess.* If it is wrong, `rdacombo scan` finds the
real bit by setting one currently-clear bit at a time across PMU `0x00..0x3f`,
pinging the chip, and restoring the register — skipping the core/DDR/charger
rails (`0x03 0x05 0x0d 0x0f 0x12 0x13 0x2a 0x2d 0x2e 0x2f 0x36`) that the SPL
programs and that would brown the board out.

### Bench procedure
At the u-boot prompt on the i96:

```
=> i2c dev 0
=> i2c probe                  # expect: nothing (chip is dark)
=> rdapmu dump 0x00 0x40      # baseline; save this
=> rdacombo id                # expect: "does not answer"
=> rdacombo on                # apply the hypothesis
=> rdacombo id                # HOPED FOR: project_id 0x5991 chip_version 0x0047
```

If `rdacombo on` does not wake it:

```
=> rdacombo scan              # sweeps 0x00..0x3f, prints "HIT — pmu[0xRR] bit N"
```

A hit is the answer: put that `(reg, bit)` into `rda_combo_power_seq[]`, turn on
`CONFIG_RDA_COMBO_POWER_AUTO` so `board_init()` latches it on every boot, and
Linux should then print `rda_combo … read project_id:5991 version:47` from the
already-merged kernel patches 15–18 with no kernel change at all.

If the scan comes up empty, the supply is not a single PMU bit and the next
moves are, in order: (1) diff a full `rdapmu dump` against one taken from the
vendor SPL prompt, (2) check whether the 26 MHz `XEN`/`BT_RF_CLKEN` gate really
is required for the I2C block (`md_sysctrl` `Cfg_Clk_Out` @ `0x11a00054` and
`Cfg_Clk_Auxclk` @ `0x11a0005c` are plain MMIO — poke them with `mw`), and only
then (3) fall back to strategy A.

### Deliberately not done
- No `md_sysctrl` clock command: those registers are ordinary MMIO, so `md`/`mw`
  already cover them (`CONFIG_CMD_MEMORY` is now on in the defconfig).
- `CONFIG_RDA_COMBO_POWER_AUTO` is **off**. Until the bench confirms the bit, a
  bootloader should not poke a guessed PMU register on every boot unattended.
- No clock driver: stage-2 has none, so `rda_i2c` takes the APB1 rate from a
  `rda,apb-clock-hz` DT property (200 MHz, the value the vendor u-boot hardcodes
  for the same block) instead of a `clocks` phandle.

---

## 10. RESOLUTION: it was pinmux

Confirmed on hardware 2026-07-22, at the u-boot prompt:

```
=> md.l 0x11a09010 1
11a09010: ffffffff              <- AP_GPIO_B_Mode: every pad in GPIO mode
=> i2c probe
Valid chip addresses:           <- nothing
=> mw.l 0x11a09010 0x3fffffff   <- clear bits 30/31 only
=> i2c probe
Valid chip addresses: 14 16
=> rdacombo id
rdacombo: project_id 0x5991 chip_version 0x0047  (RDA5991_G - expected part)
```

`0x5991 / 0x47` is byte-for-byte the vendor Debian log's
`read project_id:5991 version:47`. No modem, no mdcom, no msys, no PMU write.

### Why

Vendor board file `tgt_gpio_setting.h`:

```c
#define AS_ALT_FUNC 0        /* 0 = alternate function */
#define AS_GPIO     1        /* 1 = plain GPIO         */
// GPIO(30) // I2C1_SCL:nil-nil-nil
#define TGT_AP_HAL_GPIO_B_30_USED AS_ALT_FUNC
// GPIO(31) // I2C1_SDA:nil-nil-nil
#define TGT_AP_HAL_GPIO_B_31_USED AS_ALT_FUNC
```

I2C1 (bus 0, `_TGT_AP_I2C_BUS_ID_WIFI = 0` → `RDA_I2C1_PHYS 0x20950000`) is on
AP GPIO_B bits 30/31, and those must read **0** to reach the controller. Ours
read 1. The controller therefore clocked address bytes into unconnected pads and
saw no ACK — which, from the AP side, is *identical* to a chip with no supply.
Every symptom in §1 follows from that.

The polarity is independently proven by something that already worked: the SD
card is on GPIO_C 9..14 and `BB_GPIO_Mode` (`0x11a09008` = `0x7fff81ff`) has
those bits clear, which is why u-boot can load a kernel at all.

### Confirmed in Linux too

With `CONFIG_RDA_COMBO_POWER_AUTO=y` the mux is applied in `board_init()` and
survives the handoff, so the already-merged kernel patches 15-18 work unchanged:

```
[ 0.880000] rda_combo: RDA5991 chip version 0x47 (wlan_version 6)
[ 1.940000] rda_combo 0-0016: wifi powered on
[ 1.940000] rda-i2c 20950000.i2c: RDA I2C adapter at 0x20950000
```

`wlan_version 6` is `WLAN_VERSION_91_G`, matching the vendor image, and "wifi
powered on" means the full `rda_5991g` power-on tables ran over I2C -- not just
the id read. **Stage 2 is complete.** The original `-6` was this same bus.

### The fix, and where it lives

`board/rda/rda8810pl/rda_combo.c` `rda_combo_pinmux()`, called from
`board_init()` with `CONFIG_RDA_COMBO_POWER_AUTO=y` (now on by default). It
clears the two bits and leaves them cleared, so **Linux inherits a working
I2C1** — which matters, because our 6.6 `rda-i2c` driver has no pinctrl and
mainline has no RDA pinctrl driver at all. The original `-6` from `rda_combo` in
Linux was almost certainly this same disconnected bus, not a power problem.

`rda_combo_clocks()` additionally enables `Cfg_Clk_Auxclk` (26 MHz, `0x11a0005c`
bit 0) and `Cfg_Clk_Out` (32 kHz, `0x11a00054`, behind the `REG_DBG` protect
unlock `0xA50001`) — the two clocks the vendor combo driver requests via msys.
The chip answers I2C without them; they are enabled for the stage-3/4 RF path.

### What was wrong in §1–§5, and what was right

Wrong: the root-cause claim that the supply and 26 MHz enable are modem-owned
and unreachable from the AP. The chip's VBAT/VIO are always-on; nothing needed
switching.

Right, and still valuable:
- **The PMU is AP-reachable with no modem running.** `rdapmu dump` returned a
  map that matches the vendor SPL's `pmu_setup_init()` writes register for
  register (`0x03`=`9fff`, `0x0d`=`92d0`, `0x0f`=`1e90`, `0x12`=`1218`,
  `0x2a`=`aab5`, `0x2d`=`96ba`, `0x2e`=`12aa`, `0x2f`=`9444`, `0x36`=`6e54`),
  and writes land and read back. That is the first PMU map anyone has for this
  SoC, and strategies A/B never have to be attempted for WiFi.
- `CHIP_ID 0x8810001c` (metal id 28) cross-checks the PMU's metal-id-dependent
  branches.
- The u-boot I2C master, which is what made the answer findable in seconds.

### Methodological lesson (the expensive one)

`rdacombo scan` swept ~500 live PMU bits and reported "no bit woke the chip".
That negative was **worthless**: the detection path was itself broken, so no bit
could ever have registered as a hit. A search with no positive control cannot
produce a valid negative. The pinmux should have been verified *before* the
scan, not after it. `rdacombo scan`'s failure message now says so.

### Remaining work

- **Stage 3:** enable SDIO on `mmc@60000`, expect `mmc1: new SDIO card` +
  `Chipid: 0x6(RDA5991_G)`.
- **Stage 4:** port the `rdawlan` cfg80211 SDIO driver to 6.6.
- Addresses `0x13`/`0x15` (wifi_core / bt_core) stay quiet until the vendor
  `power_on` tables run — expected, not a fault.
- Consider a proper kernel-side pinmux (DT node or a small RDA pinctrl driver)
  so the mux does not depend on this bootloader.

---

## 11. Stage 3: the modem IS required (found 2026-07-22)

§10 concluded "no modem needed". That is true for the I2C control interface and
**wrong for SDIO**. The evidence came from mounting the official OrangePi i96
Debian SD card read-only and reading its `/boot/boot.cmd`:

```
setenv init_modem "yes"
ext2load mmc 0:1 ${modem_addr} modem.bin
if test "${init_modem}" = "yes"; then
        mdcom_loadm ${modem_addr}
        mdcom_check 1
fi
bootz ${kernel_addr} ${initrd_addr}
```

The vendor bootloader loads the modem firmware and starts the coprocessor before
Linux runs. Ours never does. `/boot/modem.bin` is exactly 2 MiB and holds the two
images §3 predicted:

| offset | size | load | entry | name |
|--------|------|------|-------|------|
| `0x000000` | 2064 | `0x01c16000` | `0x81c16000` | `Modem raminit codes` |
| `0x000850` | 2003744 | `0x02000800` | `0x82000800` | `Modem work codes` |

The vendor rootfs only auto-loads `rdawfmac` (`/etc/modules`); nothing there
starts the modem, so u-boot is the only place it happens.

### Why this explains stage 3 exactly

The RDA5991's I2C slave is a simple always-on register block — it answers with no
modem, which is why stage 2 works. Its **digital core**, which must respond to
CMD5, needs the 26 MHz reference gated by `CLK26M_REQUEST`/`XEN` — managed by the
modem through msys `SYS_GEN_CMD_AUX_CLK` / `SYS_PM`. With no modem the core is
unclocked: no SDIO response, while `DAT3_VAL` still reads high because the IO
ring is powered. Every measurement in the stage-3 investigation fits this.

Ruled out first, each by direct hardware read (see git history for detail): pad
mux (`BB_GPIO_Mode=0x7FE001FF`), SDMMC2 clock gate (`APB2=0x001FFFFF`, bit 17
set), low-frequency clocks (`Cfg_Clk_Out=0x200`, `Cfg_Clk_Auxclk=1`), `ocr_avail`
and all timing params (identical to vendor), `mclk-adj` 1 vs 3 (tested live via
u-boot `fdt set`, no change), controller clocking (`TRANS_SPEED=0x63` ≈ 1 MHz
init clock), settling time (re-probe 17 s after power-on), and completeness of the
combo power-on (all nine vendor steps report success).

### The work

Strategy A from §5: port `mdcom_loadm` + `mdcom_check` into our u-boot 2024.01
stage-2. Vendor sources: `common/cmd_mdcom.c` (33 KB) and
`arch/arm/cpu/armv7/rda/mdcom.c` (22 KB) from
`OrangePiLibra/OrangePi_i96_uboot@ac251146`. The DT must also reserve the modem
DRAM region so Linux does not use it.

**Licensing:** `modem.bin` is proprietary RDA firmware. It must NOT be committed
to meta-pantavisor or meta-orangepi-i96. Fetch it from the vendor image at build
time or require the user to supply it, as other BSPs do for closed firmware.

---

## 12. NEXT SESSION: START HERE

### State in one paragraph
Stage 2 (I2C control of the RDA5991_G) is **done and on hardware** — the chip
reports `project_id 0x5991 / chip_version 0x47` and the full nine-step vendor
power-on sequence succeeds. Stage 3 (SDIO data path) is **blocked**: every SDIO
command times out. The cause is *not* any of the nine things listed in §11, and
it is *not* simply "the modem isn't running" — that was tested directly and
disproved (below). The one remaining untested hypothesis is the **AP-side msys
client**.

### The single most important experiment already done
Booted the **vendor** SD card, used its u-boot's own `mdcom_loadm`/`mdcom_check`
to start the modem coprocessor, hand-applied our two pinmux writes, then booted
**our** 6.6 kernel from that card (via `CONFIG_ARM_APPENDED_DTB`, because vendor
u-boot 2012.04 has no `fdt` command). Everything came up — modem interface
version `0x00010001`, I2C alive, all nine power-on steps `succeed!!` — and
`mmc1` still failed identically.

**Conclusion: starting the modem is necessary-at-most, not sufficient.** Do NOT
spend days porting `mdcom_loadm` to our u-boot expecting it to fix WiFi.

### The next step
Strategy A had two halves. Half (1), starting the xcpu, is now known
insufficient. Half (2) is untested and is the leading hypothesis:

> a minimal Linux mdcom + **msys client** that sends `SYS_PM_CMD_EN` (v_bt /
> v_wifi) and `SYS_GEN_CMD_AUX_CLK` / `SYS_GEN_CMD_CLK_OUT` (26 MHz / 32 kHz)

The modem only switches those rails when the AP *asks* it over msys. Our
`rda_combo` port stubs exactly those calls (`enable_26m_rtc`,
`enable_26m_regulator`, `enable_32k_rtc`) out — see the patch-17 header. So the
combo chip's 26 MHz may still be off even with the modem running, which would
leave its digital core unclocked and its SDIO block mute while the always-on I2C
slave answers fine. That matches every observation.

IDs needed for the port (from `plat/md_sys.h`):

| symbol | value |
|--------|-------|
| `SYS_GEN_MOD` | `0x0` |
| `SYS_PM_MOD` | `0x2` |
| `SYS_GEN_CMD_CLK_OUT` | `0x1004` |
| `SYS_GEN_CMD_AUX_CLK` | `0x1005` |
| `SYS_PM_CMD_EN` | `0x1001` |

`v_bt` is msys `pm_id` 10 (`arch/arm/mach-rda/regulator-devices.c`).

Sources: `arch/arm/plat-rda/md.c` (mdcom core), `md_sys.c` (msys, 31 KB),
`modem_xcpu.c`, `comreg0_misc.c`, `smd.c` from
`OrangePiLibra/OrangePi_i96_kernel@74c4ea44`. Note msys is a real protocol —
slots, sequence numbers, async completions, an rx workqueue — so hand-crafting
frames over the vendor u-boot's `mdcom_send` command is **not** a cheap shortcut;
it is the port itself.

Also required for a full solution: our u-boot must load `modem.bin` and start the
xcpu (half 1) before Linux, since the msys client needs a running modem to talk
to. `modem.bin` is **proprietary** — see the licensing note in §11.

### Cheap things worth doing first, independent of WiFi
1. **CMA is broken.** `OF: reserved mem: failed to allocate memory for node
   'linux,cma': size 32 MiB` → falls back to 64 MiB outside the IFC DMA window,
   so both mmc controllers warn about their bounce buffer. Patch 12 is not doing
   its job. Latent DMA-correctness risk on the *working* SD controller.
2. **Our DT overclaims RAM.** We declare 256 MB; the vendor reports `DRAM: 236
   MiB` (`_TGT_AP_OS_MEM_SIZE=236`, top 20 MB reserved for CAM 4 MB + VPU 16 MB).
3. **`package-bootloader.sh` is broken** — it calls `mkrdaimage.sh` with 3 args
   where the vendor script takes 5, and the committed `u-boot-spl.bin` differs
   from the SPL inside the working `.rda` by 40 bytes. The working blob is built
   by reusing the first `0x12000` bytes of the existing
   `rda8810-spl/bootloader-hybrid-debuguart.rda` and appending a freshly
   `mkimage`d `u-boot.img` (`-A arm -O u-boot -T firmware -a/-e 0x80008000`).
   That `.rda` is deliberately **not** tracked; regenerate it that way.

### Bench aids that now exist
- `rdapmu dump|read|write` — raw PMU over ISPI. The map validated against the
  vendor SPL's writes register-for-register, so it is trustworthy.
- `rdacombo id|on|scan` — chip id, apply the pad mux + clocks, brute-force a PMU bit.
- **Fast loop, no reflash** (needs patch 21, which is in):
  ```
  devmem <reg> 32 <val>
  echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/unbind
  echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/bind
  dmesg | tail
  ```
- **Boot our kernel from the vendor card** (for anything needing the modem):
  put `pv-zImage-dtb` (zImage with dtb concatenated) and `pv-uInitrd` on the
  vendor BOOT partition, then at its prompt run modem init, the two pinmux `mw.l`
  writes, and `bootz ${kernel_addr} ${initrd_addr}`.

### Method note, learned the hard way this session
Two searches in this effort produced confident but worthless negatives because
the *detection path* was broken — a PMU bit-sweep run while the I2C bus was
unmuxed, and a "no FDT" kernel hang misread as a modem failure. **Verify the
instrument before believing a negative result.** Where possible use a positive
control: mmc0 runs the same driver as mmc1 and diffing their live registers was
worth more than any amount of reasoning.

---

## 13. The msys client exists (2026-07-23) — bench it

§12's "next step" — half (2) of strategy A, the AP-side msys client — is now
implemented as kernel patches 22–25 (`linux-yocto_%.bbappend`):

| Patch | What |
|-------|------|
| 22 | `drivers/misc/rda-mdsys.c` + `include/linux/rda-mdsys.h` — minimal mdcom SYS-channel + msys client (`CONFIG_RDA_MDSYS`) |
| 23 | `rda8810pl.dtsi`: `mdsys: mailbox@200000` node (dpram `0x00200000` + comregs `0x20980000`, irq 19/COMREG1) |
| 24 | board dts: enable mdsys; RAM 236 MB (vendor map); CMA 16 MB so it finally fits the IFC DMA window (§12 cheap items 1+2 done) |
| 25 | `rda_combo_power_main.c`: the `enable_26m_regulator`/`enable_26m_rtc`/`enable_32k_rtc` stubs now send `SYS_PM_CMD_EN(v_bt=10)` / `SYS_GEN_CMD_AUX_CLK(1)` / `SYS_GEN_CMD_CLK_OUT(1)` with the vendor's mask refcounting |

Driver behaviour: at probe it reads the heartbeat version (dpram `+0x0c`) and
handshakes with a side-effect-free `SYS_GEN_CMD_BP_INFO`. No modem → it logs
"modem not running" and every call returns `-ENODEV` fast, so the modemless
boot is unchanged. Protocol verified against vendor `md.c`/`md_sys.c` and
u-boot `defs_mdcom.h` (channel addresses match register for register).

### Resolved: the modem does NOT live in AP DRAM

The vendor u-boot decodes the `modem.bin` load addresses through
`rda_mdcom_address_modem2ap()`: the i96 target defines **no**
`_TGT_MODEM_MEM_SIZE`, so `RDA_MODEM_RAM_BASE = RDA_MD_PSRAM_BASE =
0x10000000 + 0x02000000 = AP phys 0x12000000` (4 MB dedicated modem PSRAM
behind the modem bridge window). The raminit stub (`0x01c16000`) goes to modem
internal SRAM via `RDA_ADD_M2A` (`0x11c16000`). Consequences:

- **No reserved-memory nodes are needed** — the modem firmware never occupies
  AP DRAM, and our 31 MB kernel image cannot corrupt it. The §12 vendor-card
  experiment is therefore *not* invalidated by a memory collision.
- `MD_ADDRESS_VALID`'s `0x82000000..0x84000000` ranges are **modem logical
  addresses** carried inside msys messages, not AP phys.
- The mdcom dpram at `0x00200000` is dedicated SRAM, also not DRAM.

### Bench procedure (vendor card, no reflash of our u-boot needed)

1. Put the new `pv-zImage-dtb` (zImage+dtb concatenated) and `pv-uInitrd` on
   the vendor BOOT partition.
2. At the vendor u-boot prompt: run the modem init (`mdcom_loadm` path from
   its `boot.cmd`), the two pinmux `mw.l` writes, **set bootargs**, then boot:
   ```
   ext2load mmc 0:1 ${modem_addr} modem.bin
   mdcom_loadm ${modem_addr}
   mdcom_check 1
   mw.l 0x11a09010 0x3fffffff
   mw.l 0x11a09008 0x7fe001ff
   setenv bootargs "earlycon console=ttyRDA2,921600"
   ext2load mmc 0:1 ${kernel_addr} pv-zImage-dtb
   ext2load mmc 0:1 ${initrd_addr} pv-uInitrd
   bootz ${kernel_addr} ${initrd_addr}
   ```
   The `setenv bootargs` line is NOT optional: the vendor default env does
   not name our console, so without it the kernel boots **silently** and
   looks exactly like a hang at "Starting kernel ..." (2026-07-23: one
   session was lost to precisely this — broken-instrument rule again).
3. Expect in dmesg: `rda-mdsys ...: modem running, interface version
   0x00010001`. If instead the BP_INFO handshake times out, the transport
   port needs debugging before anything else (broken-instrument rule).
4. Watch the combo power-on: the helpers now log `v_bt enable failed` /
   `aux 26M enable failed` warnings if msys commands fail — silence means the
   commands were ACKed by the modem.
5. Then the moment of truth: `mmc1: new SDIO card` in dmesg after the
   rda-mmc rebind (or automatically at boot).
6. Manual experiments without reboot, in any order:
   ```
   cat  /sys/devices/platform/200000.mailbox/modem_state
   echo "pm 10 1" > /sys/devices/platform/200000.mailbox/cmd    # v_bt on
   echo "aux 1"   > /sys/devices/platform/200000.mailbox/cmd    # 26M aux
   echo "out 1"   > /sys/devices/platform/200000.mailbox/cmd    # 32k out
   echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/unbind
   echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/bind
   dmesg | tail
   ```

If SDIO answers: stage 3 is done; port `mdcom_loadm` into our u-boot stage-2
(half 1 of strategy A) so the modem starts on our own bootloader, then move to
stage 4 (`rdawlan`). If SDIO still fails with the modem confirmed running and
all three msys commands ACKed, the remaining suspects are the vendor's
`SYS_PM_CMD_SET_LEVEL` (voltage) and whatever `wifi_power_on`'s msys-side
tables did — capture `modem_state` + a PMU dump and re-plan. For that next
level of depth: `~/Desktop/modem-cross-compiler-linux.tar.gz` has a
`mips-elf-` toolchain (the xcpu is MIPS — see the mkimage headers), so
`mips-elf-objdump -D` on the `Modem work codes` payload can recover what the
modem's own `SYS_PM_CMD_EN`/`AUX_CLK` handlers write to the PMU.

### Hard-won driver lesson (2026-07-23, found by the modemless control boot)

The dpram ring-ctrl words are uninitialized SRAM on a modemless boot: the
first driver version read a garbage `head` (`0x2f31a6f2`), indexed it
unmasked into the 512-byte tx ring, and oopsed in `memcpy_toio` — killing
init at 0.87 s. Fixed by masking every head/tail read AND gating the probe
handshake on `mdsys_rings_sane()` (all four pointers in-range and aligned;
the bootloader zeroes them when it inits mdcom). Never trust dpram contents
that only a modem-aware bootloader initializes.

---

## 14. Stage 3 with the msys client live: what is now ELIMINATED (2026-07-24)

The msys transport works. On the vendor card the driver prints
`rda-mdsys 200000.mailbox: modem running, interface version 0x00010001`,
every command returns BP status 0, and the combo power-on runs with no
`enable failed` warnings. **`mmc1` still fails.** A full bench day of
bisection eliminated, each by direct measurement on hardware:

| Suspect | How it was eliminated |
|---------|----------------------|
| msys transport / handshake | BP_INFO succeeds; all commands return status 0 |
| v_bt (pm 10), v_sdmmc (pm 7) | sent and ACKed, alone and together |
| **all EN-type rails** (pm 1/7/8/10/11/12/13, incl. **v_fm**, same die) | all seven ACKed in one batch, then power-cycle + detect: no change |
| `SYS_PM_CMD_SET_LEVEL` for v_bt | not applicable: `bt_config.msys_cmd = SYS_PM_CMD_EN`; `set_voltage_sel()` returns `-EINVAL` for EN-type rails |
| AUX_CLK payload shape | vendor `struct low_freq_clk_param { u32 enable; }` — exactly what we send |
| md_sysctrl clock gates | `Cfg_Clk_Auxclk`=1 and `Cfg_Clk_Out`=0x200 forced by hand (incl. the `REG_DBG` 0xA50001 unlock); no change |
| APB2 SDMMC2 clock gate | `0x209000A8` already `0x001FFFFF` (bit 17 set) on both boot paths |
| APB2 SDMMC2 reset | manual pulse verified by readback (`0xFFF`→`0xDFF`); our `reset_control` writes are register-exact vs vendor (`0x4C`/`0x50`, `BIT(9)`, 1 ms) |
| pad mux | `BB_GPIO_Mode = 0x7FE001FF` (bits 9–20 alt-func = both SD interfaces) |
| clock divider | our dyndbg `divider = 99` == vendor log `divider = 99` at 1 MHz |
| probe/rescan ordering | manual unbind/bind after power-on, dozens of times |
| **OFF → ON transition** | the vendor always powers off first (rfkill blocked at init). Replicated via a new `wifi_power` sysfs hook: `power_off succeed!!` then the full 9-step on. No change |
| response classification | dyndbg shows correct `cfg` per command (`0x31` R3-select for CMD5) and `NO_RSP` set immediately, resp regs zeroed |

Command-level truth from dyndbg on our kernel: CMD52/CMD8/CMD5/CMD55/CMD1 all
return `-110` with zeroed response registers — i.e. the chip is electrically
silent, not answering-and-being-rejected. The vendor's successful path shows
`mmc1: card claims to support voltages below the defined range` (a real CMD5
OCR reply) — we never get that far.

### Two operational traps found today (cost hours)

1. **The vendor u-boot's default `bootargs` name no console.** A kernel booted
   from it without `setenv bootargs "earlycon console=ttyRDA2,921600"` runs
   completely silently and looks identical to a hang at `Starting kernel ...`.
2. **The modem powers itself off ~10–20 min after a keyless boot.** u-boot says
   so up front: `Power-on key is not pressed for normal boot / Shutdown is
   needed later`. Once it goes, every msys command times out (`cmd ... timed
   out`, `-110`) — silently invalidating any experiment run late in a session.
   Check `devmem 0x20980018` (COMREG IT_CLR): bit 5 (`0x20`) set = BP shut
   down; and re-verify with a `pm` command before trusting any late result.
   **Do the decisive experiment in the first minutes after boot.**

### What is left

Everything software-visible now matches the vendor. The remaining difference
must be in state we have not looked at:

1. **Register diff against the working system.** Dump `0x11a09000..0x11a0907c`
   (pads), `0x11a00000..0x11a000fc` (md_sysctrl) and the SDMMC2 file at
   **`0x20a60800..0x20a608fc`** (note: `+0x800`, not the APBI wrapper at
   `+0x000` — `TRANS_SPEED` lives at `0x20a6083c`) on the vendor system while
   `wlan0` is up, and diff against ours. Caveat: `dd if=/dev/mem` returns
   `EFAULT` on the vendor 3.10 kernel — use a python `mmap` reader instead.
2. **Modem firmware disassembly.** `~/Desktop/modem-cross-compiler-linux.tar.gz`
   has a `mips-elf-` toolchain (the xcpu is MIPS per the mkimage headers).
   The msys magic `0xA8B1` appears at exactly 3 offsets in the extracted
   `Modem work codes` payload (`0x07c64e`, `0x07ec9a`, `0x0e7488`) — good
   anchors for finding the command dispatch and reading what the
   `SYS_PM_CMD_EN` / `AUX_CLK` handlers actually write to the PMU over ISPI.
3. **Power-key boot variant.** Hold the power button during `mdcom_check` so
   the modem does not schedule its shutdown, and see whether rail behaviour
   differs.

### Bench aids added for this work

- `wifi_power` sysfs attribute on every combo I2C client
  (`echo 0|1 > /sys/bus/i2c/devices/0-0016/wifi_power`) — forces
  `rda_wifi_power_off()` / `rda_wifi_power_on()` without a driver rebind.
- `CONFIG_DYNAMIC_DEBUG=y` in `rda8810pl.cfg`; enable the MMC trace with
  `echo 'file drivers/mmc/host/rda-mmc.c +p' > /sys/kernel/debug/dynamic_debug/control`
  (also `drivers/mmc/core/core.c` for the core's per-command lines).
- `/home/orangepi/dump-regs.sh` on the vendor rootfs (needs the python-mmap
  rewrite, see above).

---

## 15. STAGE 3 SOLVED: five pad registers, no modem (2026-07-24)

`mmc1: new SDIO card at address 4829` on our own u-boot, our own image, with
`rda-mdsys` reporting `modem not running`. The card enumerates at boot, ~2.4 s,
with no manual intervention.

### The fix

Five whole-register writes, applied in u-boot `rda_combo_pinmux()`
(`recipes-bsp/u-boot/files/rda8810-stage2/board/rda/rda8810pl/rda_combo.c`):

| register | value | note |
|----------|-------|------|
| `0x11a09008` BB_GPIO_Mode | `0x7fe0003f` | bits 6..8 matter too, not just the SDMMC2 pads 15..20 |
| `0x11a0900c` AP_GPIO_A_Mode | `0x000210fc` | reset value is `0xffffffff` (all GPIO) |
| `0x11a09010` AP_GPIO_B_Mode | `0x3f00033f` | we previously only cleared the two I2C1 bits |
| `0x11a09018` pad cfg | `0x14040040` | we never wrote it at all (read `0`) |
| `0x11a0901c` pad cfg | `0x006e4524` | differs from reset in bits 16..23 |

Values read from a running vendor system with `wlan0` up. Per-pad meaning is
undocumented; the vendor kernel programs them from its own board files. All
five are required — the bisection showed each one breaking enumeration in a
different way, e.g. AP_GPIO_B back to `0x3fffffff` kills CMD5 outright while
AP_GPIO_A back to `0xffffffff` leaves CMD5 answering but fails init at `-110`.

### How it was found, after everything else was eliminated

§14 lists a full day of eliminations (all msys rails, both md_sysctrl clock
gates, the APB2 gate and reset, clock divider, probe ordering, the vendor's
OFF→ON power transition, response classification) — every software-visible
knob measured equal to the vendor's while CMD5 stayed electrically silent.
What finally worked was **dumping registers from the running vendor system and
diffing them against ours**. Two rounds of that:

1. pads `0x11a09000..0x11a0907c` → the five differences above;
2. AP sysctrl `0x20900000..0x209000fc` → **byte-for-byte identical**, which
   ruled out the entire clock/reset domain and left the pads as the answer.

### The modem question, settled

Three controlled boots, all with our 6.6 kernel:

| u-boot | pad map | modem | SDIO |
|--------|---------|-------|------|
| vendor | applied by hand | **running** | works |
| vendor | applied by hand | **absent** (skipped `mdcom_loadm`) | **works** |
| ours   | applied by u-boot | absent | works |

The middle row is the one that matters: same vendor bootloader, modem
deliberately not loaded, still enumerates. So the modem was never the gating
factor for WiFi — §1/§11 were wrong, and the msys client (§13, patches 22–25)
is not needed for stage 3. It stays in tree because it is correct, tested, and
will be wanted for BT/FM/PM later; on a modemless boot it disables itself.

### Method notes worth keeping

- **Register-diff against a working system beats reasoning.** Two rounds of it
  solved what a day of hypothesis-testing could not. When a peripheral is
  "electrically silent" and every software knob matches, dump both sides.
- **Every earlier negative was real but incomplete.** The msys work, the clock
  gates, the OFF→ON transition — all correctly eliminated, none of them the
  cause. The eliminations are what made the register diff the obvious move.
- **Partial fixes hide the answer.** Stage 2 needed two bits of AP_GPIO_B;
  getting those right made I2C work and made it tempting to believe the pad
  story was finished. It was not — three more registers were still wrong.

### Remaining loose end

The kernel has no RDA pinctrl driver, so this lives in the bootloader and
depends on it. A DT-driven pinctrl driver is the correct home and is the
natural upstream story; until then, anyone booting a different bootloader must
replicate these five writes.


---

## 16. Stage 4: the rdawlan driver builds for 6.6 (2026-07-24)

Patches 26 (verbatim vendor import) and 27 (the forward-port) put a
`CONFIG_RDAWFMAC=y` cfg80211 driver in the kernel. It compiles and links
clean. **It has not been run on hardware yet** — that is the next bench step.

### Scope

Station only. Dropped from the vendor build: USB, BT-AMP, P2P (already off in
the vendor's own `wland_defs.h`), wireless-extensions (`wland_iw.c`), Android
private commands, monitor mode. That removes ~200 KB of the most API-rotted
code and none of it is needed for `wlan0` + scan + WPA2.

The chip "firmware" is a table of register patches compiled in from
`wland_trap_91g.h`, **not a blob**, and the driver is under a permissive
ISC-style licence — so nothing is fetched at runtime and there is no
redistribution question, unlike `modem.bin`.

### The migrations (all in patch 27)

timers (`init_timer` → `timer_setup`/`from_timer`), cfg80211 op signatures
(`add_virtual_intf` gained `name_assign_type` and lost `flags`, key ops gained
`link_id`, `get_station` takes a const MAC), cfg80211 calls (`inform_bss`
gained a frame type, `disconnected` gained `locally_generated`, `ibss_joined`
gained a channel, `scan_done`/`roamed` take parameter structs), enum renames
(`IEEE80211_BAND_*`, `STATION_INFO_*`, `IEEE80211_CHAN_NO_IBSS`), netdev
(`last_rx`/`trans_start` gone, `netif_rx_ni` folded in, `tx_timeout` gained a
queue index, `alloc_netdev` gained a name-assign type), and VFS
(`set_fs`/`KERNEL_DS` gone → `kernel_read`/`kernel_write`).

Also: the driver defined **42 static functions in the kernel's own
`cfg80211_*` namespace**, which now collides (`cfg80211_get_station`). They
are renamed `wland_cfg80211_*`.

### Bugs caught by independent review, not by the compiler

Two external models (opencode/kimi and agy/Gemini) reviewed the semantic
decisions. Four real defects came out of it, three of them introduced by the
port itself:

1. **`locally_generated` was wrong.** A blanket regex passed `true` at the
   `WLAND_E_DISCONNECT_IND` site, which is the *firmware* reporting that the
   AP deauthenticated us — the opposite of locally generated. Userspace would
   have been told the wrong thing about every AP-side disconnect.
2. **`REGULATORY_CUSTOM_REG` went into the wrong field.** Renaming
   `WIPHY_FLAG_CUSTOM_REGULATORY` kept `wiphy->flags |=`; the constant belongs
   in `wiphy->regulatory_flags`. It compiled and set a meaningless bit.
3. **`.start_ap`/`.change_beacon` were left registered after `.stop_ap` was
   dropped**, and `BIT(NL80211_IFTYPE_AP)` was still advertised — userspace
   could have started an AP with no way to stop it. All three removed.
4. **`kernel_read()` argument order** in `linux_osl.c` was the pre-4.14 form
   (dead code here, but header-exported and wrong).

Worth recording as method: the compiler proved nothing about any of these.
A second reader with fresh eyes on *the decisions* — not the diff — found
them in one pass.

Not everything reported was real: a claimed use-after-free of the watchdog
timer/kthread on remove was checked against the code and the teardown does
happen, via `wland_sdio_bus_stop()`. Agent findings need verifying like any
other claim.

### Known limitations, deliberate

- **No `mgmt_tx`.** Fine for WPA2-PSK on a full-MAC part (auth/assoc are in
  firmware, EAPOL rides the data path), but it will need porting for 802.11w
  MFP (SA Query), 802.11r FT, or WNM.
- **No scheduled scan**; wpa_supplicant falls back to normal scans.
- **RSSI is instantaneous**, not averaged — the averaging cache lives in the
  `wland_iw.c` we do not build.
- `rda_mmc_set_sdio_irq()` is a no-op shim. The vendor host exported it to
  mask the SDIO IRQ; mainline governs delivery through the driver's own
  `sdio_claim_irq()`/`sdio_release_irq()`. If spurious interrupts appear in
  the windows the vendor masked, release/re-claim there rather than reaching
  into the host driver.
- Pre-existing vendor issues left alone, worth a look if the remove path is
  ever exercised: `sdio_claim_irq()` is not released on one error path, and
  `cfg80211_inform_bss()` is called under `cfg->scan_result_lock`.

### Next bench step

Flash and boot, then look for the SDIO function binding to `rdawfmac` after
`mmc1: new SDIO card`, a `wlan0` in `ip link`, and `iw dev wlan0 scan`
returning APs. Definition of done is unchanged from §8: WPA2 association plus
DHCP.

### First hardware contact (2026-07-24 evening)

The driver bound and talked to the chip:

```
mmc1: new SDIO card at address 4829
[RDAWLAN_ERR]:<wland_sdio_probe,1615>: ------- Chipid: 0x6(RDA5991_G) -------
[RDAWLAN_ERR]:<wlan_read_mac_from_nvram,112>: nvram:can not get wifi mac from nvram   (x3)
[RDAWLAN_ERR]:<wland_bus_start,104>: nvram:get a random ether address
[RDAWLAN_ERR]:<cfg80211_reg_notifier,5255>: reg_notifier for intiator:0 not supported
[    5.000000] sched: RT throttling activated      <-- then wedged
```

Everything before the wedge is correct: the SDIO chip-id read is the ported
driver using the data path, and the MAC fallback matches the vendor's own
behaviour (three msys retries, then a random address) now that
`wlan_read_mac_from_nvram()` goes through our msys client.

**The hang was in our MMC host, not the WiFi port** (patch 28). `rda-mmc` only
told the MMC core about an SDIO interrupt when a data request happened to be
in flight:

```c
if (!priv->mrq || !priv->mrq->data)
        goto irq_done;                  /* async card IRQ dropped here */
...
mmc_request_done(host, mrq);
if (priv->sdio_irq && priv->sdio_irq_trigger)
        mmc_signal_sdio_irq(host);      /* only ever reached with an mrq */
```

Card interrupts are asynchronous by definition, so the first real one was
dropped. Clearing `SDMMC_INT_SDIO` in `INT_CLEAR` only clears the controller's
latch — the card holds its interrupt asserted on DAT1 until the function
handler services it, so the latch sets again immediately. The line storms, and
since this is a threaded (RT priority) handler on a UP box, the system wedges.

Fix: call `mmc_signal_sdio_irq()` from the hard handler *before* the request
checks. It masks the SDIO interrupt and wakes the core's sdio_irq thread,
which runs the function handler and re-enables.

This path had never been reachable: no SDIO card enumerated on this board
until the pad map landed in u-boot (§15), and nothing claimed the SDIO IRQ
until the WiFi driver existed. Worth keeping as a pattern — each stage that
unblocks a path exposes the first real bug in the layer below it.

### Follow-up: console baud (deferred until WiFi is done)

The i96 runs its console at 921600, which is the outlier — the docs say
"Most Pantavisor images default to 115200 8N1". Three places pin it and all
three must change together:

- `recipes-bsp/u-boot/files/rda8810-stage2/configs/*i96*`:
  `CONFIG_BAUDRATE` and `CONFIG_DEBUG_UART_CLOCK`;
- the board DT: `stdout-path = "serial2:921600n8"` **and** the `uart_clk`
  fixed-clock node — this port feeds the baud in as the UART's clock rate, so
  changing `stdout-path` alone is not enough;
- the kernel cmdline follows u-boot's `${baudrate}` automatically.

The vendor SPL in the first 72 KB of the bootloader blob is a binary we do not
rebuild, so anything it prints stays at 921600; in practice it prints nothing.

Deliberately deferred: 921600 is useful while bringing WiFi up, because
`rdawfmac.wland_dbg_level=5` is chatty enough that at 115200 the console
traffic would slow the driver past its own 5 s control-response timeout.

---

## 17. Stage 4 on hardware: where it stands (2026-07-24, end of session)

The driver binds, creates `wlan0`, and talks to the chip. It stops at one
specific step: the **core init patch never gets a response**.

```
wland_sdio_probe: Chipid: 0x6(RDA5991_G)        <- driver reads the chip over SDIO
wland_bus_start: nvram:get a random ether address
netdev_attach: wlan0: Rdamicro Host Driver(mac:ce:17:d8:a4:de:ba)
wland_sdio_bus_txctl: ctrl_frame_stat == false, send success   <- WID command goes out
wland_sdio_bus_rxctl: resumed on timeout                       <- 5 s, no answer
wland_set_core_init_patch: WID Result Failed
wland_sdio_trap_attach: wland_sdio_core_patch_attach failed!
```

### What the verbose trace proves (`rdawfmac.wland_dbg_level=5`, patch 30)

- **Polling works.** `Wake up watchdog thread!` every 20 ms,
  `bus->poll:1, pollrate:1`. This matters because `bus->intr` is *false*
  during the patch download (it is only set after `trap_attach` succeeds), so
  the response is meant to arrive via polling, not the interrupt.
- **The chip is alive at the SDIO register level.** `wland_sdio_flow_ctrl_91e`
  reads `INT_PENDING` as `0xc0` on the first attempt and `0xd0` on the second,
  so the chip is updating its own registers between attempts.
- **The TX succeeds.** `WRITE: addr=0x00007, length=128, ret:0`.
- **The chip never signals data.** Every poll reads `INT_STATUS = 0x0`;
  `I_AHB2SDIO` (BIT0) is never set after the command. It *was* set once, at
  chip wake-up, before the command went out.

So: command sent, chip responsive to register access, core never answers.

### Eliminated, each by direct measurement

| Suspect | Result |
|---------|--------|
| Interrupt storm / RT livelock | fixed (patches 28+29); boot 81 s → 13.5 s, no `RT throttling` |
| `netdev->dev_addr` corruption | fixed (patch 29); the `free_netdev` WARNING is gone |
| Watchdog/poll not running | **disproven** — it runs at 20 ms throughout |
| md_sysctrl 26 MHz / 32 kHz gates | **actively harmful**: with `rdacombo clocks` applied, SDIO does not even enumerate (`mmc1: Failed to initialize`). Confirms removing them from the u-boot latch was right |
| **The modem** | **exonerated for the core too.** Booted the vendor card *with* `mdcom_loadm` and our stage-4 kernel: identical failure at 8.48 s. Note the MAC line changes to a single `get invalid wifi mac address` instead of three `can not get` retries — proof the msys command actually reached a running modem and returned data. §15's conclusion holds for the core, not just enumeration |
| Wrong chip-variant flow control | disproven: `wland_sdio_flow_ctrl()` dispatches 91E/91F/**91G** to `_91e`, which is what runs |

### Next moves, in order

1. **Compare the WID frame we send against the vendor's byte for byte.** The
   vendor system can be made to dump it (`wland_dbg_area` has TX_CTRL /
   RX_WIDRSP areas, and `WLAND_DUMP` prints frames). Same command, same chip:
   if our 118-byte frame differs, that is the bug.
2. **Check the chip-kick after the data write.** The trace shows the payload
   written to `addr 0x07` with no following `URSDIO_FUNC1_INT_TO_DEVICE`
   (`addr 0x09`) write; `addr 0x09` is only written by `wland_chip_wake_up`.
   Confirm from the vendor trace whether a kick is expected there.
3. **Sanity-check `wland_write_sdio32_polling()`**, the caller — it is a
   polling-style register write path and the most likely place for a
   forward-port slip that the compiler could not catch.

### Bench aids that made this tractable

- `rdawfmac.wland_dbg_level=5` on the kernel command line (patch 30).
- The vendor card boots our kernel via `pv-zImage-dtb`, with or without
  `mdcom_loadm`, which is how the modem was isolated as a variable.

---

## 18. The three next moves, worked (2026-07-28)

All three were answered from source. Two came back negative; the third did
not survive contact with the code, but chasing it found the actual bug —
in patch 29, our own fix for the interrupt storm.

The tree to read them in is reconstructible without a build: apply patch 26
to an empty git repo (excluding the two pre-existing `rdaw80211/Kconfig` and
`Makefile` hunks), commit, then apply 27. Vendor and port are then two
commits and any function can be diffed directly.

### Move 1 — the WID frame cannot differ

No dump needed. The whole construction path is in files **patch 27 does not
touch**: `wland_set_core_init_patch()` and `wland_write_sdio32_polling()` in
`wland_trap.c`, `wland_proto_cdc_data()` and `wland_wid_hdrpush()` in
`wland_cmds.c`. They are byte-identical to the vendor. `wland_sdio.c` is
touched, but only for `timer_setup()` and the patch-29 ISR change — the
txctl path is untouched too.

### Move 2 — no chip-kick is expected

`URSDIO_FUNC1_INT_TO_DEVICE` (0x09) is written in exactly two places in the
vendor tree: `wland_chip_wake_up()`, and the power-manager tail of
`wland_preinit_cmds()`, which runs *after* the patch download and only under
`WLAND_POWER_MANAGER`. The TX path (`wland_sdio_send_pkt()`) writes
`SPKTLEN_LO`/`SPKTLEN_HI` and then the WR FIFO, and stops there — the length
registers are the trigger. Our trace matches the vendor code exactly.

### Move 3 — wrong file, right instinct

`wland_write_sdio32_polling()` lives in `wland_trap.c`, which the
forward-port never touched, so no slip was possible. But the question "who
else could be eating this response" pointed at the one thing in the path
that *was* changed.

### RESOLUTION: patch 29's ack was clearing the frame indication

`wland_sdio_bus_init()` claimed the function IRQ, but `bus->intr` stays
false until `wland_sdio_trap_attach()` succeeds. **The entire core init
patch download runs in polling mode** — `wland_sdio_watchdog_thread()` is
what is meant to see `I_AHB2SDIO` in `INT_STATUS` and pull the frame out of
the RD FIFO.

So the ISR fires in a window where it must not act, and both behaviours it
can have are wrong:

| ISR behaviour | Result |
|---|---|
| return without acking (vendor, and us before patch 29) | card holds DAT1 asserted, core re-enables after the handler, line storms → `sched: RT throttling activated` |
| ack and return (patch 29) | `I_AHB2SDIO` is **write-1-to-clear** — writing back what `INT_STATUS` reported clears the indication without ever reading the RD FIFO, so the frame is never collected and `INT_STATUS` reads 0x0 forever |

The header comment is explicit and was the giveaway: *"Indicates that data
transfer from AHB to SD is pending. Cleared by Host by writing a '1' into
this register location."*

That is every symptom in §17: TX succeeds, chip alive at register level,
`INT_STATUS` always 0x0, `I_AHB2SDIO` set exactly once — at wake-up, before
the IRQ started being consumed — and a 5 s rxctl timeout.

The vendor never had to choose, because `rda_mmc_set_sdio_irq()` masked the
SDIO interrupt at the host controller for exactly this window. §16 listed
that no-op shim as a known limitation and guessed the fix would be
"release/re-claim the function IRQ there rather than reaching into the host
driver" — which is what patch 31 does, in the simpler direction: **never
claim it in the first place until the driver leaves polling mode.**

Patch 31:

- `wland_sdio_intr_register()` keeps writing the chip's `REGISTER_MASK` at
  bus init (so the polling path still sees status bits) but no longer calls
  `sdio_claim_irq()`;
- a new `wland_sdio_intr_enable()` claims it, called from `wland_wid.c`
  right where `bus->intr = true` is set — the exact spot the vendor called
  `rda_mmc_set_sdio_irq(1, true)`;
- the `!bus->intr` branch of the ISR goes back to a bare return, now
  unreachable during bring-up, with a comment on why it must stay bare.

`sdio_release_irq()` is a no-op when the IRQ was never claimed, so the
test-mode path (which never sets `bus->intr`) needs no change.

### CONFIRMED on hardware, with the mechanism corrected (2026-07-28, later)

The falsification check was run against the **patch-29** image at
`wland_dbg_level=5`, and the line is there — exactly once:

```
[3.420000] [RDAWLAN_SDIO]:<wland_sdioh_irqhandler,1385>  isr w/o interrupt enabled, acked 0x1 and returning
```

`0x1` is `I_AHB2SDIO`. But the timeline shows the guess above was wrong about
*which* frame was lost:

```
3.400  bus_init -> intr_register: REGISTER_MASK=0x07, claims the IRQ
3.410  core-init WID built, 118 bytes, txctl queued
3.420  chip_wake_up -> SDIO IRQ fires, bus->intr == false
         read  INT_STATUS  (0x06) = 0x1   <- a frame is ALREADY pending
         write INT_PENDING (0x05) = 0x1   <- flag cleared, FIFO never read
3.450  118-byte payload -> WR FIFO, ret 0
3.470+ poller reads INT_STATUS: 0x0, 0x0, 0x0, ... forever
8.540  second attempt, identical
```

The ack happened **before** the command went out, so it did not eat the WID
response — the response never came. What it ate was the frame indication
already pending at chip wake-up. §17 recorded that bit as an incidental
detail ("it *was* set once, at chip wake-up"); it is in fact the whole story.

`wland_sdio_readframes()` is called **zero times** in the entire trace, and
there is not one read of the RD FIFO (`addr 0x08`). On the vendor, with no
ISR claiming the IRQ, the 20 ms poller sees `INT_STATUS = 0x1`, calls
`wland_sdio_readframes()`, reads `RPKTLEN` and **drains the FIFO**. Patch 29
clears the flag and leaves the packet in place, so the chip's AHB2SDIO
engine still holds an undelivered frame and never signals another.

So the fix is unchanged but its prediction is now sharp: with patch 31 the
poller should run at ~3.44 s, before the payload write, and the trace should
contain `received buffer size:N`. **If that line appears and the WID still
fails, this diagnosis is wrong too.**

### Driving the board without a reflash

Worth keeping — the whole check above was done over the serial console with
no rebuild:

- `stty -F /dev/ttyUSB1 921600 raw -echo`, then `cat` to capture and
  `printf ... > /dev/ttyUSB1` to type. picocom is not needed and holding the
  port with picocom would block this.
- `reboot -f` **halted the board** on every build up to patch 31; mainline
  6.6 had no RDA restart handler, so it needed a physical power cycle
  afterwards. Fixed by patch 32 — see §19.
- To change the cmdline for one boot: interrupt autoboot (spam a bare space
  during power-on), then replay `boot.scr` by hand with the param appended.
  There is no saved environment (`/uboot.env` is absent, the built-in default
  from `include/configs/rda8810pl.h` is used), so nothing persists anyway:

```
setenv bootargs "earlycon console=${console},${baudrate} root=/dev/ram rootfstype=ramfs rdinit=/usr/bin/pantavisor pv_storage.device=/dev/mmcblk0p2 pv_storage.fstype=ext4 panic=3 rdawfmac.wland_dbg_level=5"
load mmc 0:1 ${kernel_addr_r} zImage
load mmc 0:1 ${fdt_addr_r} ${fdtfile}
load mmc 0:1 ${ramdisk_addr_r} uInitrd
bootz ${kernel_addr_r} ${ramdisk_addr_r} ${fdt_addr_r}
```

- Which patches are in a flashed image can be read straight off the console:
  `WLAND_ERR` prints `<function,line>`, and patch 31 moves the `Chipid:` line
  in `wland_sdio_probe` from **1631** to **1661**. That is how the first
  "built and flashed" image was identified as pre-31.
- No rebind loop is available after a failure: `wlanfmac_module_init` reports
  `sdio_register_driver timeout or error` at 12.6 s and unregisters the
  driver, so `/sys/bus/sdio/drivers/` ends up empty. The driver is built-in,
  so a reboot is the only way to re-probe.

### Separate issue, visible in the same trace

`wlanfmac_module_init: sdio_register_driver timeout or error` fires at
12.64 s, *before* the second WID attempt completes at 13.52 s. That
registration semaphore has its own ~9 s timeout and the two 5 s WID retries
overrun it. Fixing the WID response should hide this, but the margin is thin
— watch for it if attach is ever slow.

### Method note

The bug was introduced by the previous fix, one commit earlier, and neither
the compiler nor the boot log flagged it — patch 29 removed a livelock and
looked like a clean win. What found it was reconstructing vendor and port as
two git commits and asking *what did we change in this path*, rather than
*what is wrong with this path*. §16 records the same lesson from the other
direction: each stage that unblocks a path exposes the first real bug in the
layer below. Here the layer below was us.

---

## 19. Machine restart and power-off (2026-07-28, patch 32)

Not a WiFi problem, but the WiFi bring-up is what made it expensive: every
`reboot` halted the board and cost a walk to the bench. There was no restart
handler because there was nothing to call — and the reason is the same modem
story that dominated stages 1–3.

The vendor resets this SoC **from the modem coprocessor**:

- `drivers/watchdog/rda_wdt.c` is not a watchdog at all. It is an AP→modem
  heartbeat — the AP bumps `ap_cnt` in shared memory and the modem resets the
  system if it stops. Its own header says so: *"Based on rda md driver - md
  heartbeat. System reset is done by Modem"*.
- `arch/arm/mach-rda/board-rda8810.c` has `//.restart = rda8810_restart,` —
  commented out.
- `arch/arm/mach-rda/include/mach/system.h` `arch_reset()` is an empty stub.

So the vendor kernel has no AP-side reset either. Mainline never starts the
modem, so `reboot(2)` fell through to halting the CPU.

### It does not need the modem

The whole-chip soft reset is a bit in the **always-on MD system controller**
at `0x11A00000` — the same block, behind the same write-protect register, as
the `Cfg_Clk_Out`/`Cfg_Clk_Auxclk` gates §14 was already poking from the AP
with the modem stopped:

| Offset | Register | Use |
|--------|----------|-----|
| `0x00` | `REG_DBG` | unlock, `0x00a50001` |
| `0x04` | `Sys_Rst_Set` | `BIT(31)` = `SYS_CTRL_SOFT_RST` (whole chip) |
| `0x80` | `WakeUp` | clear `FORCE_WAKEUP` = power off |

Restart is unlock + `BIT(31)`. Power-off is the pair the vendor bootloader's
`shutdown_system()` issues — unlock + `WakeUp = 0`, spun in a loop as the
vendor does. Names and bit positions come from the vendor u-boot header
`arch/arm/include/asm/arch-rda/reg_md_sysctrl_rda8810.h`.

Note `Sys_Rst_Set` also has `BIT(30)` = `SYS_CTRL_SET_RST_OUT`, which drives
the external reset output. If `BIT(31)` alone turns out not to reset the
board, that is the next thing to try — it is a one-constant change.

### What patch 32 adds

`drivers/power/reset/rda8810pl-restart.c`, a ~90-line platform driver using
the 6.6 sys-off API (`devm_register_restart_handler` /
`devm_register_power_off_handler`), plus `CONFIG_POWER_RESET_RDA8810PL`, and
a `system-controller@1a00000` node in the SoC dtsi. The node is in the dtsi
rather than per board: every RDA8810PL resets the same way and both in-tree
boards want it.

u-boot's `reset_cpu()` in `arch/arm/mach-rda/soc.c` was the same stub
(`while (1) ;`) and now does the same two writes, so `reset` works at the
u-boot prompt too.

**Side effect worth knowing:** this makes `panic=` effective. The pantavisor
cmdline carries `panic=3`, which until now silently did nothing — the board
will now reboot on a panic instead of sitting there.

### Untested

Written from the vendor headers, not yet run. Verify with `reboot` (should
come back through u-boot) and `poweroff`. Do not confuse this soft reset with
the APBI `SOFT_RST_L` pulse in the MMC wrapper, which is a different register
and must not be touched — see the MMC bring-up notes.

---

## 20. Stage 4 core init WORKS; the next wall is `ifconfig wlan0 up` (2026-07-28)

Patches 31 and 32 are both confirmed on hardware. §18's prediction held
exactly.

```
mmc1: new SDIO card at address 4829
[3.41] wland_sdio_trap_attach: Write core patch
[3.47] wland_sdio_watchdog_thread: Frame Ind!
[3.47] wland_sdio_readframes: received buffer size:16.     <- the poller drains it
[6.85] wland_sdio_core_patch_attach: Done(ret:0)
[8.41] Write additional patch finshed
[8.41] wland_sdio_intr_enable: Done(ret:0)                 <- IRQ claimed at handover
[8.48] wland_preinit_cmds: FirmWareVer:0x10202             <- chip firmware answers
[8.55] Set MAC (a2:08:...) / [8.62] Get MAC — match
[9.17] wland_start_chip: Done(err:0)
[9.17] wland_bus_start: Done.(ret=0)
```

Counters over the whole boot: `WID Result Failed` **0** (was 2 every boot),
`received buffer size` **87** (was 0), `isr w/o interrupt` **0**, no
`RT throttling`. Stage 4's core init patch download is done.

`reboot -f` also resets the board now (patch 32). Note plain `reboot`, which
goes through init, returns to the prompt and does nothing on this image.

### The bootloader trap that cost a flash cycle

Worth recording because it looked exactly like a stage-3 regression. The
first flash of the patch-31 kernel came up with `mmc1: Failed to initialize a
non-removable card` and no WiFi device at all. The kernel was innocent: the
image had been built with `bootloader-hybrid-debuguart.rda`, dated
**2026-07-22**, while the five-register AP pad map that makes SDIO enumerate
landed **2026-07-24** (§15, commits `0d17781` / `3e71486`). I2C and combo
power still worked because that pinmux is older — which is precisely what
made it look like the SDIO layer had broken again.

**The `.rda` blobs in `recipes-bsp/u-boot/files/rda8810-spl/` are stale build
artifacts. Never flash one without checking it.** The cheap check is to grep
the blob for the pad constants as little-endian u32:

```
0x7fe0003f  0x000210fc  0x3f00033f  0x14040040  0x006e4524
```

The jul-22 blob contains none of them; a correctly rebuilt one contains all
five. Rebuilding needs no vendor tooling — the layout is just concatenation:

```
.rda = [first 0x12000 bytes of a known-good .rda   = vendor SPL, keeps debug UART]
     + [mkimage -A arm -O u-boot -T firmware -C none
        -a 0x80008000 -e 0x80008000 -n u-boot -d <fresh u-boot.bin>]
```

`mkimage` is not installed on the dev host and the Yocto native one will not
run there (uninative loader plus a missing `libssl`). A ~90-line Python
reimplementation of the legacy header covers every type this board needs
(ramdisk, script, firmware) and was validated byte-identical against real
`mkimage` output before use.

### NEXT: `ifconfig wlan0 up` hangs

`wlan0` exists in `/sys/class/net`, but bringing it up never returns. The
process enters uninterruptible sleep, `^C` does nothing, and the console
stops emitting kernel output entirely — while the tty still **echoes** typed
characters, so the kernel is alive and the stuck task is most likely holding
`console_lock`. SysRq over serial break does not respond either
(`MAGIC_SYSRQ_SERIAL=y` with an empty sequence, so break+key should work).
Only a physical power cycle recovers it; `reboot -f` needs a working shell,
so patch 32 does not help here.

**Leading hypothesis: the interrupt path has never actually been exercised.**
`bus->sdcnt.intrcount` stayed **0** for the entire boot — every frame came
through the 20 ms poller while `bus->intr` was false. Patch 31 defers
`sdio_claim_irq()` to `wland_sdio_intr_enable()` at the `bus->intr = true`
handover, and immediately after that `bus->poll = false`. If IRQ delivery is
not actually working, the driver goes deaf at exactly that moment, and
`ndo_open` — which sends WIDs — blocks. That would make this patch 31's own
residual failure mode rather than an unrelated bug.

Against that reading: `wland_sdio_bus_rxctl()` waits with a timeout (that is
what produced `resumed on timeout` in §17), so a merely deaf interrupt should
give 5 s failures, not a permanent D state. A lock is the better fit. Both
need checking:

1. Does `bus->sdcnt.intrcount` ever increment after 8.41 s?
2. Where exactly does `ndo_open` block — `dhd_os_wait_for_event()` in
   `wland_sdio_bus_txctl()` has no obvious timeout, unlike the rxctl side.
3. Does `sdio_claim_irq()` on an already-enumerated card with the chip's
   `REGISTER_MASK` already set actually enable delivery through our
   mainline-style `rda-mmc` host?

### Bench tooling notes

- The BSP console shell has only `/sbin/ifconfig` and `/bin/busybox` — no
  `ip`, `iw` or `wpa_supplicant`. Association plus DHCP (§8's definition of
  done) will need the `pvwificonnect` container, already built in
  `deploy/images/orangepi-i96`.
- The console moved from `/dev/ttyUSB1` to `/dev/ttyUSB3` (FT232R) mid-session
  — four adapters are present, so confirm before capturing.
- Never leave two readers on the port. A stale background `dd` plus a new one
  splits the byte stream and the log is unreadable. Kill by PID; do **not**
  `pkill -f "cat /dev/ttyUSB3"`, because the pattern matches the invoking
  shell's own command line and kills it.

---

## 21. `ifconfig wlan0 up` deadlocks on the RTNL (2026-07-28, patch 33)

§20's hang is a self-deadlock, and the leading hypothesis there — a deaf
interrupt path — was wrong. Both of the driver's waits
(`dhd_os_ioctl_resp_wait()` and `dhd_os_wait_for_event()`) are
`TASK_INTERRUPTIBLE` with a 5 s timeout, so a missing interrupt could never
produce an unkillable task. That mismatch between symptom and mechanism is
what pointed at a lock.

```
netdev_open()                      <- the net core calls ndo_open with RTNL held
  wland_cfg80211_up()
    wland_update_wiphybands(cfg, notify=true)
      wiphy_apply_custom_regulatory()
        rtnl_lock()                <- already ours. mutex, uninterruptible.
```

`wiphy_apply_custom_regulatory()` did not take the RTNL on the vendor's 3.10.
On 6.6 it takes `rtnl_lock()` **and** `wiphy_lock()`
(`net/wireless/reg.c`). Same call, new locking contract — invisible to the
compiler, and it only fires when the interface is brought up, long after the
rest of the port has been proven working.

The whole driver contains exactly one other `rtnl_lock()`, in
`wland_del_if()`, guarded by `rtnl_is_locked()` and only on teardown. That
guard is itself sloppy — `rtnl_is_locked()` answers "is anyone holding it",
not "am I" — but it is not on this path.

### The fix

Delete the re-application. It was redundant:

- `wland_cfg80211_attach()` sets `wiphy->bands[NL80211_BAND_2GHZ] =
  &__wl_band_2ghz`, sets `REGULATORY_CUSTOM_REG`, then calls
  `wiphy_apply_custom_regulatory()` **before** `wiphy_register()` — the only
  point mainline supports it, and with bands populated so
  `handle_band_custom()` really runs.
- `wland_regdom` is static, and the bands `wland_update_wiphybands()`
  assigns are the *same* static structs. Their channel flags and power
  limits are already in place from attach.

The `notify` parameter existed only to gate that call, and both call sites
passed a constant, so it goes too. A comment is left in its place.

### Method note

Three hypotheses were killed by reading rather than by another bench cycle,
which matters when each cycle costs a build, a flash and a walk to the board:

- **DT collision** — patch 32's new `system-controller@1a00000` node was the
  obvious suspect for a regression that appeared in the same flash. Ruled
  out by decompiling the DTB actually on the card: no overlap with `gpioc`
  at `0x1a08000`, and nothing else claims the range.
- **Patch 28 swallowing data-completion interrupts** — plausible, since
  `mmc_wait_for_req()` *is* uninterruptible. Ruled out by reading the
  handler: `mmc_signal_sdio_irq()` runs before the `mrq` checks and falls
  through, and `rda_mmc_sdio_enable_irq()` is a read-modify-write that
  preserves the other mask bits. The clincher was the log: transfers kept
  working for 0.8 s after the IRQ was claimed, through the whole of
  `preinit_cmds`.
- **A deaf interrupt path** — ruled out by the timeouts above.

Also worth correcting from §20: "the console stopped emitting kernel output"
overstated the evidence. `wland_dbg_level` had just been set to 0, so the
driver was muted by hand; the quiet console is largely explained by that, and
does not by itself imply the stuck task held `console_lock`.

### Patch 33 confirmed on hardware (2026-07-28)

`ifconfig wlan0 up` now returns, and the interface carries traffic:

```
[163.56] netdev_open: Enter, idx=0
[163.57] wland_update_wiphybands: nmode=1, mimo_bw_cap=0
[163.57] wland_cfg80211_up: Done(err:0)          <- used to hang here forever
[163.57] netdev_open: netif_carrier_on(ndev)
[163.57] netdev_open: netif_start_queue(ndev)
[165.16] wland_netdev_start_xmit: skb->len=313
[165.16] wland_sendpkt: dest 33:33:00:00:00:fb   <- IPv6 multicast, mDNS
[165.17] wland_sdio_bus_txdata: TXDATA Wake up DPC work
```

`wland_cfg80211_up()` completes in 10 ms where it previously deadlocked, and
the boot is otherwise unchanged: `WID Result Failed` 0, `FirmWareVer:0x10202`,
`wland_bus_start Done(ret=0)`.

So stage 4 now has: chip init, `wlan0` up, and TX flowing down the SDIO path.

### What blocks the definition of done

§8 wants WPA2 association plus DHCP, and that needs a supplicant. Neither is
reachable from the BSP console today:

- the shell has only `/sbin/ifconfig` and `/bin/busybox` — no `iw`, no
  `wpa_supplicant`;
- `CONFIG_CFG80211_WEXT` is **not set**, so busybox's `iwlist`/`iwconfig`
  cannot substitute — they speak wireless extensions, and this driver is
  nl80211-only by design (§16 dropped `wland_iw.c`).

Two ways forward, and they answer different questions:

1. **Deploy the `pvwificonnect` container** (already built as
   `pvwificonnect-orangepi-i96.rootfs.ext4.gz`). This is the product path and
   the way WiFi is meant to be provisioned on a pantavisor device.
2. **Add `iw` to the BSP image** for bench work. Cheaper to iterate with, and
   it isolates driver behaviour (scan results, association) from the
   container plumbing — worth having while the driver is still unproven.

(2) first is the better debugging order: if `iw dev wlan0 scan` does not
return APs, the container would only add a layer of indirection over the same
failure.

---

## 22. STAGE 4 COMPLETE — WPA2 association and DHCP (2026-07-28)

§8's definition of done is met. The board associates with a WPA2 network and
gets a DHCP lease.

```
*AO MayThe4thBeWithUs   wifi_6eabbfc22ceb_4d61795468653474684265576974685573_managed_psk

wlan0  Link encap:Ethernet  HWaddr 6E:AB:BF:C2:2C:EB
       inet addr:192.168.68.132  Bcast:192.168.68.255  Mask:255.255.255.0
       inet6 addr: fe80::6cab:bfff:fec2:2ceb/64 Scope:Link
       UP BROADCAST RUNNING MULTICAST  MTU:1500
```

`*AO` is connman for favourite / Associated / Online. Before that, the first
successful scan on this board on mainline returned ten real APs, all under
the station MAC the driver reported.

### The tooling that made it reachable

The BSP console shell has no `iw` and no `wpa_supplicant`, and
`CONFIG_CFG80211_WEXT` is off so busybox's `iwlist`/`iwconfig` can never
substitute. The tools live in the containers, reachable with:

```
pventer -c <container> [CMD ...]     # no CMD enters an interactive shell
pventer -c os connmanctl technologies|scan wifi|services
```

Non-interactive `pventer -c os connmanctl <subcmd>` scripts cleanly over the
serial console. Joining a network without an interactive agent is easiest
through a connman config file:

```
pventer -c os sh -c 'printf "[service_home]\nType=wifi\nName=<SSID>\nPassphrase=<psk>\n" \
    > /var/lib/connman/home.config'
```

connman picks it up and associates within a few seconds.

Set `echo 0 > /sys/module/rdawfmac/parameters/wland_dbg_level` before any
console work — at level 5 the trace floods the port and makes output
unreadable.

### Known limitation: WiFi does not survive a warm reboot

After a soft reset — `reboot -f`, pantavisor's own reboot, or `reset` at the
u-boot prompt — SDIO comes back as:

```
mmc1: error -110 whilst initialising SDIO card
mmc1: Failed to initialize a non-removable card
```

Cold power-on works every time. The soft reset resets the SoC but **not** the
RDA5991, which sits behind its own supply and is left mid-session with no
clean re-initialisation, so `wland_combo`'s power-on tables run against a chip
that is already "on" and in an unknown state.

Worth noting the u-boot half of patch 32 is confirmed by the same test:
`reset` at the `=>` prompt prints `resetting .` and comes back through the
U-Boot banner into Linux.

Likely fix, for the next session: drive a real off→on transition of the combo
chip on init rather than assuming it is unpowered — the vendor sequence has a
`wifi_power_off` that our probe path does not call first. §14 already
exercised an OFF→ON transition by hand, so the pieces exist.

### Cold-boot follow-up: two things still in the way (2026-07-28)

A cold power cycle after the warm-reboot failure brings SDIO and `wlan0`
straight back, which confirms the `-110` in the previous section is specific
to the soft-reset path and not a latent enumeration problem.

Two issues showed up on that boot, neither of them blocking the §8 result
above but both blocking anything unattended:

**1. The association does not come back by itself.** After the reboot the
interface has `inet addr:169.254.224.21` — IPv4 link-local, connman's
fallback when nothing was joined. The most likely cause is that
`/var/lib/connman/home.config` was written into the `os` container's writable
layer and did not survive, but that is **not verified** — the board rebooted
again before it could be checked. First thing to confirm next session:

```
pventer -c os ls -l /var/lib/connman/
```

If the file is gone, the config belongs somewhere pantavisor persists, or the
network should be provisioned through pvwificonnect rather than by hand.

**2. The station MAC is random and changes on every boot.**

```
boot A:  HWaddr 6E:AB:BF:C2:2C:EB
boot B:  HWaddr 72:1F:DF:D8:78:62
```

This is the `nvram:can not get wifi mac from nvram` path from §16 falling back
to `get a random ether address`, which has been in every log since the driver
first bound and was easy to ignore while nothing associated. It is not
cosmetic now: a changing MAC breaks DHCP reservations, changes the connman
service identity between boots, and defeats any MAC-based ACL on the AP.

The vendor reads the MAC over msys from the modem's nvram, which we do not
have. The options are a MAC derived from something stable on the board (the
SoC chip id is readable), or one stored in the boot partition and passed in.
Either way it should be settled before this is considered usable.

Also worth knowing for bench work: the BSP shell has no `ping`.

---

## 23. Next steps

Stage 4 is functionally complete (§22): the board scans, associates with
WPA2 and takes a DHCP lease. What follows is what stands between that and
something shippable, in the order it is worth doing.

### A. Blocking — WiFi is not yet usable unattended

- [x] **A1. ~~Confirm why the association does not persist.~~ RESOLVED — it
      does persist.** The premise was wrong: `/var/lib/connman` is a
      dedicated persistent volume, `home.config` survived, and the board
      re-associates unattended at every boot. The 169.254 sighting in §22 was
      an observation made before association completed. See §24.

- [x] **A2. ~~Give the chip a stable MAC address.~~ DONE — patches 35-37
      (§28).** The address is now derived from the boot card's CID inside the
      driver, at `wland_bus_start` where the random one used to be picked, so
      it is per-board, stable across reboots and needs no provisioning.
      `rdawfmac.mac_addr=` pins one to a board instead. Note the doc's earlier
      suggestion to derive from the SoC `CHIP_ID` would have been wrong:
      `0x8810001c` is part number plus metal revision (§10 called it "metal id
      28"), identical on every RDA8810PL of that stepping — every board would
      have got the same MAC.

- [x] **A3. ~~Make WiFi survive a warm reboot.~~ DONE — patch 34 (§27).**
      `rda_combo_clients_ready()` now calls `rda_wifi_power_off()` before
      `rda_wifi_power_on()`, forcing a real OFF→ON instead of assuming the
      chip is unpowered. Confirmed across two consecutive `reboot -f` with
      no power cycle: `new SDIO card`, `wlan0` up and associated, zero
      `error -110`. Cold boot unaffected.

### B. Hardening and loose ends

- [x] **B1. ~~Provision WiFi the product way.~~ MOSTLY DONE — fixed upstream
      and verified here (§26).** The on-device tool is `pvwificonnect-cli`
      (`pvwificonnect-client` is a *host-side BLE* tool and was never going to
      be in the container). Its three §24 defects are fixed in pvwificonnect
      **v1.8.0**, which this layer now builds; `connect`, `stored` and
      `connect --stored` were all confirmed on hardware. `home.config` is no
      longer the only route.
      **Remaining:** the `network` block in config.json (the fourth fix) is
      shipped but never exercised — testing it needs the SSID/passphrase in
      `pvwificonnect/pvwificonnect-config/var/pvwificonnect/config.json`
      plus a rebuild, since `/var/pvwificonnect` in the container is on the
      ephemeral LXC overlay upper, not a persistent volume. It matters more
      than a loose end: it is SSID-keyed and replayed on every `Init`, so it
      is what makes provisioning survive the per-boot random MAC (§25) — i.e.
      the practical workaround for A2.
- [ ] **B2. Kernel-side pinctrl.** The five AP pad registers still live in
      u-boot (§15). Mainline has no RDA pinctrl driver; a DT pinctrl driver
      is the correct home and removes the dependency on our bootloader.
- [x] **B3. ~~Stop shipping stale bootloader blobs.~~ DONE.** Both blobs were
      confirmed stale — **0/5** pad constants each, `bootloader-hybrid.rda`
      (tracked, Jun 20) and `bootloader-hybrid-debuguart.rda` (untracked,
      Jul 22), against a pad map that landed Jul 24. The tracked one was
      unreferenced by any recipe or script, so it is deleted;
      `rda8810-spl/*.rda` is now gitignored, and `mk-sd-image.sh` documents
      how to verify a blob and how to lift a known-good one out of a working
      image. `u-boot-spl.bin` stays — it is the SPL half and is used by the
      documented `build-rda8810-spl.sh` procedure.
      Still open if the blob should be produced by the build rather than by
      hand: `package-bootloader.sh` wants the vendor `mkrdaimage.sh` it does
      not need, since the layout is plain concatenation.
- [ ] **B4. Fix the `rtnl_is_locked()` guard in `wland_del_if()`.** It asks
      "is anyone holding the RTNL", not "am I", so it is wrong under
      concurrency. Teardown-only today, but it is the same class of bug as
      §21 and will bite eventually.
- [ ] **B5. Console baud.** 921600 is the outlier; the rest of pantavisor is
      115200. Three places must change together — see §16. Deliberately
      deferred while the level-5 trace was needed; that need has now passed.
      No longer blocks tooling: `pvr device tty` hardcoded 115200 and so
      reported "no debug shell" on this board, making every tty subcommand
      unusable; it now takes `-b/--baud`, so
      `pvr device tty -d /dev/ttyUSB3 -b 921600 run "CMD"` works (§26).
      Still worth doing for consistency with every other pantavisor board.

### C. Driver features deliberately not ported (§16)

Only worth revisiting when something asks for them:

- [ ] **C1. `mgmt_tx`** — needed for 802.11w MFP (SA Query), 802.11r FT, WNM.
      Fine without it for WPA2-PSK on a full-MAC part.
- [ ] **C2. Scheduled scan** — wpa_supplicant falls back to normal scans.
- [ ] **C3. Averaged RSSI** — currently instantaneous; the averaging cache
      lived in the `wland_iw.c` that was dropped.

### D. Project

- [ ] **D1. Extract to `pantacor/meta-orangepi-i96`.** The split was agreed
      once WiFi worked; that condition is now met.
- [ ] **D2. Upstream what can go upstream.** Patch 32
      (`power: reset: rda8810pl`) is written against mainline style and
      marked `Upstream-Status: Pending` — it is the one piece of this work
      with a real path to mainline. The rdawlan driver is not.

---

## 24. A1 answered: the association *does* persist (2026-08-03)

Run on hardware over `/dev/ttyUSB3` at 921600. The board was already up when
the session started, on a boot nobody had touched since §22.

### The result

`wlan0` was associated and routing, unattended, with no intervention:

```
wlan0  Link encap:Ethernet  HWaddr DA:83:07:A5:CC:9C
       inet addr:192.168.68.142  Bcast:192.168.68.255  Mask:255.255.255.0
       UP BROADCAST RUNNING MULTICAST  MTU:1500
       RX packets:773  TX packets:375
```

The daemon log shows it happening at boot, 2m37s in:

```
Successfully retrieved saved networks: [MayThe4thBeWithUs]
Waiting for wifi to be connected...
Received signal: WiFi is connected.
WiFi connection check result: connected=true
WiFi is already connected, skipping AP startup
```

### Why §22 concluded the opposite

`/var/lib/connman` is **a dedicated persistent volume**, not the container's
writable layer:

```
/volumes/os/dockerovl--var-lib-connman on /var/lib/connman type overlay (rw,...)
```

`home.config` — the file hand-written in §22 — was still there, byte for
byte. The §23 hypothesis ("the writable layer is not persisted") was wrong,
and so was the plan built on it.

What actually happened in §22 is that the check was made too early. The same
false negative reproduced here: an `ifconfig wlan0` at ~2m00s uptime showed
`BROADCAST MULTICAST` and no address; the association landed at 2m37s. §22's
`169.254.224.21` was connman's link-local placeholder *during* association,
not its fallback after failing to associate.

**Method note, worth more than the result:** the doc committed to a diagnosis
from one observation, wrote it up as two hypotheses, and put the wrong one
first. It also recorded — correctly — that the board rebooted before the
check could be repeated. That caveat was the honest part and it was ignored
by the next reader. A single reading of an asynchronous system is a sample,
not a measurement.

### The MAC changes nothing about reconnection

The prediction that a per-boot random MAC would break reconnection — via
connman's `wifi_<MAC>_<ssid-hex>_<security>` service identity — was **wrong**.
`home.config` is a connman *provisioning* file matched on `Name=` (the SSID);
connman re-derives a MAC-keyed service from it on every boot. Reconnection is
MAC-independent.

What the changing MAC actually costs is visible in the same directory:

```
wifi_16e0a901118c_4d61795468653474684265576974685573_managed_psk
wifi_6eabbfc22ceb_4d61795468653474684265576974685573_managed_psk
wifi_721fdfd87862_4d61795468653474684265576974685573_managed_psk
wifi_da8307a5cc9c_4d61795468653474684265576974685573_managed_psk
```

Four directories, one network, one per boot — unbounded growth, plus the
DHCP/ACL/identity problems already in A2. Still worth fixing; not a blocker.

### pvwificonnect v1.7.0 cannot provision from the CLI

The intended test was to provision through pvwificonnect rather than by hand.
That is not possible on this version. `scan`, `status` and `ap-status` all
work — so the driver, connman and the D-Bus path are all fine — but `connect`
fails after exactly 90 s:

```
dbus.go:164: ConnectWiFiNetwork: not proceeding — timed out after 1m30s waiting for Init to finish
```

Three defects, all read from the shipped source under
`sysroots-components/cortexa5t2hf-neon/pvwificonnect-app/.../pvwificonnect/`:

1. **`connect` can never succeed.** `ConnectWiFiNetwork` gates on
   `waitForInit(90s)`, which blocks on the `initDone` channel closed only by
   `signalInitDone()` in `Init()`. `Init()` has exactly one non-vendor caller:
   `main.go:55`, in the **daemon**. `grep -rn "Init" cmd/pvwificonnect-cli/`
   returns nothing — the CLI builds its own `ConnmanDbus` whose `initDone`
   nothing will ever close. Deterministic 90 s timeout, on any board.
2. **`connect --stored` lies.** `ConnectToStoredWiFiNetwork` is a stub:
   `fmt.Println("not implemented ...")` then `return nil` — reports success
   having done nothing.
3. **The `network` block in `config.json` is dead.** `Network *WifiConf` is
   declared in the config struct and documented in the upstream README, but
   no non-vendor code reads it.

Also inconsistent: the CLI's `stored` printed `No stored WiFi networks found`
while the daemon's `SavedNetworks()` returned `[MayThe4thBeWithUs]` and the
CLI's own `scan` showed that SSID as `SAVED true`.

Consequence for this board: with no Bluetooth (`BLE: BlueZ is not available
on D-Bus … BLE provisioning disabled`), the host-side BLE `pvwificonnect-client`
is unavailable too, so the daemon's HTTP `/connections` endpoint and
`home.config` are the only provisioning routes. Worth reporting upstream —
none of it is i96-specific.

### Two behaviours worth knowing on this driver

- **Scanning drops the association.** Running `pvwificonnect-cli scan` while
  connected produced `Watcher: confirmed disconnect (Connected=false, no
  bound IP)`. The daemon's watcher recovered it 75 s later by toggling the
  technology (`Watcher: WiFi reconnected successfully after 2 attempt(s)`).
  The watcher works; scans are not free.
- **The console shell reboots the board.** An idle shell prints `System will
  reboot in 60 seconds`. `pvcontrol cmd defer-reboot 3600` holds it off, and
  bench sessions need it before anything long-running.

### Bench notes

- Console is `/dev/ttyUSB3` at **921600**, `raw -echo clocal`; §18's
  `stty` + background `cat` + `printf` method still works and is still
  preferable to holding the port with picocom.
- `wland_dbg_level=5` is on the boot cmdline; `echo 0 >
  /sys/module/rdawfmac/parameters/wland_dbg_level` first or the port floods.
- Lines much over ~70 characters get mangled on the way in — there is no
  RTS/CTS. Keep commands short; long pipelines silently lose characters.
- Containers are `os` (= alpine-connman), `pvwificonnect`, `pvr-sdk`,
  `pv-avahi`, `pv-avahi-browse`, listed by `ls /pv/logs/current/`.
- `pvr-sdk` has `curl` and `wget`; the `pvwificonnect` container has neither,
  and the BSP shell has no `wc`, `ping` or `iw`.
- The useful logs are `/pv/logs/current/pvwificonnect/lxc/console.log` (the
  daemon's own output — this is what answered A1) and
  `/pv/logs/current/os/pvwificonnect-dbus`.

### Revised next steps

A1 is closed. The blocking list is now A2 (stable MAC) and A3 (survive a warm
reboot); A3 is the one that still makes the board need a human. **Do not
`reboot` this board from a remote session** — §22's `-110` SDIO failure leaves
no wlan0 until someone power-cycles it by hand.

---

## 25. The MAC *does* break reconnection — on the product path (2026-08-03)

§24 said "The MAC changes nothing about reconnection". That is true only for
the hand-written `home.config` route, and stating it unqualified was wrong.
Tested on a freshly flashed card, provisioning through `pvwificonnect-cli`
instead:

1. Provisioned on boot A (MAC `F6:65:4E:BA:DE:CA`) — associated,
   `192.168.68.143`, DHCP lease, traffic both ways.
2. Cold power cycle. New MAC `DA:E1:85:93:66:33`.
3. Boot B came up **unassociated** — `wlan0` UP but not RUNNING, no IPv4,
   `TX 82 / RX 0` (probing, nothing joined).

The stored profile survived on disk:

```
# pventer -c os ls /var/lib/connman/
settings
wifi_f6654ebadeca_4d61795468653474684265576974685573_managed_psk
```

…keyed to boot A's MAC, and **no `home.config`** — because the CLI does not
write one. `GetStoredWiFiNetworks` walks connman's *current* services and
filters on `Saved`, so an entry keyed to a MAC no interface has any more is
invisible: `stored` reported "No stored WiFi networks found" while that
directory sat on disk. Orphaned, not lost.

So the two provisioning routes behave differently:

| Route | What it writes | Survives a MAC change? |
|---|---|---|
| hand-written `home.config` | SSID-keyed *provisioning* file, re-applied by connman every boot | **Yes** (§24) |
| `pvwificonnect-cli connect` | MAC-keyed connman *service* only | **No** |

**A2 (stable MAC) is therefore a hard blocker for A1 on the product path**, not
the cosmetic "one leaked directory per boot" §24 downgraded it to. A device
provisioned the product way does not come back after a reboot.

The `network` block in config.json (§24's upstream bug 4, now fixed) is the
product-path equivalent of `home.config` — SSID-keyed and re-applied by `Init`
each boot — so it is also the practical workaround until A2 is done.

### Method note

Both §24's over-correction and the original §22/§23 error came from the same
habit: generalising from one provisioning route to "reconnection" as a whole.
The mechanism (SSID-keyed provisioning file vs MAC-keyed service) was
observable in `/var/lib/connman` the whole time; nobody looked at *which kind*
of entry each route produced.

### Bench note: never leave more than one reader on the port

Four concurrent `cat /dev/ttyUSB3` readers had accumulated, silently splitting
the byte stream — that is what turned `cat /pv/device-id` into `{wgevmce-id`
and swallowed whole command results. Symptoms look like a flaky board. Check
with `ps -eo pid,args | grep '[c]at /dev/ttyUSB3'` before believing any weird
console output. Detached readers also do not reliably survive between tool
invocations here; opening the port once per command, reader started *before*
the write, is what works:

```
timeout N cat /dev/ttyUSB3 > out & sleep 0.4; printf 'CMD\r\n' > /dev/ttyUSB3; wait
```

Long-running device commands must be issued and captured inside a *single*
window — output produced between two capture windows is simply lost.

Also: `pvcontrol cmd defer-reboot 7200` silently does nothing (no confirmation
line). `3600` works and echoes "shell timeout deferred to 3600 seconds". The
console shell reboots the board on idle, and that reboot is warm, so it takes
WiFi down until a cold power cycle (A3).

---

## 26. pvwificonnect v1.8.0 verified; bench tooling fixed (2026-08-04)

### The upstream fixes shipped and were confirmed on hardware

§24's three defects (plus a fourth found while fixing them) are released as
pvwificonnect **v1.8.0**, and this layer now builds it — recipes renamed to
`*_v1.8.0.bb`, `src` `0582c32b…`, `vendor` `cc1ec806…`.

Fixed:

1. `ConnectWiFiNetwork` gated on `waitForInit`, whose `initDone` channel is
   closed only by `Init()` — called solely from the daemon's `main.go`. A CLI
   process therefore always burned the full 90 s timeout. `SetStandalone()`
   releases the gate for processes that never run `Init`.
2. `ConnectToStoredWiFiNetwork` was a stub printing "not implemented" and
   returning nil, i.e. reporting success. Now resolves the service from a
   warmed `netCache`, verifies the SSID is a favourite, and errors otherwise.
3. `GetStoredWiFiNetworks` built its list then `return nil, nil`.
4. The `network` block in config.json was declared and documented but read by
   nothing; `Init` now joins it before falling back to the AP.

Verified on a cold-booted board (MAC `42:4C:78:07:02:34`) running an image
built from the **published** tarballs, dev overlay disabled:

```
stored (fresh card)              -> No stored WiFi networks found.      baseline
connect -s … -p …                -> Successfully connected; 192.168.68.146   (1)
stored                           -> Stored WiFi networks: 1. MayThe4th…      (3)
connect -s NoSuchNet --stored    -> Error: no stored network named "NoSuchNet" (2)
connect -s MayThe4th… --stored   -> Successfully connected                   (2)
```

Before flashing, the arm32 binary was extracted from the image's own squashfs
object (via `/trails/0/.pvr/json` → `/objects/<sha256>` → `unsquashfs`) and
checked for the new strings — the code that boots, not the build log.

**Fix 4 is still unexercised**, and it is the one that matters most here: see
B1.

### `pvr device tty` now takes `-b/--baud`

The serial speed was hardcoded to 115200 in three places (the `serialProbe`
termios, the `stty` call in `interactive`, and the connect banner), so this
board at 921600 reported `no Pantavisor debug shell` — indistinguishable from
a board that is off or hung — and every tty subcommand was unusable against
it. Added in pvr `feat(device): add --baud flag to device tty`. Default stays
115200, unsupported rates are rejected up front with the valid list.

```
pvr device tty -d /dev/ttyUSB3 -b 921600 run "CMD"
```

This replaces the manual `stty`/`cat`/`printf` dance in §18/§25 and is much
more reliable. Two caveats:

- `runCommands` has a **10 s per-command deadline**. Anything slower (any
  `pvwificonnect-cli connect`/`stored`) prints "timeout waiting for response"
  yet still completes on the device. Robust pattern: redirect on the device
  (`… >/tmp/x.log 2>&1`) and grep the log in a second call.
- Only one process may hold the port. A stale `cat` left over from manual
  capture makes pvr report "no debug shell"; check `fuser -v /dev/ttyUSB3`
  and kill by PID before suspecting the board. That same stale reader had
  been silently eating command output during earlier manual testing.

---

## 27. A3 SOLVED — WiFi survives a warm reboot (2026-08-04, patch 34)

`reboot -f` no longer kills WiFi. The chip enumerates and associates without
anyone touching the power.

### The mechanism, and the control that proved it

The RDA5991 sits behind its own supply and is not reset by an SoC soft reset.
After a warm reboot the SoC restarts but the chip is left powered and
mid-session, while probe treats it as cold and runs the power-on tables
against that state. Every step reports success — the I2C slave answers
throughout, which is exactly why this was mistaken for a marginal
enumeration problem — but the chip never answers CMD5.

Proven on the bench before the patch was written, on a board whose SDIO was
already dead from a warm reboot. **Control first**, to rule out the rebind
itself being the cure:

```
echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/unbind
echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/bind
  -> mmc1: Failed to initialize a non-removable card        (still dead)

echo 0 > /sys/bus/i2c/devices/0-0016/wifi_power
echo 1 > /sys/bus/i2c/devices/0-0016/wifi_power
  -> rda_5991g_wifi_power_off succeed!!
  -> rda_5991g_wifi_power_on write control_mode_disable succeed!!
<same rebind>
  -> mmc1: new SDIO card at address 4829                    (recovered)
```

`wlan0` did **not** return in that hand-driven case, because rdawlan has
already unregistered itself by ~12.6 s (§18). That is not a failure of the
hypothesis — it is why the transition has to happen at init, while the
driver is still present to bind the card.

### The patch

Patch 34 adds `rda_wifi_power_off()` ahead of `rda_wifi_power_on()` in
`rda_combo_clients_ready()`.

**No settle delay**, deliberately. The bench proof happened to have seconds
between the two halves (two separate shell commands), so a delay was the
tempting thing to add. Instead the vendor's own idiom decided it:
`rda_5990_wifi_power_on()` pairs `power_off()` with an immediate `goto
_retry` and no delay. Following the vendor sequence beats inventing a
timeout — see [[rda8810-apbi-reset-poison]] for the other direction of the
same lesson. Two boots confirm no delay is needed; had one been added
blindly it would have looked equally "working" while hiding whether it
mattered.

### Confirmed on hardware

Cold boot, unchanged (the off half is a no-op on an already-off chip):

```
[   1.000000] rda_5991g_wifi_power_off succeed!!
[   2.440000] mmc1: new SDIO card at address 4829
```

Then two consecutive `reboot -f`, no power cycle at any point:

```
[   0.990000] rda_5991g_wifi_power_off succeed!!
[   2.380000] mmc1: new SDIO card at address 4829
lo  lxcbr0  sit0  wlan0
wlan0  inet addr:192.168.68.147
```

`dmesg | grep -c 'error -110|Failed to initialize'` → **0** on both.

The second reboot was run specifically because this failure had looked
intermittent before; one green boot would not have settled it.

### Consequence for the rest of the list

A3 was the item that made the board need a human. With it closed, **A2
(stable MAC) is the only remaining blocker** — and note the two interact:
patch 32 made `reboot` work, patch 34 makes WiFi survive it, but a
CLI-provisioned network still will not rejoin afterwards because its ConnMan
service is keyed to the previous boot's MAC (§25). Unattended operation
needs A2, or the config.json `network` block (B1) as the SSID-keyed
workaround.

---

## 28. A2 SOLVED — a stable per-board MAC (2026-08-05, patches 35-37)

The board keeps its address across reboots, and every board gets a different
one. With §27's warm-reboot fix and pvwificonnect v1.8.1, a provisioned
network now survives a reboot unattended — the thing §22 set out to check.

### Where the address has to be decided, and why

`wland_bus_start()` calls `wlan_read_mac_from_nvram()`, which looks for
`/data/misc/wifi/WLANMAC` — an Android-era path absent on a Pantavisor rootfs
— then falls through to `eth_random_addr()`. Its attempt to write the address
back for next boot fails for the same reason. Hence a fresh MAC every boot,
which orphans ConnMan's saved services (keyed `wifi_<mac>_<ssid>_<security>`),
breaks DHCP reservations and MAC ACLs, and leaks a profile directory per boot.

**Setting it from userspace does not work, and looked like it did.** An
early-spawn hook running `ifconfig wlan0 hw ether` was tried first: the driver
implements `ndo_set_mac_address`, the hook reported success, and two
consecutive boots showed the same address. That was luck. The driver owns the
address from `bus_start` onward and re-applies it when the interface is
opened, so the hook races it — measured on the same image, the hook landed at
5.98s on one boot (silently clobbered back to a random address) and at ~11s on
another (survived). Timestamps across boots: 5.96, 10.94, 10.99, 5.98s. Two
green boots were not evidence; the question was never "does it stick once" but
"does it survive the interface being brought up".

### Getting the CID to the driver

The one per-unit value available at 3.4s is the boot card's CID. Reaching it
took a detour worth recording:

- The WiFi SDIO card exposes **no** `cid`/`serial` — only `power, rca,
  removable, revision, subsystem`. The chip has no unique id of its own.
- `mmc_bus_type` is `static` in `drivers/mmc/core/bus.c` and appears in no
  public header, so a wireless driver cannot walk the MMC bus. This led to an
  initial (wrong) conclusion that the CID was unreachable in-kernel.
- It is reachable: `struct mmc_host` has a public `struct mmc_card *card`, and
  `mmc_card` exposes `u32 raw_cid[4]` in `linux/mmc/card.h`. **rda-mmc drives
  every controller on this SoC**, so it can see the boot card even though the
  wlan driver cannot. Wrong access path, not a missing capability.
- The host is recovered with `mmc_from_priv()`. The first cut used
  `priv->mmc`, which oopsed: `struct rda_mmc_host` declares that field but
  **nothing in the driver ever assigns it** — the only reference was the new
  one. `NULL pointer dereference at 00000290`, `ldr r3,[r1,#12]` then
  `ldr r3,[r3,#0x290]`, i.e. `priv->mmc` then `host->card`.

Patch 36 exports the CID (`include/linux/rda-mmc.h`), patch 37 derives from
it, patch 35 adds `rdawfmac.mac_addr=` to pin one:

```c
mac[0] = 0x02;                    /* locally administered, unicast */
mac[1] = (cid[0] >> 16) & 0xff;   /* OEM byte */
mac[2] = (cid[2] >> 16) & 0xff;   /* ── product serial ── */
mac[3] = (cid[2] >>  8) & 0xff;
mac[4] = (cid[2]      ) & 0xff;
mac[5] = (cid[3] >> 24) & 0xff;
```

Order: `mac_addr=` → CID → the original nvram/random path.

**Do not bake a constant into the image.** That was proposed and rejected:
every device flashed from that image would share one MAC, which is worse than
randomising. And the host cannot derive it at flash time either — through a
USB reader the card is mass-storage with no CID exposed; the CID is only
visible from a real MMC host.

### Confirmed on hardware

```
[3.36] derived mac 02:53:79:78:01:5b from the boot card CID
wlan0  HWaddr 02:53:79:78:01:5B  inet 192.168.68.148
```

Same address across cold boot and `reboot -f`; `error -110` count 0. The
address is the card's serial `0x7978015b` (matching `.../serial`) with the OEM
byte and the `0x02` prefix — verified against
CID `0353445350333247807978015b017a62`.

Derivation now happens at **3.36s**, the same instant the driver used to call
`eth_random_addr()`. There is no longer a window in which anything else could
set or clobber it. That, not the address value, is the fix.

### The end-to-end result

On the local-source build: provisioned, `reboot -f`, and the device **rejoined
unattended ~135s later** with no commands issued — the first time provisioning
has survived a reboot on this board. Re-verified on the published v1.8.1
build (workspace overlay off, recipes at v1.8.1): same derived MAC, same
lease, association on a freshly flashed card with an empty `/var/lib/connman`
— the exact case that used to fail.

### Bench notes

- **`pvr device tty` gained `-b/--baud`** (§26) and now ships in `/usr/bin/pvr`.
  Its probe sends `\r\n`, which **reopens the pantavisor debug shell**, so
  shell state does not survive between `run` invocations — a variable set in
  one call is gone in the next.
- `pvcontrol cmd defer-reboot` fails with `ERROR: /pantavisor/pv-ctrl not
  found` if issued before pantavisor's control socket is up. It reports
  nothing useful, the deferral silently does not happen, and the board reboots
  ~5 min later taking the containers with it — which presents as
  `no container found: pvwificonnect` on a later command.
- Probing the port during power-on lands in u-boot's autoboot window and
  leaves the board at `=>`. Watch passively while a board is booting.
- The console serial number moves between `/dev/ttyUSB*` across
  re-enumeration (seen at ttyUSB1, 3 and 4 in one session). Identify it by
  `udevadm info -q property -n <port> | grep ID_SERIAL_SHORT` — the i96's
  FTDI is `A50285BI`.

### Bluetooth, for the record

BLE/Improv provisioning does not work here, but less is missing than expected:
BlueZ is **already shipped and running** in the `os` (alpine-connman)
container — `/usr/lib/bluetooth/bluetoothd`, enabled in the default runlevel.
It never claims `org.bluez` because there is no adapter: the kernel has no
Bluetooth at all (`CONFIG_BT` unset, no `/sys/class/bluetooth`, 0 BT
protocols). The combo driver's BT power sequences are already ported. So the
gap is `CONFIG_BT` plus an HCI transport for the RDA5991's BT half; userspace
is waiting for it. `pvwificonnect-cli improv-serial` needs no Bluetooth at
all and is present today, though untested and it would contend with the
debug console.

---

## 29. NEXT: Bluetooth / BLE provisioning — scoping (2026-08-05)

Not started. This section exists so the work can begin without re-deriving
what is already known. Everything below was measured on hardware unless
marked otherwise.

### What already exists

- **BlueZ is shipped and running.** `/usr/lib/bluetooth/bluetoothd` in the
  `os` (alpine-connman) container, enabled in the default runlevel, pid alive.
  It never claims `org.bluez` on the system bus — verified with
  `dbus-send --system --dest=org.freedesktop.DBus … ListNames`, zero matches —
  because there is no adapter to register. Userspace is waiting for hardware.
- **The combo driver already powers the BT side.** `rda_5991g_bt_power_on()` /
  `_off()` and the per-chip variants are ported (patch 17). Nothing calls them
  yet: `rda_combo_clients_ready()` only brings up WLAN.
- **The BT control clients are already in DT.** `bt_core: bluetooth@15` and
  `bt_rf: bluetooth@16` on i2c0, alongside the WiFi pair at 0x13/0x14.
- **pvwificonnect ships a BLE/Improv server** that auto-detects and disables
  itself: `BLE: BlueZ is not available on D-Bus … BLE provisioning disabled`.
  It should light up on its own once an adapter appears.

### What is missing

1. **`CONFIG_BT` and the HCI stack** — unset today. No `/sys/class/bluetooth`,
   and `/proc/net/protocols` lists zero BT protocols. Needs at minimum
   `CONFIG_BT`, `CONFIG_BT_BREDR`/`CONFIG_BT_LE`, and a transport
   (`CONFIG_BT_HCIUART` + a protocol such as H4/LL) in `rda8810pl.cfg`.
2. **An HCI transport driver / binding for the RDA5991 BT half.** Only
   `rdawlan` was ported. Nothing in tree speaks HCI to this chip.
3. **Which UART the BT half is on — LIKELY uart1, see below.**
   Facts: the board has three UARTs, all enabled in the DTS, with aliases
   `serial0 = &uart2` (@0x10000), `serial1 = &uart1` (@0),
   `serial2 = &uart3` (@0x90000). **`serial2`/uart3 is the debug console**
   (`stdout-path = "serial2:921600n8"`, `console=ttyRDA2`). So uart1 and uart2
   are the candidates. Confirm against the schematic
   (96boards docs repo, `orangepi_i96_v1_2-print.pdf`) and the vendor
   `tgt_ap_board_config.h` before writing any DT.
4. **Pinmux, almost certainly.** Every previous peripheral on this board needed
   pads switching out of GPIO mode — I2C1 (§10), SDMMC2 (§15, five registers).
   Assume the BT UART pads need the same treatment in
   `rda_combo_pinmux()` and check `BB_GPIO_Mode`/`AP_GPIO_*_Mode` against a
   running vendor system before assuming otherwise. §15's method — dump the
   pad registers from the working vendor image and diff — is what cracked SDIO
   and would likely crack this too.

### Vendor evidence (2026-08-05) — transport and UART identified

Fetched from `OrangePiLibra/OrangePi_i96_kernel@master`. This answers most of
unknown #1.

**Transport: generic `hci_uart` with H4. There is no RDA BT driver.**
`drivers/bluetooth/` in the vendor tree contains nothing RDA-specific, and the
vendor's own `.config` has:

```
CONFIG_BT=y            CONFIG_BT_HCIUART=y      CONFIG_BT_HCIUART_H4=y
CONFIG_BT_RFCOMM=y     CONFIG_BT_BNEP=y         CONFIG_BT_HIDP=y
CONFIG_BT_RANDADDR=y
```

All mainline except `CONFIG_BT_RANDADDR`, which is a vendor addition — likely
BD-address randomisation, i.e. the same class of problem as the WiFi MAC (§28).
Expect the BD address to need the same treatment; do not assume it is stable.

**UART mapping, from `arch/arm/mach-rda/devices.c`:**

```
 * use UART3 as default console
 * use UART1 as uart2
 * UART2 is for host interface, AP never use
```

- **UART3** = console. Matches ours exactly (`serial2 = &uart3`, ttyRDA2).
- **UART2** = modem/host interface. Not ours to use.
- **UART1** = the only remaining general-purpose UART, and the vendor registers
  it with **`.wakeup = 1`** — the giveaway for a BT UART.

Corroborating: `include/rda/tgt_ap_gpio_setting.h` defines
**`_TGT_AP_GPIO_BT_HOST_WAKE  GPIO_B1`**, so BT host-wake is wired on this
board.

**So the leading candidate is our `uart1` (@0x0, `serial1`, ttyRDA1)** — it is
already `status = "okay"` in the i96 board DTS. Strong inference, not proof:
the vendor never names a BT UART outright, so confirm against the schematic or
by attaching and seeing whether the chip answers.

The same file confirms this is the right board config — its SDMMC2 values
(`MAX_FREQ 20000000`, `MCLK_INV 1`, `MCLK_ADJ 3`) are exactly what §15 landed
on independently.

### Suggested order

1. Identify the BT UART from the schematic and the vendor board config.
2. Enable `CONFIG_BT` + `CONFIG_BT_HCIUART` and confirm `/sys/class/bluetooth`
   appears and `bluetoothd` claims `org.bluez` (it will still have no adapter).
3. Call `rda_bt_power_on()` from the combo driver, and check the BT side
   answers at all.
4. Attach the UART as HCI (`btattach`, or a serdev binding in DT) and look for
   `hci0`.
5. Only then expect `pvwificonnect`'s BLE server to enable itself.

### Cheaper alternative, if BLE is not specifically required

`pvwificonnect-cli improv-serial` implements the Improv Wi-Fi **serial**
protocol and is already on the device — no Bluetooth involved. Untested here,
and it would contend with the debug console on uart3 unless one of the free
UARTs is muxed for it. Worth 20 minutes before committing to the BT stack.

### Do not repeat

`pvwificonnect-client` (the host-side tool) is **BLE-only** and needs BlueZ on
the *host*; it is unrelated to whether the device has Bluetooth. That naming
already cost one dead end (§24).

---

## 30. Bluetooth: three of four gates pass, the chip does not answer HCI
    (2026-08-05, patch 38 + BT kernel config)

BLE provisioning is **not working**. Three prerequisites are now in place and
verified; the fourth — the chip actually speaking HCI — fails, and the cause is
not yet established. Recording precisely where it stops so the next attempt
does not redo this.

### What now works

| Gate | Result |
|---|---|
| BT half powered | `rda_combo 0-0016: bt powered on` at 2.55s |
| Kernel BT stack | `/sys/class/bluetooth` exists; `CONFIG_BT_HCIUART_H4=y` |
| `btattach` present | `/usr/bin/btattach` in the `os` container |
| UART node visible in `os` | `/dev/ttyRDA1` |
| **Chip answers HCI** | **NO — `Bluetooth: hci0: Opcode 0x0c03 failed: -110`** |

`0x0c03` is `HCI_Reset`; `-110` is ETIMEDOUT. Note that **`hci0` appearing
proves nothing**: `btattach` registers the interface first and the core then
times out talking to it. `ls /sys/class/bluetooth` showing `hci0` is not
success.

Two changes landed and are correct regardless of the outcome:
- **Patch 38** calls `rda_bt_power_on()` from `rda_combo_clients_ready()`. The
  sequence had been ported since the combo driver landed and **nothing had ever
  called it**, so the BT half had never been powered on this board.
- `CONFIG_BT` / `CONFIG_BT_HCIUART` / `CONFIG_BT_HCIUART_H4` (+ SERDEV,
  RFCOMM/BNEP/HIDP) in `rda8810pl.cfg`, mirroring the vendor.

### What was ruled out

- **Not SDIO.** The combo chip exposes only `mmc1:4829:1` — one function. The
  vendor config enables no `BT_HCIBTSDIO`/`BT_MRVL`. BT is a UART device.
- **Not a missing driver.** The vendor ships no RDA-specific Bluetooth code;
  `drivers/bluetooth/` in their tree is stock. `CONFIG_BT_HCIUART_H4` is the
  whole transport.
- **Not simply the wrong node.** Tried both non-console UARTs —
  `/dev/ttyRDA1` (hw uart1) and `/dev/ttyRDA0` (hw uart2). Identical `-110`.
- **Probably not baud.** 921600 fails earlier (`Failed to flush serial port:
  I/O error`), and a rate mismatch normally yields garbage rather than
  complete silence. Only 115200 and 921600 were tried.

### The open question

**Are the BT UART pads muxed?** This is the board's recurring failure mode —
I2C1 needed two bits (§10), SDMMC2 needed five whole registers (§15), and in
both cases the peripheral looked electrically dead until the pads were fixed.

The five pad values we write from u-boot were captured from a **running vendor
system with wlan0 up** (§15). Whether Bluetooth was also active on that system
is **unknown** — if it was not, its pads were in GPIO mode in that snapshot
too, and we copied that state faithfully. Current values, confirmed on the
board:

```
0x11a09008 BB_GPIO_Mode   = 0x7FE0003F
0x11a0900c AP_GPIO_A_Mode = 0x000210FC
0x11a09010 AP_GPIO_B_Mode = 0x3F00033F
```

### Next steps, cheapest first

1. **Boot the vendor image, start Bluetooth there, and diff the pad
   registers** against the values above. This is exactly the method that
   cracked SDIO in §15 and it answers the question outright. If BT works on the
   vendor system, its pad map is the answer; if BT does *not* work there
   either, that reframes the whole task.
2. Check the schematic (`orangepi_i96_v1_2-print.pdf`, 96boards docs) for which
   SoC pins the RDA5991 BT UART actually lands on — the vendor never names it,
   and `.wakeup = 1` on hw uart1 plus `_TGT_AP_GPIO_BT_HOST_WAKE GPIO_B1` is
   inference, not proof.
3. Consider that BT may be **modem-attached**. The vendor's own comment on the
   remaining UART is *"UART2 is for host interface, AP never use"*. If the BT
   controller hangs off the modem side rather than the AP, an AP-side HCI
   attach can never work, and this becomes a modem-stack problem like §1/§11
   was wrongly assumed to be for WiFi.

### If BLE is not specifically required

`pvwificonnect-cli improv-serial` needs no Bluetooth and is already on the
device. Untested. It would contend with the debug console on uart3 unless one
of the free UARTs is used.
