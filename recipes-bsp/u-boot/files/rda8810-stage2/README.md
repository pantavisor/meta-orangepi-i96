# RDA8810PL modern u-boot stage-2 (OrangePi i96)

## BUILD STATUS: compiles clean, recipe-reproducible
Verified: u-boot 2024.01 + these files + the recipe's exact hooks build a clean
`u-boot.bin` (~440 KB) with `arm-linux-gnueabi-gcc 13.2`, `CONFIG_TEXT_BASE=0x80008000`,
`CPU_V7A`, and all pantavisor cmds (`FIT`, `CMD_SETEXPR`, `CMD_EXT4`, `ENV_IS_IN_EXT4`).
`rda_serial_*` and `rda_mmc_*` link in. `package-bootloader.sh` wraps it in the exact
64-byte legacy header (load/entry 0x80008000) the vendor SPL expects.
Build: `make rda8810pl_orangepi_i96_defconfig && make CROSS_COMPILE=arm-linux-gnueabi-`.

Bench-gated only: rda_mmc.c data path (PIO attempt; vendor uses IFC DMA), set_ios clock
divider, console UART confirmation, and the SPL->stage-2 handoff on real silicon (M5).

---


Hybrid forward-port: the **vendor SPL** (built sig-off by `../build-rda8810-spl.sh`, M1)
does DDR + clock init and then loads this **modern u-boot stage-2** from SD offset
`0x32000`. Stage-2 exists purely to give pantavisor the bootloader features the 2012
vendor u-boot lacks: **FIT, `setexpr`+`gsub`, `env exists`, `saveenv`** (see
`../boot.cmd.pvgeneric`). DDR is already up, so stage-2 only needs console + MMC + the
modern command set.

## Hardware facts (gathered, mainline-confirmed)
- SoC: RDA8810PL, single Cortex-A5 (ARMv7-A). RAM @ `0x80000000`, size `0x10000000` (256 MB).
- Console UART = **uart3 @ `0x20a90000`** (mainline DT `stdout-path = "serial2:921600n8"`,
  `serial2 = &uart3`), 921600 8n1. Fallbacks: uart1 `0x20a00000`, uart2 `0x20a10000`.
- UART regs (from Linux `drivers/tty/serial/rda-uart.c`): `CTRL 0x00`, `STATUS 0x04`
  (RX count `[6:0]`, free TX slots `[12:8]`), `RXTX_BUFFER 0x08`.
  **TX ready** when `STATUS & (0x1f<<8)` is nonzero; then write the byte to `0x08`.
- intc @ `0x20800000`, timer block @ `0x20900000` (+`0x10000`).
- Bootloader layout in the SD gap (vendor `tools/mkrdaimage.sh`):
  `[48K SPL @0x20000][24K part table][stage-2 u-boot @0x32000]`.
- Vendor u-boot env load addrs: `kernel 0x84000000`, `initrd 0x85000000`, `modem 0x82000000`.

## RESOLVED
- **stage-2 `CONFIG_TEXT_BASE = 0x80008000`** (vendor `include/configs/rda_config_defaults.h`
  `CONFIG_SYS_TEXT_BASE`). The SPL copies u-boot to `TEXT_BASE-64` (eMMC `U_BOOT_DST`), i.e.
  it skips the 64-byte (`CONFIG_UIMAGEHDR_SIZE=0x40`) legacy header so u-boot lands at
  0x80008000 — hence package-bootloader.sh wraps u-boot.bin with `mkimage -T firmware`.

## OPEN hardware unknowns (must pin on the bench)
1. Confirm console UART is really uart3 @ 0x20a90000 (else try uart1/uart2 bases above).
2. **rda_mmc.c register/command/data logic** — the one real gap; transcribe from the vendor
   driver (see that file's header) and validate SD reads on hardware.
3. SPL image form fed to mkrdaimage (raw `u-boot-spl.bin` vs vendor `u-boot-spl.img` with
   its header) — confirm what the BootROM expects (sig is off either way).

## Port files (target = modern u-boot, e.g. 2024.01 to match poky scarthgap)
Map these into the u-boot tree (via a recipe SRC_URI/patch, M2 integration):
- `drivers/serial/serial_rda.c` — **DONE here** (DM serial, compatible `rda,8810pl-uart`).
  Add `config RDA_SERIAL` to `drivers/serial/Kconfig` + Makefile `obj-$(CONFIG_RDA_SERIAL)`.
- `arch/arm/mach-rda/{Kconfig,Makefile}` — `config ARCH_RDA` (ARM, ARMv7, select DM,
  OF_CONTROL, no SPL since vendor SPL is used: `CONFIG_SPL` OFF).
- `board/rda/rda8810pl/{Kconfig,board.c,Makefile}` — `dram_init` returns the 256 MB at
  `0x80000000` (DDR already inited by vendor SPL; just report size), minimal `board_init`.
- `arch/arm/dts/rda8810pl-orangepi-i96.dts(i)` — reuse mainline (Linux v6.6
  `arch/arm/boot/dts/unisoc/rda8810pl*.dts*`); add a `u-boot,dm-pre-reloc` to the uart node.
- `configs/rda8810pl_orangepi_i96_defconfig` — `CONFIG_TEXT_BASE=<unknown #1>`,
  `ARCH_RDA`, `RDA_SERIAL`, `OF_CONTROL`, `DEFAULT_DEVICE_TREE`, plus the pantavisor needs:
  `CMD_SETEXPR` (+gsub is built-in to modern setexpr), `FIT`, `CMD_FDT`, `ENV_IS_IN_*`/
  `SAVEENV`, `CMD_EXT4`/`FS_GENERIC`, `CMD_BOOTZ`/`BOOTI`, `CMD_PART`, `ENV exists`.
- `drivers/mmc/rda_mmc.c` — **TODO (next keystone)**: port from vendor
  `drivers/mmc/rda_mmc.c` (+ `asm/arch-rda/reg_mmc.h`) to u-boot DM (`UCLASS_MMC`),
  compatible `rda,8810-mmc` (check the mainline `rda8810pl.dtsi` mmc node for the exact
  compatible + base). Needed to load kernel/FIT from SD.
- Optional: timer (rda timer or CONFIG_SYS_ARCH_TIMER), gpio.

## Integration (M2 finish) and next steps
- Add a `u-boot-orangepi-i96_2024.01.bb` style recipe: SRC_URI modern u-boot + `file://`
  these port files, `UBOOT_MACHINE = "rda8810pl_orangepi_i96_defconfig"`, builds with the
  scarthgap gcc 13. Then M3 packages `[spl][table][u-boot.img]` into the SD-gap blob.
- First bench milestone: console output (this serial driver). Then MMC, then the pantavisor
  FIT boot (M4). Iterate `CONFIG_TEXT_BASE` until SPL→stage-2 handoff prints.
