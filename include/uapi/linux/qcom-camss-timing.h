/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_QCOM_CAMSS_TIMING_H
#define _UAPI_LINUX_QCOM_CAMSS_TIMING_H

#include <linux/types.h>
#include <linux/videodev2.h>

/* A081 port ABI, not an upstream-assigned format. Native-endian fields. */
#define V4L2_META_FMT_QCOM_CAMSS_TIMING v4l2_fourcc('Q', 'C', 'F', 'T')
#define QCOM_CAMSS_TIMING_VERSION 2
#define QCOM_CAMSS_TIMING_VALID (1U << 0)
#define QCOM_CAMSS_TIMING_HEADER_PRESENT (1U << 1)
#define QCOM_CAMSS_TIMING_HEADER_MATCHED (1U << 2)
#define QCOM_CAMSS_TIMING_PATTERN_ACTIVE (1U << 3)
#define QCOM_CAMSS_TIMING_EXPOSURE_VALID (1U << 4)
/* sample_qtimer and MONOTONIC are derived from one timekeeper counter read. */
#define QCOM_CAMSS_TIMING_CLOCK_SNAPSHOT (1U << 5)

/* CSI packet timestamps, never sensor exposure start/end. Reserved fields zero. */
struct qcom_camss_frame_timing {
	__u32 version;
	__u32 length;
	__u32 flags;
	__u32 image_sequence;
	__u32 image_buffer_index;
	__u32 enqueue_cookie;
	__u32 csid;
	__u32 rdi;
	__aligned_u64 stream_id;
	__aligned_u64 sof_qtimer;
	__aligned_u64 eof_qtimer;
	__aligned_u64 sample_qtimer;
	__aligned_u64 sample_monotonic_ns;
	__aligned_u64 sample_boottime_ns;
	__aligned_u64 sof_monotonic_ns;
	__aligned_u64 eof_monotonic_ns;
	__aligned_u64 sof_boottime_ns;
	__aligned_u64 eof_boottime_ns;
	__u32 qtimer_frequency_hz;
	__u32 clock_bracket_ns;
	__u32 metadata_dropped;
	__s32 lookup_status;
	__u32 header_words[4];
	__u32 reserved[4];
	/* v2: matched sensor controls; valid only with EXPOSURE_VALID. */
	__u32 sensor_sequence;
	__u32 exposure_phase;
	__u32 exposure_lines;
	__u32 analogue_gain;
	__u32 frame_length_lines;
	__s32 exposure_status;
	__aligned_u64 exposure_ns;
	__aligned_u64 control_apply_sof_qtimer;
	__aligned_u64 control_applied_boottime_ns;
	__aligned_u64 phase_anchor_sof_qtimer;
};

#define QCOM_CAMSS_EXPOSURE_PATTERN_VERSION 1
struct qcom_camss_exposure_step {
	__u32 exposure_lines;
	__u32 analogue_gain;
	__u32 frame_length_lines;
	__u32 reserved;
};

/* Arm on a running CSID, using a recently observed QCFT SOF from that CSID.
 * Independently arm all four sensors at the SAME physical frame: userspace
 * must match the four actual hardware SOFs first, not video sequence numbers.
 * phase_at_anchor identifies that real frame. Subsequent phases follow SOF
 * IRQs. All steps must use the same frame length; no timer changes controls.
 * period_qtimer is the measured normal SOF interval at 19.2 MHz and is used
 * only to reject missing/coalesced IRQs and late I2C completion.
 * Disable/rearm after any stream restart, missing IRQ, late write or error.
 */
struct qcom_camss_exposure_pattern {
	__u32 version;
	__u32 enable;
	__aligned_u64 anchor_sof_qtimer;
	__u32 phase_at_anchor;
	__u32 period_qtimer;
	struct qcom_camss_exposure_step steps[3];
	__u32 reserved[4];
};

#define VIDIOC_QCOM_CAMSS_EXPOSURE_PATTERN \
	_IOW('V', BASE_VIDIOC_PRIVATE, struct qcom_camss_exposure_pattern)

#endif
