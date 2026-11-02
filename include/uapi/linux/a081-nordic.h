/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_A081_NORDIC_H
#define _UAPI_LINUX_A081_NORDIC_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define A081_NORDIC_DUAL_FRAME_SIZE 72

/* Both real controller packets are clocked under one CS assertion.
 * Fixed layout for both32-bit and64-bit clients. Only tx is input.
 */
struct a081_nordic_dual_transfer {
	__u8 tx[A081_NORDIC_DUAL_FRAME_SIZE];
	__u8 rx[A081_NORDIC_DUAL_FRAME_SIZE];
	__aligned_u64 irq_timestamp_ns;
	__aligned_u64 completion_timestamp_ns;
	__aligned_u64 irq_sequence;
};

#define A081_NORDIC_IOC_DUAL_TRANSFER _IOWR('N', 0xa1, struct a081_nordic_dual_transfer)

#endif
