#!/bin/sh
# M3: package the OrangePi i96 bootloader blob = vendor SPL + modern u-boot stage-2,
# in the RDA8810 layout the BootROM/SPL expect, then it gets dd'd into the SD gap
# at 0x20000 (M4 / .wks). Reuses the vendor tools/mkrdaimage.sh for the exact layout
# ([48K SPL padded][24K part table][u-boot.img]); 0x20000 + 72K = 0x32000.
#
# Inputs:
#   SPL_BIN  : sig-off vendor SPL (from build-rda8810-spl.sh)  -> spl image
#   UBOOT_BIN: modern u-boot.bin (TEXT_BASE 0x80008000)
#   MKRDA    : path to vendor tools/mkrdaimage.sh (from the vendor u-boot checkout)
#   MKIMAGE  : u-boot mkimage (u-boot-tools)
# Output: OUT/bootloader.rda  (raw blob to write at SD offset 0x20000)
set -eu
SPL_BIN="${SPL_BIN:?set SPL_BIN}"
UBOOT_BIN="${UBOOT_BIN:?set UBOOT_BIN}"
MKRDA="${MKRDA:?set MKRDA=path/to/vendor tools/mkrdaimage.sh}"
MKIMAGE="${MKIMAGE:-mkimage}"
OUT="${OUT:-.}"
TEXT_BASE=0x80008000   # vendor rda_config_defaults.h CONFIG_SYS_TEXT_BASE

# Wrap u-boot.bin in the 64-byte legacy header (CONFIG_UIMAGEHDR_SIZE=0x40) the
# vendor SPL skips when copying to TEXT_BASE-64 so u-boot lands at TEXT_BASE.
"$MKIMAGE" -A arm -O u-boot -T firmware -C none \
	-a "$TEXT_BASE" -e "$TEXT_BASE" -n "u-boot" \
	-d "$UBOOT_BIN" "$OUT/u-boot.img"

# Vendor layout. NOTE(hw): mkrdaimage.sh expects the SPL in the same form the
# vendor Makefile feeds it (spl/u-boot-spl.img with its header). If you built
# only spl/u-boot-spl.bin, confirm whether the BootROM needs the SPL header;
# the build script can emit the .img too (vendor Makefile ALL-$(CONFIG_SPL)).
sh "$MKRDA" "$SPL_BIN" "$OUT/u-boot.img" "$OUT/bootloader.rda"

echo "bootloader blob: $OUT/bootloader.rda"
echo "write into SD image at offset 0x20000 (131072), conv=notrunc"
