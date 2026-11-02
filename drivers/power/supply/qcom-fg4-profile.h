/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _QCOM_FG4_PROFILE_H
#define _QCOM_FG4_PROFILE_H

#include "qcom-fg4-core.h"

#define FG4_PROFILE_SIZE 416

struct qcom_fg4_profile {
	u8 data[FG4_PROFILE_SIZE];
	u32 id_ohms;
	u32 id_tolerance_percent;
	/* Gauge algorithm nominal from the payload, not the battery design rating. */
	u32 nominal_mah;
	u32 full_voltage_uv;
	u16 therm_coeffs[5];
	u8 therm_center;
	u32 therm_pullup_kohms;
};

struct qcom_fg4_profile_state {
	bool valid;
	bool loaded;
	bool dirty;
	bool restart_pending;
	bool side_pending;
	u8 saved_side;
	u32 measured_id_ohms;
	int cleanup_error;
};

/* IRQ completion is provided by the platform driver, never by a stale level. */
struct qcom_fg4_restart_ops {
	void (*prepare)(void *context);
	int (*wait)(void *context, unsigned int milliseconds);
	void *context;
};

int qcom_fg4_profile_validate(const struct qcom_fg4_profile *profile);
int qcom_fg4_profile_abort(struct qcom_fg4 *fg, struct qcom_fg4_profile_state *state);
int qcom_fg4_profile_start(struct qcom_fg4 *fg, const struct qcom_fg4_profile *profile,
			   struct qcom_fg4_profile_state *state,
			   const struct qcom_fg4_restart_ops *restart);

#endif
