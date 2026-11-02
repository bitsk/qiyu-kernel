// SPDX-License-Identifier: GPL-2.0-only
/* A081 BID-08 sequence recovered from the matching stock kernel and FDT. */
#include <linux/errno.h>

#include "panel-iqiyi-a081-power.h"

static void keep_error(int *first, int ret)
{
	if (ret && !*first)
		*first = ret;
}

int a081_power_off(struct a081_power *p)
{
	const struct a081_power_ops *ops = p->ops;
	int ret = 0, err, i;

	if (!p->touched && !p->rails)
		return 0;

	/* BID-08 deliberately keeps these four controls high during shutdown. */
	keep_error(&ret, ops->gpio(p->ctx, A081_BOOST, true));
	keep_error(&ret, ops->gpio(p->ctx, A081_RESET, true));
	ops->delay_ms(p->ctx, 30);
	keep_error(&ret, ops->gpio(p->ctx, A081_LED_5V, true));
	keep_error(&ret, ops->gpio(p->ctx, A081_LED1, true));
	keep_error(&ret, ops->gpio(p->ctx, A081_LED2, true));
	keep_error(&ret, ops->pins(p->ctx, false));
	for (i = A081_RAIL_COUNT - 1; i >= 0; i--) {
		if (!(p->rails & (1U << i)))
			continue;
		err = ops->rail(p->ctx, i, false);
		if (!err)
			p->rails &= ~(1U << i);
		keep_error(&ret, err);
	}
	ops->delay_ms(p->ctx, 1);
	keep_error(&ret, ops->gpio(p->ctx, A081_IO, false));
	p->prepared = false;
	if (ret) {
		p->faulted = true;
		if (!p->cleanup_error)
			p->cleanup_error = ret;
	} else {
		p->touched = false;
	}
	return ret;
}

int a081_power_on(struct a081_power *p)
{
	const struct a081_power_ops *ops = p->ops;
	int ret, i;

	if (p->faulted)
		return -EIO;
	if (p->prepared)
		return 0;

	p->touched = true;
	ret = ops->gpio(p->ctx, A081_IO, true);
	if (ret)
		goto fail;
	for (i = 0; i < A081_RAIL_COUNT; i++) {
		ret = ops->rail(p->ctx, i, true);
		if (ret)
			goto fail;
		p->rails |= 1U << i;
		if (i == A081_VDDIO || i == A081_IBB)
			ops->delay_ms(p->ctx, 20);
	}
	ret = ops->pins(p->ctx, true);
	if (ret)
		goto fail;
	/* Preserve the stock order, including the second GPIO140 high write. */
	for (i = A081_BOOST; i < A081_GPIO_COUNT; i++) {
		ret = ops->gpio(p->ctx, i, true);
		if (ret)
			goto fail;
	}
	ret = ops->gpio(p->ctx, A081_RESET, true);
	if (ret)
		goto fail;
	ops->delay_ms(p->ctx, 5);
	ret = ops->gpio(p->ctx, A081_RESET, false);
	if (ret)
		goto fail;
	ops->delay_ms(p->ctx, 25);
	p->prepared = true;
	return 0;

fail:
	/* Do not retry a partially failed electrical transaction automatically. */
	p->faulted = true;
	a081_power_off(p);
	return ret;
}
