// SPDX-License-Identifier: GPL-2.0-only
/* Qiyu MIX A081 BID-08 dual DSI display assembly. */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>

#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <video/mipi_display.h>

#include "panel-iqiyi-a081-commands.h"
#include "panel-iqiyi-a081-power.h"

struct a081_panel {
	struct drm_panel panel;
	struct drm_connector *connector;
	struct backlight_device *backlight;
	struct mipi_dsi_device *dsi[2];
	struct drm_dsc_config dsc[2];
	struct gpio_desc *gpios[A081_GPIO_COUNT];
	struct regulator *supplies[A081_RAIL_COUNT];
	struct pinctrl *pinctrl;
	struct pinctrl_state *active, *sleep;
	struct mutex lock;
	struct a081_power power;
	unsigned int prepares, enables, disables, unprepares, packets;
	unsigned int pps_updates;
	unsigned int refresh, brightness, pulse, backlight_updates;
	int last_error;
	bool enabled, pps_match;
};

static inline struct a081_panel *to_a081(struct drm_panel *panel)
{
	return container_of(panel, struct a081_panel, panel);
}

static int a081_gpio_set(void *ctx, enum a081_gpio gpio, bool value)
{
	struct a081_panel *p = ctx;

	return gpiod_direction_output(p->gpios[gpio], value);
}

static int a081_rail_set(void *ctx, enum a081_rail rail, bool enable)
{
	struct a081_panel *p = ctx;
	int ret;

	if (enable) {
		if (rail == A081_VDDIO) {
			ret = regulator_set_load(p->supplies[rail], 62000);
			if (ret)
				return ret;
		}
		ret = regulator_enable(p->supplies[rail]);
		if (ret && rail == A081_VDDIO)
			regulator_set_load(p->supplies[rail], 80);
	} else {
		ret = regulator_disable(p->supplies[rail]);
		/* A successful disable must drop the owned vote even if load fails. */
		if (!ret && rail == A081_VDDIO) {
			int err = regulator_set_load(p->supplies[rail], 80);

			if (err) {
				p->power.faulted = true;
				if (!p->power.cleanup_error)
					p->power.cleanup_error = err;
				dev_err(p->panel.dev, "vddio idle load: %d\n", err);
			}
		}
	}
	return ret;
}

static int a081_pins_set(void *ctx, bool active)
{
	struct a081_panel *p = ctx;

	return pinctrl_select_state(p->pinctrl, active ? p->active : p->sleep);
}

static void a081_delay(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static const struct a081_power_ops a081_power_ops = {
	.gpio = a081_gpio_set,
	.rail = a081_rail_set,
	.pins = a081_pins_set,
	.delay_ms = a081_delay,
};

static int a081_check_pps(struct a081_panel *p)
{
	const struct a081_command *cmd = &a081_on_commands[A081_PPS_COMMAND_INDEX];
	struct drm_dsc_picture_parameter_set packed;
	const u8 *bytes = (const u8 *)&packed;
	int i, j;

	for (i = 0; i < ARRAY_SIZE(p->dsc); i++) {
		drm_dsc_pps_payload_pack(&packed, &p->dsc[i]);
		for (j = 0; j < sizeof(packed); j++) {
			u8 expected = j < cmd->length - 1 ? cmd->payload[j + 1] : 0;

			if (bytes[j] != expected) {
				dev_err(p->panel.dev, "DSI%d PPS byte %d: %02x != stock %02x\n",
					i, j, bytes[j], expected);
				return -EINVAL;
			}
		}
	}
	return 0;
}

static int a081_commands(struct a081_panel *p, const struct a081_command *cmds,
			 unsigned int count, bool lp, bool best_effort)
{
	unsigned int i, link;
	int first = 0;

	for (i = 0; i < count; i++) {
		for (link = 0; link < ARRAY_SIZE(p->dsi); link++) {
			struct mipi_dsi_device *dsi = p->dsi[link];
			struct mipi_dsi_msg msg = {
				.channel = dsi->channel,
				.type = cmds[i].type,
				.flags = lp ? MIPI_DSI_MSG_USE_LPM : 0,
				.tx_buf = cmds[i].payload,
				.tx_len = cmds[i].length,
			};
			ssize_t ret;

			/* Generic helpers choose packet type by length. Several stock
			 * two-byte writes explicitly require a long packet instead.
			 * With MSM synchronized dual DSI, link 0 queues no transfer;
			 * link 1 triggers the same packet on both controllers.
			 */
			ret = dsi->host->ops->transfer(dsi->host, &msg);
			if (ret != msg.tx_len) {
				int err = ret < 0 ? ret : -EIO;

				dev_err(p->panel.dev, "command %u DSI%u type=%02x: %zd\n",
					i, link, msg.type, ret);
				if (!first)
					first = err;
				if (!best_effort)
					return first;
			} else {
				p->packets++;
			}
		}
		if (cmds[i].delay_ms)
			msleep(cmds[i].delay_ms);
	}
	return first;
}

static int a081_send_pps(struct a081_panel *p)
{
	struct drm_dsc_picture_parameter_set pps;
	unsigned int link;
	int ret;

	/* Stock dsi_display_enable sends a standard 128-byte PPS packet after
	 * the vendor on-command list, before enabling the video engine. The E6
	 * register write in that list does not replace this DSI packet.
	 * Link 1 triggers the synchronized transfer to both controllers.
	 */
	for (link = 0; link < ARRAY_SIZE(p->dsi); link++) {
		drm_dsc_pps_payload_pack(&pps, &p->dsc[link]);
		ret = mipi_dsi_picture_parameter_set(p->dsi[link], &pps);
		if (ret)
			return ret;
		p->pps_updates++;
	}
	dev_info(p->panel.dev, "DSC PPS sent after on-commands: links=2 bytes=128 updates=%u\n",
		 p->pps_updates);
	return 0;
}

/* Stock brightness is 0..4095, quantized to integer percent. Its B9
 * emission pulse is ((percent * 7.22 / 100 + 5) * 1000000 / Hz / 226).
 * Keep the calculation integral and the vendor minimum pulse of one.
 * The two emission windows have different offsets at 72 and 90 Hz.
 */
static int a081_set_brightness_locked(struct a081_panel *p, unsigned int level)
{
	struct a081_command cmd = { .type = MIPI_DSI_DCS_LONG_WRITE, .length = 13 };
	unsigned int percent = level * 100 / 4095;
	unsigned int pulse = level <= 16 ? 1 : (percent * 722 + 50000) * 100 /
						(p->refresh * 226);
	int ret;

	lockdep_assert_held(&p->lock);
	cmd.payload[0] = 0xb9;
	put_unaligned_be16(p->refresh == 90 ? 0x03d5 : 0x01a1, cmd.payload + 1);
	put_unaligned_be16(pulse, cmd.payload + 3);
	put_unaligned_be16(p->refresh == 90 ? 0x0fe1 : 0x1279, cmd.payload + 9);
	put_unaligned_be16(pulse, cmd.payload + 11);
	ret = a081_commands(p, &cmd, 1, true, false);
	if (!ret) {
		p->brightness = level;
		p->pulse = pulse;
		p->backlight_updates++;
	}
	dev_info(p->panel.dev, "backlight level=%u refresh=%u pulse=%u ret=%d\n",
		 level, p->refresh, pulse, ret);
	return ret;
}

static int a081_backlight_update(struct backlight_device *backlight)
{
	struct a081_panel *p = bl_get_data(backlight);
	int ret = 0;

	mutex_lock(&p->lock);
	/* Settings made while off are applied by the next panel enable. */
	if (p->enabled)
		ret = a081_set_brightness_locked(p, backlight_get_brightness(backlight));
	mutex_unlock(&p->lock);
	return ret;
}

static int a081_backlight_get(struct backlight_device *backlight)
{
	struct a081_panel *p = bl_get_data(backlight);
	int level;

	mutex_lock(&p->lock);
	level = p->enabled ? p->brightness : 0;
	mutex_unlock(&p->lock);
	return level;
}

static const struct backlight_ops a081_backlight_ops = {
	.update_status = a081_backlight_update,
	.get_brightness = a081_backlight_get,
};

static int a081_prepare(struct drm_panel *panel)
{
	struct a081_panel *p = to_a081(panel);
	int ret;

	mutex_lock(&p->lock);
	ret = p->last_error ? p->last_error : a081_power_on(&p->power);
	if (!ret)
		p->prepares++;
	else
		p->last_error = ret;
	dev_info(panel->dev, "prepare=%d rails=%x fault=%u cleanup=%d\n",
		 ret, p->power.rails, p->power.faulted, p->power.cleanup_error);
	mutex_unlock(&p->lock);
	return ret;
}

static int a081_enable(struct drm_panel *panel)
{
	struct a081_panel *p = to_a081(panel);
	int ret;

	mutex_lock(&p->lock);
	/* Read the committed mode only during the serialized enable callback.
	 * Runtime backlight writes use the cached rate under the same lock.
	 */
	p->refresh = 72;
	if (p->connector && p->connector->state && p->connector->state->crtc)
		p->refresh = drm_mode_vrefresh(&p->connector->state->crtc->state->mode);
	if (p->refresh != 72 && p->refresh != 90) {
		ret = -EINVAL;
		goto out;
	}
	ret = p->power.prepared ? a081_check_pps(p) : -EIO;
	p->pps_match = !ret;
	if (ret)
		goto out;
	ret = a081_commands(p, a081_on_commands, ARRAY_SIZE(a081_on_commands), true, false);
	if (!ret)
		ret = a081_send_pps(p);
	if (!ret)
		ret = a081_set_brightness_locked(p, backlight_get_brightness(p->backlight));
	if (ret) {
		/* Stop a partial initialization while the DSI hosts are still on.
		 * Power removal is deferred until unprepare, after host shutdown.
		 */
		a081_commands(p, a081_off_commands, ARRAY_SIZE(a081_off_commands), false, true);
		goto out;
	}
	p->enabled = true;
	p->enables++;
out:
	if (ret)
		p->last_error = ret;
	dev_info(panel->dev, "enable=%d pps_match=%u packets=%u\n",
		 ret, p->pps_match, p->packets);
	mutex_unlock(&p->lock);
	return ret;
}

static int a081_disable(struct drm_panel *panel)
{
	struct a081_panel *p = to_a081(panel);
	int ret = 0;

	mutex_lock(&p->lock);
	if (p->enabled)
		ret = a081_commands(p, a081_off_commands, ARRAY_SIZE(a081_off_commands), false, true);
	p->enabled = false;
	if (ret)
		p->last_error = ret;
	else
		p->disables++;
	dev_info(panel->dev, "disable=%d\n", ret);
	mutex_unlock(&p->lock);
	return ret;
}

static int a081_unprepare(struct drm_panel *panel)
{
	struct a081_panel *p = to_a081(panel);
	int ret;

	mutex_lock(&p->lock);
	ret = a081_power_off(&p->power);
	if (!ret)
		ret = p->power.cleanup_error;
	if (ret)
		p->last_error = ret;
	else
		p->unprepares++;
	dev_info(panel->dev, "unprepare=%d rails=%x fault=%u cleanup=%d\n",
		 ret, p->power.rails, p->power.faulted, p->power.cleanup_error);
	mutex_unlock(&p->lock);
	return ret;
}

static const struct drm_display_mode a081_modes[] = { {
	.clock = 948437,
	.hdisplay = 1920,
	.hsync_start = 1920 + 112,
	.hsync_end = 1920 + 112 + 24,
	.htotal = 1920 + 112 + 24 + 88,
	.vdisplay = 3664,
	.vsync_start = 3664 + 2459,
	.vsync_end = 3664 + 2459 + 1,
	.vtotal = 3664 + 2459 + 1 + 20,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
}, {
	/* Stock dynamic-FPS changes the vertical front porch, not the PLL. */
	.clock = 948437,
	.hdisplay = 1920,
	.hsync_start = 1920 + 112,
	.hsync_end = 1920 + 112 + 24,
	.htotal = 1920 + 112 + 24 + 88,
	.vdisplay = 3664,
	.vsync_start = 3664 + 1231,
	.vsync_end = 3664 + 1231 + 1,
	.vtotal = 3664 + 1231 + 1 + 20,
	.type = DRM_MODE_TYPE_DRIVER,
} };

static int a081_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct a081_panel *p = to_a081(panel);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(a081_modes); i++) {
		struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &a081_modes[i]);

		if (!mode)
			return -ENOMEM;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	p->connector = connector;
	connector->display_info.bpc = 8;
	return ARRAY_SIZE(a081_modes);
}

static const struct drm_panel_funcs a081_funcs = {
	.prepare = a081_prepare,
	.enable = a081_enable,
	.disable = a081_disable,
	.unprepare = a081_unprepare,
	.get_modes = a081_get_modes,
};

static ssize_t diagnostics_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct a081_panel *p = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = sysfs_emit(buf,
		"prepared=%u enabled=%u rails=0x%x faulted=%u cleanup_error=%d last_error=%d pps_match=%u prepares=%u enables=%u disables=%u unprepares=%u packets=%u pps_updates=%u refresh=%u brightness=%u pulse=%u backlight_updates=%u\n",
		p->power.prepared, p->enabled, p->power.rails, p->power.faulted,
		p->power.cleanup_error, p->last_error, p->pps_match, p->prepares,
		p->enables, p->disables, p->unprepares, p->packets, p->pps_updates,
		p->refresh, p->brightness, p->pulse, p->backlight_updates);
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR_RO(diagnostics);

/* Caller holds the panel lifecycle lock. This does not power up the hosts. */
static int a081_read_status_locked(struct a081_panel *p, ssize_t *result, u8 *value)
{
	struct mipi_dsi_device *dsi = p->dsi[0];
	u8 command = 0x0a;
	struct mipi_dsi_msg msg = {
		.channel = dsi->channel,
		.type = 0x06, /* Stock DCS read, HS; mode_flags also contain LPM. */
		.tx_buf = &command,
		.tx_len = 1,
		.rx_buf = value,
		.rx_len = 1,
	};

	lockdep_assert_held(&p->lock);
	if (!p->enabled || !p->power.prepared)
		return -EHOSTDOWN;
	if (p->power.faulted || p->last_error)
		return -EIO;

	/* The stock A081 status path reads the command master only unless
	 * qcom,cmd-sync-wait-broadcast is set. Its runtime FDT omits that flag.
	 * This assembly uses link 0 for commands; do not require a BTA reply
	 * from the second pixel link. Preserve raw results for diagnosis.
	 */
	*value = 0;
	*result = dsi->host->ops->transfer(dsi->host, &msg);
	return 0;
}

static ssize_t panel_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct a081_panel *p = dev_get_drvdata(dev);
	ssize_t result, ret;
	u8 value;

	mutex_lock(&p->lock);
	ret = a081_read_status_locked(p, &result, &value);
	if (!ret) {
		ret = sysfs_emit(buf,
			"command=0x0a state=hs link=0 ret=%zd value=0x%02x\n", result, value);
		dev_info(dev, "status cycle=%u link=0 ret=%zd value=%02x\n",
			 p->enables, result, value);
	}
	mutex_unlock(&p->lock);
	return ret;
}
static DEVICE_ATTR(panel_status, 0400, panel_status_show, NULL);

static struct attribute *a081_attrs[] = {
	&dev_attr_diagnostics.attr,
	&dev_attr_panel_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(a081);

static int a081_probe(struct mipi_dsi_device *dsi)
{
	static const char * const gpio_names[] = {
		"reset", "boost", "io-enable", "led-5v", "led1", "led2",
	};
	static const char * const supply_names[] = { "vddio", "vsp", "vsn" };
	const struct mipi_dsi_device_info secondary = { "a081-secondary", 0, NULL };
	const struct backlight_properties backlight_props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 4095,
		.brightness = 2048,
		.power = BACKLIGHT_POWER_ON,
	};
	struct device *dev = &dsi->dev;
	struct device_node *remote;
	struct mipi_dsi_host *host;
	struct a081_panel *p;
	int ret, i;

	p = devm_drm_panel_alloc(dev, struct a081_panel, panel, &a081_funcs,
				DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(p))
		return PTR_ERR(p);
	mutex_init(&p->lock);
	p->power.ops = &a081_power_ops;
	p->power.ctx = p;
	mipi_dsi_set_drvdata(dsi, p);
	p->dsi[0] = dsi;

	for (i = 0; i < A081_RAIL_COUNT; i++) {
		p->supplies[i] = devm_regulator_get(dev, supply_names[i]);
		if (IS_ERR(p->supplies[i]))
			return dev_err_probe(dev, PTR_ERR(p->supplies[i]), "supply %s\n", supply_names[i]);
		ret = regulator_set_voltage(p->supplies[i], i ? 5600000 : 1800000,
					    i ? 6000000 : 1800000);
		if (ret)
			return dev_err_probe(dev, ret, "voltage %s\n", supply_names[i]);
	}
	for (i = 0; i < A081_GPIO_COUNT; i++) {
		p->gpios[i] = devm_gpiod_get(dev, gpio_names[i], GPIOD_ASIS);
		if (IS_ERR(p->gpios[i]))
			return dev_err_probe(dev, PTR_ERR(p->gpios[i]), "GPIO %s\n", gpio_names[i]);
	}
	p->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(p->pinctrl))
		return dev_err_probe(dev, PTR_ERR(p->pinctrl), "pinctrl\n");
	p->active = pinctrl_lookup_state(p->pinctrl, "active");
	if (IS_ERR(p->active))
		return dev_err_probe(dev, PTR_ERR(p->active), "active pins\n");
	p->sleep = pinctrl_lookup_state(p->pinctrl, "sleep");
	if (IS_ERR(p->sleep))
		return dev_err_probe(dev, PTR_ERR(p->sleep), "sleep pins\n");

	remote = of_graph_get_remote_node(dev->of_node, 1, -1);
	if (!remote)
		return -ENODEV;
	host = of_find_mipi_dsi_host_by_node(remote);
	of_node_put(remote);
	if (!host)
		return -EPROBE_DEFER;
	p->dsi[1] = devm_mipi_dsi_device_register_full(dev, host, &secondary);
	if (IS_ERR(p->dsi[1]))
		return dev_err_probe(dev, PTR_ERR(p->dsi[1]), "second DSI device\n");

	for (i = 0; i < ARRAY_SIZE(p->dsc); i++) {
		struct drm_dsc_config *dsc = &p->dsc[i];

		*dsc = (struct drm_dsc_config) {
			.dsc_version_major = 1, .dsc_version_minor = 1,
			.bits_per_component = 8, .bits_per_pixel = 8 << 4,
			.block_pred_enable = true, .convert_rgb = true,
			.slice_width = 480, .slice_height = 16, .slice_count = 2,
			.pic_width = 1920, .pic_height = 3664, .line_buf_depth = 9,
		};
		drm_dsc_set_const_params(dsc);
		drm_dsc_set_rc_buf_thresh(dsc);
		ret = drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR);
		if (ret)
			return ret;
		/* Fixed E6 PPS differs from the default pre-SCR table at byte 86. */
		dsc->rc_range_params[14].range_min_qp = 12;
		dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
		ret = drm_dsc_compute_rc_parameters(dsc);
		if (ret)
			return ret;
	}
	ret = a081_check_pps(p);
	if (ret)
		return ret;
	p->pps_match = true;
	p->backlight = devm_backlight_device_register(dev, "a081-backlight", dev, p,
						      &a081_backlight_ops, &backlight_props);
	if (IS_ERR(p->backlight))
		return dev_err_probe(dev, PTR_ERR(p->backlight), "backlight\n");

	/* prepare powers/resets first; enable sends commands after host pre-enable.
	 * disable sends sleep commands; unprepare runs after the hosts stop.
	 */
	drm_panel_add(&p->panel);
	for (i = 0; i < ARRAY_SIZE(p->dsi); i++) {
		p->dsi[i]->lanes = 4;
		p->dsi[i]->format = MIPI_DSI_FMT_RGB888;
		p->dsi[i]->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM |
			MIPI_DSI_MODE_DSC_ALL_SLICES_IN_PKT;
		p->dsi[i]->dsc = &p->dsc[i];
		ret = devm_mipi_dsi_attach(dev, p->dsi[i]);
		if (ret) {
			drm_panel_remove(&p->panel);
			return dev_err_probe(dev, ret, "attach DSI%d\n", i);
		}
	}
	dev_info(dev, "A081 BID-08 dual DSI, stock PPS matched; no power cycle yet\n");
	return 0;
}

static void a081_remove(struct mipi_dsi_device *dsi)
{
	struct a081_panel *p = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&p->panel);
}

static const struct of_device_id a081_of_match[] = {
	{ .compatible = "iqiyi,a081-panel" },
	{ }
};
MODULE_DEVICE_TABLE(of, a081_of_match);

static struct mipi_dsi_driver a081_driver = {
	.probe = a081_probe,
	.remove = a081_remove,
	.driver = {
		.name = "panel-iqiyi-a081",
		.of_match_table = a081_of_match,
		.dev_groups = a081_groups,
	},
};
module_mipi_dsi_driver(a081_driver);

MODULE_DESCRIPTION("Qiyu MIX A081 dual DSI display assembly");
MODULE_LICENSE("GPL");
