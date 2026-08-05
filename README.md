# meta-orangepi-i96

Yocto/OpenEmbedded BSP layer for the **Orange Pi i96** — an RDA8810PL
(ARM Cortex-A5) board in the 96Boards IE form factor.

The board's own vendor software is a Linux 3.10 tree and a 2012 u-boot, neither
of which builds with a current toolchain. This layer builds it against mainline
instead: **linux-yocto 6.6** plus an RDA patch series, and a **u-boot 2024.01**
stage-2 loaded by the vendor SPL.

Extracted from [meta-pantavisor](https://github.com/pantavisor/meta-pantavisor),
where the bring-up was done; the full git history of that work is preserved here.

## Status

| Area | State |
|---|---|
| Boot (vendor SPL → u-boot 2024.01 → 6.6 kernel) | working |
| SD/MMC storage (`/dev/mmcblk0`) | working |
| I2C, GPIO, restart/power-off | working |
| WiFi station mode (`wlan0`) | working — associates, stable per-board MAC, survives warm reboot |
| WiFi AP / softap | **not working** — and not working on the vendor system either |
| Bluetooth | **not working** — and not working on the vendor system either |

The two "not working" rows are not gaps in this port: both were reproduced on
the vendor's own Debian image with vendor tools, and in both cases this layer's
driver is byte-identical to the vendor's on every relevant line. See sections 30
and 31 of the bring-up notes. Don't re-open them as porting problems.

## Bring-up notes

[`recipes-kernel/linux/files/MODEM-WIFI-PORT.md`](recipes-kernel/linux/files/MODEM-WIFI-PORT.md)
is the working record of the whole port — 31 sections covering what was tried,
what failed, and why. It is the single most useful file here. Notably it covers:

- the pad-mux discovery that made SDIO work at all (§15) — the board's recurring
  failure mode, where a peripheral looks electrically dead because its pads are
  still in GPIO mode
- the modem coprocessor gating the WiFi chip's 26 MHz reference (§22)
- deriving a stable WiFi MAC from the SD card CID (§28), because the board has no
  valid NVRAM MAC and otherwise randomises it every boot
- why Bluetooth (§29–30) and softap (§31) are dead ends on this hardware

## Layout

```
conf/machine/orangepi-i96.conf     machine definition (replaces meta-96boards')
recipes-bsp/u-boot/
  u-boot-orangepi-i96_2024.01.bb   modern stage-2, RDA8810PL SoC port
  files/rda8810-stage2/            the SoC port itself (mach/board/drivers/DT)
  files/rda8810-spl/               vendor SPL blob (see Licensing)
  files/build-rda8810-spl.sh       rebuilds that blob from vendor source
  files/package-bootloader.sh      SPL + stage-2 → bootloader.rda
recipes-kernel/linux/
  linux-yocto_%.bbappend           the machine's kernel config + patch series
  files/rda-mmc-*.patch            37 patches: clk/reset, dmaengine, mmc, i2c,
                                   combo power, mdcom/msys, rdawlan, restart
  files/rda8810pl.cfg              kernel config fragment
wic/orangepi-i96.wks               SD layout (raw bootloader gap + boot + rootfs)
```

## Building

This layer is a normal BSP layer and carries no distro policy. It is built via
[meta-pantavisor](https://github.com/pantavisor/meta-pantavisor)'s KAS configs:

```sh
./kas-container build kas/build-configs/release/96boards-orangepi-i96-scarthgap.yaml
```

To use it standalone, add it to `bblayers.conf` and select
`MACHINE = "orangepi-i96"`. It depends only on `core` (poky).

## Licensing

Layer metadata is MIT (`COPYING.MIT`). Recipe-built software keeps its own
licence.

`recipes-bsp/u-boot/files/rda8810-spl/u-boot-spl.bin` is a **binary**: the
board's SPL, which does DDR and clock init. It is not proprietary firmware — it
is a build of the vendor's GPL-2.0 u-boot 2012.04 from
[OrangePiLibra/OrangePi_i96_uboot](https://github.com/OrangePiLibra/OrangePi_i96_uboot),
with its signature check disabled so it will load an unsigned modern stage-2.
The corresponding source is that public repository and the exact build procedure
is `files/build-rda8810-spl.sh`, so it ships with its source available as GPL-2.0
requires. It is committed as a blob only because reproducing it needs an
era-appropriate toolchain (gcc 4.9) — the DDR timing code is not trustworthy
built with a modern compiler.
