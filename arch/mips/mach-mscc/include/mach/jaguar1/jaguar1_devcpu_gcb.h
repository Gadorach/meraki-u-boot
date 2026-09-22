/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
#ifndef _MSCC_JAGUAR1_DEVCPU_GCB_H_
#define _MSCC_JAGUAR1_DEVCPU_GCB_H_

#include <linux/bitops.h>

#define CHIP_ID				0x08

#define GPIO_OUT_SET			0x68
#define GPIO_OUT_CLR			0x6c
#define GPIO_OUT			0x70
#define GPIO_IN				0x74
#define GPIO_OE				0x78
#define GPIO_ALT(x)			(0x88 + 4 * (x))

#define PERF_SOFT_RST			0x90
#define PERF_SOFT_RST_SOFT_NON_CFG_RST	BIT(1)
#define PERF_SOFT_RST_SOFT_CHIP_RST	BIT(0)

#endif
