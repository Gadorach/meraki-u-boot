// SPDX-License-Identifier: GPL-2.0+
/* Shared board support for Cisco Meraki Jaguar1 switches. */
#include <config.h>
#include <env.h>
#include <init.h>
#include <net.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <mach/common.h>

DECLARE_GLOBAL_DATA_PTR;

#define JAGUAR1_PART_ID		0x7460
#define JAGUAR1_PART_ID_X(x)	(((x) >> 12) & 0xffff)
#define JAGUAR1_REV_ID_X(x)	(((x) >> 28) & 0xf)

#define SLAVE_GCB		((void __iomem *)0x5c010000)
#define SLAVE_PI_MODE		((void __iomem *)0x5c03000c)

#define SIO_PORT_CONFIG(g, p)	(4 * (0x42 + (g) * 48 + 9 + (p)))
#define SIO_PORT_ENABLE(g)	(4 * (0x42 + (g) * 48 + 41))
#define SIO_CONFIG(g)		(4 * (0x42 + (g) * 48 + 42))
#define SIO_CLOCK(g)		(4 * (0x42 + (g) * 48 + 43))
#define SIO_CONFIG_BURST_GAP(x)	(((x) & 0x1f) << 7)
#define SIO_CONFIG_PORT_WIDTH(x)	(((x) & 0x3) << 2)
#define SIO_CONFIG_AUTO_REPEAT	BIT(5)

#define BOARD_EEPROM_ADDR	0x51
#define BOARD_MAC_OFFSET	0x66
#define I2C_SCL			14
#define I2C_SDA			15
#define I2C_DELAY_US		5

static bool slave_present;

void board_debug_uart_init(void)
{
	mscc_gpio_set_alternate(10, 1);
	mscc_gpio_set_alternate(11, 1);
}

static void jaguar1_gpio_init(void)
{
	u32 outputs = BIT(20) | BIT(21) | BIT(23);
	u32 enables = BIT(12) | BIT(20) | BIT(21) | BIT(23);

	writel(outputs, BASE_DEVCPU_GCB + GPIO_OUT);
	writel(enables, BASE_DEVCPU_GCB + GPIO_OE);
	mscc_gpio_set_alternate(4, 1);
	mscc_gpio_set_alternate(5, 1);
	board_debug_uart_init();

#ifdef CONFIG_TARGET_MERAKI_MS320_24
	setbits_le32(BASE_DEVCPU_GCB + GPIO_OUT, BIT(18));
	setbits_le32(BASE_DEVCPU_GCB + GPIO_OE, BIT(18));
#endif
}

static bool jaguar1_slave_probe(void)
{
	u32 id, status;

	clrsetbits_le32(BASE_CFG + ICPU_PI_MST_CFG, GENMASK(4, 0), 0x1f);
	setbits_le32(BASE_CFG + ICPU_GENERAL_CTRL,
		     ICPU_GENERAL_CTRL_IF_MASTER_PI_ENA);
	setbits_le32(BASE_CFG + ICPU_PI_MST_CTRL(3),
		     ICPU_PI_MST_CTRL_DEVICE_PACED_TIMEOUT_ENA |
		     ICPU_PI_MST_CTRL_DEVICE_PACED_TIMEOUT(7));
	mscc_gpio_set_alternate(21, 1);

	writel(PERF_SOFT_RST_SOFT_CHIP_RST, SLAVE_GCB + PERF_SOFT_RST);
	mscc_vcoreiii_nop_delay(3);
	writel(0x18181818, SLAVE_PI_MODE);
	writel(0x18181818, SLAVE_PI_MODE);
	setbits_le32(BASE_CFG + ICPU_PI_MST_CTRL(3), 0x00c200b3);

	writel(ICPU_PI_MST_STATUS_TIMEOUT_ERR_STICKY,
	       BASE_CFG + ICPU_PI_MST_STATUS(3));
	id = readl(SLAVE_GCB + CHIP_ID);
	status = readl(BASE_CFG + ICPU_PI_MST_STATUS(3));
	if ((status & ICPU_PI_MST_STATUS_TIMEOUT_ERR_STICKY) ||
	    JAGUAR1_PART_ID_X(id) != JAGUAR1_PART_ID) {
		mscc_gpio_set_alternate(21, 0);
		clrbits_le32(BASE_DEVCPU_GCB + GPIO_OUT, BIT(21));
		return false;
	}

	return true;
}

static void jaguar1_sgpio_init(void __iomem *gcb)
{
	int port;

	writel(~0U, gcb + SIO_PORT_ENABLE(1));
	writel(0x14, gcb + SIO_CLOCK(1));
	writel(SIO_CONFIG_BURST_GAP(0x1f) |
	       SIO_CONFIG_PORT_WIDTH(1) | SIO_CONFIG_AUTO_REPEAT,
	       gcb + SIO_CONFIG(1));
	for (port = 0; port < 32; port++)
		writel(0x249, gcb + SIO_PORT_CONFIG(1, port));
}

static void jaguar1_phy_release(void)
{
	u16 value;
	int retry;

	writel(BIT(12), BASE_DEVCPU_GCB + GPIO_OUT_SET);
	if (mscc_phy_rd(0, 0, 0, &value))
		goto enable_fan;
	if (mscc_phy_wr(0, 0, 0, value | BIT(15)))
		goto enable_fan;
	for (retry = 0; retry < 1000; retry++) {
		if (!mscc_phy_rd(0, 0, 0, &value) && !(value & BIT(15)))
			break;
		mdelay(1);
	}
	if (retry == 1000)
		puts("Warning: copper PHY reset timed out\n");

enable_fan:
	mscc_phy_wr(0, 0, 31, 16);
	mscc_phy_wr(0, 0, 13, 0x0003);
	mscc_phy_wr(0, 0, 17, 0x0001);
	mscc_phy_wr(0, 0, 16, 0x0001);
	mscc_phy_wr(0, 0, 31, 0);
}

int board_early_init_r(void)
{
	jaguar1_gpio_init();
	setbits_le32(BASE_CFG + ICPU_GENERAL_CTRL,
		     ICPU_GENERAL_CTRL_IF_MASTER_SPI_ENA);
	(void)readl(BASE_CFG + ICPU_GENERAL_CTRL);
	writel(0, BASE_CFG + ICPU_SW_MODE);
	sync();
	jaguar1_phy_release();
	jaguar1_sgpio_init(BASE_DEVCPU_GCB);
	if (IS_ENABLED(CONFIG_MERAKI_JAGUAR1_EARLY_SWITCH_INIT)) {
		slave_present = jaguar1_slave_probe();
		if (slave_present) {
			setbits_le32(SLAVE_GCB + GPIO_ALT(0), BIT(4) | BIT(5));
			clrbits_le32(SLAVE_GCB + GPIO_ALT(1), BIT(4) | BIT(5));
			jaguar1_sgpio_init(SLAVE_GCB);
		}
	}
	gd->bd->bi_boot_params = CFG_SYS_SDRAM_BASE + 0x400;
	return 0;
}

static void i2c_release(unsigned int pin)
{
	clrbits_le32(BASE_DEVCPU_GCB + GPIO_OE, BIT(pin));
	udelay(I2C_DELAY_US);
}

static void i2c_low(unsigned int pin)
{
	writel(BIT(pin), BASE_DEVCPU_GCB + GPIO_OUT_CLR);
	setbits_le32(BASE_DEVCPU_GCB + GPIO_OE, BIT(pin));
	udelay(I2C_DELAY_US);
}

static int i2c_get(unsigned int pin)
{
	return !!(readl(BASE_DEVCPU_GCB + GPIO_IN) & BIT(pin));
}

static void i2c_start(void)
{
	i2c_release(I2C_SDA);
	i2c_release(I2C_SCL);
	i2c_low(I2C_SDA);
	i2c_low(I2C_SCL);
}

static void i2c_stop(void)
{
	i2c_low(I2C_SDA);
	i2c_release(I2C_SCL);
	i2c_release(I2C_SDA);
}

static int i2c_send_byte(u8 value)
{
	int bit, nack;

	for (bit = 7; bit >= 0; bit--) {
		if (value & BIT(bit))
			i2c_release(I2C_SDA);
		else
			i2c_low(I2C_SDA);
		i2c_release(I2C_SCL);
		i2c_low(I2C_SCL);
	}
	i2c_release(I2C_SDA);
	i2c_release(I2C_SCL);
	nack = i2c_get(I2C_SDA);
	i2c_low(I2C_SCL);
	return nack ? -EIO : 0;
}

static u8 i2c_recv_byte(bool last)
{
	u8 value = 0;
	int bit;

	i2c_release(I2C_SDA);
	for (bit = 7; bit >= 0; bit--) {
		i2c_release(I2C_SCL);
		if (i2c_get(I2C_SDA))
			value |= BIT(bit);
		i2c_low(I2C_SCL);
	}
	if (last)
		i2c_release(I2C_SDA);
	else
		i2c_low(I2C_SDA);
	i2c_release(I2C_SCL);
	i2c_low(I2C_SCL);
	i2c_release(I2C_SDA);
	return value;
}

static int board_eeprom_read(u16 offset, u8 *data, size_t length)
{
	u32 alt0 = readl(BASE_DEVCPU_GCB + GPIO_ALT(0));
	u32 alt1 = readl(BASE_DEVCPU_GCB + GPIO_ALT(1));
	u32 oe = readl(BASE_DEVCPU_GCB + GPIO_OE);
	u32 out = readl(BASE_DEVCPU_GCB + GPIO_OUT);
	int ret = -EIO;
	size_t i;

	clrbits_le32(BASE_DEVCPU_GCB + GPIO_ALT(0), BIT(I2C_SCL) | BIT(I2C_SDA));
	clrbits_le32(BASE_DEVCPU_GCB + GPIO_ALT(1), BIT(I2C_SCL) | BIT(I2C_SDA));
	i2c_start();
	if (i2c_send_byte(BOARD_EEPROM_ADDR << 1) ||
	    i2c_send_byte(offset >> 8) || i2c_send_byte(offset))
		goto out_stop;
	i2c_start();
	if (i2c_send_byte((BOARD_EEPROM_ADDR << 1) | 1))
		goto out_stop;
	for (i = 0; i < length; i++)
		data[i] = i2c_recv_byte(i == length - 1);
	ret = 0;

out_stop:
	i2c_stop();
	writel(out, BASE_DEVCPU_GCB + GPIO_OUT);
	writel(oe, BASE_DEVCPU_GCB + GPIO_OE);
	writel(alt0, BASE_DEVCPU_GCB + GPIO_ALT(0));
	writel(alt1, BASE_DEVCPU_GCB + GPIO_ALT(1));
	return ret;
}

int board_late_init(void)
{
	u8 mac[ETH_ALEN], current[ETH_ALEN];

	if (!eth_env_get_enetaddr("ethaddr", current) &&
	    !board_eeprom_read(BOARD_MAC_OFFSET, mac, sizeof(mac)) &&
	    is_valid_ethaddr(mac))
		eth_env_set_enetaddr("ethaddr", mac);
	return 0;
}

int checkboard(void)
{
	u32 id = readl(BASE_DEVCPU_GCB + CHIP_ID);

	printf("Board: Cisco Meraki %s, Jaguar1 %04x rev %x, slave %s\n",
	       CONFIG_MERAKI_JAGUAR1_MODEL, JAGUAR1_PART_ID_X(id),
	       JAGUAR1_REV_ID_X(id), slave_present ? "present" : "absent");
#ifdef CONFIG_MERAKI_JAGUAR1_DUAL
	if (!slave_present)
		puts("Warning: this target expects a second Jaguar1\n");
#else
	if (slave_present)
		puts("Warning: this target expects a single Jaguar1\n");
#endif
	return 0;
}
