#!/bin/sh
# Build the OrangePi i96 (RDA8810PL) vendor SPL with signature-check DISABLED,
# so it will load an UNSIGNED modern u-boot stage-2 (hybrid forward-port, M1).
#
# The SPL is frozen vendor firmware (DDR init + BootROM glue); build it once and
# commit the resulting u-boot-spl.bin as a blob, then dd it into the SD gap at
# 0x20000 (see mkrdaimage layout: [48K SPL][24K part table][stage-2 u-boot]).
#
# WHY the old toolchain: the vendor tree is u-boot 2012.04; its DDR-timing SPL is
# only trustworthy built with its era's gcc. We use kernel.org crosstool gcc-4.9.4.
#
# RUN IN A CLEAN ENV (container/CI). On a host that has system libfdt/dtc dev
# headers (e.g. Arch /usr/include/libfdt.h), u-boot-2012's HOSTCC tools fail to
# build with "redefinition of fdt_*"; a minimal container without libfdt-dev is
# clean. Output: ${OUT}/u-boot-spl.bin (~43K, must fit the 48K budget).
set -eu

WORK="${WORK:-/tmp/rda8810-spl}"
OUT="${OUT:-$WORK/out}"
UBOOT_REPO="https://github.com/OrangePiLibra/OrangePi_i96_uboot.git"
UBOOT_REV="ac251146bbcc60a922e1082f517e2df926106057"
TC_URL="https://mirrors.edge.kernel.org/pub/tools/crosstool/files/bin/x86_64/4.9.4/x86_64-gcc-4.9.4-nolibc-arm-linux-gnueabi.tar.xz"
# gcc-4.9 cc1 needs libmpfr.so.4 (MPFR 3.x); modern distros ship .so.6. Pull the
# compat lib from a Debian snapshot .deb (skip if your env already has .so.4).
MPFR_DEB="https://snapshot.debian.org/archive/debian/20180601T000000Z/pool/main/m/mpfr4/libmpfr4_3.1.6-1_amd64.deb"

mkdir -p "$WORK" "$OUT"; cd "$WORK"

[ -d toolchain ] || { curl -fsSL "$TC_URL" -o tc.tar.xz; mkdir toolchain; tar -xJf tc.tar.xz -C toolchain; rm -f tc.tar.xz; }
TCBIN="$WORK/toolchain/gcc-4.9.4-nolibc/arm-linux-gnueabi/bin"

mkdir -p compat
[ -e compat/libmpfr.so.4 ] || { curl -fsSL "$MPFR_DEB" -o m.deb && ( cd compat && ar x ../m.deb && tar xf data.tar.* && cp "$(find . -name 'libmpfr.so.4')" . ) || echo "WARN: mpfr compat fetch failed; relying on system libmpfr.so.4"; }

[ -d src ] || git clone "$UBOOT_REPO" src
cd src; git checkout -q "$UBOOT_REV"

# Disable signature check so the rebuilt SPL loads our unsigned modern u-boot.
sed -i 's@^#define CONFIG_SIGNATURE_CHECK_IMAGE@//&@' include/configs/rda8810.h
sed -i 's@^CONFIG_SPL_SIGNATURE_CHECK_IMAGE := y@CONFIG_SPL_SIGNATURE_CHECK_IMAGE := n@' board/rda/rda8810/config.mk

export PATH="$TCBIN:$PATH"
export LD_LIBRARY_PATH="$WORK/compat:${LD_LIBRARY_PATH:-}"
export CROSS_COMPILE=arm-linux-gnueabi-

make distclean >/dev/null 2>&1 || true
make rda8810_config
make -j"$(nproc)"            # builds spl/u-boot-spl(.bin) and u-boot.bin

arm-linux-gnueabi-objcopy -O binary spl/u-boot-spl "$OUT/u-boot-spl.bin"
sz=$(stat -c%s "$OUT/u-boot-spl.bin")
echo "SPL: $OUT/u-boot-spl.bin ($sz bytes; budget 49152)"
[ "$sz" -le 49152 ] || { echo "ERROR: SPL exceeds 48K budget"; exit 1; }
strings "$OUT/u-boot-spl.bin" | grep -q "Signature check failed" \
  && echo "WARN: signature-check string still present — verify config disable" \
  || echo "OK: signature-check compiled out"
