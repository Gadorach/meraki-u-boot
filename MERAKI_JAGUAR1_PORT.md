# Experimental Meraki Jaguar1 U-Boot port

This repository carries an initial downstream U-Boot port for Cisco Meraki
switches based on the Microchip/Microsemi VSC7460 and VSC7434 Jaguar1 family.
It is based on upstream U-Boot v2026.10-rc4 and is experimental firmware, not a
production release.

The current board targets are:

- Cisco Meraki MS42/MS42P;
- Cisco Meraki MS220-48;
- Cisco Meraki MS320-24;
- Cisco Meraki MS320-48.

## Current status

Hardware validation on an MS42P includes:

- cold and warm boot from the native 256 KiB SPI-NOR boot region;
- Jaguar1 PLL, TLB, 128 MiB DDR2, UART, watchdog shutdown, fan, and LEDs;
- primary and secondary VSC7460 discovery and initialization;
- polled Ethernet on front-panel port 2 with repeated ping and TFTP commands;
- 10 MiB-class TFTP transfers;
- Macronix MX25L12805D probe, repeated reads, erase/write verification, and
  restoration of the tested sector;
- standard FIT parsing and entry into a Linux 3.18 test kernel.

The port is not complete. In particular:

- the current image is a single-stage bring-up build;
- NAND and UBI are not implemented in U-Boot;
- persistent U-Boot environment storage is not enabled;
- automatic flash boot and production update policy are intentionally disabled;
- the switch dataplane is not configured for normal forwarding;
- the other listed board targets have build coverage but require equivalent
  hardware qualification.

The intended production design uses a minimal NOR stage1, redundant DDR-loaded
stage2 slots, redundant SPI-NOR environments, an OpenWrt recovery FIT in NOR,
and A/B OpenWrt kernel/rootfs volumes in NAND UBI. See
`board/meraki/jaguar1/MS42P_SYSTEM_ARCHITECTURE.md` for the evolving design.

## Safety

Replacing the first 256 KiB of SPI NOR can make the switch unbootable. Preserve
a complete flash dump and verify an independent external-programmer recovery
path before writing an image. Do not power the board while an external SPI
programmer is electrically connected to the flash.

## Building

Use a MIPS little-endian cross compiler. For example:

```sh
make O=build/ms42p ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- \
  meraki_ms42p_jaguar1_defconfig
make O=build/ms42p ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j8
```

The generated `u-boot.bin` is a native reset image linked at `0x40000000`.
Review the board README and architecture document before using it on hardware.
