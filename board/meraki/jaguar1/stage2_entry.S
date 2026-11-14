/* SPDX-License-Identifier: GPL-2.0+ */
#include <asm/asm.h>
#include <asm/mipsregs.h>
#include <asm/regdef.h>

#if CONFIG_TEXT_BASE != 0x81000000
#error "Jaguar1 stage2 must be linked at 0x81000000"
#endif
#ifndef CONFIG_SKIP_LOWLEVEL_INIT
#error "Jaguar1 stage2 must skip low-level DDR initialization"
#endif

	.set noreorder

LEAF(mips_mach_early_init)
	mfc0	t0, CP0_STATUS
	li	t1, ST0_ERL
	nor	t1, zero, t1
	and	t0, t0, t1
	mtc0	t0, CP0_STATUS
	ehb
	jr	ra
	 nop
	END(mips_mach_early_init)
