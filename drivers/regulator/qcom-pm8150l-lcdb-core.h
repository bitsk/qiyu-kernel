/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_PM8150L_LCDB_CORE_H
#define _QCOM_PM8150L_LCDB_CORE_H

#include <linux/ktime.h>
#include <linux/regmap.h>
#include <linux/types.h>

#define LCDB_LDO 0
#define LCDB_NCP 1
#define LCDB_RT_STATUS 0x10
#define LCDB_BOOST_VOLTAGE 0x41
#define LCDB_MODULE_READY 0x45
#define LCDB_ENABLE 0x46
#define LCDB_LDO_VOLTAGE 0x71
#define LCDB_FORCE_PD 0x79
#define LCDB_NCP_VOLTAGE 0x81
#define LCDB_MODULE_EN 0x80
#define LCDB_VREG_OK 0x01
#define LCDB_SHORT_CIRCUIT 0x02

/* All operations require the parent driver's mutex. */
struct lcdb_core {
	struct regmap *regmap;
	unsigned int base;
	unsigned int enabled_mask;
	unsigned int poll_delay_us;
	unsigned int write_attempts;
	bool inherited_enabled;
	bool faulted;
	bool short_circuit;
	int cleanup_error;
	ktime_t last_off;
};

int lcdb_core_init(struct lcdb_core *lcdb);
int lcdb_core_voltage_uv(unsigned int selector);
int lcdb_core_get_voltage_sel(struct lcdb_core *lcdb, unsigned int id);
int lcdb_core_set_voltage_sel(struct lcdb_core *lcdb, unsigned int id,
			      unsigned int selector);
int lcdb_core_restore_voltages(struct lcdb_core *lcdb, unsigned int ldo,
			       unsigned int ncp, unsigned int boost);
int lcdb_core_is_enabled(struct lcdb_core *lcdb, unsigned int id);
int lcdb_core_enable(struct lcdb_core *lcdb, unsigned int id);
int lcdb_core_disable(struct lcdb_core *lcdb, unsigned int id);
int lcdb_core_fault(struct lcdb_core *lcdb, bool short_circuit);

#endif
