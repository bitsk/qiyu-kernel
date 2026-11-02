// SPDX-License-Identifier: GPL-2.0-only
/* PM8150B FG GEN4 profile, telemetry and power_supply integration. */
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <soc/qcom/qcom-spmi-pmic.h>

#include "qcom-fg4-profile.h"

struct fg4_chip {
	struct qcom_fg4 core;
	struct qcom_fg4_profile profile;
	struct qcom_fg4_profile_state profile_state;
	struct completion soc_ready;
	int irqs[5];
	struct device *dev;
	struct power_supply *psy;
	struct power_supply_desc desc;
	/* Keeps property reads outside suspend and teardown cleanup. */
	struct mutex access_lock;
	bool suspended;
};

static enum power_supply_property fg4_properties[] = {
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
};

static int fg4_get_measurement(struct fg4_chip *chip, enum power_supply_property prop,
			       union power_supply_propval *val)
{
	int present, temperature, ret;

	/* A retained DMA request can stall the gauge algorithm between reads. */
	if (chip->core.restore_pending || chip->core.dma_clear_pending) {
		mutex_lock(&chip->core.lock);
		ret = qcom_fg4_release(&chip->core);
		mutex_unlock(&chip->core.lock);
		if (ret)
			return ret;
	}
	ret = qcom_fg4_present(&chip->core, &present);
	if (ret)
		return ret;
	if (prop == POWER_SUPPLY_PROP_PRESENT) {
		val->intval = present;
		return 0;
	}
	if (!present)
		return -ENODATA;
	if ((prop == POWER_SUPPLY_PROP_CAPACITY || prop == POWER_SUPPLY_PROP_TEMP) &&
	    !chip->profile_state.valid)
		return -ENODATA;
	switch (prop) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return qcom_fg4_voltage(&chip->core, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return qcom_fg4_current(&chip->core, &val->intval);
	case POWER_SUPPLY_PROP_CAPACITY:
		return qcom_fg4_capacity(&chip->core, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		ret = qcom_fg4_temperature(&chip->core, &temperature);
		if (!ret)
			val->intval = temperature / 100;
		return ret;
	default:
		return -EINVAL;
	}
}

static int fg4_get_property(struct power_supply *psy, enum power_supply_property prop,
			    union power_supply_propval *val)
{
	struct fg4_chip *chip = power_supply_get_drvdata(psy);
	int ret;

	if (prop == POWER_SUPPLY_PROP_MANUFACTURER) {
		val->strval = "Qualcomm";
		return 0;
	}
	if (prop == POWER_SUPPLY_PROP_MODEL_NAME) {
		val->strval = "PM8150B FG GEN4";
		return 0;
	}
	mutex_lock(&chip->access_lock);
	if (chip->suspended)
		ret = -EHOSTDOWN;
	else
		ret = fg4_get_measurement(chip, prop, val);
	if (chip->core.cleanup_error)
		dev_err_ratelimited(chip->dev, "DMA access cleanup pending: %d\n",
				    chip->core.cleanup_error);
	mutex_unlock(&chip->access_lock);
	return ret;
}

static const struct power_supply_desc fg4_desc = {
	.name = "pm8150b-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = fg4_properties,
	.num_properties = ARRAY_SIZE(fg4_properties),
	.get_property = fg4_get_property,
};

static irqreturn_t fg4_changed(int irq, void *data)
{
	struct fg4_chip *chip = data;
	struct power_supply *psy;

	if (irq == chip->irqs[0])
		complete(&chip->soc_ready);
	if (irq == chip->irqs[2]) {
		mutex_lock(&chip->access_lock);
		chip->profile_state.valid = false;
		mutex_unlock(&chip->access_lock);
	}
	psy = READ_ONCE(chip->psy);
	if (psy)
		power_supply_changed(psy);
	return IRQ_HANDLED;
}

static void fg4_stop_notifications(void *data)
{
	struct fg4_chip *chip = data;
	int i;

	WRITE_ONCE(chip->psy, NULL);
	for (i = 0; i < ARRAY_SIZE(chip->irqs); i++)
		if (chip->irqs[i] > 0)
			synchronize_irq(chip->irqs[i]);
}

static void fg4_prepare_restart(void *context)
{
	struct fg4_chip *chip = context;

	synchronize_irq(chip->irqs[0]);
	reinit_completion(&chip->soc_ready);
}

static int fg4_wait_restart(void *context, unsigned int milliseconds)
{
	struct fg4_chip *chip = context;

	return wait_for_completion_timeout(&chip->soc_ready, msecs_to_jiffies(milliseconds)) ?
		0 : -ETIMEDOUT;
}

static void fg4_cleanup(void *data)
{
	struct fg4_chip *chip = data;
	int ret;

	mutex_lock(&chip->access_lock);
	ret = qcom_fg4_profile_abort(&chip->core, &chip->profile_state);
	mutex_unlock(&chip->access_lock);
	if (ret)
		dev_err(chip->dev, "profile/DMA cleanup pending on removal: %d\n", ret);
}

static int fg4_parse_profile(struct fg4_chip *chip)
{
	struct qcom_fg4_profile *profile = &chip->profile;
	struct fwnode_handle *battery;
	u32 capacity, maximum_uv, value, coefficients[5];
	int ret, i;

	if (device_property_count_u8(chip->dev, "qcom,fg-profile-data") != FG4_PROFILE_SIZE)
		return -EINVAL;
	ret = device_property_read_u8_array(chip->dev, "qcom,fg-profile-data",
					    profile->data, FG4_PROFILE_SIZE);
	if (ret)
		return ret;
	ret = device_property_read_u32(chip->dev, "qcom,battery-id-ohms", &profile->id_ohms);
	if (ret)
		return ret;
	ret = device_property_read_u32(chip->dev, "qcom,battery-id-tolerance-percent",
				       &profile->id_tolerance_percent);
	if (ret)
		return ret;
	ret = device_property_read_u32(chip->dev, "qcom,fg-full-voltage-microvolt",
				       &profile->full_voltage_uv);
	if (ret)
		return ret;
	ret = device_property_read_u32_array(chip->dev, "qcom,therm-coefficients", coefficients, 5);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(coefficients); i++) {
		if (coefficients[i] > U16_MAX)
			return -EINVAL;
		profile->therm_coeffs[i] = coefficients[i];
	}
	ret = device_property_read_u32(chip->dev, "qcom,therm-center-offset", &value);
	if (ret || value > U8_MAX)
		return ret ?: -EINVAL;
	profile->therm_center = value;
	ret = device_property_read_u32(chip->dev, "qcom,therm-pull-up-ohms", &value);
	if (ret || value % 1000)
		return ret ?: -EINVAL;
	profile->therm_pullup_kohms = value / 1000;
	battery = fwnode_find_reference(dev_fwnode(chip->dev), "monitored-battery", 0);
	if (IS_ERR(battery))
		return PTR_ERR(battery);
	ret = fwnode_property_read_u32(battery, "charge-full-design-microamp-hours", &capacity);
	if (!ret)
		ret = fwnode_property_read_u32(battery, "voltage-max-design-microvolt",
					       &maximum_uv);
	fwnode_handle_put(battery);
	if (ret)
		return ret;
	if (!capacity || capacity % 1000 || profile->full_voltage_uv > maximum_uv)
		return -EINVAL;
	profile->nominal_mah = get_unaligned_le16(profile->data + 412);
	return qcom_fg4_profile_validate(profile);
}

static int fg4_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"soc-ready", "msoc-delta", "batt-missing", "batt-temp-delta", "vbatt-low",
	};
	struct power_supply_config config = {};
	struct qcom_fg4_restart_ops restart = {
		.prepare = fg4_prepare_restart,
		.wait = fg4_wait_restart,
	};
	const struct qcom_spmi_pmic *pmic;
	struct power_supply *psy;
	struct fg4_chip *chip;
	u32 base;
	int ret, irq, i;

	pmic = qcom_pmic_get(&pdev->dev);
	if (IS_ERR(pmic))
		return dev_err_probe(&pdev->dev, PTR_ERR(pmic), "could not get PMIC revision\n");
	if (pmic->subtype != PM8150B_SUBTYPE)
		return -ENODEV;
	ret = device_property_read_u32(&pdev->dev, "reg", &base);
	if (ret || base > 0xf600 || (base & 0xff))
		return dev_err_probe(&pdev->dev, ret ?: -EINVAL, "invalid FG base address\n");
	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &pdev->dev;
	chip->core.base = base;
	chip->core.major = pmic->major;
	chip->core.map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->core.map)
		return -ENODEV;
	mutex_init(&chip->access_lock);
	mutex_init(&chip->core.lock);
	init_completion(&chip->soc_ready);
	ret = fg4_parse_profile(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "invalid battery profile parameters\n");
	ret = qcom_fg4_check_hardware(&chip->core);
	if (ret)
		return dev_err_probe(chip->dev, ret, "FG peripheral subtype check failed\n");
	ret = devm_add_action_or_reset(chip->dev, fg4_cleanup, chip);
	if (ret)
		return ret;
	/* Install SOC-ready before a possible profile restart. No psy is published yet. */
	irq = platform_get_irq_byname(pdev, names[0]);
	if (irq < 0)
		return irq;
	chip->irqs[0] = irq;
	ret = devm_request_threaded_irq(chip->dev, irq, NULL, fg4_changed,
					IRQF_ONESHOT, names[0], chip);
	if (ret)
		return ret;
	restart.context = chip;
	ret = qcom_fg4_profile_start(&chip->core, &chip->profile, &chip->profile_state, &restart);
	if (ret)
		return dev_err_probe(chip->dev, ret, "battery profile initialization failed\n");
	dev_info(chip->dev, "battery ID %u ohms; profile %s\n",
		 chip->profile_state.measured_id_ohms,
		 chip->profile_state.loaded ? "loaded" : "identity matched");
	chip->desc = fg4_desc;
	config.drv_data = chip;
	config.fwnode = dev_fwnode(chip->dev);
	psy = devm_power_supply_register(chip->dev, &chip->desc, &config);
	if (IS_ERR(psy))
		return dev_err_probe(chip->dev, PTR_ERR(psy), "could not register battery\n");
	WRITE_ONCE(chip->psy, psy);
	ret = devm_add_action_or_reset(chip->dev, fg4_stop_notifications, chip);
	if (ret)
		return ret;
	for (i = 1; i < ARRAY_SIZE(names); i++) {
		irq = platform_get_irq_byname(pdev, names[i]);
		if (irq < 0)
			return irq;
		chip->irqs[i] = irq;
		ret = devm_request_threaded_irq(chip->dev, irq, NULL, fg4_changed,
						IRQF_ONESHOT, names[i], chip);
		if (ret) {
			chip->irqs[i] = 0;
			return ret;
		}
	}
	ret = devm_device_init_wakeup(chip->dev);
	if (ret)
		return ret;
	/* The final IRQ in the binding is battery voltage low. */
	ret = devm_pm_set_wake_irq(chip->dev, irq);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, chip);
	return 0;
}

static int fg4_suspend(struct device *dev)
{
	struct fg4_chip *chip = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&chip->access_lock);
	mutex_lock(&chip->core.lock);
	ret = qcom_fg4_release(&chip->core);
	mutex_unlock(&chip->core.lock);
	if (!ret)
		chip->suspended = true;
	mutex_unlock(&chip->access_lock);
	return ret;
}

static int fg4_resume(struct device *dev)
{
	struct fg4_chip *chip = dev_get_drvdata(dev);

	mutex_lock(&chip->access_lock);
	chip->suspended = false;
	mutex_unlock(&chip->access_lock);
	power_supply_changed(chip->psy);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fg4_pm_ops, fg4_suspend, fg4_resume);

static const struct of_device_id fg4_of_match[] = {
	{ .compatible = "qcom,pm8150b-fg" },
	{ }
};
MODULE_DEVICE_TABLE(of, fg4_of_match);

static struct platform_driver fg4_driver = {
	.probe = fg4_probe,
	.driver = {
		.name = "qcom-fg4",
		.of_match_table = fg4_of_match,
		.pm = pm_sleep_ptr(&fg4_pm_ops),
	},
};
module_platform_driver(fg4_driver);

MODULE_DESCRIPTION("Qualcomm PM8150B FG GEN4 profile and telemetry");
MODULE_LICENSE("GPL");
