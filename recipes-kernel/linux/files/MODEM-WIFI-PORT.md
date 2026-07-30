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
