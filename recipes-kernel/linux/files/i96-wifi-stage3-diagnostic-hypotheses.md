# Orange Pi i96 (RDA8810PL) — Stage 3 (SDIO Data Path) Diagnostic Hypotheses & Source Reference Guide

## Overview & Current State

* **Target Device**: Orange Pi i96 (Unisoc / RDA8810PL SoC + RDA5991_G WiFi/BT combo chip)
* **Stage 2 Status**: **RESOLVED** — `rda_combo` I2C control bus is functional (`project_id 0x5991`, `chip_version 0x0047`).
* **Stage 3 Status**: **BLOCKED** — `mmc1` (`20a60000.mmc` / SDMMC2) fails to enumerate the SDIO card. SDIO commands (specifically `CMD5` `IO_SEND_OP_COND`) time out (`-ETIMEDOUT`), and no SDIO card is reported by the MMC core.

---

## Technical Hypotheses & Evidence Sources

Below are the 7 diagnostic hypotheses explaining why Stage 3 SDIO enumeration is failing, accompanied by exact source code citations, file locations, register offsets, and architectural rationale.

---

### Hypothesis 1: Probe Order / Timing Race Between `rda_combo` Power-On & `rda-mmc` Rescan

#### Why this happens
During Linux boot, `rda-mmc` registers the `mmc1` host controller (`mmc@60000`). The MMC core immediately triggers an initial `mmc_rescan()` work item which sends `CMD0` and `CMD5` (`IO_SEND_OP_COND`). If `rda_combo` (I2C power tables) or `rda-mdsys` (msys IPC for 26 MHz clock / `v_bt` rail) execute **after** `mmc1` has already finished its initial probe scan, `mmc1` sees a dark chip, sets `cmd->error = -ETIMEDOUT`, and marks the slot empty. The Linux MMC subsystem **never** re-scans a non-removable card slot automatically.

#### Source Evidence
* **Documentation Reference**: [MODEM-WIFI-PORT.md:L555-L565](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md#L555-L565) (Notes how `dmesg | tail` and host re-bind via `/sys/bus/platform/drivers/rda-mmc/unbind` are used to re-trigger card detection after power is stable).
* **MMC Driver Probe**: [rda-mmc-09-mmc-host-add-rda-micro-sd-mmc-driver.patch](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-09-mmc-host-add-rda-micro-sd-mmc-driver.patch) (`drivers/mmc/host/rda-mmc.c` calling `mmc_add_host()` which queues `mmc_rescan`).
* **CMD5 Timeout Patch**: [rda-mmc-20-mmc-rda-report-a-missing-response-as-a-timeout.patch:L35-L46](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-20-mmc-rda-report-a-missing-response-as-a-timeout.patch#L35-L46) (Explicitly handles missing `CMD5` response by marking `cmd->error = -ETIMEDOUT`).

#### Diagnostic Verification
Run a manual host re-bind via sysfs after system boot and `rda_combo` initialization:
```bash
echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/unbind
echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/bind
dmesg | tail -n 25
```

---

### Hypothesis 2: Missing Hardware Reset Pulse (`WIFI_REG_ON` / `WIFI_PWD_N` Toggle)

#### Why this happens
WiFi/BT combo chips (including RDA5990/RDA5991 family) feature an internal digital power manager and SDIO MAC state machine. Supplying LDO power (`v_bt` / 1.8V) and reference clock (26 MHz) is necessary, but the internal digital core expects a **LOW $\rightarrow$ HIGH pulse on `WIFI_REG_ON` / `WIFI_PWD_N`** *after* clock and power rails have stabilized. If the enable line is pulled high continuously during board boot up or never toggled, the SDIO MAC/PHY stays latched in reset.

#### Source Evidence
* **Vendor Kernel Power Driver**: `OrangePiLibra/OrangePi_i96_kernel` (`arch/arm/plat-rda/include/plat/rda_combo_power.h` and `drivers/net/wireless/rdaw80211/rdawlan/wland_power.c` `wifi_power_on()`).
* **Ported Combo Driver**: [rda-mmc-17-wifi-add-rda-combo-power-controller.patch:L435-L480](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-17-wifi-add-rda-combo-power-controller.patch#L435-L480) (`rda5990_wf_setup_A2_power()` which handles `gpio_wf_rst` and `gpio_wf_pwdn`).
* **Mainline Port Document**: [MODEM-WIFI-PORT.md:L505-L514](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md#L505-L514) (Section 12 notes: "the combo chip's 26 MHz may still be off even with the modem running, which would leave its digital core unclocked and its SDIO block mute while the always-on I2C slave answers fine").

#### Diagnostic Verification
Check DTS node `wifi` in `rda8810pl-orangepi-i96.dts` for `wifi_pwren` or `wifi_reset` GPIO definitions and verify whether toggling the reset pin LOW for 50 ms after `rda_combo` probe releases the SDIO core.

---

### Hypothesis 3: `SYS_PM_CMD_SET_LEVEL` (LDO Output Voltage) Missing in `msys` IPC

#### Why this happens
`SYS_PM_CMD_EN(v_bt=10)` instructs the modem coprocessor (`BB xcpu`) to set the enable bit for regulator rail #10 (`v_bt`). However, in the RDA PMU architecture, `SYS_PM_CMD_EN` only toggles enable/disable. The output voltage level is controlled by `SYS_PM_CMD_SET_LEVEL`. If `SYS_PM_CMD_SET_LEVEL` is not invoked for `v_bt`, the PMU LDO remains at 0V or sub-operational voltage, leaving the 1.8V SDIO IO ring unpowered while the I2C slave (powered by `v_i2c`) continues to work.

#### Source Evidence
* **Vendor Kernel Regulator Protocol**: `OrangePiLibra/OrangePi_i96_kernel` (`arch/arm/plat-rda/md_sys.c` `rda_msys_send_cmd()`, `arch/arm/mach-rda/regulator.c` `rda_regulator_set_voltage()`, `arch/arm/mach-rda/regulator-devices.c` where `v_bt` is mapped to `pm_id = 10`).
* **Kernel IPC Client Patch**: [rda-mmc-24-arm-dts-orangepi-i96-enable-mdsys-fix-ram-cma.patch](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-24-arm-dts-orangepi-i96-enable-mdsys-fix-ram-cma.patch) (Enabling `mdsys` mailbox node `mailbox@200000`).
* **Ported msys Driver**: Patch 25 (`rda_combo_power_main.c`), which issues `SYS_PM_CMD_EN(v_bt=10, 1)`, but does not currently issue `SYS_PM_CMD_SET_LEVEL(10, 1800)`.
* **Port Plan Document**: [MODEM-WIFI-PORT.md:L518-L528](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md#L518-L528) (Table of msys IDs: `SYS_PM_MOD = 0x2`, `SYS_PM_CMD_EN = 0x1001`, `SYS_PM_CMD_SET_LEVEL = 0x1002`, `v_bt = 10`).

#### Diagnostic Verification
Test sending `SYS_PM_CMD_SET_LEVEL` (`0x1002`) with voltage parameter `1800` (1800 mV) before `SYS_PM_CMD_EN` over `mdsys` sysfs debug interface:
```bash
echo "pm_level 10 1800" > /sys/devices/platform/200000.mailbox/cmd
echo "pm 10 1"       > /sys/devices/platform/200000.mailbox/cmd
```

---

### Hypothesis 4: `SYS_GEN_CMD_AUX_CLK` Parameter / Frequency Selection Mismatch

#### Why this happens
The 26 MHz reference clock for the RDA5991 combo chip comes from the RDA8810PL `AUXCLK` pin, controlled by the modem coprocessor over msys message `SYS_GEN_CMD_AUX_CLK`. In the vendor kernel, `apsys_enable_aux_clk()` formats a specific multi-word command containing clock source flags (`CLK_AUX_SOURCE_26M` vs `13M`, client mask). Passing a scalar `1` in `msys` may leave the clock gate open but connected to an un-clocked internal divider (0 Hz).

#### Source Evidence
* **Vendor Clock Driver**: `OrangePiLibra/OrangePi_i96_kernel` (`arch/arm/plat-rda/ap_clk.c` `apsys_enable_aux_clk()`).
* **Kernel Mailbox Implementation**: Patch 22 (`drivers/misc/rda-mdsys.c`) and Patch 25 (`rda_combo_power_main.c`).
* **Port Plan Document**: [MODEM-WIFI-PORT.md:L518-L525](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md#L518-L525) (`SYS_GEN_MOD = 0x0`, `SYS_GEN_CMD_AUX_CLK = 0x1005`, `SYS_GEN_CMD_CLK_OUT = 0x1004`).

#### Diagnostic Verification
Audit `apsys_enable_aux_clk()` in vendor 3.10 `ap_clk.c` to confirm whether `SYS_GEN_CMD_AUX_CLK` expects a source frequency mask (e.g. `0x1` for 26 MHz, `0x2` for 13 MHz).

---

### Hypothesis 5: SDMMC2 (`mmc1`) AP Pad Muxing & Pull-Up Configuration

#### Why this happens
The RDA8810PL pad multiplexer maps pins between GPIO and peripheral alternate functions. For SDMMC1 (SD card on `mmc0`), pins are mapped via `BB_GPIO_Mode` (`0x11a09008`). For SDMMC2 (`mmc1` / WiFi SDIO), pins are mapped via `AP_GPIO_A_Mode` (`0x11a0900c`) or `BB_GPIO_Mode`. If any SDMMC2 DAT line or CMD line pad has mode bit = 1 (GPIO mode) or has disabled pull-up resistors, `CMD5` will time out due to a floating CMD line.

#### Source Evidence
* **I2C Pinmux Precedent (Stage 2)**: [recipes-bsp/u-boot/files/rda8810-stage2/board/rda/rda8810pl/rda_combo.c:L37-L52](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-bsp/u-boot/files/rda8810-stage2/board/rda/rda8810pl/rda_combo.c#L37-L52) (Proves that pad mode bits coming out of reset defaulting to 1/GPIO broke I2C; fixing `AP_GPIO_B_Mode` `0x11a09010` resolved Stage 2).
* **SD Pad Mode Registers**: [MODEM-WIFI-PORT.md:L319-L326](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md#L319-L326) (`AP_GPIO_B_Mode` at `0x11a09010`, `BB_GPIO_Mode` at `0x11a09008`).
* **Vendor Pinmux Headers**: `OrangePiLibra/OrangePi_i96_uboot` (`tgt_gpio_setting.h` where `AS_ALT_FUNC = 0` and `AS_GPIO = 1`).

#### Diagnostic Verification
Compare physical pad register dumps between vendor Debian 3.10 and 6.6 kernel using `devmem`:
```bash
devmem 0x11a09000 32 # AP_GPIO_A_Mode
devmem 0x11a09008 32 # BB_GPIO_Mode
devmem 0x11a09010 32 # AP_GPIO_B_Mode
```

---

### Hypothesis 6: Initial Clock Frequency Divider Calculation (`< 400 kHz`)

#### Why this happens
During initial card discovery (`mmc_rescan`), the Linux MMC core requests an initial clock rate of $\le 400\text{ kHz}$. In `rda-mmc.c`, the host clock divisor is computed from the parent APB clock (200 MHz). A 200 MHz to 400 kHz step requires a divisor of 500. If the divisor calculation in `rda_mmc_set_ios()` overflows or truncates the `CLK_DIV` bitfield in register `SDMMC_CLKDB`, the actual output clock on the SDMMC2 CLK pin could exceed maximum limits (> 50 MHz) or output 0 Hz, causing `CMD5` to time out.

#### Source Evidence
* **RDA MMC Host Driver Patch**: [rda-mmc-09-mmc-host-add-rda-micro-sd-mmc-driver.patch](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-09-mmc-host-add-rda-micro-sd-mmc-driver.patch) (`drivers/mmc/host/rda-mmc.c` `rda_mmc_set_ios()` and `SDMMC_CLKDB` register formatting).
* **Missing Response Handling Patch**: [rda-mmc-20-mmc-rda-report-a-missing-response-as-a-timeout.patch:L8-L21](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-20-mmc-rda-report-a-missing-response-as-a-timeout.patch#L8-L21) (Discusses `CMD5` response handling and `ocr_avail` matching `MMC_VDD_32_33 | MMC_VDD_33_34`).

#### Diagnostic Verification
Log `ios->clock`, `master_clock`, and `CLK_DIV` register values in `rda_mmc_set_ios()` during `mmc1` probe to confirm the physical clock frequency is $\approx 400\text{ kHz}$.

---

### Hypothesis 7: Missing RDA5991 I2C Power/Init Sequence Prior to SDIO Probe

#### Why this happens
In the vendor wireless stack (`rdawlan` / `wland_power.c`), `wifi_power_on()` sends a multi-step sequence of 9 I2C register writes to the RDA5991 chip. These writes configure internal LDO regulators, enable internal PLLs, and configure SDIO pad drive strength. If `mmc1` probes before `rda_combo` has finished sending these 9 I2C writes, the RDA5991 internal SDIO MAC will not answer `CMD5`.

#### Source Evidence
* **RDA Combo Power Controller Patch**: [rda-mmc-17-wifi-add-rda-combo-power-controller.patch:L560-L580](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-17-wifi-add-rda-combo-power-controller.patch#L560-L580) (`rda_5991g_power_on_seq` table containing the 9 I2C register writes executed in `rda_combo_power_probe()`).
* **Kernel Log Proof**: [MODEM-WIFI-PORT.md:L360-L370](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md#L360-L370) (`[ 1.940000] rda_combo 0-0016: wifi powered on` confirming all 9 steps executed).

#### Diagnostic Verification
Ensure `rda_combo` completes all 9 steps *before* `mmc1` is bound, or manually trigger the `unbind`/`bind` cycle (Hypothesis 1) after `rda_combo` logs `wifi powered on`.

---

## Source Verification Matrix

| Hypothesis | Key Source File | Line / Location | Evidence Summary |
|---|---|---|---|
| **1. Timing Race** | [MODEM-WIFI-PORT.md](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md) | L555–L565 | sysfs unbind/bind re-triggers `mmc_rescan` post-boot. |
| **2. Reset Pulse** | [rda-mmc-17-wifi-add-rda-combo-power-controller.patch](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-17-wifi-add-rda-combo-power-controller.patch) | L435–L480 | `rda5990_wf_setup_A2_power()` toggles `gpio_wf_rst` / `pwdn`. |
| **3. Voltage Level** | [MODEM-WIFI-PORT.md](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md) | L518–L528 | `SYS_PM_CMD_SET_LEVEL` (`0x1002`) sets LDO voltage for `v_bt` (`pm_id=10`). |
| **4. Clock Payload** | Vendor `ap_clk.c` / [rda-mdsys.c](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-24-arm-dts-orangepi-i96-enable-mdsys-fix-ram-cma.patch) | Patch 22/24 | `SYS_GEN_CMD_AUX_CLK` (`0x1005`) enables 26 MHz AUXCLK. |
| **5. Pad Mux** | [rda_combo.c](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-bsp/u-boot/files/rda8810-stage2/board/rda/rda8810pl/rda_combo.c) | L37–L52 | `AP_GPIO_B_Mode` (`0x11a09010`) & `BB_GPIO_Mode` (`0x11a09008`). |
| **6. Clock Divisor** | [rda-mmc-20-...patch](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/rda-mmc-20-mmc-rda-report-a-missing-response-as-a-timeout.patch) | L8–L21 | `CMD5` timeout handling & init clock divider calculations. |
| **7. I2C Sequence** | [MODEM-WIFI-PORT.md](file:///home/sergiom/Code/pantacor/meta-pantavisor/recipes-kernel/linux/files/MODEM-WIFI-PORT.md) | L360–L370 | 9-step `rda_5991g_power_on_seq` must precede `mmc1` probe. |
