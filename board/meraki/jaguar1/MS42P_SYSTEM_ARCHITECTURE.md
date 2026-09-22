# MS42P boot and storage architecture

## Goals

The MS42P boot chain must remain recoverable when a normal firmware update is
interrupted or a NAND image is invalid. Routine updates should modify NAND
only. NOR writes should be limited to deliberate bootloader, recovery image,
or stable-kernel updates performed from a known-good environment.

The design has four layers:

1. A small, rarely changed NOR stage1 initializes the CPU and DDR and selects a
   validated stage2.
2. A full U-Boot stage2 provides networking, NAND BCH/UBI support, persistent
   configuration, image verification, and recovery policy.
3. NOR contains a self-contained recovery Linux environment and a stable
   fallback kernel.
4. NAND contains replaceable production kernels, root filesystems, and writable
   state.

This document describes the target structure. Only features explicitly marked
as validated should be considered available in the current image.

## Boot flow

### Stage1

Stage1 remains in the first 256 KiB of NOR and owns the reset vectors, PLL,
TLB, DDR training, inherited-watchdog shutdown, UART, and stage2 validation.
It must not depend on NAND.

Stage1 tries the primary stage2 slot, then the golden stage2 slot. Each stage2
image must have a header and payload CRC, bounded load and entry addresses, and
a maximum size matching its NOR slot. A held reset button selects the golden
slot first. If neither slot validates, stage1 stays at a serial recovery prompt.

Stage1 and the golden stage2 are not rewritten during normal updates.

### Stage2

Stage2 runs from DDR and contains the complete feature set:

- SPI NOR read, erase, write, and verification;
- Micron raw NAND with software BCH4 per 512 bytes;
- bad-block handling and UBI;
- DHCP, ping, TFTP, FIT verification, and serial loading;
- redundant persistent environment;
- reset-button and boot-count recovery policy;
- production, rollback, and recovery boot commands.

Stage2 first checks explicit recovery requests, then tries the selected NAND
slot. An invalid NAND kernel, failed UBI attach, failed rootfs load, or exhausted
boot count falls back to the NOR recovery environment.

### Linux

The preferred production system loads a kernel FIT from NAND and mounts its
matching NAND rootfs. A stable kernel remains in NOR as an independent fallback.
The NOR recovery FIT includes its own initramfs and does not require NAND to
offer UART, SSH, and a recovery web updater.

## Proposed NOR layout

The 16 MiB NOR is treated as a fixed recovery device. All offsets are erase
aligned and are independent of the former vendor partition map.

| Offset | Size | Purpose |
| ---: | ---: | --- |
| `0x000000` | `0x040000` | Stage1 and reset vectors |
| `0x040000` | `0x100000` | Primary stage2 |
| `0x140000` | `0x100000` | Golden stage2 |
| `0x240000` | `0xa80000` | Self-contained recovery FIT |
| `0xcc0000` | `0x320000` | Stable fallback kernel FIT |
| `0xfe0000` | `0x010000` | Environment copy 0 |
| `0xff0000` | `0x010000` | Environment copy 1 |

The final 128 KiB is two redundant 64 KiB environments. A 128-byte reservation
is not sufficient for a U-Boot environment or NOR erase granularity. The exact
layout remains provisional until both stage images and the recovery FIT are
built and size assertions pass.

The recovery FIT allocation is sized for the current approximately 2.1 MiB
kernel and 7.9 MiB recovery rootfs. The stable-kernel slot intentionally has no
large rootfs because it boots a NAND rootfs or falls back to the recovery FIT.

## Proposed NAND layout

The 128 MiB Micron NAND should be exposed as one UBI MTD partition. UBI owns
physical eraseblock placement and bad-block avoidance; fixed raw offsets must
not be assigned to production filesystems.

Initial logical volumes should include:

| Volume | Suggested type | Purpose |
| --- | --- | --- |
| `kernel_a` | static | Slot A FIT kernel |
| `kernel_b` | static | Slot B FIT kernel |
| `rootfs_a` | static or dynamic | Slot A immutable rootfs |
| `rootfs_b` | static or dynamic | Slot B immutable rootfs |
| `overlay` | dynamic | Writable system overlay |
| `persist` | dynamic, autoresize | Configuration, logs, and retained state |

Final volume sizes depend on the produced images and bad-block reserve. The
layout generator must calculate them from usable PEBs rather than assuming the
full nominal capacity.

The proven NAND geometry is:

- 128 MiB, x8;
- 2048-byte pages and 64-byte OOB;
- 128 KiB eraseblocks;
- software BCH strength 4 per 512-byte step;
- 28 ECC bytes at OOB offsets 36 through 63;
- bad-block markers in OOB byte 0 on the first two pages.

## Recovery policy

Recovery can be requested through any of these paths:

- hold the reset button while stage1 starts;
- press the reset button during a visible stage2 boot-delay window;
- set a one-shot `recovery_requested` environment variable;
- exceed the production boot limit;
- fail validation or loading of both NAND slots.

During the stage2 delay, the power LED should blink a documented pattern. A
button press changes the pattern and boots the NOR recovery FIT. The normal
boot path leaves the power LED in its standard orange boot state until Linux
takes ownership.

The recovery Linux environment must provide:

- serial console;
- network configuration and SSH;
- a small recovery web interface;
- image upload and signature/hash validation;
- NAND slot creation, update, verification, and selection;
- explicitly unlocked NOR update operations;
- full diagnostic logs suitable for remote support.

## Update and write policy

Normal firmware updates write the inactive NAND kernel and rootfs volumes,
verify them, set a pending slot, and reboot. Linux confirms a healthy boot only
after required services and storage are available. Failed pending boots revert
to the previous slot.

NOR is read-only during ordinary updates. A NOR update requires an explicit
recovery-mode unlock and must follow this order:

1. write and verify the inactive stage2 slot or recovery image;
2. boot-test it;
3. update the primary stage2 or stable kernel only after validation;
4. never rewrite stage1 and both stage2 copies in one operation.

Raw NAND erase/write commands are diagnostic tools, not the production updater.
Production writes should use UBI volume operations so bad blocks and erase
counters remain managed.

## Persistent environment

Stage2 uses two redundant SPI-NOR environment sectors:

```text
environment 0: 0xfe0000, size 0x10000
environment 1: 0xff0000, size 0x10000
```

Expected policy variables include:

```text
active_slot=a
previous_slot=b
pending_slot=
upgrade_available=0
bootcount=0
bootlimit=3
recovery_requested=0
```

Stage1 slot selection must not depend solely on this environment. Its primary
and golden stage2 fallback must still work when both environment sectors are
erased or corrupt.

## Image formats

Linux kernels should use FIT images. FIT provides explicit architecture, load
address, entry point, compression, initramfs, device tree, and integrity
metadata. Signed FIT verification can be added after the functional boot chain
is stable.

Stage2 may initially use a CRC-protected legacy U-Boot image because the loader
is small and deterministic. A signed stage2 container is preferable before
shipping unattended updates.

The existing postmerkOS RAM-boot artifacts can be tested immediately using
legacy U-Boot wrappers:

- kernel payload at `0x81000000`;
- SquashFS ramdisk payload at `0x87000000`;
- `bootm` with the kernel and ramdisk wrappers.

Successful TFTP boot of those artifacts is the immediate validation target.

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
- Keep NAND under UBI. The OpenWrt port may use A/B-specific `kernel_a`,
  `kernel_b`, `rootfs_a`, and `rootfs_b` volumes, but its target scripts must
  expose normal `rootfs_data` overlay behavior for the selected slot.
- Implement `platform_check_image`, `platform_do_upgrade`, slot selection, and
  rollback in the OpenWrt target so `sysupgrade` writes only the inactive NAND
  slot during an ordinary upgrade.
- Preserve configuration using OpenWrt's normal sysupgrade backup mechanism and
  the dedicated persistent volume. Do not store board identity in replaceable
  rootfs volumes.
- Expose NOR recovery/stable-kernel partitions read-only to normal Linux. The
  recovery updater may unlock them only for an explicitly requested NOR update.
- Keep U-Boot environment names and boot arguments independent of OpenWrt build
  numbers so newer OpenWrt kernels can be installed without changing stage1.

The current postmerkOS RAM image is a bring-up test because it already contains
the platform support and a conventional Linux/initramfs structure. Passing that
test proves U-Boot TFTP, FIT parsing, memory placement, and Linux handoff; it
does not replace validation with the final OpenWrt kernel and rootfs images.

## Implementation phases

1. Validate secondary-ASIC PI mapping, all SFP LED defaults, Ethernet, DHCP,
   TFTP, and the existing RAM-boot Linux image.
2. Split the build into minimal stage1 and full stage2 images and boot stage2
   from RAM before changing the NOR layout.
3. Add read-only NAND identification, BCH reads, bad-block scanning, and UBI
   volume enumeration to stage2.
4. Compare NAND reads against archived corrected/raw hashes before enabling any
   destructive command.
5. Build and boot the self-contained NOR recovery FIT.
6. Back up current NOR/NAND, migrate the NOR layout, and initialize redundant
   environments.
7. Create NAND A/B volumes and validate rollback.
8. Add reset-button sampling, LED recovery indication, guarded NOR updates,
   SSH, and the recovery web updater.

## Current validation status

The native U-Boot currently has validated cold/warm boot, 128 MiB DDR, UART,
read-only SPI NOR access, fan enable, orange power LED, and master-ASIC SGPIO.
The secondary-ASIC mapping fix and Linux TFTP boot remain hardware tests. NAND,
persistent environment, stage splitting, reset-button recovery, and partition
migration are planned but must not be treated as implemented.
