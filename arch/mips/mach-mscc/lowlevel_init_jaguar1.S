/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Jaguar1 PLL setup, following the source-released VCore-III loader.
 */

#include <asm/asm.h>
#include <asm/regdef.h>

#define BASE_HSIO				0x602d0000
#define REG_HSIO(x)				(BASE_HSIO + ((x) * 4))
#define PLL5G_CFG0				REG_HSIO(0)
#define PLL5G_CFG2				REG_HSIO(2)
#define PLL5G_STATUS0				REG_HSIO(6)
#define PLL5G_STATUS0_LOCK_STATUS		0x1
#define PLL5G_CFG0_CPU_CLK_DIV_M		(0x3f << 6)
#define PLL5G_CFG0_CPU_CLK_DIV(x)		((x) << 6)

	.set noreorder
LEAF(pll_init)
	/* The released sequence waits for the reset PLL before reprogramming it. */
1:	lw	v0, PLL5G_STATUS0
	andi	v1, v0, PLL5G_STATUS0_LOCK_STATUS
	beqz	v1, 1b
	 nop

	li	v1, 0x00610400
	sw	v1, PLL5G_CFG2
	li	v1, 0x00610c00
	sw	v1, PLL5G_CFG2
	li	v1, 0x00610800
	sw	v1, PLL5G_CFG2
	li	v1, 0x00610000
	sw	v1, PLL5G_CFG2

2:	lw	v0, PLL5G_STATUS0
	andi	v1, v0, PLL5G_STATUS0_LOCK_STATUS
	beqz	v1, 2b
	 nop

	/* Divider code 6 selects the 416.66 MHz CPU clock. */
	lw	v0, PLL5G_CFG0
	li	v1, ~PLL5G_CFG0_CPU_CLK_DIV_M
	and	v0, v0, v1
	ori	v0, v0, PLL5G_CFG0_CPU_CLK_DIV(6)
	sw	v0, PLL5G_CFG0

3:	lw	v0, PLL5G_STATUS0
	andi	v1, v0, PLL5G_STATUS0_LOCK_STATUS
	beqz	v1, 3b
	 nop

	jr	ra
	 nop
	END(pll_init)
