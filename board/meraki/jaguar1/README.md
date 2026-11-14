# Cisco Meraki Jaguar1

This board directory covers the VSC7460/VSC7434 Jaguar1 systems used by the
MS42/MS42P, MS220-48, MS320-24, and MS320-48 families. The SoC cold-start code
is shared with the other VCore-III platforms; board topology remains selected
by an explicit defconfig so a missing secondary ASIC is never used as a model
identifier.

The port initializes the Jaguar1 PLL, 128 MiB x16 DDR2, UART, mapped SPI NOR,
the optional PI-CS3 secondary ASIC, copper-PHY reset, SGPIO LEDs, and the board
EEPROM base MAC. The small reset-vector **stage1** has no persistent U-Boot
environment; the DDR-resident MS42P **stage2** uses separate redundant
environment sectors at NOR `0x1c0000`/`0x1d0000`. Stage2 generation 9
passes a **pre-network validated** per-board `ethaddr` into an OpenVTSS
Linux FDT node before booting, without writing the environment during
normal startup. Board B's EEPROM fallback was unavailable on target; a
subsequently generated random U-Boot network MAC is not passed to Linux. This
does not make the original stage1 layout or other Jaguar1 board families
interchangeable.

All four targets use the same polled master-Jaguar physical-port-2 path (chip
port/PHY 1), DHCP/TFTP commands, and network initialization lifecycle. The
driver does not configure the secondary Jaguar, VStaX, or FDMA.

Build with a GNU MIPS little-endian cross compiler, for example:

```sh
make O=build/ms42p ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- \
  meraki_ms42p_jaguar1_defconfig
make O=build/ms42p ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- -j8
```

The resulting `u-boot.bin` is a native reset image linked at `0x40000000`, not
a LinuxLoader RAM payload. Each defconfig enforces the 256 KiB vendor-loader
limit at build time. Hardware validation and a recovery-capable flashing method
are still required before replacing that region.

Reference image sizes built with GCC 10.3.0 are:

| Defconfig | `u-boot.bin` |
| --- | ---: |
| `meraki_ms42p_jaguar1_defconfig` | 260774 bytes |
| `meraki_ms220_48_jaguar1_defconfig` | 261230 bytes |
| `meraki_ms320_24_jaguar1_defconfig` | 261262 bytes |
| `meraki_ms320_48_jaguar1_defconfig` | 261230 bytes |

Before writing an image, preserve a full SPI NOR dump and verify an independent
recovery path. Initial qualification must cover cold reset and UART output,
repeated DDR training plus `mtest`, SPI NOR probe and read-only comparison,
EEPROM MAC loading, expected secondary-ASIC detection, PHY reset, link, DHCP,
and TFTP transfers.

For the RS232-attached test unit, the recovery connector and CH341 programmer
must be disconnected before applying power; the unit cannot boot safely while
the programmer is attached. If startup fails, switch the smart power outlet off
immediately and leave the unit unpowered until intervention is available. Attach
the CH341 only after power is off, and remove it again before the next boot.
