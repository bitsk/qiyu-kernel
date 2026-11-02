/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QC_MSM_CAMSS_EXPOSURE_H
#define QC_MSM_CAMSS_EXPOSURE_H
#include <linux/types.h>
struct csid_device;
struct camss_exposure;
struct qcom_camss_frame_timing;
struct qcom_camss_exposure_pattern;
int camss_exposure_init(struct csid_device *csid);
void camss_exposure_start(struct csid_device *csid);
void camss_exposure_stop(struct csid_device *csid);
void camss_exposure_sof(struct csid_device *csid, u64 sof);
void camss_exposure_lookup(struct csid_device *csid, u64 sof,
			   struct qcom_camss_frame_timing *record);
long camss_exposure_configure(struct csid_device *csid,
			     const struct qcom_camss_exposure_pattern *pattern);
#endif
