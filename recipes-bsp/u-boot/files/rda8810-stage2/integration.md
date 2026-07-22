# Integrating the RDA8810 stage-2 port into a modern u-boot tree

These files mirror the u-boot source layout. Copy them into a u-boot checkout
(e.g. v2024.01) and apply the small hooks below to existing files. The Yocto
recipe `../../u-boot-orangepi-i96_2024.01.bb` automates this.

## Files to drop in (as-is)
    drivers/serial/serial_rda.c
    drivers/mmc/rda_mmc.c
    arch/arm/mach-rda/{Kconfig,Makefile,soc.c}
    board/rda/rda8810pl/{Kconfig,Makefile,board.c}
    include/configs/rda8810pl.h
    configs/rda8810pl_orangepi_i96_defconfig
    arch/arm/dts/rda8810pl-orangepi-i96-u-boot.dtsi
Also copy the mainline base DTs into arch/arm/dts/:
    rda8810pl.dtsi, rda8810pl-orangepi-i96.dts   (from Linux v6.6 unisoc/)
and add `dtb-$(CONFIG_ARCH_RDA) += rda8810pl-orangepi-i96.dtb` to arch/arm/dts/Makefile.

## Hooks to existing files
1. arch/arm/Kconfig — add inside the main `choice` of ARCH targets:
       config ARCH_RDA
               bool "RDA Micro SoCs"
               select DM
               select OF_CONTROL
   and near the other `source "arch/arm/mach-*/Kconfig"` lines:
       source "arch/arm/mach-rda/Kconfig"

2. drivers/serial/Kconfig — add:
       config RDA_SERIAL
               bool "RDA Micro UART driver"
               depends on DM_SERIAL
               help
                 Console UART for RDA8810PL (OrangePi i96).
   drivers/serial/Makefile — add:
       obj-$(CONFIG_RDA_SERIAL) += serial_rda.o

3. drivers/mmc/Kconfig — add:
       config RDA_MMC
               bool "RDA Micro SD/MMC driver"
               depends on DM_MMC
   drivers/mmc/Makefile — add:
       obj-$(CONFIG_RDA_MMC) += rda_mmc.o

## Build
    make rda8810pl_orangepi_i96_defconfig
    make CROSS_COMPILE=arm-poky-linux-gnueabi-   # scarthgap gcc 13 is fine for 2024.01
Produces u-boot.bin (TEXT_BASE 0x80008000). Package with ../package-bootloader.sh.

## Bench-tunable values (confirm on hardware)
- console UART base (uart3 0x20a90000; fallbacks uart1/uart2) in the DT
- rda_mmc.c register/command/data logic (transcribe from vendor, see that file)
- TEXT_BASE 0x80008000 is from vendor rda_config_defaults.h — should be exact
