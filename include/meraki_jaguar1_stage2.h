/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __MERAKI_JAGUAR1_STAGE2_H
#define __MERAKI_JAGUAR1_STAGE2_H

#include <linux/types.h>

#define MERAKI_JAGUAR1_STAGE2_MAGIC		0x5332534d
#define MERAKI_JAGUAR1_STAGE2_VERSION		2
#define MERAKI_JAGUAR1_STAGE2_HEADER_SIZE	0x1000
#define MERAKI_JAGUAR1_STAGE2_SLOT_SIZE		0xc0000
#define MERAKI_JAGUAR1_STAGE2_PAYLOAD_MAX	0xbf000
#define MERAKI_JAGUAR1_STAGE2_LOAD_ADDR		0x81000000
#define MERAKI_JAGUAR1_STAGE2_ENTRY_ADDR	0x81000000
#define MERAKI_JAGUAR1_STAGE2_SLOT_A_OFFSET	0x040000
#define MERAKI_JAGUAR1_STAGE2_SLOT_B_OFFSET	0x100000
#define MERAKI_JAGUAR1_HANDOFF_ADDR		0x80fff000
#define MERAKI_JAGUAR1_RECOVERY_MAGIC		0x52564352

struct meraki_jaguar1_stage2_header {
	__le32 magic;
	__le16 version;
	__le16 header_size;
	__le64 generation;
	__le32 payload_size;
	__le32 load_address;
	__le32 entry_address;
	__le32 board_id;
	__le32 payload_crc32;
	__le32 header_crc32;
	__le32 flags;
	u8 reserved[MERAKI_JAGUAR1_STAGE2_HEADER_SIZE - 44];
} __packed;

_Static_assert(sizeof(struct meraki_jaguar1_stage2_header) ==
	       MERAKI_JAGUAR1_STAGE2_HEADER_SIZE, "invalid stage2 header size");

#endif
