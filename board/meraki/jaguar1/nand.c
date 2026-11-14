// SPDX-License-Identifier: GPL-2.0+
/* MS42/MS42P Micron NAND connected to Jaguar1 PI chip select 1. */
#include <nand.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/mtd/rawnand.h>
#include <linux/sizes.h>
#include <mach/common.h>
#include <mach/tlb.h>

#define MS42P_NAND_BASE			0x54000000
#define MS42P_NAND_ALE			BIT(13)
#define MS42P_NAND_CLE			BIT(14)
#define MS42P_NAND_CS			1
#define MS42P_NAND_CS_GPIO		19
#define MS42P_NAND_GPIO_MODE_PIN	20
#define MS42P_NAND_TLB_INDEX		6

#define MS42P_NAND_SIZE			SZ_128M
#define MS42P_NAND_PAGE_SIZE		SZ_2K
#define MS42P_NAND_OOB_SIZE		64
#define MS42P_NAND_ERASE_SIZE		SZ_128K
#define MS42P_NAND_ECC_STEP		512
#define MS42P_NAND_ECC_STRENGTH		4

static struct nand_chip ms42p_nand;
static void __iomem *ms42p_nand_io;

static void ms42p_nand_cmd_ctrl(struct mtd_info *mtd, int command,
				unsigned int ctrl)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	u8 __iomem *address = ms42p_nand_io;

	if (ctrl & NAND_CLE)
		address += MS42P_NAND_CLE;
	if (ctrl & NAND_ALE)
		address += MS42P_NAND_ALE;
	if (ctrl & NAND_CTRL_CHANGE)
		chip->IO_ADDR_W = (void __iomem *)address;

	if (command != NAND_CMD_NONE)
		writeb(command, chip->IO_ADDR_W);
}

static bool ms42p_nand_geometry_valid(struct mtd_info *mtd,
				      struct nand_chip *chip)
{
	return mtd->size == MS42P_NAND_SIZE &&
	       mtd->writesize == MS42P_NAND_PAGE_SIZE &&
	       mtd->oobsize == MS42P_NAND_OOB_SIZE &&
	       mtd->erasesize == MS42P_NAND_ERASE_SIZE &&
	       !(chip->options & NAND_BUSWIDTH_16);
}

void board_nand_init(void)
{
	struct nand_chip *chip = &ms42p_nand;
	struct mtd_info *mtd = nand_to_mtd(chip);
	int ret;

	setbits_le32(BASE_CFG + ICPU_GENERAL_CTRL,
		     ICPU_GENERAL_CTRL_IF_MASTER_PI_ENA);
	clrsetbits_le32(BASE_CFG + ICPU_PI_MST_CFG,
			ICPU_PI_MST_CFG_CLK_DIV_M,
			ICPU_PI_MST_CFG_CLK_DIV(2));
	mscc_gpio_set_alternate(MS42P_NAND_CS_GPIO, 1);
	mscc_gpio_set_alternate(MS42P_NAND_GPIO_MODE_PIN, 0);
	clrsetbits_le32(BASE_CFG + ICPU_PI_MST_CTRL(MS42P_NAND_CS),
			ICPU_PI_MST_CTRL_WAITCC_M,
			ICPU_PI_MST_CTRL_WAITCC(16));

	/* The existing reset map uses entries 0-5; CS1 is outside KSEG1. */
	create_tlb(MS42P_NAND_TLB_INDEX, MS42P_NAND_BASE, SZ_16M,
		   MMU_REGIO_RW, MMU_REGIO_RW);
	ms42p_nand_io = (void __iomem *)MS42P_NAND_BASE;

	chip->IO_ADDR_R = ms42p_nand_io;
	chip->IO_ADDR_W = ms42p_nand_io;
	chip->cmd_ctrl = ms42p_nand_cmd_ctrl;
	chip->chip_delay = 30;
	chip->options |= NAND_NO_SUBPAGE_WRITE;
	chip->ecc.mode = NAND_ECC_SOFT_BCH;
	chip->ecc.size = MS42P_NAND_ECC_STEP;
	chip->ecc.strength = MS42P_NAND_ECC_STRENGTH;

	/* The generic BCH layout reserves the BBM and places 28 ECC bytes last. */
	ret = nand_scan(mtd, 1);
	if (ret) {
		printf("MS42P NAND scan failed: %d\n", ret);
		return;
	}
	/* Partial reads fail when generic software BCH fetches ECC via RNDOUT. */
	chip->options &= ~NAND_SUBPAGE_READ;
	if (!ms42p_nand_geometry_valid(mtd, chip)) {
		printf("Unsupported MS42P NAND geometry: %llu/%u/%u/%u x%s\n",
		       mtd->size, mtd->writesize, mtd->oobsize, mtd->erasesize,
		       chip->options & NAND_BUSWIDTH_16 ? "16" : "8");
		return;
	}

	ret = nand_register(0, mtd);
	if (ret) {
		printf("MS42P NAND registration failed: %d\n", ret);
		return;
	}
}
