// SPDX-License-Identifier: (GPL-2.0+ OR MIT)
/*
 * Copyright (c) 2018 Microsemi Corporation
 */

#include <init.h>
#include <asm/global_data.h>

#include <asm/io.h>
#include <asm/types.h>

#include <mach/tlb.h>
#include <mach/ddr.h>

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_SOC_JAGUAR1
static __always_inline
#else
static inline
#endif
int vcoreiii_train_bytelane(void)
{
	int ret;

	ret = hal_vcoreiii_train_bytelane(0);

#if defined(CONFIG_SOC_OCELOT) || defined(CONFIG_SOC_JR2) || \
	defined(CONFIG_SOC_SERVALT) || defined(CONFIG_SOC_SERVAL)
	if (ret)
		return ret;
	ret = hal_vcoreiii_train_bytelane(1);
#endif

	return ret;
}

int vcoreiii_ddr_init(void)
{
	register int res;

#ifdef CONFIG_SOC_JAGUAR1
	/*
	 * Jaguar1's released loader retrains only while reset boot mode is
	 * active and retries indefinitely after a PHY FIFO reset.
	 */
	if (readl(BASE_CFG + ICPU_GENERAL_CTRL) &
	    ICPU_GENERAL_CTRL_BOOT_MODE_ENA) {
		hal_vcoreiii_init_memctl();
		hal_vcoreiii_wait_memctl();
		while (vcoreiii_train_bytelane() != DDR_TRAIN_OK || dram_check())
			hal_vcoreiii_ddr_failed();
		hal_vcoreiii_ddr_verified();
	}
	res = 0;
#else
	if (!(readl(BASE_CFG + ICPU_MEMCTRL_STAT)
	      & ICPU_MEMCTRL_STAT_INIT_DONE)) {
		hal_vcoreiii_init_memctl();
		hal_vcoreiii_wait_memctl();
		if (hal_vcoreiii_init_dqs() || vcoreiii_train_bytelane())
			hal_vcoreiii_ddr_failed();
	}

	res = dram_check();
	if (res == 0)
		hal_vcoreiii_ddr_verified();
	else
		hal_vcoreiii_ddr_failed();
#endif

	/*  Remap DDR to kuseg: Clear boot-mode */
	clrbits_le32(BASE_CFG + ICPU_GENERAL_CTRL,
		     ICPU_GENERAL_CTRL_BOOT_MODE_ENA);
	/* - and read-back to activate/verify */
	readl(BASE_CFG + ICPU_GENERAL_CTRL);

	return res;
}

int print_cpuinfo(void)
{
	printf("MSCC VCore-III MIPS 24Kec\n");

	return 0;
}

int dram_init(void)
{
	gd->ram_size = CFG_SYS_SDRAM_SIZE;
	return 0;
}
