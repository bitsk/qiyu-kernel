// SPDX-License-Identifier: GPL-2.0-only
/*
 * PM8150L v3 LCD bias regulators, initially developed for Qiyu MIX (A081).
 * Register encodings/sequences: Qualcomm qpnp-lcdb-regulator reference.
 * Both outputs share the module enable; each output owns a logical vote.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <soc/qcom/qcom-spmi-pmic.h>

#include "qcom-pm8150l-lcdb-core.h"

/* STS1..STS6 are excluded: those registers require a write to latch status. */
static const unsigned int lcdb_readable_offsets[] = {
	0x10, 0x40, 0x41, 0x45, 0x46, 0x47, 0x4b, 0x50,
	0x53, 0x55, 0x5b, 0x60, 0x62, 0x66, 0x71, 0x75,
	0x77, 0x79, 0x7b, 0x7c, 0x7f, 0x81, 0x85, 0x87,
	0x8b, 0x8c, 0x8f,
};

struct pm8150l_lcdb {
	struct device *dev;
	struct lcdb_core core;
	struct mutex lock;
	const struct qcom_spmi_pmic *pmic;
	struct regulator_dev *rdev[2];
	int irq;
};

static int lcdb_enable(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&lcdb->lock);
	ret = lcdb_core_enable(&lcdb->core, rdev_get_id(rdev));
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int lcdb_disable(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&lcdb->lock);
	ret = lcdb_core_disable(&lcdb->core, rdev_get_id(rdev));
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int lcdb_is_enabled(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&lcdb->lock);
	ret = lcdb_core_is_enabled(&lcdb->core, rdev_get_id(rdev));
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int lcdb_get_voltage_sel(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&lcdb->lock);
	ret = lcdb_core_get_voltage_sel(&lcdb->core, rdev_get_id(rdev));
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int lcdb_set_voltage_sel(struct regulator_dev *rdev, unsigned int selector)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&lcdb->lock);
	ret = lcdb_core_set_voltage_sel(&lcdb->core, rdev_get_id(rdev), selector);
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int lcdb_get_status(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	unsigned int status;
	int ret;

	mutex_lock(&lcdb->lock);
	if (lcdb->core.faulted) {
		ret = REGULATOR_STATUS_ERROR;
		goto out;
	}
	ret = lcdb_core_is_enabled(&lcdb->core, rdev_get_id(rdev));
	if (ret <= 0) {
		if (!ret)
			ret = REGULATOR_STATUS_OFF;
		goto out;
	}
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + LCDB_RT_STATUS, &status);
	if (!ret)
		ret = !(status & LCDB_SHORT_CIRCUIT) && (status & LCDB_VREG_OK) ?
			REGULATOR_STATUS_ON : REGULATOR_STATUS_ERROR;
out:
	mutex_unlock(&lcdb->lock);
	return ret;
}

static int lcdb_get_error_flags(struct regulator_dev *rdev, unsigned int *flags)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	unsigned int status;
	int ret;

	mutex_lock(&lcdb->lock);
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + LCDB_RT_STATUS, &status);
	if (!ret) {
		*flags = lcdb->core.faulted ? REGULATOR_ERROR_FAIL : 0;
		if (lcdb->core.short_circuit || (status & LCDB_SHORT_CIRCUIT))
			*flags |= REGULATOR_ERROR_OVER_CURRENT;
	}
	mutex_unlock(&lcdb->lock);
	return ret;
}

static const struct regulator_ops lcdb_ops = {
	.enable = lcdb_enable,
	.disable = lcdb_disable,
	.is_enabled = lcdb_is_enabled,
	.list_voltage = regulator_list_voltage_linear_range,
	.map_voltage = regulator_map_voltage_linear_range,
	.get_voltage_sel = lcdb_get_voltage_sel,
	.set_voltage_sel = lcdb_set_voltage_sel,
	.get_status = lcdb_get_status,
	.get_error_flags = lcdb_get_error_flags,
};

static const struct linear_range lcdb_voltage_ranges[] = {
	REGULATOR_LINEAR_RANGE(4000000, 0, 9, 100000),
	REGULATOR_LINEAR_RANGE(4950000, 10, 31, 50000),
};

static const struct regulator_desc lcdb_desc[] = {
	{
		.name = "lcdb-ldo",
		.of_match = "ldo",
		.id = LCDB_LDO,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &lcdb_ops,
		.n_voltages = 32,
		.linear_ranges = lcdb_voltage_ranges,
		.n_linear_ranges = ARRAY_SIZE(lcdb_voltage_ranges),
	},
	{
		.name = "lcdb-ncp",
		.of_match = "ncp",
		.id = LCDB_NCP,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.ops = &lcdb_ops,
		.n_voltages = 32,
		.linear_ranges = lcdb_voltage_ranges,
		.n_linear_ranges = ARRAY_SIZE(lcdb_voltage_ranges),
	},
};

static irqreturn_t lcdb_sc_irq(int irq, void *data)
{
	struct pm8150l_lcdb *lcdb = data;
	unsigned int status;
	bool short_circuit;
	int ret, cleanup, i;

	mutex_lock(&lcdb->lock);
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + LCDB_RT_STATUS, &status);
	if (!ret && !(status & LCDB_SHORT_CIRCUIT)) {
		mutex_unlock(&lcdb->lock);
		return IRQ_HANDLED;
	}
	mutex_unlock(&lcdb->lock);
	/* Confirm the PM8150L non-touch-wake short-circuit indication. */
	usleep_range(2000, 2100);
	mutex_lock(&lcdb->lock);
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + LCDB_RT_STATUS, &status);
	if (!ret && !(status & LCDB_SHORT_CIRCUIT)) {
		mutex_unlock(&lcdb->lock);
		return IRQ_HANDLED;
	}
	short_circuit = !ret && (status & LCDB_SHORT_CIRCUIT);
	cleanup = lcdb_core_fault(&lcdb->core, short_circuit);
	mutex_unlock(&lcdb->lock);
	/* Stop after a confirmed fault; do not repeatedly power-cycle a panel. */
	disable_irq_nosync(irq);
	dev_err(lcdb->dev, "LCDB fault latched: status_read=%d short=%u cleanup=%d\n",
		ret, short_circuit, cleanup);
	for (i = 0; i < ARRAY_SIZE(lcdb->rdev); i++)
		regulator_notifier_call_chain(lcdb->rdev[i],
			REGULATOR_EVENT_FAIL | (short_circuit ? REGULATOR_EVENT_OVER_CURRENT : 0),
			NULL);
	return IRQ_HANDLED;
}

/* Bring-up diagnostics; values are register setpoints, not voltage measurements. */
static ssize_t lcdb_snapshot_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct pm8150l_lcdb *lcdb = dev_get_drvdata(dev);
	unsigned int values[ARRAY_SIZE(lcdb_readable_offsets)];
	unsigned int i;
	ssize_t len;
	int ret;

	mutex_lock(&lcdb->lock);
	for (i = 0; i < ARRAY_SIZE(lcdb_readable_offsets); i++) {
		ret = regmap_read(lcdb->core.regmap,
			lcdb->core.base + lcdb_readable_offsets[i], &values[i]);
		if (ret) {
			mutex_unlock(&lcdb->lock);
			return ret;
		}
	}
	len = sysfs_emit(buf,
		"mode=regulator regulator_provider=1 irq_requested=1 sequential_reads=1\n"
		"pmic_subtype=0x%02x pmic_major=%u pmic_minor=%u base=0x%04x\n"
		"enabled_mask=0x%x inherited_enabled=%u faulted=%u short_circuit=%u cleanup_error=%d write_attempts=%u\n",
		lcdb->pmic->subtype, lcdb->pmic->major, lcdb->pmic->minor, lcdb->core.base,
		lcdb->core.enabled_mask, lcdb->core.inherited_enabled, lcdb->core.faulted,
		lcdb->core.short_circuit, lcdb->core.cleanup_error, lcdb->core.write_attempts);
	for (i = 0; i < ARRAY_SIZE(lcdb_readable_offsets); i++)
		len += sysfs_emit_at(buf, len, "reg_%02x=0x%02x\n",
				    lcdb_readable_offsets[i], values[i]);
	mutex_unlock(&lcdb->lock);
	return len;
}
static DEVICE_ATTR_RO(lcdb_snapshot);

static struct attribute *lcdb_attrs[] = {
	&dev_attr_lcdb_snapshot.attr,
	NULL,
};
ATTRIBUTE_GROUPS(lcdb);

static int pm8150l_lcdb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { .dev = dev };
	struct pm8150l_lcdb *lcdb;
	unsigned int mode, i, original_boost;
	int ret, original_ldo, original_ncp, cleanup;

	lcdb = devm_kzalloc(dev, sizeof(*lcdb), GFP_KERNEL);
	if (!lcdb)
		return -ENOMEM;
	lcdb->dev = dev;
	mutex_init(&lcdb->lock);
	lcdb->pmic = qcom_pmic_get(dev);
	if (IS_ERR(lcdb->pmic))
		return dev_err_probe(dev, PTR_ERR(lcdb->pmic), "PMIC identity unavailable\n");
	if (lcdb->pmic->subtype != PM8150L_SUBTYPE || lcdb->pmic->major < 3)
		return dev_err_probe(dev, -ENODEV, "Requires PM8150L revision 3 or newer\n");
	lcdb->core.regmap = dev_get_regmap(dev->parent, NULL);
	if (!lcdb->core.regmap)
		return dev_err_probe(dev, -ENODEV, "Missing parent SPMI regmap\n");
	ret = of_property_read_u32(dev->of_node, "reg", &lcdb->core.base);
	if (ret)
		return dev_err_probe(dev, ret, "Missing LCDB address\n");
	if (lcdb->core.base != 0xec00)
		return dev_err_probe(dev, -EINVAL, "Unexpected PM8150L LCDB address\n");
	/* Touch wake and external HWEN require different sequencing. */
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + 0x40, &mode);
	if (ret || (mode & 0x80))
		return dev_err_probe(dev, ret ?: -EOPNOTSUPP, "Auto touch-wake mode\n");
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + 0x60, &mode);
	if (ret || (mode & 0x08))
		return dev_err_probe(dev, ret ?: -EOPNOTSUPP, "Touch-wake mode\n");
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + LCDB_ENABLE, &mode);
	if (ret || (mode & 0x40))
		return dev_err_probe(dev, ret ?: -EOPNOTSUPP, "External HWEN mode\n");
	ret = lcdb_core_init(&lcdb->core);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read initial LCDB state\n");
	original_ldo = lcdb_core_get_voltage_sel(&lcdb->core, LCDB_LDO);
	if (original_ldo < 0)
		return original_ldo;
	original_ncp = lcdb_core_get_voltage_sel(&lcdb->core, LCDB_NCP);
	if (original_ncp < 0)
		return original_ncp;
	ret = regmap_read(lcdb->core.regmap, lcdb->core.base + LCDB_BOOST_VOLTAGE,
			  &original_boost);
	if (ret)
		return ret;
	lcdb->irq = platform_get_irq_byname(pdev, "sc-err");
	if (lcdb->irq < 0)
		return lcdb->irq;
	platform_set_drvdata(pdev, lcdb);
	config.driver_data = lcdb;
	config.regmap = lcdb->core.regmap;
	/* A081 stock logs confirm the same 5.5 V -> 5.7 V constraint handoff.
	 * Allow the regulator core to apply DT constraints, using ordered writes.
	 */
	for (i = 0; i < ARRAY_SIZE(lcdb->rdev); i++) {
		lcdb->rdev[i] = devm_regulator_register(dev, &lcdb_desc[i], &config);
		if (IS_ERR(lcdb->rdev[i])) {
			ret = PTR_ERR(lcdb->rdev[i]);
			goto restore_voltages;
		}
	}
	ret = devm_request_threaded_irq(dev, lcdb->irq, NULL, lcdb_sc_irq,
					IRQF_ONESHOT, "pm8150l-lcdb-sc", lcdb);
	if (ret)
		goto restore_voltages;
	dev_info(dev, "A081_LCDB_REGULATORS PM8150L v%u.%u inherited=%u votes=0x%x writes=%u\n",
		 lcdb->pmic->major, lcdb->pmic->minor, lcdb->core.inherited_enabled,
		 lcdb->core.enabled_mask, lcdb->core.write_attempts);
	return 0;

restore_voltages:
	mutex_lock(&lcdb->lock);
	cleanup = lcdb_core_restore_voltages(&lcdb->core, original_ldo, original_ncp,
					     original_boost & 0x3f);
	if (cleanup)
		dev_err(dev, "Probe rollback failed: %d\n", cleanup);
	mutex_unlock(&lcdb->lock);
	return dev_err_probe(dev, ret, "Cannot register LCDB regulators/IRQ\n");
}

static const struct of_device_id pm8150l_lcdb_of_match[] = {
	{ .compatible = "qcom,pm8150l-lcdb" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8150l_lcdb_of_match);

static struct platform_driver pm8150l_lcdb_driver = {
	.probe = pm8150l_lcdb_probe,
	.driver = {
		.name = "qcom-pm8150l-lcdb",
		.of_match_table = pm8150l_lcdb_of_match,
		.dev_groups = lcdb_groups,
	},
};
module_platform_driver(pm8150l_lcdb_driver);

MODULE_DESCRIPTION("PM8150L v3 LCD bias regulators");
MODULE_LICENSE("GPL");
