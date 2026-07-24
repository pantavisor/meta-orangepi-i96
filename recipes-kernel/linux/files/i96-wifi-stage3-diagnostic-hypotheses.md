# Orange Pi i96 (RDA8810PL) — Stage 3 (SDIO Data Path) Resolution & Technical Reference

## Overview & Status

* **Target Device**: Orange Pi i96 (Unisoc / RDA8810PL SoC + RDA5991_G WiFi/BT combo chip)
* **Stage 2 Status**: **RESOLVED** — `rda_combo` I2C control bus is functional (`project_id 0x5991`, `chip_version 0x0047`).
* **Stage 3 Status**: **RESOLVED ON HARDWARE (2026-07-24)** — SDIO enumeration succeeded! `mmc1` (`20a60000.mmc` / SDMMC2) recognized the RDA5991_G chip, read the CIS tuples, and assigned card address 4829:
  ```text
  [ 359.910000] rda-mmc 20a60000.mmc: card claims to support voltages below defined range
  [ 359.910000] mmc1: queuing unknown CIS tuple 0x10 [07 00 11 00 8e] (5 bytes)
  [ 359.920000] mmc1: queuing unknown CIS tuple 0x10 [07 00 31 00 32] (5 bytes)
  [ 359.920000] mmc1: new SDIO card at address 4829
  ```

---

## Root Cause & Hardware Solution

Stage 3 blockage was caused by **missing pad multiplexing and RF GPIO configuration** in AP pad mode registers. While `BB_GPIO_Mode` bits 15..20 controlled SDMMC2 data lines, the RF control pins in `RF_GPIO_Mode` (`0x11a09018`) and middle bits in `AP_GPIO_B_Mode` (`0x11a09010`) were unconfigured (defaulting to GPIO mode/0x0), leaving the internal SDIO PHY disconnected.

### Required Pad Register Values (Verified on Hardware)

```bash
devmem 0x11a09008 32 0x7fe0003f  # BB_GPIO_Mode
devmem 0x11a0900c 32 0x000210fc  # AP_GPIO_A_Dir
devmem 0x11a09010 32 0x3f00033f  # AP_GPIO_B_Mode
devmem 0x11a09018 32 0x14040040  # RF_GPIO_Mode
devmem 0x11a0901c 32 0x006e4524  # Pad Config
```

---

## Verified Working Sequence

1. **Apply Pad Register Configuration**:
   ```bash
   devmem 0x11a09008 32 0x7fe0003f
   devmem 0x11a0900c 32 0x000210fc
   devmem 0x11a09010 32 0x3f00033f
   devmem 0x11a09018 32 0x14040040
   devmem 0x11a0901c 32 0x006e4524
   ```

2. **Power-Cycle WiFi via I2C**:
   ```bash
   echo 0 > /sys/bus/i2c/devices/0-0016/wifi_power
   echo 1 > /sys/bus/i2c/devices/0-0016/wifi_power
   ```

3. **Trigger MMC Host Re-bind**:
   ```bash
   echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/unbind
   echo 20a60000.mmc > /sys/bus/platform/drivers/rda-mmc/bind
   ```

4. **Expected Result (`dmesg`)**:
   ```text
   [ 359.910000] mmc1: queuing unknown CIS tuple 0x10 [07 00 11 00 8e] (5 bytes)
   [ 359.920000] mmc1: queuing unknown CIS tuple 0x10 [07 00 31 00 32] (5 bytes)
   [ 359.920000] mmc1: new SDIO card at address 4829
   ```

---

## Next Steps for Stage 4

Now that Stage 3 (SDIO data path) is **SOLVED**, the remaining task for complete WiFi functionality is **Stage 4**:
* Porting the `rdawlan` (`rdawfmac` cfg80211 SDIO driver) to Linux 6.6 so that `wlan0` netdev registers and WPA2 scanning/association functions.
