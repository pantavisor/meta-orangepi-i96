# Orange Pi i96 (RDA8810PL) — Modem / WiFi bring-up port plan

Status: **stage 2 solved (pinmux); stage 3: msys client implemented (§13), awaiting bench.**

- **Stage 2 (I2C control) — DONE.** Not a modem problem: two pinmux bits. See §10.
- **Stage 3 (SDIO data) — BLOCKED, cause identified §11.** The vendor u-boot loads
  a 2 MB `modem.bin` and starts the modem coprocessor *before* booting Linux. We
  never have. The RDA5991's I2C slave runs without it, but its digital core needs
  the modem-gated 26 MHz, so SDIO never answers. §1 was right about the modem —
  it was only wrong about which interface the modem gates.

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
