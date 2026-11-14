/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2018 Microsemi Corporation
 */

#ifndef __VCOREIII_H
#define __VCOREIII_H

#include <linux/sizes.h>

/* Onboard devices */

#define CFG_SYS_INIT_SP_OFFSET       0x400000

#define CFG_SYS_NS16550_CLK		CONFIG_SYS_MIPS_TIMER_FREQ

#define CFG_SYS_SDRAM_BASE		0x80000000
#if defined(CONFIG_DDRTYPE_H5TQ1G63BFA) || \
	defined(CONFIG_DDRTYPE_MT47H128M8HQ) || \
	defined(CONFIG_DDRTYPE_MT47H64M16)
#define CFG_SYS_SDRAM_SIZE		(128 * SZ_1M)
#elif defined(CONFIG_DDRTYPE_MT41J128M16HA) || defined(CONFIG_DDRTYPE_MT41K128M16JT)
#define CFG_SYS_SDRAM_SIZE		(256 * SZ_1M)
#elif defined(CONFIG_DDRTYPE_H5TQ4G63MFR) || defined(CONFIG_DDRTYPE_MT41K256M16)
#define CFG_SYS_SDRAM_SIZE		(512 * SZ_1M)
#else
#error Unknown DDR size - please add!
#endif

#ifdef CONFIG_MERAKI_JAGUAR1_STAGE2
#define CFG_EXTRA_ENV_SETTINGS					\
	"loadaddr=0x82000000\0"					\
	"fit_addr_r=0x82000000\0"				\
	"console=ttyS0,115200\0"					\
	"bootargs_extra=\0"						\
	"setup_recovery=setenv bootargs console=${console} "		\
		"${bootargs_extra}\0"					\
	"setup_production=setenv bootargs console=${console} ubi.mtd=ubi " \
		"ubi.block=0,rootfs root=/dev/ubiblock0_1 "		\
		"rootfstype=squashfs ro rootwait ${bootargs_extra}\0"	\
	"netboot=run setup_recovery && tftpboot ${fit_addr_r} ${bootfile} && " \
		"bootm ${fit_addr_r}\0"				\
	"recovery_off=0x1e0000\0"					\
	"recovery_size=0xe20000\0"				\
	"nor_recovery=sf probe && sf read ${fit_addr_r} ${recovery_off} " \
		"${recovery_size} && run setup_recovery && "		\
		"bootm ${fit_addr_r}\0"				\
	"nand_boot=ubi part ubi && ubi check kernel && ubi check rootfs && " \
		"ubi read ${fit_addr_r} kernel && iminfo ${fit_addr_r} && " \
		"run setup_production && bootm ${fit_addr_r}\0"		\
	"recovery_requested=0\0"					\
	""
#elif defined(CONFIG_SOC_JAGUAR1)
#define CFG_EXTRA_ENV_SETTINGS ""
#else
#define CFG_EXTRA_ENV_SETTINGS					\
	"loadaddr=0x81000000\0"						\
	"spi_image_off=0x00100000\0"					\
	"console=ttyS0,115200\0"					\
	"setup=setenv bootargs console=${console} ${mtdparts}"		\
	"${bootargs_extra}\0"						\
	"spiboot=run setup; sf probe; sf read ${loadaddr}"		\
	"${spi_image_off} 0x600000; bootm ${loadaddr}\0"		\
	"ubootfile=u-boot.bin\0"					\
	"update=sf probe;mtdparts;dhcp ${loadaddr} ${ubootfile};"	\
	"sf erase UBoot 0x100000;"					\
	"sf write ${loadaddr} UBoot  ${filesize}\0"			\
	"bootcmd=run spiboot\0"						\
	""
#endif
#endif				/* __VCOREIII_H */
