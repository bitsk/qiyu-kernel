// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2016-2018, 2021, The Linux Foundation. All rights reserved.
 *
 * PM8150B fuel gauge register and DMA access.
 * Register layout and conversions derived from Qualcomm's GPL FG GEN4 driver
 * (fg-util.c, fg-memif.c and fg-reg.h). See references/power for pinned sources.
 * Profile initialization uses this transport; capacity learning is separate.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "qcom-fg4-core.h"

#define FG4_SOC_READY		0x010
#define FG4_SOC_ENABLE		0x046
#define FG4_SOC_VALUE		0x009
#define FG4_VOLTAGE		0x1a0
#define FG4_CURRENT		0x1a2
#define FG4_SHADOW_OFFSET	0x006
#define FG4_PEEK_MUX		0x1ee
#define FG4_PEEK_READ		0x1ef
#define FG4_BATT_MISSING		0x210
#define FG4_MEM_STATUS		0x310
#define FG4_MEM_ARB		0x340
#define FG4_MEM_INTF		0x350
#define FG4_DMA_STATUS		0x370
#define FG4_DMA_CTL		0x371
#define FG4_MEM_REQUEST		BIT(7)
#define FG4_IACS_SELECT		BIT(5)
#define FG4_ARB_REQUEST		BIT(0)
#define FG4_ARB_LOW_LATENCY	BIT(1)
#define FG4_DMA_PARTITIONED	BIT(1)
#define FG4_DMA_CLEAR_LOG		BIT(0)
#define FG4_DMA_ERRORS		(BIT(1) | BIT(2))
#define FG4_MEM_GRANTED		BIT(3)
#define FG4_ALG_ACTIVE		BIT(3)
#define FG4_TEMP_BYTE		(328 * 2)
#define FG4_READ_TRIES		5

int qcom_fg4_check_hardware(struct qcom_fg4 *fg)
{
	static const u8 types[] = { 0x10, 0x11, 0x13, 0x0d };
	unsigned int i, val;
	int ret;

	if ((fg->base & 0xff) || fg->base > 0xf600 || !fg->major)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		ret = regmap_read(fg->map, fg->base + 0x100 * i + 5, &val);
		if (ret)
			return ret;
		if (val != types[i])
			return -ENODEV;
	}
	return 0;
}

static int fg4_release_clear_log(struct qcom_fg4 *fg)
{
	unsigned int value;
	int ret;

	if (!fg->dma_clear_pending)
		return 0;
	ret = regmap_update_bits(fg->map, fg->base + FG4_DMA_CTL, FG4_DMA_CLEAR_LOG, 0);
	if (ret)
		return ret;
	ret = regmap_read(fg->map, fg->base + FG4_DMA_CTL, &value);
	if (ret)
		return ret;
	if (value & FG4_DMA_CLEAR_LOG)
		return -EIO;
	fg->dma_clear_pending = false;
	return 0;
}

/* Caller serializes the entire transaction, including retrying cleanup. */
int qcom_fg4_release(struct qcom_fg4 *fg)
{
	int ret, first = 0;

	if (!fg->restore_pending)
		goto clear_log;
	ret = regmap_update_bits(fg->map, fg->base + FG4_MEM_INTF,
				 FG4_MEM_REQUEST | FG4_IACS_SELECT, 0);
	if (ret)
		first = ret;
	/* Release arbitration even if clearing the interface request failed. */
	ret = regmap_update_bits(fg->map, fg->base + FG4_MEM_ARB, FG4_ARB_REQUEST, 0);
	if (ret && !first)
		first = ret;
	if (first)
		goto out;

	ret = regmap_update_bits(fg->map, fg->base + FG4_DMA_CTL,
				 FG4_DMA_PARTITIONED, fg->saved_dma);
	if (ret)
		first = ret;
	ret = regmap_update_bits(fg->map, fg->base + FG4_MEM_ARB,
				 FG4_ARB_LOW_LATENCY, fg->saved_arb);
	if (ret && !first)
		first = ret;
	if (fg->major == 1) {
		ret = regmap_write(fg->map, fg->base + FG4_PEEK_MUX, fg->saved_mux);
		if (ret && !first)
			first = ret;
	}
out:
	fg->restore_pending = !!first;
clear_log:
	ret = fg4_release_clear_log(fg);
	if (ret && !first)
		first = ret;
	fg->cleanup_error = first;
	return first;
}

/* Called only after excluding another MEMIF owner, before requesting SRAM. */
static int fg4_clear_dma_errors(struct qcom_fg4 *fg, unsigned int status,
				unsigned int control)
{
	unsigned int value;
	int ret, cleanup;

	if (!(status & FG4_DMA_ERRORS) && !(control & FG4_DMA_CLEAR_LOG))
		return 0;
	/* Normalize a stale asserted bit, then issue at most one clear request. */
	fg->dma_clear_pending = true;
	ret = fg4_release_clear_log(fg);
	if (ret)
		return ret;
	if (status & FG4_DMA_ERRORS) {
		fg->dma_clear_pending = true;
		ret = regmap_update_bits(fg->map, fg->base + FG4_DMA_CTL,
					 FG4_DMA_CLEAR_LOG, FG4_DMA_CLEAR_LOG);
		cleanup = fg4_release_clear_log(fg);
		if (ret || cleanup)
			return ret ?: cleanup;
	}
	ret = regmap_read(fg->map, fg->base + FG4_DMA_STATUS, &value);
	if (ret)
		return ret;
	/* A stuck/reappearing error is never accepted as a successful transfer. */
	return value & FG4_DMA_ERRORS ? -EIO : 0;
}

static int fg4_poll(struct qcom_fg4 *fg, u16 reg, u8 mask, bool set,
		    unsigned int attempts)
{
	unsigned int val, i;
	int ret;

	for (i = 0; i < attempts; i++) {
		ret = regmap_read(fg->map, fg->base + reg, &val);
		if (ret)
			return ret;
		if (!!(val & mask) == set)
			return 0;
		if (i + 1 < attempts)
			usleep_range(10000, 10100);
	}
	return -ETIMEDOUT;
}

static int fg4_request(struct qcom_fg4 *fg)
{
	unsigned int val, arb, dma, mux = 0;
	int ret;

	ret = qcom_fg4_release(fg);
	if (ret)
		return ret;
	/* Do not take over an outstanding request from another owner. */
	ret = regmap_read(fg->map, fg->base + FG4_MEM_INTF, &val);
	if (ret)
		return ret;
	if (val & (FG4_MEM_REQUEST | FG4_IACS_SELECT))
		return -EBUSY;
	ret = regmap_read(fg->map, fg->base + FG4_MEM_ARB, &arb);
	if (ret)
		return ret;
	if (arb & FG4_ARB_REQUEST)
		return -EBUSY;
	ret = regmap_read(fg->map, fg->base + FG4_DMA_STATUS, &val);
	if (ret)
		return ret;
	ret = regmap_read(fg->map, fg->base + FG4_DMA_CTL, &dma);
	if (ret)
		return ret;
	ret = fg4_clear_dma_errors(fg, val, dma);
	if (ret)
		return ret;
	if (fg->major == 1) {
		ret = regmap_read(fg->map, fg->base + FG4_PEEK_MUX, &mux);
		if (ret)
			return ret;
	}
	fg->saved_dma = dma & FG4_DMA_PARTITIONED;
	fg->saved_arb = arb & FG4_ARB_LOW_LATENCY;
	fg->saved_mux = mux;
	fg->restore_pending = true;
	/* Mark pending before writing: a failed bus write can have taken effect. */
	ret = regmap_update_bits(fg->map, fg->base + FG4_DMA_CTL,
				 FG4_DMA_PARTITIONED, FG4_DMA_PARTITIONED);
	if (ret)
		return ret;
	ret = regmap_update_bits(fg->map, fg->base + FG4_MEM_ARB,
				 FG4_ARB_LOW_LATENCY, FG4_ARB_LOW_LATENCY);
	if (ret)
		return ret;
	if (fg->major == 1) {
		ret = regmap_write(fg->map, fg->base + FG4_PEEK_MUX, 0xac);
		if (ret)
			return ret;
		ret = fg4_poll(fg, FG4_PEEK_READ, FG4_ALG_ACTIVE, false, 35);
		if (ret)
			return ret;
		usleep_range(1000, 1010);
	}
	ret = regmap_update_bits(fg->map, fg->base + FG4_MEM_ARB,
				 FG4_ARB_REQUEST, FG4_ARB_REQUEST);
	if (ret)
		return ret;
	ret = regmap_update_bits(fg->map, fg->base + FG4_MEM_INTF,
				 FG4_MEM_REQUEST | FG4_IACS_SELECT, FG4_MEM_REQUEST);
	if (ret)
		return ret;
	usleep_range(40, 50);
	ret = fg4_poll(fg, FG4_MEM_STATUS, FG4_MEM_GRANTED, true, 50);
	if (!ret && fg->major == 1)
		usleep_range(1000, 1010);
	return ret;
}

static int fg4_sram_transfer(struct qcom_fg4 *fg, unsigned int offset,
			     void *read_data, const void *write_data, size_t len)
{
	/* Exclusive ends in bytes; the vendor table's unused word 486 is excluded. */
	static const unsigned int ends[] = { 128, 340, 550, 600, 812, FG4_SRAM_SIZE };
	unsigned int start = 0, bank = 0, addr, val, copied = 0;
	size_t count;
	const u8 *source = write_data;
	u8 verify[8];
	u8 *buffer;
	int ret, cleanup;

	if ((!read_data == !write_data) || !len || offset >= FG4_SRAM_SIZE ||
	    len > FG4_SRAM_SIZE - offset)
		return -EINVAL;
	buffer = read_data ? kmalloc(len, GFP_KERNEL) : NULL;
	if (read_data && !buffer)
		return -ENOMEM;
	mutex_lock(&fg->lock);
	fg->cleanup_error = 0;
	ret = fg4_request(fg);
	if (ret)
		goto out;
	while (copied < len) {
		while (offset >= ends[bank])
			start = ends[bank++];
		addr = fg->base + 0x420 + 0x100 * bank + offset - start;
		/* Bound accesses to one DMA partition and one SPMI read payload. */
		count = min_t(size_t, 8, min_t(size_t, len - copied, ends[bank] - offset));
		if (write_data) {
			ret = regmap_bulk_write(fg->map, addr, source + copied, count);
			if (ret)
				goto out;
			ret = regmap_bulk_read(fg->map, addr, verify, count);
			if (!ret && memcmp(verify, source + copied, count))
				ret = -EIO;
		} else {
			ret = regmap_bulk_read(fg->map, addr, buffer + copied, count);
		}
		if (ret)
			goto out;
		copied += count;
		offset += count;
	}
	ret = regmap_read(fg->map, fg->base + FG4_DMA_STATUS, &val);
	if (ret)
		goto out;
	if (val & FG4_DMA_ERRORS) {
		ret = -EIO;
		goto out;
	}
	ret = regmap_read(fg->map, fg->base + FG4_MEM_STATUS, &val);
	if (!ret && !(val & FG4_MEM_GRANTED))
		ret = -EAGAIN;
out:
	cleanup = qcom_fg4_release(fg);
	if (!ret)
		ret = cleanup;
	if (!ret && read_data)
		memcpy(read_data, buffer, len);
	mutex_unlock(&fg->lock);
	kfree(buffer);
	return ret;
}

int qcom_fg4_sram_read(struct qcom_fg4 *fg, unsigned int offset,
		       void *data, size_t len)
{
	return fg4_sram_transfer(fg, offset, data, NULL, len);
}

int qcom_fg4_sram_write(struct qcom_fg4 *fg, unsigned int offset,
			const void *data, size_t len)
{
	/* A failed write can change SRAM; the profile owner must invalidate it. */
	return fg4_sram_transfer(fg, offset, NULL, data, len);
}

int qcom_fg4_battery_id(struct qcom_fg4 *fg, unsigned int *ohms)
{
	static const unsigned int pullups[] = { 400000, 100000, 30000 };
	const unsigned int full_scale = 0x70e4;
	unsigned int i, status, code, distance, best_distance = full_scale;
	unsigned int best_code = 0, best_pullup = 0;
	u64 resistance;
	u8 raw[2];
	int ret;

	for (i = 0; i < ARRAY_SIZE(pullups); i++) {
		ret = regmap_read(fg->map, fg->base + 0x265 + 8 * i, &status);
		if (ret)
			return ret;
		if (!(status & BIT(0)))
			continue;
		ret = regmap_bulk_read(fg->map, fg->base + 0x266 + 8 * i, raw, 2);
		if (ret)
			return ret;
		code = get_unaligned_le16(raw);
		/* Reject saturation and short/open endpoints before dividing. */
		if (!code || code >= full_scale)
			continue;
		distance = abs((int)code - (int)(full_scale / 2));
		if (distance < best_distance) {
			best_distance = distance;
			best_code = code;
			best_pullup = pullups[i];
		}
	}
	if (!best_code)
		return -ENODATA;
	resistance = div_u64((u64)best_pullup * best_code, full_scale - best_code);
	if (resistance > U32_MAX)
		return -ERANGE;
	*ohms = resistance;
	return 0;
}

int qcom_fg4_present(struct qcom_fg4 *fg, int *value)
{
	unsigned int val;
	int ret = regmap_read(fg->map, fg->base + FG4_BATT_MISSING, &val);

	if (!ret)
		*value = !(val & BIT(0));
	return ret;
}

static int fg4_read_pair(struct qcom_fg4 *fg, u16 reg, u16 *value)
{
	u8 sample[2], shadow[2];
	int ret, i;

	for (i = 0; i < FG4_READ_TRIES; i++) {
		ret = regmap_bulk_read(fg->map, fg->base + reg, sample, sizeof(sample));
		if (ret)
			return ret;
		ret = regmap_bulk_read(fg->map, fg->base + reg + FG4_SHADOW_OFFSET,
				       shadow, sizeof(shadow));
		if (ret)
			return ret;
		if (!memcmp(sample, shadow, sizeof(sample))) {
			*value = get_unaligned_le16(sample);
			return 0;
		}
	}
	return -EAGAIN;
}

int qcom_fg4_voltage(struct qcom_fg4 *fg, int *value)
{
	u16 raw;
	int ret = fg4_read_pair(fg, FG4_VOLTAGE, &raw);

	if (!ret)
		*value = div_u64((u64)raw * 122070, 1000);
	return ret;
}

int qcom_fg4_current(struct qcom_fg4 *fg, int *value)
{
	u16 raw;
	int ret = fg4_read_pair(fg, FG4_CURRENT, &raw);

	/* FG uses positive for discharge; power_supply uses positive for charge. */
	if (!ret)
		*value = -div_s64((s64)sign_extend32(raw, 15) * 488281, 1000);
	return ret;
}

int qcom_fg4_capacity(struct qcom_fg4 *fg, int *value)
{
	unsigned int val;
	u8 pair[2];
	int ret, i;

	ret = regmap_read(fg->map, fg->base + FG4_SOC_ENABLE, &val);
	if (ret)
		return ret;
	if (!(val & BIT(7)))
		return -ENODATA;
	ret = regmap_read(fg->map, fg->base + FG4_SOC_READY, &val);
	if (ret)
		return ret;
	if (!(val & BIT(1)))
		return -ENODATA;
	for (i = 0; i < FG4_READ_TRIES; i++) {
		ret = regmap_bulk_read(fg->map, fg->base + FG4_SOC_VALUE, pair, sizeof(pair));
		if (ret)
			return ret;
		if (pair[0] != pair[1])
			continue;
		if (!pair[0])
			*value = 0;
		else if (pair[0] == 255)
			*value = 100;
		else
			*value = DIV_ROUND_CLOSEST((pair[0] - 1) * 98, 253) + 1;
		return 0;
	}
	return -EAGAIN;
}

int qcom_fg4_temperature(struct qcom_fg4 *fg, int *millicelsius)
{
	u8 raw[2];
	int ret = qcom_fg4_sram_read(fg, FG4_TEMP_BYTE, raw, sizeof(raw));

	if (!ret)
		*millicelsius = sign_extend32(get_unaligned_le16(raw), 9) * 250;
	return ret;
}
