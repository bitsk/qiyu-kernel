/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MEDIA_OV7251_FRAME_GROUP_H
#define _MEDIA_OV7251_FRAME_GROUP_H
#include <linux/types.h>
#include <linux/videodev2.h>
/* Internal subdevice operations; all calls are from sleepable context. */
#define OV7251_FRAME_RELEASE 0
#define OV7251_FRAME_ACQUIRE 1
#define OV7251_FRAME_APPLY 2
struct ov7251_frame_group {
	u32 action;
	u32 exposure_lines;
	u32 analogue_gain;
	u32 frame_length_lines;
	u64 exposure_ns;
};
#define OV7251_SUBDEV_FRAME_GROUP \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 1, struct ov7251_frame_group)
#endif
