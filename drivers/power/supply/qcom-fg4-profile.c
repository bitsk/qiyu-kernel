// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2018-2020 The Linux Foundation. All rights reserved.
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * PM8150B profile initialization, derived from qpnp-fg-gen4.c and fg-util.c.
 * Transport and failure handling are local. See references/power for provenance.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#include "qcom-fg4-profile.h"

#define FG4_PROFILE_BYTE		130
#define FG4_PROFILE_ID_SIZE	24
#define FG4_INTEGRITY_BYTE	598
#define FG4_SYS_CONFIG_BYTE	120
#define FG4_FIRST_CURRENT_BYTE	942
#define FG4_RESTART_REG		0x048
#define FG4_RESTART_GO		BIT(0)
#define FG4_SIDE_LOAD		BIT(0)

int qcom_fg4_profile_validate(const struct qcom_fg4_profile *profile)
{
	if (!profile || !profile->id_ohms || profile->id_tolerance_percent > 100 ||
	    !profile->nominal_mah || profile->nominal_mah > 32767 ||
	    profile->full_voltage_uv < 2000000 || profile->full_voltage_uv > 5500000)
		return -EINVAL;
	/* NOM_CAP word 271 is within the fixed 416-byte profile. */
	if (get_unaligned_le16(profile->data + 412) != profile->nominal_mah)
		return -EINVAL;
	if (profile->therm_pullup_kohms != 30 && profile->therm_pullup_kohms != 100 &&
	    profile->therm_pullup_kohms != 400)
		return -EINVAL;
	return 0;
}

static int fg4_sram_update(struct qcom_fg4 *fg, unsigned int offset, u8 mask, u8 value)
{
	u8 old;
	int ret;

	ret = qcom_fg4_sram_read(fg, offset, &old, 1);
	if (ret)
		return ret;
	old = (old & ~mask) | (value & mask);
	return qcom_fg4_sram_write(fg, offset, &old, 1);
}

int qcom_fg4_profile_abort(struct qcom_fg4 *fg, struct qcom_fg4_profile_state *state)
{
	u8 invalid = 0;
	int ret, first;

	state->valid = false;
	mutex_lock(&fg->lock);
	first = qcom_fg4_release(fg);
	mutex_unlock(&fg->lock);
	if (state->restart_pending) {
		ret = regmap_update_bits(fg->map, fg->base + FG4_RESTART_REG, FG4_RESTART_GO, 0);
		if (!ret)
			state->restart_pending = false;
		else if (!first)
			first = ret;
	}
	if (state->dirty) {
		ret = qcom_fg4_sram_write(fg, FG4_INTEGRITY_BYTE, &invalid, 1);
		if (!ret)
			state->dirty = false;
		else if (!first)
			first = ret;
	}
	if (state->side_pending) {
		ret = fg4_sram_update(fg, FG4_SYS_CONFIG_BYTE, FG4_SIDE_LOAD, state->saved_side);
		if (!ret)
			state->side_pending = false;
		else if (!first)
			first = ret;
	}
	state->cleanup_error = first;
	return first;
}

static int fg4_profile_matches(struct qcom_fg4 *fg, const struct qcom_fg4_profile *profile,
			       bool *matches)
{
	static const u8 valid_integrity[] = { 0x09, 0x03, 0x07, 0x0d, 0x13, 0x17, 0x1d };
	u8 integrity, identity[FG4_PROFILE_ID_SIZE], nominal[2];
	int ret, i;

	*matches = false;
	ret = qcom_fg4_sram_read(fg, FG4_INTEGRITY_BYTE, &integrity, 1);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(valid_integrity); i++)
		if (integrity == valid_integrity[i])
			break;
	if (i == ARRAY_SIZE(valid_integrity))
		return 0;
	ret = qcom_fg4_sram_read(fg, FG4_PROFILE_BYTE, identity, sizeof(identity));
	if (ret)
		return ret;
	ret = qcom_fg4_sram_read(fg, 271 * 2, nominal, sizeof(nominal));
	if (ret)
		return ret;
	/* Other profile fields can change during normal gauge operation. */
	*matches = !memcmp(identity, profile->data, sizeof(identity)) &&
		   get_unaligned_le16(nominal) == profile->nominal_mah;
	return 0;
}

static int fg4_profile_parameters(struct qcom_fg4 *fg, const struct qcom_fg4_profile *profile)
{
	u8 coefficients[10], raw[2];
	unsigned int pullup;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(profile->therm_coeffs); i++)
		put_unaligned_le16(profile->therm_coeffs[i], coefficients + 2 * i);
	ret = qcom_fg4_sram_write(fg, 4 * 2, coefficients, sizeof(coefficients));
	if (ret)
		return ret;
	ret = qcom_fg4_sram_write(fg, 7, &profile->therm_center, 1);
	if (ret)
		return ret;
	pullup = profile->therm_pullup_kohms == 30 ? 1 :
		 profile->therm_pullup_kohms == 100 ? 2 : 3;
	ret = regmap_update_bits(fg->map, fg->base + 0x281, 3, pullup);
	if (ret)
		return ret;
	/* FG full-voltage threshold; this does not program a charger limit. */
	put_unaligned_le16(div_u64((u64)profile->full_voltage_uv * 1000, 244141), raw);
	ret = qcom_fg4_sram_write(fg, 23 * 2, raw, sizeof(raw));
	if (ret)
		return ret;
	if (fg->major == 1)
		return fg4_sram_update(fg, 241 * 2, BIT(1), 0);
	if (fg->major != 2)
		return 0;
	/* The V2 Rslow scale workaround uses words which hold V1 learned capacity. */
	put_unaligned_le16(0x4000, raw);
	ret = qcom_fg4_sram_write(fg, 285 * 2, raw, sizeof(raw));
	if (ret)
		return ret;
	return qcom_fg4_sram_write(fg, 281 * 2, raw, sizeof(raw));
}

static int fg4_restart(struct qcom_fg4 *fg, struct qcom_fg4_profile_state *state,
		       const struct qcom_fg4_restart_ops *restart)
{
	int ret, cleanup;

	restart->prepare(restart->context);
	state->restart_pending = true;
	ret = regmap_update_bits(fg->map, fg->base + FG4_RESTART_REG,
				 FG4_RESTART_GO, FG4_RESTART_GO);
	if (!ret)
		ret = restart->wait(restart->context, 1000);
	/* Preserve a timeout even when clearing RESTART_GO succeeds. */
	cleanup = regmap_update_bits(fg->map, fg->base + FG4_RESTART_REG, FG4_RESTART_GO, 0);
	if (!cleanup)
		state->restart_pending = false;
	return ret ?: cleanup;
}

int qcom_fg4_profile_start(struct qcom_fg4 *fg, const struct qcom_fg4_profile *profile,
			   struct qcom_fg4_profile_state *state,
			   const struct qcom_fg4_restart_ops *restart)
{
	unsigned int value, difference;
	u8 sys_config, integrity = 0, raw[2] = { 0, 0 };
	bool matches;
	int ret, present;

	ret = qcom_fg4_profile_validate(profile);
	if (ret || !restart || !restart->prepare || !restart->wait)
		return ret ?: -EINVAL;
	ret = qcom_fg4_profile_abort(fg, state);
	if (ret)
		return ret;
	state->loaded = false;
	ret = qcom_fg4_present(fg, &present);
	if (ret || !present)
		return ret ?: -ENODEV;
	ret = qcom_fg4_battery_id(fg, &value);
	if (ret)
		return ret;
	state->measured_id_ohms = value;
	difference = value > profile->id_ohms ? value - profile->id_ohms : profile->id_ohms - value;
	if ((u64)difference * 100 > (u64)profile->id_ohms * profile->id_tolerance_percent)
		return -ENODEV;
	ret = regmap_read(fg->map, fg->base + FG4_RESTART_REG, &value);
	if (ret || (value & FG4_RESTART_GO))
		return ret ?: -EBUSY;
	ret = regmap_read(fg->map, fg->base + 0x046, &value);
	if (ret || !(value & BIT(7)))
		return ret ?: -ENODATA;
	ret = qcom_fg4_sram_read(fg, FG4_SYS_CONFIG_BYTE, &sys_config, 1);
	if (ret || (sys_config & FG4_SIDE_LOAD))
		return ret ?: -EBUSY;
	state->saved_side = sys_config & FG4_SIDE_LOAD;
	ret = fg4_profile_matches(fg, profile, &matches);
	if (ret)
		return ret;
	/* Any failed profile/parameter write must invalidate the integrity marker. */
	state->dirty = true;
	if (!matches) {
		ret = qcom_fg4_sram_write(fg, FG4_INTEGRITY_BYTE, &integrity, 1);
		if (ret)
			goto fail;
		ret = qcom_fg4_sram_write(fg, FG4_PROFILE_BYTE, profile->data, FG4_PROFILE_SIZE);
		if (ret)
			goto fail;
		state->side_pending = true;
		ret = fg4_sram_update(fg, FG4_SYS_CONFIG_BYTE, FG4_SIDE_LOAD, FG4_SIDE_LOAD);
		if (ret)
			goto fail;
		ret = qcom_fg4_sram_write(fg, FG4_FIRST_CURRENT_BYTE, raw, sizeof(raw));
		if (ret)
			goto fail;
		integrity = 0x09;
		ret = qcom_fg4_sram_write(fg, FG4_INTEGRITY_BYTE, &integrity, 1);
		if (ret)
			goto fail;
		ret = fg4_restart(fg, state, restart);
		if (ret)
			goto fail;
		ret = fg4_sram_update(fg, FG4_SYS_CONFIG_BYTE, FG4_SIDE_LOAD, state->saved_side);
		if (ret)
			goto fail;
		state->side_pending = false;
		if (fg->major == 1)
			msleep(1000);
		put_unaligned_le16(profile->nominal_mah, raw);
		ret = qcom_fg4_sram_write(fg, (fg->major == 1 ? 285 : 287) * 2, raw, sizeof(raw));
		if (ret)
			goto fail;
		state->loaded = true;
	}
	ret = fg4_profile_parameters(fg, profile);
	if (ret)
		goto fail;
	ret = regmap_read(fg->map, fg->base + 0x010, &value);
	if (ret || !(value & BIT(1))) {
		ret = ret ?: -ENODATA;
		goto fail;
	}
	ret = qcom_fg4_present(fg, &present);
	if (ret || !present) {
		ret = ret ?: -ENODEV;
		goto fail;
	}
	state->dirty = false;
	state->valid = true;
	return 0;
fail:
	qcom_fg4_profile_abort(fg, state);
	return ret;
}
