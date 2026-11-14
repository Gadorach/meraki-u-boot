# MS42P boot and storage architecture

## Goals

The MS42P boot chain must remain recoverable when a normal firmware update is
interrupted or a NAND image is invalid. Routine updates should modify NAND
only. NOR writes should be limited to deliberate bootloader, recovery image,
or environment updates performed from a known-good environment.

The design has four layers:

1. A small, rarely changed NOR stage1 initializes the CPU and DDR and selects a
   validated stage2.
2. A full U-Boot stage2 provides networking, NAND BCH/UBI support, persistent
   configuration, image verification, and recovery policy.
3. NOR contains one self-contained recovery Linux environment.
4. NAND contains one replaceable production system and its writable state.

This document describes the target structure. Only features explicitly marked
as validated should be considered available in the current image.

## Boot flow

### Stage1

Stage1 remains in the first 256 KiB of NOR and owns the reset vectors, PLL,
TLB, DDR training, inherited-watchdog shutdown, UART, and stage2 validation.
It must not depend on NAND.

Stage1 validates both stage2 slots and normally selects the valid image with the
highest generation. Each stage2 image has a 4 KiB header containing a magic,
format version, board ID, generation, payload size, load address, entry address,
header CRC32, and payload CRC32. Stage1 rejects an image whose target, bounds,
addresses, or CRCs do not match the fixed slot contract. A held reset button
selects the older valid image. If neither slot validates, stage1 stays at a
serial recovery prompt. On the MS42P, the front-panel reset button is primary
Jaguar1 GPIO 13, active low; stage1 debounces it before using it as the
older-generation override. The per-platform Kconfig setting defaults to -1 for
Jaguar1 boards without a confirmed assignment.

Stage1 is not rewritten during normal updates. Stage2 updates always write and
verify the inactive slot before increasing its generation; they never erase
both stage2 slots in one operation.

### Stage2

Stage2 runs from DDR and contains the complete feature set:

- SPI NOR read, erase, write, and verification;
- Micron raw NAND with software BCH4 per 512 bytes;
- bad-block handling and UBI;
- DHCP, ping, TFTP, FIT verification, and serial loading;
- redundant persistent environment;
- reset-button recovery policy;
- production NAND and NOR recovery boot commands.

Stage2 first checks the recovery request latched by stage1 or set in its
environment. A recovery request boots the NOR recovery FIT. Otherwise stage2
boots the single production image from NAND. An invalid NAND kernel, failed UBI
attach, or missing rootfs volume falls back to NOR recovery.

The production command is one `&&` chain: attach the fixed `ubi` MTD partition,
check that the `kernel` and `rootfs` volumes exist, read the FIT from `kernel`,
validate all FIT hashes with `iminfo`, install production boot arguments, and
invoke `bootm`. Any failure returns to `bootcmd`, which runs `nor_recovery`.
Production arguments select `ubi.mtd=ubi`, request a block device for the
`rootfs` volume, and mount `/dev/ubiblock0_1` as read-only SquashFS. Recovery
uses a separate setup command containing no UBI or NAND root arguments.

The stage1 request is a one-shot RAM handoff word at `0x80fff000`, immediately
below the stage2 load address. Stage1 writes the recovery magic when reset is
held and clears it on an ordinary boot. Stage2 consumes and clears the word
before evaluating its environment and boot command.

### Linux

The production system loads one kernel FIT from NAND and mounts its NAND rootfs.
The NOR recovery FIT includes its own initramfs and does not require NAND to
offer UART, DHCP-client networking, SSH, LuCI, and an HTTP recovery updater. It
must not mount or create a persistent overlay: every recovery boot starts from
the compiled image so a saved password or broken configuration cannot lock out
recovery. Removing the separate NOR fallback-kernel slot leaves enough space for
a current OpenWrt kernel, device tree, and useful recovery initramfs in one
independently verified FIT.

## NOR layout

The 16 MiB NOR is treated as a fixed recovery device. All offsets are erase
aligned and are independent of the former vendor partition map.

| Offset | Size | Purpose |
| ---: | ---: | --- |
| `0x000000` | `0x040000` | Stage1 and reset vectors |
| `0x040000` | `0x0c0000` | Stage2 slot A, including 4 KiB header |
| `0x100000` | `0x0c0000` | Stage2 slot B, including 4 KiB header |
| `0x1c0000` | `0x010000` | Environment copy 0 |
| `0x1d0000` | `0x010000` | Environment copy 1 |
| `0x1e0000` | `0xe20000` | Self-contained OpenWrt recovery FIT |

The two 64 KiB environments are erase-aligned and deliberately outside both
stage2 slots. Their offsets are fixed ABI once stage2 images are deployed. Each
768 KiB stage2 slot leaves substantial room for networking, NAND/UBI, SHA-256,
and update policy while keeping stage1's validation simple. The 14.125 MiB
recovery allocation consumes the rest of NOR and must be size-checked by the
OpenWrt image recipe.

Stage2 reads that fixed-size region at `0x82000000`, ending at `0x82e20000`.
This is below the top of the 128 MiB DDR window, and the kernel decompression
range remains below stage2's `0x81000000` link address. The recovery command
chains probe, read, setup, and `bootm` with `&&` so a failed SPI operation cannot
boot stale RAM contents.

## NAND layout

The Micron MT29F1G08ABADAWP is an x8 device on Jaguar1 PI CS1 at `0x54000000`.
Address bit 13 drives ALE, address bit 14 drives CLE, GPIO 19 selects alternate
function 1 for CS1, and GPIO 20 remains in GPIO mode. PI CS1 uses `WAITCC=16`.
Stage2 exposes the entire 128 MiB device as one fixed MTD partition named `ubi`.
UBI owns physical eraseblock placement and bad-block avoidance; fixed raw
offsets are not assigned to production filesystems.

The production image is intentionally not A/B. Bootloader redundancy and the
independent NOR recovery system provide the recovery path while avoiding the
space and update complexity of duplicate NAND systems. Logical volumes should
include:

| Volume | Suggested type | Purpose |
| --- | --- | --- |
| `kernel` (ID 0) | dynamic, sized to the production FIT limit | FIT kernel and device tree |
| `rootfs` (ID 1) | dynamic, sized to the production rootfs limit | Immutable SquashFS rootfs |
| `rootfs_data` (ID 2) | dynamic, autoresize | Writable production overlay and configuration |

These are standard dynamic UBI volumes. The stated limits are presented to the
image/update tooling, not fixed raw NAND offsets. The UBI layout generator must
calculate usable logical eraseblocks after bad-block reserve and UBI overhead.
Image creation must fail if the kernel and rootfs volumes do not fit.
`rootfs_data` receives all remaining capacity. The IDs are part of the boot ABI
because the kernel command line names `/dev/ubiblock0_1`.

The proven NAND geometry is:

- 128 MiB, x8;
- 2048-byte pages and 64-byte OOB;
- 128 KiB eraseblocks;
- software BCH strength 4 per 512-byte step;
- 28 ECC bytes at OOB offsets 36 through 63;
- bad-block markers in OOB byte 0 on the first two pages.

U-Boot uses its generic large-page software-BCH OOB layout rather than a
board-private formatter. It reserves OOB bytes 0 and 1, exposes bytes 2 through
35 as free, and places the four seven-byte BCH codes at bytes 36 through 63.
Micron identification enables scanning byte 0 in each of the first two pages,
so the factory bad-block marker is never consumed by ECC or free OOB data.

## Recovery policy

Recovery can be requested through any of these paths:

- hold the reset button before power-on, causing stage1 to select the older
  stage2 and latch a NOR-recovery request for it;
- press the reset button during a visible stage2 boot-delay window;
- set a persistent `recovery_requested` environment variable;
- fail validation or loading of the production NAND image.

During the stage2 delay, the power LED should blink a documented pattern. A
button press changes the pattern and boots the NOR recovery FIT. The normal
boot path leaves the power LED in its standard orange boot state until Linux
takes ownership.

The recovery Linux environment must provide:

- serial console;
- DHCP-client network configuration and SSH;
- LuCI and an HTTP recovery interface;
- image upload and signature/hash validation;
- production NAND image creation, update, and verification;
- explicitly unlocked NOR update operations;
- full diagnostic logs suitable for remote support.

Holding reset after the production NAND system is already running is a Linux
factory-reset action, not a bootloader recovery request. It erases/reformats
`rootfs_data` and reboots while leaving the immutable NAND rootfs, NOR recovery,
and U-Boot slots unchanged.

## Update and write policy

Normal firmware updates replace and verify the single NAND kernel and rootfs.
An interrupted or invalid update falls back to the independent NOR recovery
system on the next boot; NAND does not maintain a second production slot.

NOR is read-only during ordinary updates. A NOR update requires an explicit
recovery-mode unlock and must follow this order:

1. write and verify the inactive stage2 slot or recovery image;
2. boot-test it;
3. increase the new stage2 header generation only after validation;
4. never rewrite stage1 and both stage2 copies in one operation.

Raw NAND erase/write commands are diagnostic tools, not the production updater.
Production writes should use UBI volume operations so bad blocks and erase
counters remain managed.

## Persistent environment

Stage2 uses two redundant SPI-NOR environment sectors:

```text
environment 0: 0x1c0000, size 0x10000
environment 1: 0x1d0000, size 0x10000
```

Expected policy variables include:

```text
recovery_requested=0
```

Stage1 slot selection does not read this environment. It uses only the validated
generation fields in the two stage2 headers and the reset-button override, so it
still works when both environment sectors are erased or corrupt.

The Ethernet address is per-device identity and must be provisioned from the
logic-board sticker, not compiled into a platform image. This MS42P is
provisioned as `00:18:0A:56:C4:79` in both redundant environment copies.
Generation-9 MS42P stage2 passes a pre-network validated `ethaddr` as
the six-byte `local-mac-address` on Linux's OpenVTSS FIFO-netdev device-tree
node during `bootm`. When a persistent address is absent or invalid, stage2
leaves the property absent even if its Ethernet driver subsequently creates
a random `ethaddr`. It does not generate a shared fallback MAC. The existing
`board_late_init()` attempts to obtain `ethaddr` from the board EEPROM at
I²C `0x51`, offset `0x66` when the environment supplies no valid value;
the Board B target EEPROM-fallback test reported **unavailable** and generated
a random U-Boot network MAC, which generation 9 did not hand to Linux. Both
redundant environment sectors were restored with the operator-confirmed
Board B address and checked by CRC32/readback. See
`/home/gadorach/src/openvtss-meraki/openvtss-integration/evidence/ms42p-stage2-gen9-random-mac-guard-target-20261003.md`.

## Image formats

Linux kernels use FIT images. FIT provides explicit architecture, load address,
entry point, compression, device tree, and integrity metadata. Production FITs
contain a kernel and device tree. The NOR recovery FIT additionally contains an
initramfs. Signed FIT verification can be added after the functional boot chain
is stable.

Stage2 may initially use a CRC-protected legacy U-Boot image because the loader
is small and deterministic. A signed stage2 container is preferable before
shipping unattended updates.

The first OpenWrt validation artifact is a normal FIT with a current kernel,
MS42P device tree, and embedded initramfs. Stage2 and the current bring-up image
load it above the kernel destination and invoke ordinary `bootm`; no PMOSLIVE or
legacy fixed-address compatibility path is part of the production contract.

## OpenWrt compatibility

The production image and partition interfaces must follow OpenWrt conventions
rather than preserving the vendor firmware format.

- Build from the latest practical upstream OpenWrt release and its current
  supported Linux kernel, carrying only the Jaguar1/MS42P patches that have not
  yet reached upstream.
- Use an MS42P device tree with stable `compatible` strings and explicit NOR,
  NAND, Ethernet, UART, GPIO, reset-button, and LED descriptions.
- Package production and recovery kernels as FIT images. Production FITs contain
  the kernel and device tree; the recovery FIT additionally contains its
  initramfs.
- Enable SHA-256 FIT hashes in stage2. Signed FIT configurations should be used
  once key provisioning and recovery-key rotation are defined.
- Keep NAND under UBI with one `kernel`, one `rootfs`, and one `rootfs_data`
  dynamic volume, assigned IDs 0, 1, and 2 respectively. The production system
  uses normal persistent overlay behavior.
- Implement `platform_check_image` and `platform_do_upgrade` so `sysupgrade`
  replaces and verifies the single production NAND image. Failed NAND boot must
  enter NOR recovery rather than trying a second NAND slot.
- Preserve configuration using OpenWrt's normal sysupgrade backup mechanism and
  the dedicated persistent volume. Do not store board identity in replaceable
  rootfs volumes.
- Expose the NOR recovery partition read-only to normal Linux. The
  recovery updater may unlock them only for an explicitly requested NOR update.
- Keep U-Boot environment names and boot arguments independent of OpenWrt build
  numbers so newer OpenWrt kernels can be installed without changing stage1.

The postmarketOS RAM image was an early bring-up test for U-Boot TFTP, FIT
parsing, memory placement, and Linux handoff. A native OpenWrt 6.18 FIT with the
MS42P device tree and embedded initramfs has now superseded that test.

## Implementation phases

1. Build and boot the minimal OpenWrt FIT over TFTP using standard FDT handoff.
2. Split the build into minimal stage1 and full stage2 images and boot stage2
   from RAM before changing the NOR layout.
3. Add read-only NAND identification, BCH reads, bad-block scanning, and UBI
   volume enumeration to stage2. Implemented and host-built; hardware validation
   remains pending.
4. Compare NAND reads against archived corrected/raw hashes before enabling any
   destructive command.
5. Build and boot the self-contained NOR OpenWrt recovery FIT. Complete on the
   MS42P.
6. Back up current NOR/NAND, migrate the new NOR layout, and initialize redundant
   environments. The NOR migration and environment initialization are complete;
   NAND remains unchanged.
7. Create the single production NAND kernel/rootfs/rootfs_data layout and
   validate fallback to NOR recovery.
8. Add Linux factory-reset button handling, LED recovery indication, guarded NOR
   updates, VTSS networking, DHCP, SSH, LuCI, and the recovery web updater.

## Later diagnostics

Add one small stage2-only GPIO discovery command. It snapshots all valid GPIO
inputs, polls once per second, reports only pins whose state changed, and exits
cleanly when the serial console receives Ctrl-C. This is a diagnostic aid for
identifying reset and other board-specific signals; it is not part of normal
boot policy.

## Current validation status

The installed native U-Boot currently has validated cold/warm boot, 128 MiB DDR,
UART, SPI NOR read/erase/write with restoration, fan enable, orange power LED,
master-ASIC SGPIO, secondary-ASIC PI mapping, repeated ping/TFTP commands, and
full FIT transfer. The 103,678-byte stage1 image is installed in its 256 KiB
partition. Slot ABI version 2 binds each container to its compiled platform ID.
The 307,902-byte stage2 image is installed as generation 4 in slot A and
generation 3 in slot B. Cold and warm boots validate both headers and
payloads, select slot A, load it at `0x81000000`, and enter stage2. Erasing only
slot A's header caused stage1 to reject A and successfully boot slot B; restoring
the verified header returned normal selection to A. A CRC-valid,
higher-generation slot tagged for MS220-48 was also rejected on the MS42P.

From stage2, SPI NOR, Ethernet reinitialization, ping, TFTP, on-target SHA-256
verification, redundant environment saves/loads, and the persistent per-device
MAC address are validated. Both the explicit recovery command and the default
boot command read the recovery FIT from NOR and boot it to an OpenWrt root shell.

The minimal OpenWrt 6.18.52 initramfs reaches a root shell through standard
FIT/FDT handoff. The CP0 timer runs on CPU IRQ 7, and Jaguar1 ICPU source 6
drives `ttyS0` as Linux IRQ 8. Only one CPU is currently described and brought
online. The stage1 and stage2 defconfig pairs for MS42/MS42P, MS220-48, MS320-24,
and MS320-48 all build successfully and use the same slot ABI; hardware
validation currently covers only the MS42P. NAND, physical reset-button
behavior, Linux switch support, production boot, and guarded update automation
remain unvalidated and must not be treated as available.

The MS42P stage2 source now includes the CS1 raw-NAND driver, software BCH4/512,
generic BBM-preserving OOB layout, fixed whole-device `ubi` partition, MTD/NAND/
UBI commands, and fail-chained production FIT loading. A clean GCC 14.4.0
OpenWrt-toolchain build produces a 490,934-byte `u-boot.bin`, which is 291,402
bytes below the `0xbf000` slot payload limit. Static host tests cover the wiring,
geometry, ECC, partition, Kconfig scope, production chain, and recovery argument
isolation. This new image has not been run on hardware and has not been flashed;
the installed 307,902-byte stage2 remains the previously validated image.

## Read-only hardware qualification

Stop autoboot and run these commands before any UBI attach. They issue only
NAND reads and RAM writes; record the complete serial output and compare both
hashes with an independently archived dump:

```text
nand info
mtd list
nand bad
nand dump 0
nand dump.ecc 0
nand read ${fit_addr_r} 0 0x20000
hash sha256 ${fit_addr_r} 0x20000
nand read.raw 0x83000000 0 1
hash sha256 0x83000000 0x840
```

Only after the raw geometry, BBM list, corrected data, and raw page-plus-OOB
hashes agree, exercise the logical path below. These commands contain no erase,
write, create, remove, or save operation. UBI attach itself is not guaranteed to
be electrically read-only if it discovers interrupted metadata that requires
recovery, so take and verify a full NAND backup first:

```text
ubi part ubi
ubi info l
ubi check kernel
ubi check rootfs
ubi check rootfs_data
ubi read ${fit_addr_r} kernel
hash sha256 ${fit_addr_r} ${filesize}
iminfo ${fit_addr_r}
run setup_production
printenv bootargs
run setup_recovery
printenv bootargs
```

Do not use `saveenv`, `nand write`, `nand erase`, `nand scrub`, `nand markbad`,
`mtd write`, `mtd erase`, `ubi create`, `ubi write`, `ubi remove`, `sf erase`,
or `sf write` during read-only qualification.
