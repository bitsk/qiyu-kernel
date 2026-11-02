/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_FG4_CORE_H
#define _QCOM_FG4_CORE_H

#include <linux/mutex.h>
#include <linux/types.h>

struct regmap;

#define FG4_SRAM_SIZE 972

struct qcom_fg4 {
	struct regmap *map;
	/* Serializes DMA acquisition, data access and pending cleanup. */
	struct mutex lock;
	u16 base;
	unsigned int major;
	/* Keep this snapshot until every cleanup transaction has succeeded. */
	bool restore_pending;
	/* A failed clear-log write may have reached the PMIC. */
	bool dma_clear_pending;
	u8 saved_dma, saved_arb, saved_mux;
	int cleanup_error;
};

int qcom_fg4_check_hardware(struct qcom_fg4 *fg);
int qcom_fg4_release(struct qcom_fg4 *fg);
int qcom_fg4_sram_read(struct qcom_fg4 *fg, unsigned int offset,
		       void *data, size_t len);
int qcom_fg4_sram_write(struct qcom_fg4 *fg, unsigned int offset,
			const void *data, size_t len);
int qcom_fg4_battery_id(struct qcom_fg4 *fg, unsigned int *ohms);
int qcom_fg4_present(struct qcom_fg4 *fg, int *value);
int qcom_fg4_voltage(struct qcom_fg4 *fg, int *value);
int qcom_fg4_current(struct qcom_fg4 *fg, int *value);
int qcom_fg4_capacity(struct qcom_fg4 *fg, int *value);
int qcom_fg4_temperature(struct qcom_fg4 *fg, int *millicelsius);

#endif
