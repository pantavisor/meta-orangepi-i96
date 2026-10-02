SUMMARY = "Orange Pi i96: keep pantavisor's boot environment on the vfat boot partition"
DESCRIPTION = "Storage mount helper for PV_STORAGE_MNTTYPE=bootvfat: mounts the \
ext4 storage, then the vfat boot partition over <storage>/boot, so u-boot reads \
uboot.txt from FAT instead of an ext4 it cannot replay the journal of."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://pvmnt.bootvfat"
S = "${WORKDIR}"

do_install() {
	install -d ${D}/btools
	install -m 0755 ${WORKDIR}/pvmnt.bootvfat ${D}/btools/pvmnt.bootvfat
}

FILES:${PN} = "/btools/pvmnt.bootvfat"
