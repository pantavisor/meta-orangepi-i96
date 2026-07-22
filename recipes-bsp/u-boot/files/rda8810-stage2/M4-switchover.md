# M4 — switch OrangePi i96 from u-boot-dummy to the real bootloader

Do this ONLY after the stage-2 u-boot prints a console and reads the SD on the
bench (M5). Until then the board stays on `u-boot-dummy` so the Yocto build
stays green.

## Board yaml: kas/build-configs/release/96boards-orangepi-i96-scarthgap.yaml
Remove the `bootloader-dummy` local_conf_header block and replace with:

    bootloader-rda: |
        PREFERRED_PROVIDER_virtual/bootloader = "u-boot-orangepi-i96"
        # build the bootloader blob (vendor SPL + stage-2) and the SD image
        IMAGE_FSTYPES:append = " wic"
        WKS_FILE = "orangepi-i96.wks"

Keep the linux-yocto 6.6 kernel block — pantavisor packages it as the per-revision
FIT (`/trails/<rev>/bsp/pantavisor.fit`) that boot.cmd.pvgeneric loads. (The kernel
currently relies on a separate dtb; the FIT path bundles kernel+dtb+initrd, which is
what the modern u-boot + pantavisor expect — no appended-dtb needed once on FIT.)

## Bootloader blob into the image
Two options:
1. Build-time: a small image recipe/append runs `files/package-bootloader.sh` in
   do_image:prepend with SPL_BIN (from build-rda8810-spl.sh / an SPL recipe) and
   UBOOT_BIN=${DEPLOY_DIR_IMAGE}/u-boot-orangepi-i96.bin, dropping bootloader.rda
   into ${DEPLOY_DIR_IMAGE} so the .wks rawcopy picks it up.
2. Bench: build the blob by hand and `dd` it onto the wic at offset 0x20000
   (conv=notrunc) for the first flashes while iterating.

## Pantavisor boot wiring
The pantavisor `recipes-bsp/u-boot/u-boot%.bbappend` already adds boot.cmd.pvgeneric
-> boot.scr and the UBOOT_ENV machinery; it matches `u-boot%`, so it applies to
`u-boot-orangepi-i96` automatically. Confirm the bbappend's `do_deploy`/UBOOT_ENV
produces boot.scr for this board and that the .wks boot partition includes it.

## Verify (M5, on hardware)
- serial @ 921600 ttyS0: vendor SPL banner -> "U-Boot 2024.01" stage-2 banner
- `mmc dev 0; mmc info; ls mmc 0:1` works (rda_mmc.c filled in)
- pantavisor: boot.cmd.pvgeneric selects revision, loads pantavisor.fit, boots
- pull power mid-boot -> rollback to checkpoint revision (pv_try/pv.env)
