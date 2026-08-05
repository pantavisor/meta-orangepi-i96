# OrangePi i96 (RDA8810PL): build the current linux-yocto kernel with the
# current toolchain instead of the abandoned 3.10 vendor tree. RDA8810PL is
# supported in mainline (v6.6): generic multi_v7_defconfig + a small RDA
# fragment + the mainline DT. Everything here is scoped to orangepi-i96 so no
# other machine is affected.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

COMPATIBLE_MACHINE:orangepi-i96 = "orangepi-i96"
KMACHINE:orangepi-i96 = "orangepi-i96"
KBUILD_DEFCONFIG:orangepi-i96 = "multi_v7_defconfig"
KCONFIG_MODE:orangepi-i96 = "alldefconfig"
KERNEL_DEVICETREE:orangepi-i96 = "unisoc/rda8810pl-orangepi-i96.dtb"
SRC_URI:append:orangepi-i96 = " file://rda8810pl.cfg"

# SD/MMC support: backport of Dang Huynh's mainline series "RDA8810PL SD/MMC
# support" (Sept 2025, lore 20250919-rda8810pl-mmc-v1-9-d4f08a05ba4d@mainlining.org)
# — clk+reset driver, IFC dmaengine, rda-mmc host, SDMMC DT nodes. Without it
# 6.6 has no RDA MMC host, so /dev/mmcblk0 never appears and pantavisor cannot
# mount its storage. MAINTAINERS/doc-only hunks stripped for clean 6.6 apply.
SRC_URI:append:orangepi-i96 = " \
    file://rda-mmc-02-dt-bindings-clock-add-rda-micro-rda8810pl-clock-.patch \
    file://rda-mmc-03-dt-bindings-dma-add-rda-ifc-dma.patch \
    file://rda-mmc-05-gpio-rda-make-irq-optional.patch \
    file://rda-mmc-06-gpio-rda-make-direction-register-unreadable.patch \
    file://rda-mmc-07-clk-add-clock-and-reset-driver-for-rda-micro-rda.patch \
    file://rda-mmc-08-dmaengine-add-rda-ifc-driver.patch \
    file://rda-mmc-09-mmc-host-add-rda-micro-sd-mmc-driver.patch \
    file://rda-mmc-10-arm-dts-unisoc-rda8810pl-add-sdmmc-controllers.patch \
    file://rda-mmc-11-mmc-rda-bounce-low-dram.patch \
    file://rda-mmc-12-arm-dts-orangepi-i96-pin-cma-in-dma-window.patch \
    file://rda-mmc-13-fix-irq-locking-hazards.patch \
    file://rda-mmc-14-never-sleep-under-lock.patch \
"

# WiFi bring-up stage 1: I2C controller (the RDA599x combo chip's power and
# identification interface is I2C). See docs in the rda-mmc series comments.
SRC_URI:append:orangepi-i96 = " \
    file://rda-mmc-15-i2c-add-rda8810pl-controller.patch \
    file://rda-mmc-16-arm-dts-rda8810pl-add-i2c.patch \
    file://rda-mmc-17-wifi-add-rda-combo-power-controller.patch \
    file://rda-mmc-18-arm-dts-orangepi-i96-add-combo-clients.patch \
    file://rda-mmc-19-arm-dts-orangepi-i96-enable-sdio-wifi.patch \
    file://rda-mmc-20-mmc-rda-report-a-missing-response-as-a-timeout.patch \
    file://rda-mmc-21-mmc-rda-fix-drvdata-type-confusion-in-remove.patch \
"

# WiFi bring-up stage 3: the modem coprocessor gates the RDA5991's 26 MHz
# reference, so the SDIO core never answers on a modemless boot. Minimal
# mdcom/msys client so the AP can ask the (bootloader-started) modem for the
# v_bt supply and the 26M/32k clocks. Also fixes the board RAM size (236 MB
# vendor map) and the CMA pool (16 MB so it actually fits in the IFC DMA
# window). See recipes-kernel/linux/files/MODEM-WIFI-PORT.md.
SRC_URI:append:orangepi-i96 = " \
    file://rda-mmc-22-misc-add-rda8810pl-mdcom-msys-client.patch \
    file://rda-mmc-23-arm-dts-rda8810pl-add-mdsys-mailbox.patch \
    file://rda-mmc-24-arm-dts-orangepi-i96-enable-mdsys-fix-ram-cma.patch \
    file://rda-mmc-25-wifi-rda-combo-clocks-ldo-via-msys.patch \
"

# WiFi stage 4: the rdawlan cfg80211 driver, giving an actual wlan0. The SDIO
# data path itself came up in stage 3 (the fix was the pad map in u-boot, see
# MODEM-WIFI-PORT.md section 15); this is the driver that binds to the
# enumerated SDIO function.
SRC_URI:append:orangepi-i96 = " \
    file://rda-mmc-26-wifi-rdawlan-import-vendor-driver.patch \
    file://rda-mmc-27-wifi-rdawlan-forward-port-to-6.6.patch \
    file://rda-mmc-28-mmc-rda-signal-async-sdio-interrupts.patch \
    file://rda-mmc-29-wifi-rdawlan-ack-irq-and-fix-mac-set.patch \
    file://rda-mmc-30-wifi-rdawlan-debug-module-params.patch \
    file://rda-mmc-31-wifi-rdawlan-claim-sdio-irq-when-leaving-poll.patch \
    file://rda-mmc-33-wifi-rdawlan-no-custom-regd-under-rtnl.patch \
    file://rda-mmc-34-wifi-combo-force-off-on-at-init.patch \
    file://rda-mmc-35-wifi-rdawlan-mac-addr-param.patch \
    file://rda-mmc-36-mmc-rda-export-boot-card-cid.patch \
    file://rda-mmc-37-wifi-rdawlan-mac-from-boot-cid.patch \
    file://rda-mmc-38-wifi-combo-power-bt-on.patch \
    file://rda-mmc-39-wifi-rdawlan-restore-ap-mode.patch \
"

# Machine restart and power-off. Unrelated to WiFi, but the WiFi bring-up is
# what made its absence expensive: reboot(2) only halted the CPU, so every
# iteration needed a physical power cycle. The vendor resets this SoC from the
# modem coprocessor, which mainline never starts; the soft-reset bit in the
# always-on MD system controller does it without one.
SRC_URI:append:orangepi-i96 = " \
    file://rda-mmc-32-power-reset-add-rda8810pl-restart.patch \
"
