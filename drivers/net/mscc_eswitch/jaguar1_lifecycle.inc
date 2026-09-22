/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Source-locked chip-0 Jaguar1 lifecycle, derived from the MS42P path.
 * Canonical source: openvtss-v0.19.0 vtss_jaguar1_lifecycle.c and plan
 * builders. Runtime-dependent reads are intentionally retained.
 */
#define JR_LC_PORT_COUNT 12653u
#define JR_LC_VCAP_COUNT 22u
#define JR_LC_QOS_COUNT 1716u
#define JR_LC_PACKET_COUNT 117u
#define JR_LC_L2_COUNT 496u
#define JR_LC_MISC_COUNT 0u
#define JR_LC_L3_COUNT 27u
#define JR_LC_TOTAL_COUNT 15031u

#define JR_LC_GROUP_PORT 0u
#define JR_LC_GROUP_VCAP 1u
#define JR_LC_GROUP_QOS 2u
#define JR_LC_GROUP_PACKET 3u
#define JR_LC_GROUP_L2 4u
#define JR_LC_GROUP_MISC 5u
#define JR_LC_GROUP_L3 6u

#define JR_LC_OP_READ 0u
#define JR_LC_OP_WRITE 2u
#define JR_LC_OP_WRITE_LAST_READ 3u
#define JR_LC_OP_WRITE_MASKED 4u
#define JR_LC_OP_POLL_MATCH 5u
#define JR_LC_OP_DELAY_US 9u
#define JR_LC_POLL_UNBOUNDED 2u
#define JR_LC_POLL_BOUNDED 4u
#define JR_LC_SOURCE_POLL_MAX_READS 100u
#define JR_LC_SOURCE_POLL_INTERVAL_US 1000u

#ifndef JR_LIFECYCLE_OP_OBSERVER
#define JR_LIFECYCLE_OP_OBSERVER(group, index, kind, metadata, word, value, \
		mask, interval, max_reads) ((void)0)
#endif

struct jr_lc_op {
	bootnet_u32 word, value, mask, interval_us, max_reads;
	bootnet_u8 kind, metadata;
};

struct jr_lc_port_cursor {
	bootnet_u32 operation, chip_id, rcomp_status, ana_read_value;
	bootnet_u8 sqs_next_port;
	bootnet_u8 sqs[0x278u];
};

static bootnet_u32 jr_lc_last_read = 0xffffffffu;
static bootnet_u32 jr_lc_completed = 0xffffffffu;

static struct jr_lc_op jr_lc_op(bootnet_u32 word, bootnet_u32 value,
		bootnet_u32 mask, bootnet_u8 kind, bootnet_u8 metadata)
{
	struct jr_lc_op op = {
		.word = word, .value = value, .mask = mask, .kind = kind,
		.metadata = metadata,
		.interval_us = metadata == JR_LC_POLL_BOUNDED ? 1000u : 0u,
		.max_reads = metadata == JR_LC_POLL_BOUNDED ? 100u : 0u,
	};
	return op;
}

static struct jr_lc_op jr_lc_read(bootnet_u32 word)
{
	return jr_lc_op(word, 0, 0, JR_LC_OP_READ, 0);
}
static struct jr_lc_op jr_lc_write(bootnet_u32 word, bootnet_u32 value)
{
	return jr_lc_op(word, value, 0, JR_LC_OP_WRITE, 0);
}
static struct jr_lc_op jr_lc_write_last(bootnet_u32 word)
{
	return jr_lc_op(word, 0, 0, JR_LC_OP_WRITE_LAST_READ, 0);
}
static struct jr_lc_op jr_lc_wrm(bootnet_u32 word, bootnet_u32 value,
		bootnet_u32 mask)
{
	return jr_lc_op(word, value, mask, JR_LC_OP_WRITE_MASKED, 0);
}
static struct jr_lc_op jr_lc_poll(bootnet_u32 word, bootnet_u32 mask,
		bootnet_u8 metadata)
{
	return jr_lc_op(word, 0, mask, JR_LC_OP_POLL_MATCH, metadata);
}
static struct jr_lc_op jr_lc_delay(bootnet_u32 usec)
{
	return jr_lc_op(0, usec, 0, JR_LC_OP_DELAY_US, 0);
}

static int jr_lc_apply(bootnet_u32 group, bootnet_u32 index,
		const struct jr_lc_op *op)
{
	bootnet_u32 reads, value;
	int status;

	jr_detail = 0x40000000u | (group << 20) | index;
	JR_LIFECYCLE_OP_OBSERVER(group, index, op->kind, op->metadata, op->word,
				 op->value, op->mask, op->interval_us,
				 op->max_reads);
	switch (op->kind) {
	case JR_LC_OP_READ:
		jr_lc_last_read = jr_read(op->word);
		break;
	case JR_LC_OP_WRITE:
		jr_write(op->word, op->value);
		break;
	case JR_LC_OP_WRITE_LAST_READ:
		jr_write(op->word, jr_lc_last_read);
		break;
	case JR_LC_OP_WRITE_MASKED:
		jr_write_masked(op->word, op->value, op->mask);
		break;
	case JR_LC_OP_POLL_MATCH:
		for (reads = 0; reads < JR_LC_SOURCE_POLL_MAX_READS; reads++) {
			if (jr_abort_requested())
				return BOOTNET_BACKEND_ABORTED;
			value = jr_read(op->word);
			if ((value & op->mask) == (op->value & op->mask))
				break;
			if (reads + 1u != JR_LC_SOURCE_POLL_MAX_READS) {
				status = jr_delay_us(JR_LC_SOURCE_POLL_INTERVAL_US);
				if (status)
					return status;
			}
		}
		if (reads == JR_LC_SOURCE_POLL_MAX_READS)
			return -ETIMEDOUT;
		break;
	case JR_LC_OP_DELAY_US:
		status = jr_delay_us(op->value);
		if (status)
			return status;
		break;
	default:
		return -EINVAL;
	}
	jr_lc_completed++;
	return 0;
}

static bootnet_u32 jr_lc_sqs_word(const struct jr_lc_port_cursor *cursor,
		bootnet_u32 word)
{
	const bootnet_u8 *v = &cursor->sqs[word * 4u];
	return v[0] | ((bootnet_u32)v[1] << 8) | ((bootnet_u32)v[2] << 16) |
	       ((bootnet_u32)v[3] << 24);
}

static void jr_lc_sqs_set_word(struct jr_lc_port_cursor *cursor,
		bootnet_u32 word, bootnet_u32 value)
{
	bootnet_u8 *v = &cursor->sqs[word * 4u];
	v[0] = value; v[1] = value >> 8; v[2] = value >> 16; v[3] = value >> 24;
}

static int jr_lc_make_sd1g(struct jr_lc_port_cursor *c, bootnet_u32 i,
		struct jr_lc_op *op, bootnet_u8 *pending)
{
	bootnet_u32 rcomp = c->rcomp_status & 0xfu;
	int early = (c->chip_id >> 28) == 0u;
	*pending = 0;
	switch (i) {
	case 0: *op = jr_lc_read(0x4002u); *pending = 1; break;
	case 1: *op = jr_lc_write(0xb4008u, 0x300u); break;
	case 2: *op = jr_lc_write(0xb4008u, 0x1300u); break;
	case 3: *op = jr_lc_poll(0xb4009u, 0x1000u, JR_LC_POLL_BOUNDED); break;
	case 4: *op = jr_lc_read(0xb4009u); *pending = 2; break;
	case 5: *op = jr_lc_wrm(0xb400cu, 0x182ac0u | ((rcomp - 3u) & 0xfu), 0x08383bcfu); break;
	case 6: *op = jr_lc_wrm(0xb400bu, 0xc0u, 0x1fe0u); break;
	case 7: *op = jr_lc_wrm(0xb400du, 0x18000u | ((rcomp - 4u) & 0xfu), 0x1e00fu); break;
	case 8: *op = jr_lc_wrm(0xb400fu, 0x40001u, early ? 0x80040081u : 0x80040001u); break;
	case 9: *op = jr_lc_wrm(0xb4010u, early ? 0x20c880u : 0xc880u, 0x20ff80u); break;
	case 10: *op = jr_lc_wrm(0xb4016u, 1u, 0x101u); break;
	case 11: case 15: case 19: *op = jr_lc_write(0xb4018u, 0x81ffffffu); break;
	case 12: case 16: case 20: *op = jr_lc_poll(0xb4018u, 0x80000000u, JR_LC_POLL_BOUNDED); break;
	case 13: *op = jr_lc_delay(40u); break;
	case 14: *op = jr_lc_wrm(0xb400fu, 0x80000000u, 0x80000000u); break;
	case 17: *op = jr_lc_delay(90u); break;
	case 18: *op = jr_lc_wrm(0xb4016u, 0u, 1u); break;
	default: return -EINVAL;
	}
	return 0;
}

static int jr_lc_make_sd6g(struct jr_lc_port_cursor *c, bootnet_u32 i,
		struct jr_lc_op *op, bootnet_u8 *pending)
{
	bootnet_u32 rcomp = c->rcomp_status & 0xfu;
	*pending = 0;
	switch (i) {
	case 0: *op = jr_lc_write(0xb4008u, 0x300u); break;
	case 1: *op = jr_lc_write(0xb4008u, 0x1300u); break;
	case 2: *op = jr_lc_poll(0xb4009u, 0x1000u, JR_LC_POLL_BOUNDED); break;
	case 3: *op = jr_lc_read(0xb4009u); *pending = 2; break;
	case 4: *op = jr_lc_wrm(0xb401cu, 0x21004000u | (((rcomp + 1u) * 0x10u) & 0xffu), 0x7f83e0f0u); break;
	case 5: *op = jr_lc_wrm(0xb401du, 0x30u, 0x1ffu); break;
	case 6: *op = jr_lc_wrm(0xb401au, 0x0400be50u | ((rcomp + 2u) & 0xfu), 0x0e03ffffu); break;
	case 7: *op = jr_lc_wrm(0xb401bu, 0x69du, 0xf9fu); break;
	case 8: *op = jr_lc_wrm(0xb4019u, 6u, 0x1f0eu); break;
	case 9: *op = jr_lc_wrm(0xb401eu, 0x111u, 0x131u); break;
	case 10: *op = jr_lc_wrm(0xb4020u, 0x43082u, 0x14ff86u); break;
	case 11: *op = jr_lc_wrm(0xb401fu, 0x40090u, 0x800400f0u); break;
	case 12: *op = jr_lc_wrm(0xb4028u, 0u, 0x100u); break;
	case 13: *op = jr_lc_wrm(0xb4028u, 1u, 0x101u); break;
	case 14: case 18: case 23: *op = jr_lc_write(0xb402bu, 0x8000ffffu); break;
	case 15: case 19: case 24: *op = jr_lc_poll(0xb402bu, 0x80000000u, JR_LC_POLL_BOUNDED); break;
	case 16: case 25: *op = jr_lc_delay(60u); break;
	case 17: *op = jr_lc_wrm(0xb401fu, 0x80000000u, 0x80000000u); break;
	case 20: *op = jr_lc_delay(130u); break;
	case 21: *op = jr_lc_wrm(0xb4028u, 0u, 1u); break;
	case 22: *op = jr_lc_wrm(0xb401bu, 0u, 1u); break;
	default: return -EINVAL;
	}
	return 0;
}

static int jr_lc_make_asm(bootnet_u32 i, struct jr_lc_op *op)
{
	static const bootnet_u8 table[48] = {
		4,5,3,4,5,0,4,5,1,4,5,3,4,5,0,1,4,5,2,4,5,3,4,2,
		5,4,0,5,4,3,5,1,4,2,5,4,0,5,4,3,1,4,5,2,4,5,6,15
	};
	if (i == 0) *op = jr_lc_write(0x90700u, 1u);
	else if (i == 1) *op = jr_lc_write(0x90801u, 0u);
	else if (i < 50) *op = jr_lc_wrm(0x90701u + i - 2u, table[i - 2u], 0xfu);
	else if (i == 50) *op = jr_lc_write(0x90801u, 0x30u);
	else if (i == 51) *op = jr_lc_poll(0x90700u, 1u, JR_LC_POLL_BOUNDED);
	else return -EINVAL;
	return 0;
}

static int jr_lc_make_ana(struct jr_lc_port_cursor *c, bootnet_u32 i,
		struct jr_lc_op *op, bootnet_u8 *pending)
{
	static const bootnet_u32 setup[22][3] = {
		{0x948c5,1,1},{0x244403,4,4},{0x244403,2,2},{0x244403,1,1},
		{0x204e99,0x1f,0},{0x29ba69,1,1},{0x29ba69,2,2},{0x29d6b1,0x80,0x80},
		{0x29d6b1,0x10,0x10},{0x29d6b1,1,1},{0x29d6b1,0x800,0x800},
		{0x29bbcc,1,1},{0x29bbd9,1,1},{0x29ba63,0x100,0x100},
		{0x29bbe6,1,1},{0x29bbf3,1,1},{0x29d6c0,1,1},{0x29bbf4,4,4},
		{0x29bbf7,4,4},{0x29ba6b,0xffffffff,0xffffffff},{0x204f2d,0xc,0xe},
		{0x204f2d,1,1}
	};
	static const bootnet_u32 polls[21][2] = {
		{0x244403,4},{0x948c5,1},{0x244403,4},{0x244403,2},{0x244403,1},
		{0x204e99,8},{0x204e99,4},{0x204e99,1},{0x29ba69,1},{0x29ba69,2},
		{0x29d6b1,0x80},{0x29d6b1,0x10},{0x29d6b1,1},{0x29bbcc,1},
		{0x29bbd9,1},{0x29bbe6,1},{0x29bbf3,1},{0x29d6c0,1},{0x29bbf4,4},
		{0x29bbf7,4},{0x204f2d,1}
	};
	bootnet_u32 word;
	*pending = 0;
	if (i < 22) {
		*op = setup[i][2] ? jr_lc_wrm(setup[i][0], setup[i][1], setup[i][2]) :
			jr_lc_write(setup[i][0], setup[i][1]);
		return 0;
	}
	i -= 22;
	if (i < 70) {
		word = 0x9402au + (i / 2u) * 0x40u;
		if (!(i & 1u)) { *op = jr_lc_read(word); *pending = 3; }
		else *op = jr_lc_write(word, c->ana_read_value | 2u);
		return 0;
	}
	i -= 70;
	if (i == 0) { *op = jr_lc_read(0x244407u); *pending = 3; }
	else if (i == 1) *op = jr_lc_write(0x244407u, c->ana_read_value | 0x2000u);
	else if (i == 2) *op = jr_lc_wrm(0x244404u, 4u, 4u);
	else if (i < 24) *op = jr_lc_poll(polls[i - 3u][0], polls[i - 3u][1], JR_LC_POLL_BOUNDED);
	else if (i == 24) *op = jr_lc_wrm(0x29ba6bu, 0u, 0u);
	else return -EINVAL;
	return 0;
}

static int jr_lc_make_sqs(bootnet_u32 i, struct jr_lc_op *op)
{
	static const bootnet_u32 reset[8] = {0x305e2d,0x2c532d,0x2c5360,0x305e79,0x2c5358,0x305e71,0x2c535b,0x305e74};
	static const bootnet_u32 ao[10] = {0,~0u,1,~0u,2,~0u,3,~0u,~0u,~0u};
	static const bootnet_u32 aw[5] = {0x2c5354,0x2c5355,0x2c5356,0x2c5357,0x2c5359};
	static const bootnet_u32 av[10] = {2,0x100,2,0x200,6,0x3e,4,0x7c00,2,8};
	static const bootnet_u32 bo[10] = {0,~0u,1,~0u,2,~0u,~0u,~0u,~0u,~0u};
	static const bootnet_u32 bw[5] = {0x305e6d,0x305e6e,0x305e6f,0x305e72,0x305e73};
	static const bootnet_u32 bv[10] = {2,0x100,2,0x200,2,0x3e,2,0x10,2,0x20};
	bootnet_u32 row, col, word;
	if (i < 8) { *op = jr_lc_wrm(reset[i], 1, 1); return 0; }
	if (i < 16) { *op = jr_lc_poll(reset[i - 8], 1, JR_LC_POLL_BOUNDED); return 0; }
	i -= 16;
	if (i < 2800) {
		row = i / 10; col = i % 10;
		word = col == 8 ? 0x2c7001u + row * 8u :
		       ao[col] != ~0u ? 0x2c4001u + row * 0x10u + ao[col] : aw[col / 2u];
		*op = jr_lc_write(word, av[col]); return 0;
	}
	i -= 2800;
	if (i < 4640) {
		row = i / 10; col = i % 10;
		word = (col == 6 || col == 8) ? 0x307001u + row * 8u + (col - 6u) / 2u :
		       bo[col] != ~0u ? 0x304001u + row * 0x10u + bo[col] : bw[col / 2u];
		*op = jr_lc_write(word, bv[col]); return 0;
	}
	i -= 4640;
	switch (i) {
	case 0: *op = jr_lc_write(0x2c533cu, 0); break;
	case 1: *op = jr_lc_write(0x305e44u, 0); break;
	case 2: *op = jr_lc_wrm(0x2c5300u, 0xe00, 0xf00); break;
	case 3: *op = jr_lc_wrm(0xa8054u, 0xe, 0xf); break;
	case 4: *op = jr_lc_wrm(0x305e00u, 0xe00, 0xf00); break;
	default: return -EINVAL;
	}
	return 0;
}

static void jr_lc_sqs_calculate(struct jr_lc_port_cursor *c, bootnet_u32 port)
{
	bootnet_u32 q, word, byte, old;
	if (port == 0) {
		jr_lc_sqs_set_word(c, 0, 0); jr_lc_sqs_set_word(c, 1, 0);
		jr_lc_sqs_set_word(c, 2, 0); c->sqs[12] = 0;
		jr_lc_sqs_set_word(c, 4, 10); jr_lc_sqs_set_word(c, 5, 61);
		jr_lc_sqs_set_word(c, 6, 10); jr_lc_sqs_set_word(c, 7, 10);
		jr_lc_sqs_set_word(c, 79, 0x6680); jr_lc_sqs_set_word(c, 80, 0x6644);
		jr_lc_sqs_set_word(c, 81, 0x6637); c->sqs[328] = 0;
		jr_lc_sqs_set_word(c, 83, 10); jr_lc_sqs_set_word(c, 84, 61);
		jr_lc_sqs_set_word(c, 85, 10); jr_lc_sqs_set_word(c, 86, 10);
	}
	c->sqs[59] = 0;
	if (port == 33 || port == 34) {
		for (q = 0; q < 8; q++) {
			word = 15 + 8 * q; byte = 4 * word;
			jr_lc_sqs_set_word(c, word, 80); jr_lc_sqs_set_word(c, word + 1, 63);
			jr_lc_sqs_set_word(c, word + 2, 63); jr_lc_sqs_set_word(c, word + 3, 0);
			jr_lc_sqs_set_word(c, word + 4, 80);
			c->sqs[byte + 20] = 1; c->sqs[byte + 21] = 0; c->sqs[byte + 22] = 0;
			c->sqs[byte + 23] = 0; c->sqs[byte + 24] = 0; c->sqs[byte + 25] = 0;
			c->sqs[byte + 26] = 1; jr_lc_sqs_set_word(c, word + 7, 0);
		}
		c->sqs[56] = 1; c->sqs[57] = 1; c->sqs[58] = 0; c->sqs[59] = 1;
		jr_lc_sqs_set_word(c, 8, 80); jr_lc_sqs_set_word(c, 9, 63);
		jr_lc_sqs_set_word(c, 10, 63); jr_lc_sqs_set_word(c, 11, 10);
		jr_lc_sqs_set_word(c, 12, 0);
		jr_lc_sqs_set_word(c, 1, jr_lc_sqs_word(c, 1) + 40);
		jr_lc_sqs_set_word(c, 2, jr_lc_sqs_word(c, 2) + 40);
		jr_lc_sqs_set_word(c, 0, jr_lc_sqs_word(c, 0) + 54);
		jr_lc_sqs_set_word(c, 79, jr_lc_sqs_word(c, 79) - 64);
		jr_lc_sqs_set_word(c, 80, jr_lc_sqs_word(c, 79) - 60);
		jr_lc_sqs_set_word(c, 81, jr_lc_sqs_word(c, 79) - 73);
	}
	if (port >= 81 && port <= 88) {
		old = jr_lc_sqs_word(c, 79);
		jr_lc_sqs_set_word(c, 94, 0x294); jr_lc_sqs_set_word(c, 95, 0x258);
		jr_lc_sqs_set_word(c, 96, 0x24b); jr_lc_sqs_set_word(c, 97, 0xf);
		jr_lc_sqs_set_word(c, 98, 0);
		c->sqs[396] = 1; c->sqs[397] = 0; c->sqs[398] = 1; c->sqs[399] = 0;
		c->sqs[400] = 1; c->sqs[401] = 0; c->sqs[402] = 1;
		jr_lc_sqs_set_word(c, 101, 0); jr_lc_sqs_set_word(c, 87, 0x294);
		jr_lc_sqs_set_word(c, 88, 0x258); jr_lc_sqs_set_word(c, 89, 0x24b);
		jr_lc_sqs_set_word(c, 90, 0); jr_lc_sqs_set_word(c, 91, 0);
		jr_lc_sqs_set_word(c, 92, 0x3e7);
		c->sqs[372] = 0; c->sqs[373] = 0; c->sqs[374] = 0; c->sqs[375] = 1;
		jr_lc_sqs_set_word(c, 79, old - 15);
		if (port == 88) {
			jr_lc_sqs_set_word(c, 80, old - 75);
			jr_lc_sqs_set_word(c, 81, old - 88);
		}
	}
}

static bootnet_u32 jr_lc_sqs_set_port_count(bootnet_u32 port)
{
	return port < 32 ? 85 : port < 35 ? 69 : port < 81 ? 27 : 15;
}

static bootnet_u32 jr_lc_sqs_queue_selector(bootnet_u32 port, int oqs,
		bootnet_u32 queue)
{
	if (port < 32 || !oqs) return port * 8 + queue;
	if (port == 80) return 0x1c0 + queue;
	return port + 0x177;
}

static int jr_lc_make_sqs_side(struct jr_lc_port_cursor *c, bootnet_u32 port,
		int oqs, bootnet_u32 i, struct jr_lc_op *op)
{
	static const bootnet_u32 iq[4] = {0x2c6000,0x2c6200,0x2c6400,0x2c6600};
	static const bootnet_u32 oq[4] = {0x306000,0x306200,0x306400,0x306600};
	static const bootnet_u32 ic[4] = {0x2c5200,0x2c5240,0x2c5280,0x2c52c0};
	static const bootnet_u32 oc[4] = {0x308800,0x308880,0x308900,0x308a00};
	const bootnet_u32 *qb = oqs ? oq : iq, *cb = oqs ? oc : ic;
	bootnet_u32 base = oqs ? 79 : 0;
	bootnet_u32 count = !oqs ? 8 : port < 32 ? 8 : port < 81 ? 4 : 1;
	bootnet_u32 q, field, selector, row, byte, value;
	if (i < count * 4) {
		q = i / 4; field = i % 4; selector = jr_lc_sqs_queue_selector(port, oqs, q);
		row = base + 15 + 8 * q;
		if (field == 0) {
			byte = 4 * row;
			value = ((jr_lc_sqs_word(c, row + 4) & 0x7fff) << 16) | 0x80;
			value |= c->sqs[byte + 21] ? 0x40 : 0;
			value |= c->sqs[byte + 20] ? 0x20 : 0;
			value |= c->sqs[byte + 23] ? 0x10 : 0;
			value |= c->sqs[byte + 22] ? 0x08 : 0;
			value |= c->sqs[byte + 24] ? 0x04 : 0;
			value |= c->sqs[byte + 25] ? 0x02 : 0;
			value |= c->sqs[byte + 26] ? 0x01 : 0;
		} else if (field == 1) {
			value = ((jr_lc_sqs_word(c, row + 1) & 0x7fff) << 16) |
				(jr_lc_sqs_word(c, row + 2) & 0x7fff);
		} else if (field == 2) {
			value = ((jr_lc_sqs_word(c, row) & 0x7fff) << 16) |
				(jr_lc_sqs_word(c, row + 3) & 0x7fff);
		} else value = jr_lc_sqs_word(c, row + 7);
		*op = jr_lc_write(qb[field] + selector, value); return 0;
	}
	i -= count * 4;
	if (i < 4) {
		if (i == 0) {
			byte = 4 * base;
			value = (c->sqs[byte + 56] ? 8 : 0) | (c->sqs[byte + 57] ? 4 : 0) |
				(c->sqs[byte + 58] ? 2 : 0) | (c->sqs[byte + 59] ? 1 : 0);
		} else if (i == 1) value = ((jr_lc_sqs_word(c, base + 9) & 0x7fff) << 16) |
			(jr_lc_sqs_word(c, base + 10) & 0x7fff);
		else if (i == 2) value = ((jr_lc_sqs_word(c, base + 8) & 0x7fff) << 16) |
			(jr_lc_sqs_word(c, base + 11) & 0x7fff);
		else value = jr_lc_sqs_word(c, base + 12) & 0x7fff;
		*op = jr_lc_write(cb[i] + port, value); return 0;
	}
	i -= 4;
	if (oqs && i == 0) {
		*op = jr_lc_write(0x308980u + port, jr_lc_sqs_word(c, base + 13) & 0x7fff);
		return 0;
	}
	if (oqs) i--;
	if (i == 0) {
		byte = 4 * base;
		value = ((jr_lc_sqs_word(c, base) & 0x7fff) << 16) |
			(c->sqs[byte + 13] ? 1 : 0) | (c->sqs[byte + 12] ? 2 : 0);
		*op = jr_lc_write(oqs ? 0x305e05u : 0x2c5305u, value);
	} else if (i == 1) {
		value = ((jr_lc_sqs_word(c, base + 1) & 0x7fff) << 16) |
			(jr_lc_sqs_word(c, base + 2) & 0x7fff);
		*op = jr_lc_write(oqs ? 0x305e06u : 0x2c5306u, value);
	} else if (i < 6) *op = jr_lc_write((oqs ? 0x305e01u : 0x2c5301u) + i - 2,
		jr_lc_sqs_word(c, base + i + 2));
	else return -EINVAL;
	return 0;
}

static int jr_lc_make_sqs_set(struct jr_lc_port_cursor *c, bootnet_u32 i,
		struct jr_lc_op *op)
{
	bootnet_u32 port, count, iq_count;
	for (port = 0; port < 89; port++) {
		count = jr_lc_sqs_set_port_count(port);
		if (i < count) {
			iq_count = port < 35 ? 42 : 0;
			if (c->sqs_next_port == port) { jr_lc_sqs_calculate(c, port); c->sqs_next_port++; }
			else if (c->sqs_next_port != port + 1) return -EINVAL;
			if (i < iq_count) return jr_lc_make_sqs_side(c, port, 0, i, op);
			return jr_lc_make_sqs_side(c, port, 1, i - iq_count, op);
		}
		i -= count;
	}
	return -EINVAL;
}

static int jr_lc_make_qsys(bootnet_u32 i, struct jr_lc_op *op)
{
	static const bootnet_u32 bases[3] = {0xa8057,0xa805f,0xa8067};
	bootnet_u32 row, field;
	if (i == 0) *op = jr_lc_write(0xa8056u, 1);
	else if (i < 25) { row = (i - 1) / 3; field = (i - 1) % 3;
		*op = jr_lc_write(bases[field] + row, field == 1 ? 0xff : 0); }
	else if (i == 25) *op = jr_lc_write(0xa8053u, 1);
	else return -EINVAL;
	return 0;
}

static int jr_lc_make_scheduler(bootnet_u32 i, struct jr_lc_op *op)
{
	static const bootnet_u8 table[144] = {
		0x1c,0x1d,0x1b,0,0x1e,6,0x1c,0x1d,0x1b,0x0c,0x12,0x1e,
		0x1c,0x1d,0x1b,1,7,0x1e,0x1c,0x1d,0x1b,0x0d,0x13,2,
		0x1c,0x1d,0x1b,0x1e,8,0x0e,0x1c,0x1d,0x1b,0x1e,0x14,3,
		0x1c,0x1d,0x1b,9,0x1e,0x0f,0x1c,0x1d,0x1b,0x15,4,0x1e,
		0x1c,0x1d,0x1b,0x0a,0x10,0x1e,0x1c,0x1d,0x1b,0x16,5,0x0b,
		0x1c,0x1d,0x1b,0x1e,0x11,0x17,0x1c,0x1d,0x1b,0x1e,0x21,0x1f,
		0x1c,0x1d,0x1b,0,0x1e,6,0x1c,0x1d,0x1b,0x0c,0x12,0x1e,
		0x1c,0x1d,0x1b,1,7,0x1e,0x1c,0x1d,0x1b,0x0d,0x13,2,
		0x1c,0x1d,0x1b,0x1e,8,0x0e,0x1c,0x1d,0x1b,0x1e,0x14,3,
		0x1c,0x1d,0x1b,9,0x1e,0x0f,0x1c,0x1d,0x1b,0x15,4,0x1e,
		0x1c,0x1d,0x1b,0x0a,0x10,0x1e,0x1c,0x1d,0x1b,0x16,5,0x0b,
		0x1c,0x1d,0x1b,0x1e,0x11,0x17,0x1c,0x1d,0x1b,0x1e,0x3f,0x1f
	};
	switch (i) {
	case 0: *op = jr_lc_wrm(0xacd5d,4,4); return 0;
	case 1: *op = jr_lc_wrm(0xacccb,4,4); return 0;
	case 2: *op = jr_lc_write(0xaca1d,4); return 0;
	case 3: *op = jr_lc_write(0xacd64,0); return 0;
	case 4: *op = jr_lc_wrm(0xaca1e,0x1a00,0x1f00); return 0;
	case 5: *op = jr_lc_wrm(0xaca1e,0x1a0000,0x1f0000); return 0;
	}
	i -= 6;
	if (i < 576) *op = jr_lc_write(0xac400u + i, i == 142 ? 0x1a : table[i % 144]);
	else if (i == 576) *op = jr_lc_poll(0xacd5d, 4, JR_LC_POLL_BOUNDED);
	else if (i == 577) *op = jr_lc_write(0xacd64, 0x240);
	else return -EINVAL;
	return 0;
}

static int jr_lc_make_port_tail(bootnet_u32 i, struct jr_lc_op *op)
{
	static const bootnet_u32 sa[6] = {1,0,1,0,1,0}, sb[6] = {8,8,16,16,0,0};
	static const bootnet_u32 sw[8] = {0x2c535c,0x2c535e,0x305e75,0x305e77,0x2c535d,0x2c535f,0x305e76,0x305e78};
	static const bootnet_u32 sv[8] = {1,0xc00,1,0xc00,0,0xc00,0,0xc00};
	static const bootnet_u32 fa[4] = {1,0,1,0}, fb[4] = {1,1,0xe,0xe};
	bootnet_u32 row; int second;
	switch (i) {
	case 0: *op=jr_lc_wrm(0x34b900,2,2); return 0;
	case 1: *op=jr_lc_write(0x34b9e1,1); return 0;
	case 2: *op=jr_lc_poll(0x34b900,2,JR_LC_POLL_BOUNDED); return 0;
	case 3: *op=jr_lc_poll(0x34b9e1,1,JR_LC_POLL_BOUNDED); return 0;
	case 4: *op=jr_lc_wrm(0x34b9d9,1,1); return 0;
	case 5: *op=jr_lc_wrm(0x34b98e,2,2); return 0;
	case 6: *op=jr_lc_wrm(0x34b98f,2,2); return 0;
	case 7: *op=jr_lc_wrm(0x34b98e,1,1); return 0;
	case 8: *op=jr_lc_wrm(0x34b98f,1,1); return 0;
	case 9: *op=jr_lc_write(0xb02d9,0x1424); return 0;
	case 10: *op=jr_lc_write(0x29bb88,0x20000); return 0;
	case 11: *op=jr_lc_write(0x29bbc8,1); return 0;
	}
	if (i < 44) *op = jr_lc_write(0x29d401u + (i - 12) * 0x10, 0xff6);
	else if (i < 56) { row=(i-44)/2; second=(i-44)&1;
		*op=jr_lc_write((second?0x29bbd3u:0x29bbcdu)+row,second?sb[row]:sa[row]); }
	else if (i < 64) { row=i-56; *op=jr_lc_write(sw[row],sv[row]); }
	else if (i < 72) { row=(i-64)/2; second=(i-64)&1;
		*op=jr_lc_write((second?0x34b9ddu:0x34b9d9u)+row,second?fb[row]:fa[row]); }
	else if (i < 76) *op=jr_lc_write(0xb02d4u+i-72,0x1428127f);
	else return -EINVAL;
	return 0;
}

static int jr_lc_make_port_op(struct jr_lc_port_cursor *c,
		struct jr_lc_op *op, bootnet_u8 *pending)
{
	bootnet_u32 i = c->operation;
	*pending = 0;
	if (i == 0) { *op=jr_lc_write(0x403c,0); return 0; }
	if (i < 22) return jr_lc_make_sd1g(c,i-1,op,pending);
	if (i < 48) return jr_lc_make_sd6g(c,i-22,op,pending);
	if (i < 100) return jr_lc_make_asm(i-48,op);
	if (i < 217) return jr_lc_make_ana(c,i-100,op,pending);
	if (i < 7678) return jr_lc_make_sqs(i-217,op);
	if (i < 11967) return jr_lc_make_sqs_set(c,i-7678,op);
	if (i < 11993) return jr_lc_make_qsys(i-11967,op);
	if (i < 12577) return jr_lc_make_scheduler(i-11993,op);
	if (i < JR_LC_PORT_COUNT) return jr_lc_make_port_tail(i-12577,op);
	return -EINVAL;
}

static int jr_lc_run_port(void)
{
	struct jr_lc_port_cursor c = {0};
	struct jr_lc_op op;
	bootnet_u8 pending;
	int status;
	while (c.operation < JR_LC_PORT_COUNT) {
		status=jr_lc_make_port_op(&c,&op,&pending); if (status) return status;
		status=jr_lc_apply(JR_LC_GROUP_PORT,c.operation,&op); if (status) return status;
		c.operation++;
		if (pending == 1) c.chip_id=jr_lc_last_read;
		else if (pending == 2) c.rcomp_status=jr_lc_last_read;
		else if (pending == 3) c.ana_read_value=jr_lc_last_read;
	}
	return c.sqs_next_port == 89 ? 0 : -EINVAL;
}

static int jr_lc_emit(bootnet_u32 group, bootnet_u32 *index,
		struct jr_lc_op op)
{
	int status=jr_lc_apply(group,*index,&op);
	if (!status) (*index)++;
	return status;
}
#define JR_LC_EMIT(g,i,e) do { int s_=jr_lc_emit((g),&(i),(e)); if (s_) return s_; } while (0)

static int jr_lc_run_vcap(void)
{
	static const bootnet_u32 b[4][3]={{0x9c029,0x9c001,0x9c000},{0xa0055,0xa0001,0xa0000},
		{0xa422c,0xa4024,0xa4000},{0xb801d,0xb8001,0xb8000}};
	bootnet_u32 i=0,n;
	for(n=0;n<4;n++) {
		JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_write(b[n][0],1));
		JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_poll(b[n][0],1,JR_LC_POLL_BOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_write(b[n][1],9));
		JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_poll(b[n][1],1,JR_LC_POLL_UNBOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_write(b[n][2],1));
	}
	JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_write(0x34b901,1));
	JR_LC_EMIT(JR_LC_GROUP_VCAP,i,jr_lc_write(0x29d68f,4));
	return i == JR_LC_VCAP_COUNT ? 0 : -EINVAL;
}

static int jr_lc_run_qos(void)
{
	bootnet_u32 i=0,port,p,q;
	for(port=0;port<33;port++) {
		for(p=0;p<4;p++) { q=4*port+p;
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29bc00+q,0x3f));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29bd00+q,0));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29be00+q,0x1ffff));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29dc00+8*port,0x14));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29dc01+8*port+p,0)); }
		for(p=0;p<8;p++) { q=8*port+p;
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29d000+q,0x1ffff));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29d200+q,0x3f));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29d800+2*q,0x14));
			JR_LC_EMIT(JR_LC_GROUP_QOS,i,jr_lc_write(0x29d801+2*q,0)); }
	}
	return i == JR_LC_QOS_COUNT ? 0 : -EINVAL;
}

static int jr_lc_run_packet(void)
{
	static const bootnet_u32 pcp_qos[8]={1,0,2,3,4,5,6,7};
	bootnet_u32 i=0,g,port,base,pcp,replica,address;
	for(g=0;g<4;g++) {
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0x800a+g,5));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0x801c+g,1));
	}
	JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(0x29ba63,0x200,0x200));
	JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xb0685,1));
	JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xb0687,0x5f));
	JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(0x90862,0x10,0x10));
	JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(0x90863,0x10,0x10));
	for(port=0x21;port<=0x22;port++) {
		base=0x94854+0x40*(port-0x21);
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(base-0x13,4,4));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(base-0x13,0,1));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0x9c001,((port+0x1000)<<4)|3));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_poll(0x9c001,1,JR_LC_POLL_UNBOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(0x9c025,0x80,0x180));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0x9c001,((port+0x1000)<<4)|1));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_poll(0x9c001,1,JR_LC_POLL_UNBOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(base-0x12,1,0x7ff9));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(base,0x20,0x20));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(base,0x2000,0x2000));
		for(pcp=0;pcp<8;pcp++) for(replica=0;replica<2;replica++) {
			address=base+pcp-0x10+8*replica;
			JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(address,pcp_qos[pcp],7));
			JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_wrm(address+0x16,0,3));
		}
	}
	for(port=0x21;port<=0x22;port++) {
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0x94019+port*0x40,0xf3f1));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xa4024,((port+0x200)<<4)|3));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_poll(0xa4024,1,JR_LC_POLL_UNBOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_read(0xa422b));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xa4228,0x008001c0));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xa4229,0x80));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xa422a,0xffffffff));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write_last(0xa422b));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_write(0xa4024,((port+0x200)<<4)|1));
		JR_LC_EMIT(JR_LC_GROUP_PACKET,i,jr_lc_poll(0xa4024,1,JR_LC_POLL_UNBOUNDED));
	}
	return i == JR_LC_PACKET_COUNT ? 0 : -EINVAL;
}

static int jr_lc_run_l2(void)
{
	bootnet_u32 i=0,port,bit;
	int special;
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x204eaa,0));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x204e97,1));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x203fff,0));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x200007,0xffffffff));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x200006,0x100,0x000fff0b));
	for(port=0;port<32;port++) {
		special=port==27||port==28; bit=1u<<port;
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9c001,0x10003+0x10*port));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_poll(0x9c001,1,JR_LC_POLL_UNBOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x9c025,0,0x180));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x9c025,special?0:0x20,0x20));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x9c026,0x100,0xfff00));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9c001,0x10001+0x10*port));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_poll(0x9c001,1,JR_LC_POLL_UNBOUNDED));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x94002+0x40*port,9,0x7ff9));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x94000+0x40*port,0,0x11e));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x94935,0,bit));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x204e9d,0,bit));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x348022+0x40*port,special?0x20:0x21,0x3f));
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x348000+0x40*port,
			special?0x88a80000:0x88a80001,0xffff0fff));
	}
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x200002,8,8));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9490f,0x88a8));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9800a,0x01000001));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9800b,3));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9800a,0x03000095));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0x9800b,2));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0xa8070,0x41));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_write(0xacd65,0x41));
	for(port=0;port<32;port++)
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0xb0299+port,1,1));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x244402,0x10000,0x10000));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0xb03d2,0xffffff,0xffffff));
	JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0xb03d1,0xffffff,0xffffff));
	for(port=0;port<32;port++)
		JR_LC_EMIT(JR_LC_GROUP_L2,i,jr_lc_wrm(0x94001+0x40*port,1,1));
	return i == JR_LC_L2_COUNT ? 0 : -EINVAL;
}

static int jr_lc_run_l3(void)
{
	static const bootnet_u32 bases[2]={0x29bbe7,0x29d6b4};
	static const bootnet_u32 values[4]={0,1,0,1}, masks[4]={8,8,0x10,0x10};
	bootnet_u32 i=0,leg,item;
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(0x204f3f,3));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(0x204e99,0xe));
	for(leg=0;leg<2;leg++) for(item=0;item<4;item++) {
		JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(bases[leg]+item,values[item]));
		JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(bases[leg]+6+item,masks[item]));
	}
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(0x29d6c0,1));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(0x29bbf3,1));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_poll(0x29d6c0,1,JR_LC_POLL_BOUNDED));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_poll(0x29bbf3,1,JR_LC_POLL_BOUNDED));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(0x204e9e,0x1fff));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_wrm(0x29baaf,0x180000,0x180000));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_poll(0x204f3f,0xffffffff,JR_LC_POLL_UNBOUNDED));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_poll(0x204e99,0xe,JR_LC_POLL_UNBOUNDED));
	JR_LC_EMIT(JR_LC_GROUP_L3,i,jr_lc_write(0x204e98,0xffffffff));
	return i == JR_LC_L3_COUNT ? 0 : -EINVAL;
}

static int jr_master_lifecycle_init(void)
{
	int status;
	jr_lc_completed=0; jr_lc_last_read=0;
	status=jr_lc_run_port(); if(status) return status;
	status=jr_lc_run_vcap(); if(status) return status;
	status=jr_lc_run_qos(); if(status) return status;
	status=jr_lc_run_packet(); if(status) return status;
	status=jr_lc_run_l2(); if(status) return status;
	status=jr_lc_run_l3(); if(status) return status;
	return jr_lc_completed == JR_LC_TOTAL_COUNT ? 0 : -EINVAL;
}

#undef JR_LC_EMIT
