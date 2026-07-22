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
"
