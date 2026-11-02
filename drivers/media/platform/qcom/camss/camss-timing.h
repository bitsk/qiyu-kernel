/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QC_MSM_CAMSS_TIMING_H
#define QC_MSM_CAMSS_TIMING_H

#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-v4l2.h>

struct camss_video;
struct camss_buffer;

struct camss_timing {
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;
	spinlock_t pending_lock;
	struct list_head pending;
	struct camss_video *video;
	u64 stream_id;
	u32 dropped;
	bool streaming;
	bool registered;
};

int camss_timing_register(struct camss_video *video);
void camss_timing_unregister(struct camss_video *video);
void camss_timing_begin_stream(struct camss_video *video);
void camss_timing_buffer_done(struct camss_video *video,
			      struct camss_buffer *image);

#endif
