/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/* Jaguar1 VCore-III target map. */
#ifndef _MSCC_JAGUAR1_H_
#define _MSCC_JAGUAR1_H_

#define MSCC_IO_ORIGIN1_OFFSET		0x60000000
#define MSCC_IO_ORIGIN1_SIZE		0x01000000
#define MSCC_IO_ORIGIN2_OFFSET		0x70000000
#define MSCC_IO_ORIGIN2_SIZE		0x00200000

#define BASE_CFG			((void __iomem *)0x70000000)
#define BASE_DEVCPU_GCB		((void __iomem *)0x60010000)

/* PI chip-select 3 is connected to the optional second Jaguar1. */
#define MSCC_JAGUAR1_PI_CS3_TO	0x5c000000

#endif
