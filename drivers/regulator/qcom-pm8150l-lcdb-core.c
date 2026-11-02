// SPDX-License-Identifier: GPL-2.0-only
/* PM8150L v3 LCD bias sequencing; register access is shared with host tests. */
#include <linux/delay.h>
#include <linux/errno.h>

#include "qcom-pm8150l-lcdb-core.h"

#define LCDB_HEADROOM_UV 150000
#define LCDB_OFF_ON_US 20000
#define LCDB_POLL_ATTEMPTS 10

static int lcdb_read(struct lcdb_core *lcdb, unsigned int offset, unsigned int *val)
{
	return regmap_read(lcdb->regmap, lcdb->base + offset, val);
}

static int lcdb_write(struct lcdb_core *lcdb, unsigned int offset, unsigned int val)
{
	lcdb->write_attempts++;
	return regmap_write(lcdb->regmap, lcdb->base + offset, val);
}

static int lcdb_update(struct lcdb_core *lcdb, unsigned int offset,
		       unsigned int mask, unsigned int val)
{
	lcdb->write_attempts++;
	return regmap_update_bits(lcdb->regmap, lcdb->base + offset, mask, val);
}

int lcdb_core_voltage_uv(unsigned int selector)
{
	if (selector > 31)
		return -EINVAL;
	if (selector < 10)
		return 4000000 + selector * 100000;
	return 4950000 + (selector - 10) * 50000;
}

int lcdb_core_init(struct lcdb_core *lcdb)
{
	static const unsigned int soft_us[] = { 0, 500, 1000, 2000 };
	static const unsigned int debounce_us[] = { 2, 4, 16, 32 };
	static const unsigned int delay_us[] = { 0, 1000, 4000, 8000 };
	static const unsigned int soft_regs[] = { 0x5b, 0x7f, 0x8f };
	static const unsigned int debounce_regs[] = { 0x55, 0x75, 0x85 };
	unsigned int val, i;
	int ret;

	ret = lcdb_read(lcdb, LCDB_ENABLE, &val);
	if (ret)
		return ret;
	/* Like shared enable GPIOs, each output has its own logical vote. */
	lcdb->inherited_enabled = !!(val & LCDB_MODULE_EN);
	lcdb->enabled_mask = lcdb->inherited_enabled ? 3 : 0;
	lcdb->last_off = ktime_get_boottime();
	lcdb->poll_delay_us = 100;
	for (i = 0; i < 3; i++) {
		ret = lcdb_read(lcdb, soft_regs[i], &val);
		if (ret)
			return ret;
		lcdb->poll_delay_us += soft_us[val & 3];
		ret = lcdb_read(lcdb, debounce_regs[i], &val);
		if (ret)
			return ret;
		lcdb->poll_delay_us += debounce_us[val & 3];
	}
	ret = lcdb_read(lcdb, 0x66, &val);
	if (ret)
		return ret;
	lcdb->poll_delay_us += delay_us[(val >> 2) & 3];
	return 0;
}

/* Always try to remove force-pulldown, even when the disable write fails. */
static int lcdb_force_off(struct lcdb_core *lcdb)
{
	unsigned int enabled;
	int first, ret, read_ret;

	first = lcdb_write(lcdb, LCDB_FORCE_PD, 0x81);
	ret = lcdb_write(lcdb, LCDB_ENABLE, 0);
	if (!first)
		first = ret;
	read_ret = lcdb_read(lcdb, LCDB_ENABLE, &enabled);
	if (!read_ret && !(enabled & LCDB_MODULE_EN)) {
		lcdb->enabled_mask = 0;
		lcdb->last_off = ktime_get_boottime();
		usleep_range(10000, 11000);
	} else if (!first) {
		first = read_ret ? read_ret : -EIO;
	}
	/* Do not hold a forced discharge on an output that may still be on. */
	ret = lcdb_write(lcdb, LCDB_FORCE_PD, 0);
	if (!first)
		first = ret;
	if (first) {
		lcdb->faulted = true;
		lcdb->cleanup_error = first;
	}
	return first;
}

int lcdb_core_fault(struct lcdb_core *lcdb, bool short_circuit)
{
	lcdb->faulted = true;
	lcdb->short_circuit |= short_circuit;
	return lcdb_force_off(lcdb);
}

int lcdb_core_is_enabled(struct lcdb_core *lcdb, unsigned int id)
{
	unsigned int val;
	int ret;

	if (id > LCDB_NCP)
		return -EINVAL;
	ret = lcdb_read(lcdb, LCDB_ENABLE, &val);
	if (ret)
		return ret;
	return !!(val & LCDB_MODULE_EN) && !!(lcdb->enabled_mask & (1U << id));
}

int lcdb_core_enable(struct lcdb_core *lcdb, unsigned int id)
{
	unsigned int enabled, status, attempt;
	s64 remaining;
	int ret;

	if (id > LCDB_NCP)
		return -EINVAL;
	if (lcdb->faulted)
		return -EIO;
	ret = lcdb_read(lcdb, LCDB_ENABLE, &enabled);
	if (ret)
		return ret;
	if (!(enabled & LCDB_MODULE_EN)) {
		/* The delay belongs to the shared module, not an individual rdev. */
		remaining = LCDB_OFF_ON_US - ktime_us_delta(ktime_get_boottime(), lcdb->last_off);
		if (remaining > 0)
			usleep_range(remaining, remaining + 100);
		ret = lcdb_update(lcdb, LCDB_MODULE_READY, 0x80, 0x80);
		if (ret)
			return ret;
		ret = lcdb_write(lcdb, LCDB_ENABLE, LCDB_MODULE_EN);
		if (ret)
			goto rollback;
	}
	for (attempt = 0; attempt < LCDB_POLL_ATTEMPTS; attempt++) {
		usleep_range(lcdb->poll_delay_us, lcdb->poll_delay_us + 100);
		ret = lcdb_read(lcdb, LCDB_RT_STATUS, &status);
		if (ret)
			goto rollback;
		if (status & LCDB_SHORT_CIRCUIT) {
			lcdb->faulted = true;
			lcdb->short_circuit = true;
			ret = -EIO;
			goto rollback;
		}
		if (status & LCDB_VREG_OK) {
			lcdb->enabled_mask |= 1U << id;
			return 0;
		}
	}
	ret = -ETIMEDOUT;
rollback:
	/* A failed enable or loss of VREG_OK must not leave unowned bias on. */
	if (lcdb->enabled_mask)
		lcdb->faulted = true;
	lcdb_force_off(lcdb);
	return ret;
}

/* Restore all three setpoints on failed probe, including excess headroom. */
int lcdb_core_restore_voltages(struct lcdb_core *lcdb, unsigned int ldo,
			       unsigned int ncp, unsigned int boost)
{
	int ret, max_uv;

	if (ldo > 31 || ncp > 31 || boost > 63)
		return -EINVAL;
	max_uv = lcdb_core_voltage_uv(ldo > ncp ? ldo : ncp);
	if (4700000 + (int)boost * 25000 < max_uv + LCDB_HEADROOM_UV)
		return -EINVAL;
	ret = lcdb_core_set_voltage_sel(lcdb, LCDB_NCP, ncp);
	if (!ret)
		ret = lcdb_core_set_voltage_sel(lcdb, LCDB_LDO, ldo);
	if (!ret)
		ret = lcdb_update(lcdb, LCDB_BOOST_VOLTAGE, 0x3f, boost);
	if (ret) {
		lcdb_core_fault(lcdb, false);
		lcdb->cleanup_error = ret;
	}
	return ret;
}

int lcdb_core_disable(struct lcdb_core *lcdb, unsigned int id)
{
	unsigned int mask, enabled;
	int ret;

	if (id > LCDB_NCP)
		return -EINVAL;
	mask = lcdb->enabled_mask & ~(1U << id);
	if (mask) {
		lcdb->enabled_mask = mask;
		return 0;
	}
	ret = lcdb_read(lcdb, LCDB_ENABLE, &enabled);
	if (ret)
		return ret;
	if (!(enabled & LCDB_MODULE_EN) && !lcdb->cleanup_error) {
		lcdb->enabled_mask = 0;
		return 0;
	}
	return lcdb_force_off(lcdb);
}

int lcdb_core_get_voltage_sel(struct lcdb_core *lcdb, unsigned int id)
{
	unsigned int val;
	int ret;

	if (id > LCDB_NCP)
		return -EINVAL;
	ret = lcdb_read(lcdb, id == LCDB_LDO ? LCDB_LDO_VOLTAGE : LCDB_NCP_VOLTAGE, &val);
	if (ret)
		return ret;
	return val & 0x1f;
}

int lcdb_core_set_voltage_sel(struct lcdb_core *lcdb, unsigned int id,
			      unsigned int selector)
{
	unsigned int boost, new_boost, status, reg, ceiling;
	int old_ldo, old_ncp, old_sel, other_uv, target_uv, needed_uv, max_old;
	int ret, cleanup;

	if (id > LCDB_NCP || selector > 31)
		return -EINVAL;
	if (lcdb->faulted)
		return -EIO;
	ret = lcdb_read(lcdb, LCDB_RT_STATUS, &status);
	if (ret)
		return ret;
	if (status & LCDB_SHORT_CIRCUIT) {
		lcdb_core_fault(lcdb, true);
		return -EIO;
	}
	old_ldo = lcdb_core_get_voltage_sel(lcdb, LCDB_LDO);
	if (old_ldo < 0)
		return old_ldo;
	old_ncp = lcdb_core_get_voltage_sel(lcdb, LCDB_NCP);
	if (old_ncp < 0)
		return old_ncp;
	ret = lcdb_read(lcdb, LCDB_BOOST_VOLTAGE, &boost);
	if (ret)
		return ret;
	boost &= 0x3f;
	max_old = lcdb_core_voltage_uv(old_ldo > old_ncp ? old_ldo : old_ncp);
	if (4700000 + (int)boost * 25000 < max_old + LCDB_HEADROOM_UV)
		return -EINVAL;
	old_sel = id == LCDB_LDO ? old_ldo : old_ncp;
	if (selector == (unsigned int)old_sel)
		return 0;
	other_uv = lcdb_core_voltage_uv(id == LCDB_LDO ? old_ncp : old_ldo);
	target_uv = lcdb_core_voltage_uv(selector);
	needed_uv = (target_uv > other_uv ? target_uv : other_uv) + LCDB_HEADROOM_UV;
	new_boost = needed_uv <= 4700000 ? 0 : (needed_uv - 4700000 + 24999) / 25000;
	if (new_boost > 63)
		return -EINVAL;
	reg = id == LCDB_LDO ? LCDB_LDO_VOLTAGE : LCDB_NCP_VOLTAGE;
	/* Raise boost first; lower it only after the output voltage falls. */
	if (new_boost > boost) {
		ret = lcdb_update(lcdb, LCDB_BOOST_VOLTAGE, 0x3f, new_boost);
		if (ret)
			goto rollback;
	}
	ret = lcdb_update(lcdb, reg, 0x1f, selector);
	if (ret)
		goto rollback;
	if (new_boost < boost) {
		ret = lcdb_update(lcdb, LCDB_BOOST_VOLTAGE, 0x3f, new_boost);
		if (ret)
			goto rollback;
	}
	return 0;

rollback:
	/* A failed bus write may have reached hardware. Restore enough headroom
	 * for either output value before restoring the original selector.
	 */
	ceiling = boost > new_boost ? boost : new_boost;
	cleanup = lcdb_update(lcdb, LCDB_BOOST_VOLTAGE, 0x3f, ceiling);
	if (!cleanup)
		cleanup = lcdb_update(lcdb, reg, 0x1f, old_sel);
	if (!cleanup)
		cleanup = lcdb_update(lcdb, LCDB_BOOST_VOLTAGE, 0x3f, boost);
	if (cleanup) {
		lcdb_core_fault(lcdb, false);
		lcdb->cleanup_error = cleanup;
	}
	return ret;
}
