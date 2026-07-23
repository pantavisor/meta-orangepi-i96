SUMMARY = "Modern u-boot stage-2 for OrangePi i96 (RDA8810PL), loaded by the vendor SPL"
DESCRIPTION = "Hybrid forward-port: the vendor SPL (DDR/clock init, built sig-off by \
build-rda8810-spl.sh) loads this modern u-boot, which provides the FIT/setexpr/env-exists/ \
save features pantavisor's boot.cmd.pvgeneric needs. RDA8810PL SoC port files live in \
files/rda8810-stage2/ (see its README.md + integration.md)."

# Reuse poky's u-boot 2024.01 build infrastructure (correct CC/HOSTCC, do_compile/
# do_deploy, uboot-config). PE=1 from u-boot-common.inc makes this outrank the dead
# meta-96boards vendor u-boot-orangepi-i96 recipe (same PN).
require recipes-bsp/u-boot/u-boot-common.inc
require recipes-bsp/u-boot/u-boot.inc

DEPENDS += "bc-native dtc-native python3-pyelftools-native"

# the RDA8810 forward-port (the patch): drivers + mach + board + defconfig + DT.
# Also search poky's u-boot files dir so the CVE patches pulled in by
# u-boot-common.inc (e.g. CVE-2025-24857.patch) resolve for this recipe.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:${COREBASE}/meta/recipes-bsp/u-boot/files:"
SRC_URI += "file://rda8810-stage2"

PROVIDES += "u-boot virtual/bootloader"
COMPATIBLE_MACHINE = "orangepi-i96"

# The generic u-boot%.bbappend merges pv.distroboot.cfg
# (CONFIG_BOOTCOMMAND="run distro_bootcmd") into every u-boot, clobbering the
# defconfig's bootcmd. This modern 2024.01 stage-2 has BOOTSTD, where the legacy
# distro_bootcmd env does not exist, so `boot` would die with
# '## Error: "distro_bootcmd" not defined' (same fix as the rockchip-mainline
# dynamic layer). Drop the fragment and let the defconfig's
# "load mmc 0:1 ${loadaddr} boot.scr && source" bootcmd stand.
SRC_URI:remove = "file://pv.distroboot.cfg"
UBOOT_MACHINE = "rda8810pl_orangepi_i96_defconfig"
# The machine conf sets UBOOT_SUFFIX="rda" for the old vendor RDA-packaged output.
# Our modern stage-2 produces a plain u-boot.bin; the bootable .rda (SPL + stage-2)
# is assembled separately (package-bootloader.sh / mk-sd-image.sh).
UBOOT_SUFFIX = "bin"

# Drop our port files into the u-boot tree + apply the small Kconfig/Makefile hooks
# (verified to build standalone; see files/rda8810-stage2/integration.md).
do_configure:prepend() {
    cp -a ${WORKDIR}/rda8810-stage2/. ${S}/

    if ! grep -q 'config ARCH_RDA' ${S}/arch/arm/Kconfig; then
        sed -i '/^config ARCH_AT91$/i config ARCH_RDA\n\tbool "RDA Micro RDA8810PL"\n\tselect CPU_V7A\n' ${S}/arch/arm/Kconfig
        sed -i 's@source "arch/arm/mach-socfpga/Kconfig"@source "arch/arm/mach-rda/Kconfig"\nsource "arch/arm/mach-socfpga/Kconfig"@' ${S}/arch/arm/Kconfig
    fi
    grep -q 'CONFIG_ARCH_RDA' ${S}/arch/arm/Makefile || \
        sed -i '/^machine-$(CONFIG_ARCH_SOCFPGA)/i machine-$(CONFIG_ARCH_RDA)\t\t+= rda' ${S}/arch/arm/Makefile
    grep -q RDA_SERIAL ${S}/drivers/serial/Kconfig || \
        printf '\nconfig RDA_SERIAL\n\tbool "RDA Micro UART"\n\tdepends on DM_SERIAL\n' >> ${S}/drivers/serial/Kconfig
    # Register DEBUG_UART_RDA inside u-boot's "Select which UART will provide the
    # debug UART" choice so the defconfig's early-debug-uart selection resolves.
    if ! grep -q 'DEBUG_UART_RDA' ${S}/drivers/serial/Kconfig; then
        awk '1; /default DEBUG_UART_NS16550/{
            print "";
            print "config DEBUG_UART_RDA";
            print "\tbool \"RDA Micro debug UART\"";
            print "\tdepends on RDA_SERIAL";
            print "\thelp";
            print "\t  RDA8810PL early debug UART (uart3 @ 0x20a90000).";
        }' ${S}/drivers/serial/Kconfig > ${S}/drivers/serial/Kconfig.rda && \
        mv ${S}/drivers/serial/Kconfig.rda ${S}/drivers/serial/Kconfig
    fi
    grep -q RDA_SERIAL ${S}/drivers/serial/Makefile || \
        echo 'obj-$(CONFIG_RDA_SERIAL) += serial_rda.o' >> ${S}/drivers/serial/Makefile
    grep -q RDA_MMC ${S}/drivers/mmc/Kconfig || \
        printf '\nconfig RDA_MMC\n\tbool "RDA Micro SD/MMC"\n\tdepends on DM_MMC\n' >> ${S}/drivers/mmc/Kconfig
    grep -q RDA_MMC ${S}/drivers/mmc/Makefile || \
        echo 'obj-$(CONFIG_RDA_MMC) += rda_mmc.o' >> ${S}/drivers/mmc/Makefile
    grep -q RDA_I2C ${S}/drivers/i2c/Kconfig || \
        printf '\nconfig RDA_I2C\n\tbool "RDA Micro RDA8810PL I2C"\n\tdepends on DM_I2C\n' >> ${S}/drivers/i2c/Kconfig
    grep -q RDA_I2C ${S}/drivers/i2c/Makefile || \
        echo 'obj-$(CONFIG_RDA_I2C) += rda_i2c.o' >> ${S}/drivers/i2c/Makefile
    grep -q RDA_TIMER ${S}/drivers/timer/Kconfig || \
        printf '\nconfig RDA_TIMER\n\tbool "RDA Micro RDA8810PL HWTIMER"\n\tdepends on TIMER\n' >> ${S}/drivers/timer/Kconfig
    grep -q RDA_TIMER ${S}/drivers/timer/Makefile || \
        echo 'obj-$(CONFIG_RDA_TIMER) += rda_timer.o' >> ${S}/drivers/timer/Makefile
    grep -q 'rda8810pl-orangepi-i96' ${S}/arch/arm/dts/Makefile || \
        echo 'dtb-$(CONFIG_ARCH_RDA) += rda8810pl-orangepi-i96.dtb' >> ${S}/arch/arm/dts/Makefile
}
