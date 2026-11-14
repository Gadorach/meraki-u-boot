// SPDX-License-Identifier: GPL-2.0+
#include <cpu_func.h>
#include <hang.h>
#include <u-boot/crc.h>
#include <asm/io.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <meraki_jaguar1_stage2.h>
#include <mach/common.h>

#define RESET_BUTTON_SAMPLES	8
#define RESET_BUTTON_DELAY_MS	2

struct stage2_slot {
	const char *name;
	u32 offset;
	u32 payload_size;
	u64 generation;
	bool valid;
};

static void set_recovery_handoff(bool requested)
{
	volatile u32 *handoff =
		(volatile u32 *)(uintptr_t)MERAKI_JAGUAR1_HANDOFF_ADDR;

	*handoff = requested ? MERAKI_JAGUAR1_RECOVERY_MAGIC : 0;
	flush_cache(MERAKI_JAGUAR1_HANDOFF_ADDR, sizeof(*handoff));
}

static bool reset_button_pressed(void)
{
	int gpio = CONFIG_MERAKI_JAGUAR1_RESET_BUTTON_GPIO;
	int sample;

	if (gpio < 0)
		return false;

	mscc_gpio_set_alternate(gpio, 0);
	clrbits_le32(BASE_DEVCPU_GCB + GPIO_OE, BIT(gpio));
	for (sample = 0; sample < RESET_BUTTON_SAMPLES; sample++) {
		if (readl(BASE_DEVCPU_GCB + GPIO_IN) & BIT(gpio))
			return false;
		mdelay(RESET_BUTTON_DELAY_MS);
	}

	return true;
}

static int validate_slot(struct stage2_slot *slot)
{
	struct meraki_jaguar1_stage2_header header;
	const u8 *flash = (const u8 *)(uintptr_t)(MSCC_FLASH_TO + slot->offset);
	u32 expected_header_crc;
	u32 payload_crc;
	size_t i;

	memcpy(&header, flash, sizeof(header));
	if (le32_to_cpu(header.magic) != MERAKI_JAGUAR1_STAGE2_MAGIC ||
	    le16_to_cpu(header.version) != MERAKI_JAGUAR1_STAGE2_VERSION ||
	    le16_to_cpu(header.header_size) != MERAKI_JAGUAR1_STAGE2_HEADER_SIZE ||
	    le32_to_cpu(header.flags) ||
	    !le32_to_cpu(header.payload_size) ||
	    le32_to_cpu(header.payload_size) > MERAKI_JAGUAR1_STAGE2_PAYLOAD_MAX ||
	    le32_to_cpu(header.load_address) != MERAKI_JAGUAR1_STAGE2_LOAD_ADDR ||
	    le32_to_cpu(header.entry_address) != MERAKI_JAGUAR1_STAGE2_ENTRY_ADDR ||
	    le32_to_cpu(header.board_id) != CONFIG_MERAKI_JAGUAR1_BOARD_ID)
		return -EINVAL;

	for (i = 0; i < sizeof(header.reserved); i++) {
		if (header.reserved[i])
			return -EINVAL;
	}

	expected_header_crc = le32_to_cpu(header.header_crc32);
	header.header_crc32 = 0;
	if (crc32(0, (const u8 *)&header, sizeof(header)) != expected_header_crc)
		return -EBADMSG;

	slot->payload_size = le32_to_cpu(header.payload_size);
	payload_crc = crc32(0, flash + MERAKI_JAGUAR1_STAGE2_HEADER_SIZE,
			    slot->payload_size);
	if (payload_crc != le32_to_cpu(header.payload_crc32))
		return -EBADMSG;

	slot->generation = le64_to_cpu(header.generation);
	slot->valid = true;
	return 0;
}

static void __noreturn boot_slot(const struct stage2_slot *slot)
{
	typedef void __noreturn (*stage2_entry_t)(void);
	const u8 *source = (const u8 *)(uintptr_t)(MSCC_FLASH_TO + slot->offset +
						     MERAKI_JAGUAR1_STAGE2_HEADER_SIZE);
	u8 *destination = (u8 *)(uintptr_t)MERAKI_JAGUAR1_STAGE2_LOAD_ADDR;
	u32 source_crc;

	printf("Loading stage2 slot %s generation %llu (%u bytes)\n",
	       slot->name, (unsigned long long)slot->generation,
	       slot->payload_size);
	source_crc = crc32(0, source, slot->payload_size);
	memcpy(destination, source, slot->payload_size);
	if (crc32(0, destination, slot->payload_size) != source_crc) {
		puts("Stage2 DDR copy verification failed\n");
		hang();
	}

	flush_cache(MERAKI_JAGUAR1_STAGE2_LOAD_ADDR, slot->payload_size);
	((stage2_entry_t)(uintptr_t)MERAKI_JAGUAR1_STAGE2_ENTRY_ADDR)();
	unreachable();
}

int meraki_jaguar1_stage1_boot(void)
{
	struct stage2_slot slots[] = {
		{ .name = "A", .offset = MERAKI_JAGUAR1_STAGE2_SLOT_A_OFFSET },
		{ .name = "B", .offset = MERAKI_JAGUAR1_STAGE2_SLOT_B_OFFSET },
	};
	const struct stage2_slot *selected;
	bool older_requested;
	int ret_a, ret_b;

	ret_a = validate_slot(&slots[0]);
	ret_b = validate_slot(&slots[1]);
	if (ret_a)
		printf("Stage2 slot A invalid (%d)\n", ret_a);
	if (ret_b)
		printf("Stage2 slot B invalid (%d)\n", ret_b);

	if (!slots[0].valid && !slots[1].valid) {
		puts("No valid stage2 slot; staying at the serial prompt\n");
		return -ENOENT;
	}

	older_requested = reset_button_pressed();
	set_recovery_handoff(older_requested);
	if (older_requested && slots[0].valid && slots[1].valid) {
		puts("Reset button held; selecting the older stage2 generation\n");
		if (slots[0].generation < slots[1].generation)
			selected = &slots[0];
		else
			selected = &slots[1];
	} else if (!slots[1].valid ||
		   (slots[0].valid &&
		    slots[0].generation >= slots[1].generation)) {
		selected = &slots[0];
	} else {
		selected = &slots[1];
	}

	boot_slot(selected);
}
