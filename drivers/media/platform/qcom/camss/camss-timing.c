// SPDX-License-Identifier: GPL-2.0
/* SM8250 per-image CSI timing, delivered independently of image storage. */
#include <linux/qcom-camss-timing.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "camss.h"
#include "camss-video.h"

struct camss_timing_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head entry;
};

static int timing_queue_setup(struct vb2_queue *q, unsigned int *buffers,
			      unsigned int *planes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	if (*planes)
		return *planes == 1 && sizes[0] >= sizeof(struct qcom_camss_frame_timing)
			? 0 : -EINVAL;
	*planes = 1;
	sizes[0] = sizeof(struct qcom_camss_frame_timing);
	return 0;
}

static int timing_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < sizeof(struct qcom_camss_frame_timing) ||
	    !vb2_plane_vaddr(vb, 0))
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, sizeof(struct qcom_camss_frame_timing));
	return 0;
}

static void timing_queue(struct vb2_buffer *vb)
{
	struct camss_timing *timing = vb2_get_drv_priv(vb->vb2_queue);
	struct camss_timing_buffer *buf =
		container_of(to_vb2_v4l2_buffer(vb), struct camss_timing_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&timing->pending_lock, flags);
	list_add_tail(&buf->entry, &timing->pending);
	spin_unlock_irqrestore(&timing->pending_lock, flags);
}

static int timing_start(struct vb2_queue *q, unsigned int count)
{
	struct camss_timing *timing = vb2_get_drv_priv(q);
	unsigned long flags;

	spin_lock_irqsave(&timing->pending_lock, flags);
	timing->streaming = true;
	spin_unlock_irqrestore(&timing->pending_lock, flags);
	return 0;
}

static void timing_stop(struct vb2_queue *q)
{
	struct camss_timing *timing = vb2_get_drv_priv(q);
	struct camss_timing_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&timing->pending_lock, flags);
	timing->streaming = false;
	list_for_each_entry_safe(buf, tmp, &timing->pending, entry) {
		list_del(&buf->entry);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&timing->pending_lock, flags);
}

static const struct vb2_ops timing_queue_ops = {
	.queue_setup = timing_queue_setup,
	.buf_prepare = timing_prepare,
	.buf_queue = timing_queue,
	.start_streaming = timing_start,
	.stop_streaming = timing_stop,
};

static int timing_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, "qcom-camss", sizeof(cap->driver));
	return 0;
}

static int timing_enum_fmt(struct file *file, void *priv,
			   struct v4l2_fmtdesc *fmt)
{
	if (fmt->index)
		return -EINVAL;
	fmt->pixelformat = V4L2_META_FMT_QCOM_CAMSS_TIMING;
	strscpy(fmt->description, "CAMSS per-image CSI timing",
		sizeof(fmt->description));
	return 0;
}

static int timing_fmt(struct file *file, void *priv, struct v4l2_format *fmt)
{
	memset(&fmt->fmt.meta, 0, sizeof(fmt->fmt.meta));
	fmt->fmt.meta.dataformat = V4L2_META_FMT_QCOM_CAMSS_TIMING;
	fmt->fmt.meta.buffersize = sizeof(struct qcom_camss_frame_timing);
	return 0;
}

static const struct v4l2_ioctl_ops timing_ioctl_ops = {
	.vidioc_querycap = timing_querycap,
	.vidioc_enum_fmt_meta_cap = timing_enum_fmt,
	.vidioc_g_fmt_meta_cap = timing_fmt,
	.vidioc_s_fmt_meta_cap = timing_fmt,
	.vidioc_try_fmt_meta_cap = timing_fmt,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations timing_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.poll = vb2_fop_poll,
	.mmap = vb2_fop_mmap,
};

static void timing_release(struct video_device *vdev)
{
	struct camss_timing *timing = video_get_drvdata(vdev);
	struct camss *camss = timing->video->camss;

	media_entity_cleanup(&vdev->entity);
	mutex_destroy(&timing->lock);
	if (atomic_dec_and_test(&camss->ref_count))
		camss_delete(camss);
}

int camss_timing_register(struct camss_video *video)
{
	struct camss_timing *timing = &video->timing;
	struct video_device *vdev = &timing->vdev;
	struct vb2_queue *q = &timing->queue;
	struct media_link *link;
	int ret;

	if (!video->timing_supported)
		return 0;

	static_assert(sizeof(struct qcom_camss_frame_timing) == 216);
	timing->video = video;
	mutex_init(&timing->lock);
	spin_lock_init(&timing->pending_lock);
	INIT_LIST_HEAD(&timing->pending);
	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP;
	q->drv_priv = timing;
	q->ops = &timing_queue_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->buf_struct_size = sizeof(struct camss_timing_buffer);
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &timing->lock;
	q->dev = video->camss->dev;
	ret = vb2_queue_init(q);
	if (ret)
		goto destroy_lock;
	ret = media_entity_pads_init(&vdev->entity, 0, NULL);
	if (ret)
		goto destroy_lock;

	vdev->fops = &timing_fops;
	vdev->ioctl_ops = &timing_ioctl_ops;
	vdev->release = timing_release;
	vdev->v4l2_dev = video->vdev.v4l2_dev;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING;
	vdev->queue = q;
	vdev->lock = &timing->lock;
	snprintf(vdev->name, sizeof(vdev->name), "%s_timing", video->vdev.name);
	video_set_drvdata(vdev, timing);
	atomic_inc(&video->camss->ref_count);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		atomic_dec(&video->camss->ref_count);
		media_entity_cleanup(&vdev->entity);
		goto destroy_lock;
	}
	timing->registered = true;
	link = media_create_ancillary_link(&video->vdev.entity, &vdev->entity);
	if (IS_ERR(link)) {
		ret = PTR_ERR(link);
		camss_timing_unregister(video);
		return ret;
	}
	return 0;

destroy_lock:
	mutex_destroy(&timing->lock);
	return ret;
}

void camss_timing_unregister(struct camss_video *video)
{
	if (!video->timing.registered)
		return;
	video->timing.registered = false;
	vb2_video_unregister_device(&video->timing.vdev);
}

void camss_timing_begin_stream(struct camss_video *video)
{
	struct camss_timing *timing = &video->timing;
	unsigned long flags;

	if (!timing->registered)
		return;
	spin_lock_irqsave(&timing->pending_lock, flags);
	if (!++timing->stream_id)
		timing->stream_id++;
	timing->dropped = 0;
	spin_unlock_irqrestore(&timing->pending_lock, flags);
}

void camss_timing_buffer_done(struct camss_video *video,
			      struct camss_buffer *image)
{
	struct camss_timing *timing = &video->timing;
	struct qcom_camss_frame_timing *record = &image->timing;
	struct camss_timing_buffer *buf;
	unsigned long flags;

	if (!timing->registered)
		return;
	spin_lock_irqsave(&timing->pending_lock, flags);
	if (!timing->streaming)
		goto unlock;
	if (list_empty(&timing->pending)) {
		timing->dropped++;
		goto unlock;
	}
	buf = list_first_entry(&timing->pending, struct camss_timing_buffer, entry);
	list_del(&buf->entry);
	record->stream_id = timing->stream_id;
	record->metadata_dropped = timing->dropped;
	record->image_sequence = image->vb.sequence;
	record->image_buffer_index = image->vb.vb2_buf.index;
	record->enqueue_cookie = image->frame_header_cookie;
	memcpy(vb2_plane_vaddr(&buf->vb.vb2_buf, 0), record, sizeof(*record));
	buf->vb.sequence = image->vb.sequence;
	buf->vb.vb2_buf.timestamp = image->vb.vb2_buf.timestamp;
	vb2_buffer_done(&buf->vb.vb2_buf,
		       record->flags & QCOM_CAMSS_TIMING_VALID ?
		       VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR);
unlock:
	spin_unlock_irqrestore(&timing->pending_lock, flags);
}
