/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _PANEL_IQIYI_A081_POWER_H
#define _PANEL_IQIYI_A081_POWER_H

#include <linux/types.h>

enum a081_gpio {
	A081_RESET, A081_BOOST, A081_IO, A081_LED_5V, A081_LED1, A081_LED2,
	A081_GPIO_COUNT,
};

enum a081_rail { A081_VDDIO, A081_LAB, A081_IBB, A081_RAIL_COUNT };

struct a081_power_ops {
	/* GPIO values are logical: RESET=true asserts the active-low reset. */
	int (*gpio)(void *ctx, enum a081_gpio gpio, bool value);
	int (*rail)(void *ctx, enum a081_rail rail, bool enable);
	int (*pins)(void *ctx, bool active);
	void (*delay_ms)(void *ctx, unsigned int ms);
};

/* Caller serializes all operations. Mask tracks only acquired enable votes. */
struct a081_power {
	const struct a081_power_ops *ops;
	void *ctx;
	unsigned int rails;
	bool touched;
	bool prepared;
	bool faulted;
	int cleanup_error;
};

int a081_power_on(struct a081_power *power);
int a081_power_off(struct a081_power *power);

#endif
