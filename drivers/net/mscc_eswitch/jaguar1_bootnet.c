// SPDX-License-Identifier: GPL-2.0
/*
 * Shared Meraki Jaguar1 master-Jaguar TFTP path.
 *
 * This deliberately owns only physical port 2 (chip port/PHY 1), uses the
 * polled DEVCPU_QS path, and never touches the slave Jaguar, VStaX, or FDMA.
 */
#include <console.h>
#include <dm.h>
#include <errno.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <net.h>
#include <time.h>
#include <watchdog.h>

typedef u8 bootnet_u8;
typedef u16 bootnet_u16;
typedef u32 bootnet_u32;

#define BOOTNET_BACKEND_ABORTED (-EINTR)
#define JR_SWITCH_BASE		0x60000000u
#define JR_CHIP_ID		0x00004002u
#define JR_CHIP_ID_VALUE	0x174600e9u

#define JR_MIIM_STATUS		0x00004028u
#define JR_MIIM_CMD		0x0000402au
#define JR_MIIM_DATA		0x0000402bu
#define JR_MIIM_BUSY		0x00000008u
#define JR_MIIM_VALID		0x80000000u
#define JR_MIIM_READ		0x00000004u
#define JR_MIIM_WRITE		0x00000002u

#define JR_XTR_GRP_CFG		0x0000800au
#define JR_XTR_MAP		0x0000800eu
#define JR_XTR_RD		0x00008018u
#define JR_XTR_DATA_PRESENT	0x00008021u
#define JR_INJ_WR		0x00008028u
#define JR_INJ_CTRL		0x0000802du
#define JR_INJ_STATUS		0x00008032u

#define JR_INJ_READY		0x00000020u
#define JR_INJ_WATERMARK	0x00000400u
#define JR_INJ_SOF		0x00040000u
#define JR_INJ_EOF		0x00080000u

#define JR_XTR_EOF_0		0x80000000u
#define JR_XTR_EOF_1		0x80000001u
#define JR_XTR_EOF_2		0x80000002u
#define JR_XTR_EOF_3		0x80000003u
#define JR_XTR_PRUNED		0x80000004u
#define JR_XTR_ABORT		0x80000005u
#define JR_XTR_ESCAPE		0x80000006u
#define JR_XTR_NOT_READY	0x80000007u

#define JR_BOOT_PHYSICAL_PORT	2u
#define JR_BOOT_CHIP_PORT	1u
#define JR_ATOM_BASE_PHY	0u
#define JR_BOOT_PHY		1u
#define JR_PHY_PAGE_STD		0x0000u
#define JR_PHY_PAGE_EXT		0x0001u
#define JR_PHY_PAGE_EXT2	0x0002u
#define JR_PHY_PAGE_EXT3	0x0003u
#define JR_PHY_PAGE_GPIO	0x0010u
#define JR_PHY_PAGE_TEST	0x2a30u
#define JR_PHY_PAGE_TR		0x52b5u
#define JR_DEV1G_BASE		0x00014000u
#define JR_POLL_LIMIT		100000u
#define JR_FRAME_WORD_LIMIT	512u
#define JR_MAX_FRAME		600u

static void __iomem * const jr_switch = (void __iomem *)JR_SWITCH_BASE;
static int jr_active = -1;
static int jr_initialized;
static bootnet_u32 jr_detail = 0xffffffffu;
static bootnet_u32 jr_aux0 = 0xffffffffu;
static bootnet_u32 jr_aux1 = 0xffffffffu;

struct jr_atom_tr_write {
	bootnet_u16 reg18;
	bootnet_u16 reg17;
	bootnet_u16 reg16;
};

/* Canonical OpenVTSS rev-C Atom12 script and patch. */
static const struct jr_atom_tr_write jr_atom_rev_c_tr[] = {
	{ 0x0032, 0x303d, 0x9792 }, { 0x00a0, 0xf147, 0x97a0 },
	{ 0x0005, 0x2f54, 0x8fe4 }, { 0x0004, 0x01bd, 0x8fae },
	{ 0x000f, 0x000f, 0x8fac }, { 0x0000, 0x0004, 0x87fe },
	{ 0x0006, 0x0150, 0x8fe0 }, { 0x0012, 0x480a, 0x8f82 },
	{ 0x0000, 0x0034, 0x8f80 }, { 0x0000, 0x0012, 0x82e0 },
	{ 0x0005, 0x0208, 0x83a2 }, { 0x0000, 0x9186, 0x83b2 },
	{ 0x000e, 0x3700, 0x8fb0 }, { 0x0004, 0x9fa1, 0x9688 },
	{ 0x0000, 0xffff, 0x8fd2 }, { 0x0003, 0x9fa0, 0x968a },
	{ 0x0020, 0x640b, 0x9690 }, { 0x0000, 0x2220, 0x8258 },
	{ 0x0000, 0x2a20, 0x825a }, { 0x0000, 0x3060, 0x825c },
	{ 0x0000, 0x3fa0, 0x825e }, { 0x0000, 0xe0f0, 0x83a6 },
	{ 0x0000, 0x4489, 0x8f92 }, { 0x0000, 0x7000, 0x96a2 },
	{ 0x0010, 0x2048, 0x96a6 }, { 0x00ff, 0x0000, 0x96a0 },
	{ 0x0091, 0x9880, 0x8fe8 }, { 0x0004, 0xd602, 0x8fea },
	{ 0x00ef, 0xef00, 0x96b0 }, { 0x0000, 0x7100, 0x96b2 },
	{ 0x0000, 0x5064, 0x96b4 }, { 0x0050, 0x100f, 0x87fa },
	{ 0x0071, 0xf6d9, 0x8488 }, { 0x0000, 0x0db6, 0x848e },
	{ 0x0059, 0x6596, 0x849c }, { 0x0000, 0x0514, 0x849e },
	{ 0x0041, 0x0280, 0x84a2 }, { 0x0000, 0x0000, 0x84a4 },
	{ 0x0000, 0x0000, 0x84a6 }, { 0x0000, 0x0000, 0x84a8 },
	{ 0x0000, 0x0000, 0x84aa }, { 0x007d, 0xf7dd, 0x84ae },
	{ 0x006d, 0x95d4, 0x84b0 }, { 0x0049, 0x2410, 0x84b2 },
	{ 0x0068, 0x8980, 0x8f90 }
};

static const bootnet_u8 jr_atom_rev_c_patch[] = {
	0x42, 0x81, 0x02, 0x42, 0x76, 0x02, 0x42, 0x8b, 0x02, 0x42, 0x8e, 0x02,
	0x40, 0x33, 0x02, 0x41, 0x7d, 0xef, 0x03, 0x03, 0xf5, 0x62, 0x02, 0x1d,
	0xf2, 0x12, 0x41, 0xe2, 0x02, 0x09, 0xbb, 0xed, 0xff, 0xe5, 0xfc, 0x54,
	0x38, 0x64, 0x20, 0x70, 0x08, 0x65, 0xff, 0x70, 0x04, 0xed, 0x44, 0x80,
	0xff, 0x22, 0x90, 0x47, 0xd0, 0xe0, 0x54, 0xef, 0xf0, 0x44, 0x80, 0xf0,
	0x90, 0x47, 0xd8, 0xe0, 0x54, 0x7f, 0xf0, 0x44, 0x80, 0xf0, 0xa3, 0xe0,
	0x54, 0xfc, 0xf0, 0xf0, 0x90, 0x47, 0xd7, 0xe0, 0x54, 0x7f, 0x12, 0x42,
	0x6a, 0x90, 0x47, 0xd6, 0xe0, 0x54, 0xe7, 0xf0, 0x44, 0x18, 0xf0, 0x90,
	0x47, 0xd5, 0xe0, 0x54, 0x8f, 0xf0, 0x44, 0x20, 0xf0, 0x90, 0x47, 0xd7,
	0xe0, 0x54, 0xc7, 0xf0, 0x44, 0x18, 0xf0, 0x7c, 0x00, 0xe5, 0x6f, 0x54,
	0x0f, 0xfd, 0x64, 0x01, 0x70, 0x2f, 0xe5, 0x6f, 0x30, 0xe4, 0x2a, 0xa3,
	0xe0, 0x44, 0x02, 0xf0, 0x54, 0xfb, 0xf0, 0x90, 0x47, 0xdc, 0xe0, 0x44,
	0x04, 0xf0, 0x90, 0x47, 0xda, 0xe0, 0x54, 0xc7, 0xf0, 0x44, 0x18, 0xf0,
	0xa3, 0xe0, 0x54, 0xf0, 0xf0, 0x44, 0x0e, 0xf0, 0x90, 0x47, 0xda, 0xe0,
	0x54, 0xf8, 0xf0, 0xf0, 0x22, 0xed, 0x64, 0x01, 0x4c, 0x70, 0x0f, 0x90,
	0x47, 0xd5, 0xe0, 0x54, 0x7f, 0xf0, 0x44, 0x80, 0x12, 0x42, 0x63, 0x44,
	0x07, 0xf0, 0x90, 0x47, 0xd8, 0xe0, 0x54, 0xfd, 0xf0, 0x44, 0x04, 0xf0,
	0x90, 0x47, 0xdc, 0xe0, 0x54, 0xfb, 0x12, 0x42, 0x6a, 0xa3, 0xe0, 0x54,
	0xf0, 0xf0, 0x44, 0x06, 0xf0, 0x22, 0xe4, 0xf5, 0xfb, 0x7d, 0x1c, 0xe4,
	0xff, 0x12, 0x3c, 0x2a, 0xad, 0x07, 0xac, 0x06, 0xec, 0x54, 0xc0, 0xff,
	0xed, 0x54, 0x3f, 0x4f, 0xf5, 0x20, 0x30, 0x06, 0x2c, 0x30, 0x01, 0x08,
	0xa2, 0x04, 0x72, 0x03, 0x92, 0x07, 0x80, 0x21, 0x30, 0x04, 0x06, 0x7b,
	0xcc, 0x7d, 0x11, 0x80, 0x0d, 0x30, 0x03, 0x06, 0x7b, 0xcc, 0x7d, 0x10,
	0x80, 0x04, 0x7b, 0x66, 0x7d, 0x16, 0xe4, 0xff, 0x12, 0x3b, 0x49, 0xee,
	0x4f, 0x24, 0xff, 0x92, 0x07, 0xaf, 0xfb, 0x74, 0xb6, 0x2f, 0xf8, 0xe6,
	0xff, 0xa6, 0x20, 0x20, 0x07, 0x39, 0x8f, 0x20, 0x30, 0x07, 0x34, 0x30,
	0x00, 0x31, 0x20, 0x04, 0x2e, 0x20, 0x03, 0x2b, 0xe4, 0xf5, 0xff, 0x75,
	0xfc, 0xc2, 0xe5, 0xfc, 0x30, 0xe0, 0xfb, 0xaf, 0xfe, 0xef, 0x20, 0xe3,
	0x1a, 0xae, 0xfd, 0x44, 0x08, 0xf5, 0xfe, 0x75, 0xfc, 0x80, 0xe5, 0xfc,
	0x30, 0xe0, 0xfb, 0x8f, 0xfe, 0x8e, 0xfd, 0x75, 0xfc, 0x80, 0xe5, 0xfc,
	0x30, 0xe0, 0xfb, 0x05, 0xfb, 0xaf, 0xfb, 0xef, 0xc3, 0x94, 0x0c, 0x50,
	0x03, 0x02, 0x40, 0xe2, 0xe4, 0xf5, 0xfb, 0x22, 0xe5, 0x6f, 0x30, 0xe5,
	0x5f, 0x90, 0x47, 0xd5, 0x30, 0xe4, 0x2c, 0x12, 0x42, 0x6e, 0x90, 0x47,
	0xd8, 0xe0, 0x54, 0x1f, 0xf0, 0x12, 0x42, 0x63, 0xf0, 0x90, 0x47, 0xd5,
	0xe0, 0x54, 0x3f, 0xf0, 0x44, 0xc0, 0xf0, 0xa3, 0xe0, 0x54, 0xfc, 0xf0,
	0x44, 0x01, 0xf0, 0x90, 0x47, 0xdc, 0xe0, 0x54, 0xc7, 0xf0, 0x44, 0x28,
	0xf0, 0x80, 0x1d, 0xe0, 0x54, 0xc7, 0xf0, 0x44, 0x08, 0xf0, 0x90, 0x47,
	0xda, 0xe0, 0x44, 0x04, 0xf0, 0x90, 0x47, 0xd8, 0xe0, 0x12, 0x42, 0x5e,
	0xf0, 0x90, 0x47, 0xd6, 0xe0, 0x44, 0x40, 0xf0, 0x90, 0x47, 0xe0, 0xe0,
	0x54, 0xf8, 0xf0, 0x44, 0x05, 0xf0, 0x12, 0x42, 0x5e, 0x44, 0x06, 0xf0,
	0x22, 0xe4, 0xf5, 0x4b, 0x75, 0x4c, 0x01, 0xf5, 0x4d, 0x7f, 0x40, 0x7e,
	0x02, 0x12, 0x3b, 0x72, 0xe4, 0xf5, 0x21, 0xe5, 0x21, 0x25, 0xe0, 0x24,
	0x3e, 0xf5, 0x82, 0xe4, 0x34, 0x42, 0xf5, 0x83, 0xe4, 0x93, 0xfe, 0x74,
	0x01, 0x93, 0xf5, 0x4b, 0xee, 0xff, 0x8f, 0x4c, 0xe5, 0x4c, 0x30, 0xe7,
	0x04, 0x7f, 0xff, 0x80, 0x02, 0x7f, 0x00, 0x8f, 0x4d, 0xe5, 0x21, 0x25,
	0xe0, 0xff, 0xe4, 0x33, 0x44, 0x02, 0xfe, 0x12, 0x3b, 0x72, 0x05, 0x21,
	0xe5, 0x21, 0xc3, 0x94, 0x10, 0x40, 0xc4, 0xe4, 0xf5, 0x4b, 0xf5, 0x4c,
	0xf5, 0x4d, 0x7f, 0x40, 0x7e, 0x02, 0x02, 0x3b, 0x72, 0x17, 0x55, 0xfc,
	0xaf, 0xeb, 0x8c, 0xd9, 0xec, 0x7f, 0xff, 0x45, 0x31, 0xbb, 0x3e, 0x15,
	0xee, 0xf8, 0x8a, 0xd9, 0x60, 0xf4, 0x6b, 0xf5, 0xde, 0x01, 0xa4, 0xfb,
	0xf4, 0xeb, 0xf4, 0xfc, 0x03, 0x54, 0x1f, 0xf0, 0x44, 0x40, 0xf0, 0xa3,
	0xe0, 0x54, 0xf8, 0xf0, 0x22, 0xf0, 0x90, 0x47, 0xda, 0xe0, 0x54, 0xc7,
	0xf0, 0x44, 0x10, 0xf0, 0x22, 0x7b, 0x01, 0x7a, 0x00, 0x7d, 0xee, 0x7f,
	0x92, 0x02, 0x36, 0x7f, 0x90, 0x47, 0xfe, 0xe0, 0x54, 0xf8, 0x44, 0x02,
	0xf0, 0x22, 0x02, 0x40, 0xdf, 0x22
};

static int jr_abort_requested(void)
{
	schedule();
	return ctrlc();
}

static bootnet_u32 jr_read(bootnet_u32 word)
{
	return readl(jr_switch + word * sizeof(bootnet_u32));
}

static void jr_write(bootnet_u32 word, bootnet_u32 value)
{
	writel(value, jr_switch + word * sizeof(bootnet_u32));
}

static void jr_write_masked(bootnet_u32 word, bootnet_u32 value,
			    bootnet_u32 mask)
{
	jr_write(word, (jr_read(word) & ~mask) | (value & mask));
}

static int jr_delay_us(bootnet_u32 microseconds)
{
	ulong start = timer_get_us();

	while (timer_get_us() - start < microseconds) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		udelay(min(microseconds, 1000u));
	}
	return 0;
}

static int jr_delay_abortable_ms(bootnet_u32 milliseconds)
{
	ulong start = get_timer(0);

	while (get_timer(start) < milliseconds) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		udelay(1000);
	}
	return 0;
}

static int jr_poll_clear(bootnet_u32 word, bootnet_u32 mask)
{
	bootnet_u32 polls = JR_POLL_LIMIT;

	while (jr_read(word) & mask) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		if (polls-- == 0u)
			return -ETIMEDOUT;
	}
	return 0;
}

#include "jaguar1_lifecycle.inc"

static int jr_miim_wait(void)
{
	return jr_poll_clear(JR_MIIM_STATUS, JR_MIIM_BUSY);
}

static int jr_miim_read(bootnet_u32 phy, bootnet_u32 reg,
			bootnet_u16 *value)
{
	bootnet_u32 data;

	if (jr_miim_wait())
		return -EIO;
	jr_write(JR_MIIM_CMD, JR_MIIM_VALID | (phy << 25) | (reg << 20) |
		 JR_MIIM_READ);
	if (jr_miim_wait())
		return -EIO;
	data = jr_read(JR_MIIM_DATA);
	if (data & 0x00030000u)
		return -EIO;
	*value = data;
	return 0;
}

static int jr_miim_write(bootnet_u32 phy, bootnet_u32 reg,
			 bootnet_u16 value)
{
	if (jr_miim_wait())
		return -EIO;
	jr_write(JR_MIIM_CMD, JR_MIIM_VALID | (phy << 25) | (reg << 20) |
		 ((bootnet_u32)value << 4) | JR_MIIM_WRITE);
	return jr_miim_wait();
}

static int jr_phy_select_page_at(bootnet_u32 phy, bootnet_u16 page)
{
	return jr_miim_write(phy, 31u, page);
}

static int jr_phy_select_page(bootnet_u16 page)
{
	return jr_phy_select_page_at(JR_BOOT_PHY, page);
}

static int jr_phy_read_page(bootnet_u16 page, bootnet_u32 reg,
			    bootnet_u16 *value)
{
	int result;

	if (jr_phy_select_page(page))
		return -EIO;
	result = jr_miim_read(JR_BOOT_PHY, reg, value);
	if (jr_phy_select_page(0u))
		return -EIO;
	return result;
}

static int jr_phy_write_page(bootnet_u16 page, bootnet_u32 reg,
			     bootnet_u16 value)
{
	int result;

	if (jr_phy_select_page(page))
		return -EIO;
	result = jr_miim_write(JR_BOOT_PHY, reg, value);
	if (jr_phy_select_page(0u))
		return -EIO;
	return result;
}

static int jr_phy_write_masked_page(bootnet_u16 page, bootnet_u32 reg,
				    bootnet_u16 value, bootnet_u16 mask)
{
	bootnet_u16 old;

	if (jr_phy_read_page(page, reg, &old))
		return -EIO;
	return jr_phy_write_page(page, reg, (old & ~mask) | (value & mask));
}

static int jr_phy_poll_page(bootnet_u16 page, bootnet_u32 reg,
			    bootnet_u16 mask)
{
	bootnet_u32 polls = 500u;
	bootnet_u16 value;
	int status;

	while (polls-- != 0u) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		if (jr_phy_read_page(page, reg, &value))
			return -EIO;
		if (!(value & mask))
			return 0;
		status = jr_delay_us(10000u);
		if (status)
			return status;
	}
	return -ETIMEDOUT;
}

static int jr_atom_select(bootnet_u16 page)
{
	return jr_phy_select_page_at(JR_ATOM_BASE_PHY, page);
}

static int jr_atom_read(bootnet_u32 reg, bootnet_u16 *value)
{
	return jr_miim_read(JR_ATOM_BASE_PHY, reg, value);
}

static int jr_atom_write(bootnet_u32 reg, bootnet_u16 value)
{
	return jr_miim_write(JR_ATOM_BASE_PHY, reg, value);
}

static int jr_atom_masked(bootnet_u32 reg, bootnet_u16 value,
			  bootnet_u16 mask)
{
	bootnet_u16 old;

	if (jr_atom_read(reg, &old))
		return -EIO;
	return jr_atom_write(reg, (old & ~mask) | (value & mask));
}

static int jr_atom_wait_micro(void)
{
	bootnet_u32 polls;
	bootnet_u16 value;
	int status;

	if (jr_atom_select(JR_PHY_PAGE_GPIO))
		return -EIO;
	for (polls = 0; polls < 1000u; polls++) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		if (jr_atom_read(18u, &value))
			return -EIO;
		if (!(value & 0x8000u))
			return jr_atom_select(JR_PHY_PAGE_STD);
		status = jr_delay_abortable_ms(1u);
		if (status)
			return status;
	}
	return -ETIMEDOUT;
}

static int jr_atom_command(bootnet_u16 command)
{
	if (jr_atom_select(JR_PHY_PAGE_GPIO) || jr_atom_write(18u, command))
		return -EIO;
	return jr_atom_wait_micro();
}

static int jr_atom_lcpll_reset(void)
{
	static const bootnet_u16 first[] = {0x8023u, 0xd7ddu, 0x8686u, 0x80c0u};
	static const bootnet_u16 second[] = {0x82d6u, 0x80c0u};
	bootnet_u32 i;
	bootnet_u16 ignored;
	int status;

	for (i = 0; i < ARRAY_SIZE(first); i++) {
		status = jr_atom_command(first[i]);
		if (status)
			return status;
	}
	status = jr_delay_abortable_ms(10u);
	if (status)
		return status;
	for (i = 0; i < ARRAY_SIZE(second); i++) {
		status = jr_atom_command(second[i]);
		if (status)
			return status;
	}
	status = jr_delay_abortable_ms(10u);
	if (status)
		return status;
	if (jr_atom_select(JR_PHY_PAGE_EXT3) || jr_atom_read(20u, &ignored))
		return -EIO;
	return jr_delay_abortable_ms(110u);
}

static int jr_atom_rev_c_script(void)
{
	bootnet_u32 i;

	if (jr_atom_select(JR_PHY_PAGE_STD) ||
	    jr_atom_masked(22u, 0x0001u, 0x0001u) ||
	    jr_atom_write(24u, 0x0040u) ||
	    jr_atom_select(JR_PHY_PAGE_EXT2) ||
	    jr_atom_write(16u, 0x02beu) ||
	    jr_atom_select(JR_PHY_PAGE_TEST) ||
	    jr_atom_write(20u, 0x4420u) || jr_atom_write(24u, 0x0c00u) ||
	    jr_atom_write(9u, 0x18c8u) ||
	    jr_atom_masked(8u, 0x8000u, 0x8000u) ||
	    jr_atom_write(5u, 0x1320u) || jr_atom_select(JR_PHY_PAGE_TR))
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(jr_atom_rev_c_tr); i++) {
		if (jr_atom_write(18u, jr_atom_rev_c_tr[i].reg18) ||
		    jr_atom_write(17u, jr_atom_rev_c_tr[i].reg17) ||
		    jr_atom_write(16u, jr_atom_rev_c_tr[i].reg16))
			return -EIO;
	}
	if (jr_atom_select(JR_PHY_PAGE_TEST) ||
	    jr_atom_masked(8u, 0x0000u, 0x8000u) ||
	    jr_atom_select(JR_PHY_PAGE_STD) ||
	    jr_atom_masked(22u, 0x0000u, 0x0001u))
		return -EIO;
	return 0;
}

static int jr_atom_micro_assert_reset(void)
{
	int status = jr_atom_command(0x800fu);

	if (status)
		return status;
	if (jr_atom_select(JR_PHY_PAGE_GPIO) ||
	    jr_atom_masked(12u, 0x0000u, 0x0800u) ||
	    jr_atom_write(9u, 0x005bu) || jr_atom_write(10u, 0x005bu) ||
	    jr_atom_masked(12u, 0x0800u, 0x0800u) ||
	    jr_atom_write(18u, 0x800fu) ||
	    jr_atom_masked(0u, 0x0000u, 0x8000u) ||
	    jr_atom_write(18u, 0x0000u) ||
	    jr_atom_masked(12u, 0x0000u, 0x0800u) ||
	    jr_atom_select(JR_PHY_PAGE_STD))
		return -EIO;
	return 0;
}

static int jr_atom_rev_c_patch_load(void)
{
	bootnet_u32 i;
	bootnet_u16 crc;
	int status = jr_atom_micro_assert_reset();

	if (status)
		return status;
	if (jr_atom_select(JR_PHY_PAGE_GPIO) ||
	    jr_atom_write(0u, 0x7009u) || jr_atom_write(12u, 0x5002u) ||
	    jr_atom_write(11u, 0x0000u))
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(jr_atom_rev_c_patch); i++)
		if (jr_atom_write(12u, 0x5000u | jr_atom_rev_c_patch[i]))
			return -EIO;
	if (jr_atom_write(12u, 0x0000u) ||
	    jr_atom_select(JR_PHY_PAGE_STD) ||
	    jr_atom_select(JR_PHY_PAGE_GPIO) ||
	    jr_atom_write(0u, 0x4018u) || jr_atom_write(0u, 0xc018u) ||
	    jr_atom_select(JR_PHY_PAGE_EXT) ||
	    jr_atom_write(25u, 0x4000u) ||
	    jr_atom_write(26u, ARRAY_SIZE(jr_atom_rev_c_patch) + 1u) ||
	    jr_atom_select(JR_PHY_PAGE_GPIO) || jr_atom_write(18u, 0x8008u))
		return -EIO;
	status = jr_atom_wait_micro();
	if (status)
		return status;
	if (jr_atom_select(JR_PHY_PAGE_EXT) || jr_atom_read(25u, &crc) ||
	    jr_atom_select(JR_PHY_PAGE_STD))
		return -EIO;
	jr_aux1 = crc;
	if (crc != 0x8f96u)
		return -EIO;
	if (jr_atom_select(JR_PHY_PAGE_GPIO) ||
	    jr_atom_write(3u, 0x1defu) || jr_atom_write(4u, 0x4012u) ||
	    jr_atom_write(5u, 0x09b8u) || jr_atom_write(6u, 0x401au) ||
	    jr_atom_write(7u, 0x3c47u) || jr_atom_write(8u, 0x4020u) ||
	    jr_atom_write(12u, 0x0700u) || jr_atom_write(0u, 0x4018u) ||
	    jr_atom_write(0u, 0xc018u) || jr_atom_select(JR_PHY_PAGE_STD))
		return -EIO;
	return 0;
}

static int jr_atom12_package_init(void)
{
	bootnet_u16 id1, id2, value;
	int status;

	jr_detail = 0x080u;
	if (jr_atom_select(JR_PHY_PAGE_STD) || jr_atom_read(2u, &id1) ||
	    jr_atom_read(3u, &id2))
		return -EIO;
	jr_aux0 = ((bootnet_u32)id1 << 16) | id2;
	if (id1 != 0x0007u || id2 != 0x06e2u)
		return -ENODEV;
	if (jr_atom_select(JR_PHY_PAGE_EXT) || jr_atom_read(23u, &value) ||
	    jr_atom_select(JR_PHY_PAGE_STD))
		return -EIO;
	jr_aux1 = (value >> 11) & 0x1fu;
	if (jr_aux1 != 0u)
		return -ENODEV;
	jr_detail = 0x081u;
	status = jr_atom_lcpll_reset();
	if (status)
		return status;
	jr_detail = 0x082u;
	if (jr_atom_rev_c_script())
		return -EIO;
	jr_detail = 0x083u;
	return jr_atom_rev_c_patch_load();
}

static int jr_phy_micro_command(bootnet_u16 command, bootnet_u16 *result)
{
	bootnet_u32 polls;
	bootnet_u16 value;
	int status;

	if (jr_phy_select_page(JR_PHY_PAGE_GPIO) ||
	    jr_miim_write(JR_BOOT_PHY, 18u, command))
		return -EIO;
	for (polls = 0; polls < 1000u; polls++) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		if (jr_miim_read(JR_BOOT_PHY, 18u, &value))
			return -EIO;
		if (!(value & 0x8000u)) {
			if (result)
				*result = value;
			return jr_phy_select_page(JR_PHY_PAGE_STD);
		}
		status = jr_delay_abortable_ms(1u);
		if (status)
			return status;
	}
	return -ETIMEDOUT;
}

static int jr_phy_patch_suspend(int suspend)
{
	bootnet_u16 value;

	if (jr_phy_read_page(JR_PHY_PAGE_GPIO, 18u, &value))
		return -EIO;
	if (suspend) {
		if (!(value & 0x4000u))
			return jr_phy_micro_command(0x800fu, NULL);
	} else if (value & 0x4000u) {
		return jr_phy_micro_command(0x8009u, NULL);
	}
	return 0;
}

static int jr_phy_soft_reset(void)
{
	bootnet_u16 value;
	int status;

	if (jr_phy_read_page(JR_PHY_PAGE_STD, 0u, &value) ||
	    jr_phy_write_page(JR_PHY_PAGE_STD, 0u, value | 0x8000u))
		return -EIO;
	status = jr_delay_abortable_ms(1u);
	if (status)
		return status;
	return jr_phy_poll_page(JR_PHY_PAGE_STD, 0u, 0x8000u);
}

static int jr_phy_atom_autoneg(void)
{
	bootnet_u16 value;
	int status = jr_phy_patch_suspend(1);

	if (status)
		return status;
	if (jr_phy_write_masked_page(JR_PHY_PAGE_TEST, 8u, 0x8000u, 0x8000u) ||
	    jr_phy_write_page(JR_PHY_PAGE_TR, 16u, 0xa7fau) ||
	    jr_phy_write_masked_page(JR_PHY_PAGE_TR, 17u, 0x1000u, 0x1000u) ||
	    jr_phy_write_page(JR_PHY_PAGE_TR, 16u, 0x87fau) ||
	    jr_phy_write_page(JR_PHY_PAGE_TR, 16u, 0xaf82u) ||
	    jr_phy_write_masked_page(JR_PHY_PAGE_TR, 17u, 0x0002u, 0x000fu) ||
	    jr_phy_write_page(JR_PHY_PAGE_TR, 16u, 0x8f82u) ||
	    jr_phy_write_masked_page(JR_PHY_PAGE_TEST, 8u, 0x0000u, 0x8000u))
		return -EIO;
	status = jr_phy_patch_suspend(0);
	if (status)
		return status;
	if (jr_phy_write_masked_page(JR_PHY_PAGE_STD, 18u, 0x0000u, 0x0080u) ||
	    jr_phy_write_masked_page(JR_PHY_PAGE_EXT, 19u, 0x0000u, 0x000cu) ||
	    jr_phy_write_page(JR_PHY_PAGE_STD, 4u, 0x01e1u) ||
	    jr_phy_read_page(JR_PHY_PAGE_STD, 9u, &value))
		return -EIO;
	value = (value & ~0x0f00u) | 0x0700u;
	if (jr_phy_write_page(JR_PHY_PAGE_STD, 9u, value) ||
	    jr_phy_read_page(JR_PHY_PAGE_STD, 0u, &value))
		return -EIO;
	value = (value & ~0x1800u) | 0x1000u;
	if (jr_phy_write_page(JR_PHY_PAGE_STD, 0u, value))
		return -EIO;
	return jr_phy_write_page(JR_PHY_PAGE_STD, 0u, value | 0x0200u);
}

static int jr_phy_sgmii_reset(void)
{
	int status;

	if (jr_phy_write_masked_page(JR_PHY_PAGE_GPIO, 19u, 0x4000u, 0xc000u))
		return -EIO;
	status = jr_phy_patch_suspend(1);
	if (status)
		return status;
	status = jr_phy_micro_command(0x80b0u, NULL);
	if (status)
		return status;
	status = jr_phy_patch_suspend(0);
	if (status)
		return status;
	if (jr_phy_write_page(JR_PHY_PAGE_STD, 23u, 0x0000u))
		return -EIO;
	return jr_phy_soft_reset();
}

static int jr_table_command(bootnet_u32 word, bootnet_u32 value)
{
	if (jr_read(word) & 1u)
		return -EBUSY;
	jr_write(word, value);
	return jr_poll_clear(word, 1u);
}

static int jr_phy_cold_init(void)
{
	bootnet_u16 id1, id2, value;
	int status = jr_atom12_package_init();

	if (status)
		return status;
	jr_detail = 0x100u;
	if (jr_phy_select_page(0u) || jr_miim_read(JR_BOOT_PHY, 2u, &id1) ||
	    jr_miim_read(JR_BOOT_PHY, 3u, &id2))
		return -EIO;
	jr_aux0 = ((bootnet_u32)id1 << 16) | id2;
	if (id1 != 7u || id2 != 0x06e2u)
		return -ENODEV;
	if (jr_phy_read_page(JR_PHY_PAGE_EXT, 23u, &value))
		return -EIO;
	jr_aux1 = (value >> 11) & 0x1fu;
	if (jr_aux1 != 1u)
		return -ENODEV;
	jr_detail = 0x105u;
	if (jr_phy_write_page(0u, 4u, 0u) ||
	    jr_phy_write_page(0u, 0u, 0x1840u) ||
	    jr_phy_write_page(0u, 18u, 0x0008u) ||
	    jr_phy_write_page(1u, 19u, 0u) ||
	    jr_phy_write_page(3u, 16u, 0x2100u) ||
	    jr_phy_write_page(1u, 20u, 0xa018u))
		return -EIO;
	return jr_delay_us(192000u);
}

static int jr_phy_activate(void)
{
	bootnet_u16 value;
	int status;

	jr_detail = 0x110u;
	status = jr_phy_sgmii_reset();
	if (status)
		return status;
	jr_detail = 0x140u;
	if (jr_phy_read_page(2u, 17u, &value) ||
	    jr_phy_write_page(2u, 17u, value | 0x8000u) ||
	    jr_phy_write_page(10800u, 25u, 0u) ||
	    jr_phy_read_page(1u, 20u, &value))
		return -EIO;
	value = (value & ~0x001cu) | 0x0018u;
	if (jr_phy_write_page(1u, 20u, value) ||
	    jr_phy_write_masked_page(JR_PHY_PAGE_GPIO, 14u, 0x0000u, 0x3000u))
		return -EIO;
	return jr_phy_atom_autoneg();
}

static void jr_port_buffers(void)
{
	bootnet_u32 queue;

	for (queue = 0; queue < 8u; queue++) {
		jr_write(0x002c6208u + queue, (30u << 16) | 30u);
		jr_write(0x002c6408u + queue, 40u << 16);
		jr_write(0x002c6608u + queue, 0u);
		jr_write(0x002c6008u + queue,
			 (2u << 16) | (queue < 6u ? 0xa1u : 0x81u));
		jr_write(0x00306208u + queue, queue < 6u ?
			 (230u << 16) | 217u : (120u << 16) | 107u);
		jr_write(0x00306408u + queue, queue < 6u ?
			 (290u << 16) | 15u : (180u << 16) | 120u);
		jr_write(0x00306608u + queue, 0u);
		jr_write(0x00306008u + queue, queue < 6u ? 0xafu : 0x83u);
	}
	jr_write(0x002c5241u, (180u << 16) | 180u);
	jr_write(0x002c5281u, 200u << 16);
	jr_write(0x002c52c1u, 190u);
	jr_write(0x002c5201u, 9u);
	jr_write(0x00308881u, (600u << 16) | 587u);
	jr_write(0x00308901u, 660u << 16);
	jr_write(0x00308a01u, 620u);
	jr_write(0x00308981u, 200u);
	jr_write(0x00308801u, 3u);
	jr_write(0x002c5306u, (5280u << 16) | 5280u);
	jr_write(0x00305e06u, (8660u << 16) | 8647u);
	jr_write(0x002c5305u, (5840u << 16) | 1u);
	jr_write(0x00305e05u, (8720u << 16) | 3u);
}

static int jr_cpu_copy_init(void)
{
	bootnet_u32 saved;

	if (jr_table_command(0x000a4024u, 0x2013u))
		return -EIO;
	saved = jr_read(0x000a422bu);
	jr_write(0x000a4228u, 0x008001c2u);
	jr_write(0x000a4229u, 0u);
	jr_write(0x000a422au, 0xffffffffu);
	jr_write(0x000a422bu, saved);
	if (jr_table_command(0x000a4024u, 0x2011u) ||
	    jr_table_command(0x000a4024u, 0x2013u) ||
	    jr_read(0x000a4228u) != 0x008001c2u ||
	    jr_read(0x000a4229u) != 0u ||
	    jr_read(0x000a422au) != 0xffffffffu)
		return -EIO;
	jr_write_masked(JR_XTR_MAP, 1u, 0x31u);
	jr_write_masked(0x00094059u, 0xf3f2u, 0xf3f2u);
	return 0;
}

static void jr_dev1g_init(void)
{
	jr_write(JR_DEV1G_BASE + 0x00u, 0x11u);
	jr_write(JR_DEV1G_BASE + 0x12u, 0x485u);
	jr_write(JR_DEV1G_BASE + 0x0eu, 0x11u);
	jr_write(JR_DEV1G_BASE + 0x13u, 0x143u);
	jr_write(JR_DEV1G_BASE + 0x16u, 1u);
	jr_write(JR_DEV1G_BASE + 0x17u, 0x100u);
	jr_write_masked(JR_DEV1G_BASE + 0x15u, 1u, 1u);
	jr_write_masked(JR_DEV1G_BASE + 0x1au, 0u, 1u);
	jr_write_masked(JR_DEV1G_BASE + 0x03u, 0x0b00u, 0x1f00u);
	jr_write(JR_DEV1G_BASE + 0x00u, 0x200000u);
	jr_write(JR_DEV1G_BASE + 0x0du, 0x11u);
}

static int jr_port_activate(void)
{
	jr_detail = 0x500u;
	if (jr_read(JR_XTR_GRP_CFG) != 5u)
		return -EIO;
	jr_dev1g_init();
	if (jr_read(JR_DEV1G_BASE) != 0x00200000u ||
	    jr_read(JR_DEV1G_BASE + 0x0du) != 0x11u)
		return -EIO;
	return jr_phy_activate();
}

static void jr_port_datapath_init(void)
{
	bootnet_u32 queue;

	jr_write(0x000b02ecu, 3u);
	jr_write(0x000b02bau, 0x0c1010ffu);
	for (queue = 0; queue < 8u; queue++) {
		jr_write_masked(0x002c6008u + queue, 1u, 1u);
		jr_write_masked(0x00306008u + queue, 1u, 1u);
	}
	jr_write_masked(0x000aca54u, 0u, 0xffu);
	jr_write_masked(0x00204e9bu, 2u, 2u);
	jr_write_masked(0x00204e9cu, 2u, 2u);
}

static int jr_link_ready(void)
{
	bootnet_u16 ignored, bmsr;

	if (jr_phy_select_page(0u) ||
	    jr_miim_read(JR_BOOT_PHY, 1u, &ignored) ||
	    jr_miim_read(JR_BOOT_PHY, 1u, &bmsr))
		return 0;
	return (bmsr & 0x0024u) == 0x0024u &&
	       (jr_read(JR_DEV1G_BASE + 0x1fu) & 0x11u) == 0x11u;
}

static int jr_bootnet_init(void)
{
	bootnet_u32 polls;
	bootnet_u16 bmsr;
	int status;

	jr_active = 0;
	jr_detail = 0x001u;
	jr_aux0 = jr_read(JR_CHIP_ID);
	jr_aux1 = JR_BOOT_PHYSICAL_PORT;
	if (jr_aux0 != JR_CHIP_ID_VALUE)
		return -ENODEV;
	jr_active = 1;
	status = jr_phy_cold_init();
	if (status)
		return status;
	jr_detail = 0x200u;
	status = jr_master_lifecycle_init();
	if (status)
		return status;
	jr_detail = 0x300u;
	status = jr_port_activate();
	if (status)
		return status;
	jr_detail = 0x600u;
	for (polls = 0; polls < 3000u; polls++) {
		if (jr_abort_requested())
			return BOOTNET_BACKEND_ABORTED;
		if (jr_link_ready()) {
			jr_detail = 0x700u;
			jr_port_datapath_init();
			jr_detail = 0x800u;
			jr_port_buffers();
			jr_detail = 0x900u;
			if (jr_cpu_copy_init())
				return -EIO;
			jr_detail = 0u;
			return 0;
		}
		status = jr_delay_us(10000u);
		if (status)
			return status;
	}
	if (!jr_phy_select_page(0u) && !jr_miim_read(JR_BOOT_PHY, 1u, &bmsr))
		jr_aux0 = bmsr;
	jr_aux1 = jr_read(JR_DEV1G_BASE + 0x1fu);
	return -ETIMEDOUT;
}

static bootnet_u32 jr_word_from_bytes(const bootnet_u8 *data,
				      bootnet_u32 length)
{
	bootnet_u32 value = 0, i;

	for (i = 0; i < length; i++)
		value |= (bootnet_u32)data[i] << (8u * i);
	return value;
}

static int jr_send(struct udevice *dev, void *packet, int length)
{
	static const bootnet_u8 ifh[24] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
		0x80, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x30, 0x00, 0x10, 0x03, 0x00, 0x00
	};
	const bootnet_u8 *frame = packet;
	bootnet_u32 status = 0, polls = JR_POLL_LIMIT;
	bootnet_u32 frame_length, frame_size, offset, bytes;

	(void)dev;
	if (!frame || length < 14 || length > (int)JR_MAX_FRAME)
		return -EINVAL;
	if (!jr_active)
		return -ENETDOWN;
	frame_size = length;
	frame_length = frame_size < 60u ? 60u : frame_size;
	do {
		if (jr_abort_requested())
			return -EINTR;
		status = jr_read(JR_INJ_STATUS);
		if (status & JR_INJ_WATERMARK)
			return -ENOSPC;
		if (status & JR_INJ_READY)
			break;
	} while (polls-- != 0u);
	if (!(status & JR_INJ_READY))
		return -ETIMEDOUT;
	jr_write(JR_INJ_CTRL, JR_INJ_SOF);
	for (offset = 0; offset < sizeof(ifh); offset += 4u)
		jr_write(JR_INJ_WR, jr_word_from_bytes(ifh + offset, 4u));
	for (offset = 0; offset < frame_length; offset += 4u) {
		bytes = min(frame_length - offset, 4u);
		if (offset >= frame_size) {
			jr_write(JR_INJ_WR, 0u);
		} else {
			bytes = min(bytes, frame_size - offset);
			jr_write(JR_INJ_WR, jr_word_from_bytes(frame + offset, bytes));
		}
	}
	jr_write(JR_INJ_CTRL, JR_INJ_EOF | ((frame_length & 3u) << 16));
	jr_write(JR_INJ_WR, 0u);
	status = jr_read(JR_INJ_STATUS);
	jr_aux0 = (status << 16) | (jr_read(0x0009005du) & 0xffffu);
	jr_aux1 = jr_read(0x00008033u);
	return (status & JR_INJ_READY) && !(status & JR_INJ_WATERMARK) ?
		0 : -EIO;
}

/* Return zero for data, one for EOF data, and negative for an error. */
static int jr_rx_word(bool first, bootnet_u32 *value, bootnet_u32 *valid)
{
	bootnet_u32 token, polls = JR_POLL_LIMIT;

	if (jr_abort_requested())
		return -EINTR;
	token = jr_read(JR_XTR_RD);
	if (token == JR_XTR_NOT_READY) {
		if (first)
			return -EAGAIN;
		do {
			if (jr_abort_requested())
				return -EINTR;
			if (polls-- == 0u)
				return -ETIMEDOUT;
			token = jr_read(JR_XTR_RD);
		} while (token == JR_XTR_NOT_READY);
	}
	if (token == JR_XTR_ABORT)
		return -EIO;
	if (token >= JR_XTR_EOF_0 && token <= JR_XTR_PRUNED) {
		*valid = 4u - (token & 3u);
		token = jr_read(JR_XTR_RD);
		if (token == JR_XTR_ESCAPE)
			token = jr_read(JR_XTR_RD);
		*value = token;
		return 1;
	}
	if (token == JR_XTR_ESCAPE)
		token = jr_read(JR_XTR_RD);
	*value = token;
	*valid = 4u;
	return 0;
}

static void jr_store_word(bootnet_u8 *frame, bootnet_u32 maximum,
			  bootnet_u32 offset, bootnet_u32 value,
			  bootnet_u32 valid)
{
	bootnet_u32 i;

	for (i = 0; i < valid && offset + i < maximum; i++)
		frame[offset + i] = value >> (8u * i);
}

static void jr_discard_frame(void)
{
	bootnet_u32 value, valid, words;

	for (words = 0; words < JR_FRAME_WORD_LIMIT; words++)
		if (jr_rx_word(false, &value, &valid) != 0)
			break;
}

static int jr_recv(struct udevice *dev, int flags, uchar **packetp)
{
	bootnet_u8 prefix[28];
	bootnet_u8 *frame = net_rx_packets[0];
	bootnet_u32 value, valid, offset, word, chip_port;
	int overflow = 0, status;

	(void)dev;
	(void)flags;
	if (!(jr_read(JR_XTR_DATA_PRESENT) & 1u))
		return -EAGAIN;
	for (word = 0; word < ARRAY_SIZE(prefix) / 4u; word++) {
		status = jr_rx_word(word == 0u, &value, &valid);
		if (status || valid != 4u) {
			if (status != 1)
				jr_discard_frame();
			return status == -EINTR ? status : -EAGAIN;
		}
		jr_store_word(prefix, sizeof(prefix), word * 4u, value, 4u);
	}
	chip_port = ((prefix[23] & 7u) << 3) | (prefix[24] >> 5);
	offset = 0;
	do {
		if (offset / 4u >= JR_FRAME_WORD_LIMIT) {
			jr_discard_frame();
			return -EOVERFLOW;
		}
		status = jr_rx_word(false, &value, &valid);
		if (status < 0) {
			jr_discard_frame();
			return status == -EINTR ? status : -EAGAIN;
		}
		if (offset < JR_MAX_FRAME + 4u)
			jr_store_word(frame, JR_MAX_FRAME, offset, value, valid);
		else
			overflow = 1;
		offset += valid;
	} while (status == 0);
	if (chip_port != JR_BOOT_CHIP_PORT || overflow || offset < 18u ||
	    offset > JR_MAX_FRAME + 4u)
		return -EAGAIN;
	*packetp = frame;
	return offset - 4u;
}

static int jr_start(struct udevice *dev)
{
	int status;

	(void)dev;
	if (jr_initialized) {
		jr_write(JR_DEV1G_BASE + 0x0du, 0x11u);
		jr_active = 1;
		return 0;
	}
	status = jr_bootnet_init();
	if (!status)
		jr_initialized = 1;
	else
		jr_active = 0;
	if (status)
		printf("Jaguar1 bootnet init failed: %d detail=%08x aux=%08x/%08x\n",
		       status, jr_detail, jr_aux0, jr_aux1);
	return status;
}

static void jr_stop(struct udevice *dev)
{
	(void)dev;
	if (jr_active != 1)
		return;
	jr_write(JR_DEV1G_BASE + 0x0du, 0u);
	jr_active = 0;
}

static int jr_probe(struct udevice *dev)
{
	(void)dev;
	if (jr_read(JR_CHIP_ID) != JR_CHIP_ID_VALUE)
		return -ENODEV;
	return 0;
}

static const struct eth_ops jr_ops = {
	.start = jr_start,
	.stop = jr_stop,
	.send = jr_send,
	.recv = jr_recv,
};

static const struct udevice_id jr_ids[] = {
	{ .compatible = "meraki,jaguar1-bootnet" },
	{ }
};

U_BOOT_DRIVER(meraki_jaguar1_bootnet) = {
	.name = "meraki-jaguar1-bootnet",
	.id = UCLASS_ETH,
	.of_match = jr_ids,
	.probe = jr_probe,
	.ops = &jr_ops,
	.plat_auto = sizeof(struct eth_pdata),
};
