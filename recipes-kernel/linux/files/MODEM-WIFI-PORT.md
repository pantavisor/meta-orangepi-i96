# Orange Pi i96 (RDA8810PL) — Modem / WiFi bring-up port plan

Status: **scoped, not started.** This is the design/starting doc for the WiFi
bring-up effort. WiFi stages 1–2 (I2C + combo power driver) are already built and
merged into the kernel patch series (`rda-mmc-15..18`); they are blocked on the
one hardware fact captured here.

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

### A′. u-boot ISPI shortcut — *try this first, ~1 day*
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
- **First experiment:** port just the ISPI helpers + the vendor's `v_bt` LDO
  enable and `26M` enable into our u-boot `board_init`, boot our normal
  pantavisor image, check for `rda_combo … chip version 0x47`.

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

## 8. Definition of done
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
