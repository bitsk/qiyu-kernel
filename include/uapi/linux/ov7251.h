/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef __UAPI_OV7251_H__
#define __UAPI_OV7251_H__
#include <linux/v4l2-controls.h>
/* Local allocation pending upstream review; discover by QUERY_EXT_CTRL. */
#define V4L2_CID_OV7251_FRAME_SYNC (V4L2_CID_USER_BASE + 0x1240)
#define OV7251_FRAME_SYNC_FREE_RUNNING 0
#define OV7251_FRAME_SYNC_MASTER 1
#define OV7251_FRAME_SYNC_SLAVE 2
#endif
