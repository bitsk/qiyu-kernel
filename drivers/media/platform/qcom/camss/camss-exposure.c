// SPDX-License-Identifier: GPL-2.0
/* A081 OV7251 frame requests: OEM CRM sensor pipeline delay is two frames. */
#include <linux/ktime.h>
#include <linux/gpio/consumer.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/qcom-camss-timing.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <clocksource/arm_arch_timer.h>
#include <media/ov7251.h>
#include "camss.h"
#include "camss-csid.h"
#include "camss-exposure.h"

#define EXPOSURE_HISTORY 64
#define SENSOR_PIPELINE_DELAY 2

struct exposure_applied {
	u64 sequence;
	u64 source_sof;
	u64 completed_boot;
	u64 exposure_ns;
	u32 phase;
	struct qcom_camss_exposure_step step;
	bool valid;
};

struct exposure_frame {
	u64 sof;
	u64 sequence;
	u64 anchor_sof;
	bool active;
	int status;
	struct exposure_applied applied;
};

struct camss_exposure {
	struct csid_device *csid;
	struct gpio_descs *phase_gpios;
	struct v4l2_subdev *sensor;
	struct mutex config_lock;
	spinlock_t lock;
	struct kthread_work work;
	struct kthread_worker *worker;
	struct qcom_camss_exposure_pattern pattern;
	struct exposure_frame frames[EXPOSURE_HISTORY];
	struct exposure_applied applied[EXPOSURE_HISTORY];
	u64 sequence;
	u64 last_sof;
	u64 anchor_sequence;
	u64 source_sequence;
	u64 source_sof;
	u64 epoch;
	bool streaming;
	bool active;
	bool owned;
	bool pending;
	int fault;
	const char *fault_reason;
	u64 fault_sequence;
	u64 fault_qtimer;
	u64 queue_qtimer;
	u64 worker_start_qtimer;
	u64 bus_done_qtimer;
	int worker_ret;
	int worker_cpu;
	bool fault_reported;
};

/* C1/C2 select the hand sampled by the MCU at the next FSIN: 10/01/00. */
static void exposure_controller_phase(struct camss_exposure *s, u32 phase)
{
	unsigned long values = phase < 2 ? BIT(phase) : 0;

	if (s->phase_gpios)
		gpiod_set_array_value(2, s->phase_gpios->desc,
				      s->phase_gpios->info, &values);
}

static int exposure_controller_init(struct camss_exposure *s)
{
	struct device *dev = s->csid->camss->dev;
	u32 reference;
	int ret;

	if (!device_property_present(dev, "qcom,controller-phase-gpios"))
		return 0;
	ret = device_property_read_u32(dev, "qcom,controller-phase-csid", &reference);
	if (ret)
		return ret;
	if (s->csid->id != reference)
		return 0;
	s->phase_gpios = devm_gpiod_get_array(dev, "qcom,controller-phase", GPIOD_OUT_LOW);
	if (IS_ERR(s->phase_gpios))
		return PTR_ERR(s->phase_gpios);
	if (s->phase_gpios->ndescs != 2 ||
	    gpiod_cansleep(s->phase_gpios->desc[0]) ||
	    gpiod_cansleep(s->phase_gpios->desc[1]))
		return dev_err_probe(dev, -EINVAL, "Controller phase needs two non-sleeping GPIOs\n");
	return 0;
}

/* Called with s->lock held. Retain the first cause, not a later consequence. */
static void exposure_fault(struct camss_exposure *s, int error, const char *reason)
{
	if (s->fault)
		return;
	s->fault = error;
	s->fault_reason = reason;
	s->fault_sequence = s->sequence;
	s->fault_qtimer = arch_timer_read_counter();
	exposure_controller_phase(s, 2);
}

/* One diagnostic per arm, after pending work has finished. This keeps console
 * output out of the SOF-to-CCI budget and retains the actual bus return time.
 */
static void exposure_report_fault(struct camss_exposure *s)
{
	unsigned long flags;
	const char *reason;
	u64 source, queued, start, done, sequence, observed, fault_ticks;
	u32 period;
	int error, bus_ret, worker_cpu;

	if (!READ_ONCE(s->fault))
		return;
	spin_lock_irqsave(&s->lock, flags);
	if (!s->fault || s->fault_reported || s->pending) {
		spin_unlock_irqrestore(&s->lock, flags);
		return;
	}
	s->fault_reported = true;
	error = s->fault;
	reason = s->fault_reason;
	source = s->source_sof;
	queued = s->queue_qtimer;
	start = s->worker_start_qtimer;
	done = s->bus_done_qtimer;
	sequence = s->source_sequence;
	observed = s->fault_sequence;
	fault_ticks = s->fault_qtimer;
	period = s->pattern.period_qtimer;
	bus_ret = s->worker_ret;
	worker_cpu = s->worker_cpu;
	spin_unlock_irqrestore(&s->lock, flags);
	dev_warn(s->csid->camss->dev,
		 "exposure fault csid=%u reason=%s ret=%d bus_ret=%d source_sof=%llu source_seq=%llu queue_qtimer=%llu worker_start=%llu worker_cpu=%d bus_done=%llu observed_seq=%llu fault_qtimer=%llu period=%u\n",
		 s->csid->id, reason, error, bus_ret, source, sequence,
		 queued, start, worker_cpu, done, observed, fault_ticks, period);
}

static void exposure_worker(struct kthread_work *work)
{
	struct camss_exposure *s = container_of(work, struct camss_exposure, work);
	struct exposure_applied applied = { 0 };
	struct ov7251_frame_group group = { .action = OV7251_FRAME_APPLY };
	unsigned long flags;
	u64 epoch, source_seq, ticks, worker_start = arch_timer_read_counter();
	u32 period;
	int ret;

	spin_lock_irqsave(&s->lock, flags);
	/* Diagnostic CPU at the first worker lock; unbound work may migrate. */
	s->worker_cpu = raw_smp_processor_id();
	if (!s->streaming || !s->active || s->fault || !s->pending) {
		s->worker_start_qtimer = worker_start;
		s->pending = false;
		spin_unlock_irqrestore(&s->lock, flags);
		exposure_report_fault(s);
		return;
	}
	s->worker_start_qtimer = worker_start;
	epoch = s->epoch;
	source_seq = s->source_sequence;
	applied.sequence = source_seq + SENSOR_PIPELINE_DELAY;
	applied.source_sof = s->source_sof;
	applied.phase = (applied.sequence - s->anchor_sequence +
			 s->pattern.phase_at_anchor) % 3;
	applied.step = s->pattern.steps[applied.phase];
	period = s->pattern.period_qtimer;
	spin_unlock_irqrestore(&s->lock, flags);

	group.exposure_lines = applied.step.exposure_lines;
	group.analogue_gain = applied.step.analogue_gain;
	group.frame_length_lines = applied.step.frame_length_lines;
	/* CCI/I2C may sleep: never run this from the SOF IRQ or a spinlock. */
	ret = v4l2_subdev_call(s->sensor, core, command,
			      OV7251_SUBDEV_FRAME_GROUP, &group);
	ticks = arch_timer_read_counter();
	applied.completed_boot = ktime_get_boottime_ns();
	applied.exposure_ns = group.exposure_ns;

	spin_lock_irqsave(&s->lock, flags);
	if (s->epoch == epoch && s->active) {
		s->bus_done_qtimer = ticks;
		s->worker_ret = ret;
		/* A missed/coalesced SOF or a write crossing the next frame cannot
		 * be assigned to the intended target. Keep later metadata invalid.
		 */
		if (!ret && (s->sequence != source_seq ||
		    ticks < applied.source_sof ||
		    ticks - applied.source_sof >= period))
			ret = -ETIME;
		if (ret)
			exposure_fault(s, ret, s->worker_ret ? "sensor-group" : "worker-deadline");
		if (!s->fault) {
			applied.valid = true;
			s->applied[applied.sequence % EXPOSURE_HISTORY] = applied;
		}
	}
	s->pending = false;
	spin_unlock_irqrestore(&s->lock, flags);
	exposure_report_fault(s);
}

static void exposure_destroy_worker(void *data)
{
	kthread_destroy_worker(data);
}

int camss_exposure_init(struct csid_device *csid)
{
	struct camss_exposure *s;
	int ret;

	s = devm_kzalloc(csid->camss->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->csid = csid;
	ret = exposure_controller_init(s);
	if (ret)
		return ret;
	mutex_init(&s->config_lock);
	spin_lock_init(&s->lock);
	kthread_init_work(&s->work, exposure_worker);
	/* Each request must finish before the next SOF, including wakeup from
	 * CCI completion. HIGHPRI workqueues still use the fair scheduler.
	 * Give only this sensor's bounded, sleeping transactions an independent
	 * FIFO worker; do not change shared workers or pin it to the IRQ CPU.
	 */
	s->worker = kthread_create_worker(0, "camss-exp%u", csid->id);
	if (IS_ERR(s->worker))
		return PTR_ERR(s->worker);
	sched_set_fifo(s->worker->task);
	wake_up_process(s->worker->task);
	ret = devm_add_action_or_reset(csid->camss->dev,
				       exposure_destroy_worker, s->worker);
	if (ret)
		return ret;
	csid->exposure = s;
	return 0;
}

static struct v4l2_subdev *exposure_find_sensor(struct csid_device *csid)
{
	struct media_entity *entity = &csid->subdev.entity;
	struct media_pad *pad;
	unsigned int depth;

	for (depth = 0; depth < 16; depth++) {
		if (entity->function == MEDIA_ENT_F_CAM_SENSOR)
			return media_entity_to_v4l2_subdev(entity);
		if (!entity->num_pads || !(entity->pads[0].flags & MEDIA_PAD_FL_SINK))
			return NULL;
		pad = media_pad_remote_pad_first(&entity->pads[0]);
		if (!pad || !is_media_entity_v4l2_subdev(pad->entity))
			return NULL;
		entity = pad->entity;
	}
	return NULL;
}

/* Caller has disabled SOF events and synchronized its IRQ before stop. */
void camss_exposure_stop(struct csid_device *csid)
{
	struct camss_exposure *s = csid->exposure;
	struct ov7251_frame_group group = { .action = OV7251_FRAME_RELEASE };
	unsigned long flags;

	if (!s)
		return;
	mutex_lock(&s->config_lock);
	spin_lock_irqsave(&s->lock, flags);
	s->active = false;
	s->streaming = false;
	s->epoch++;
	exposure_controller_phase(s, 2);
	spin_unlock_irqrestore(&s->lock, flags);
	/* Drain any IRQ that has selected a work item but not queued it yet. */
	synchronize_irq(csid->irq);
	kthread_cancel_work_sync(&s->work);
	if (s->owned)
		v4l2_subdev_call(s->sensor, core, command, OV7251_SUBDEV_FRAME_GROUP, &group);
	s->owned = false;
	s->sensor = NULL;
	spin_lock_irqsave(&s->lock, flags);
	s->pending = false;
	memset(s->frames, 0, sizeof(s->frames));
	memset(s->applied, 0, sizeof(s->applied));
	spin_unlock_irqrestore(&s->lock, flags);
	mutex_unlock(&s->config_lock);
}

void camss_exposure_start(struct csid_device *csid)
{
	struct camss_exposure *s = csid->exposure;
	unsigned long flags;

	camss_exposure_stop(csid);
	mutex_lock(&s->config_lock);
	s->sensor = exposure_find_sensor(csid);
	spin_lock_irqsave(&s->lock, flags);
	s->streaming = true;
	s->active = false;
	s->pending = false;
	s->sequence = 0;
	s->last_sof = 0;
	s->fault = 0;
	s->fault_reported = false;
	s->fault_reason = NULL;
	s->source_sof = 0;
	s->source_sequence = 0;
	s->queue_qtimer = 0;
	s->worker_cpu = -1;
	s->worker_start_qtimer = 0;
	s->bus_done_qtimer = 0;
	s->worker_ret = 0;
	s->epoch++;
	memset(s->frames, 0, sizeof(s->frames));
	memset(s->applied, 0, sizeof(s->applied));
	spin_unlock_irqrestore(&s->lock, flags);
	mutex_unlock(&s->config_lock);
}

void camss_exposure_sof(struct csid_device *csid, u64 sof)
{
	struct camss_exposure *s = csid->exposure;
	struct exposure_frame *frame;
	struct exposure_applied *applied;
	unsigned long flags;
	bool queue = false;
	u64 delta;

	if (!sof)
		return;
	spin_lock_irqsave(&s->lock, flags);
	if (!s->streaming)
		goto unlock;
	if (s->last_sof) {
		delta = sof - s->last_sof;
		if (s->active && !s->fault && (sof <= s->last_sof ||
		    delta < s->pattern.period_qtimer / 2 ||
		    delta > s->pattern.period_qtimer * 3ULL / 2))
			exposure_fault(s, -EPIPE, "sof-interval");
		s->sequence++;
	}
	s->last_sof = sof;
	if (s->active && s->pending && !s->fault)
		exposure_fault(s, -ETIME, "sof-pending");
	frame = &s->frames[s->sequence % EXPOSURE_HISTORY];
	memset(frame, 0, sizeof(*frame));
	frame->sof = sof;
	frame->sequence = s->sequence;
	frame->active = s->active;
	frame->anchor_sof = s->active ? s->pattern.anchor_sof_qtimer : 0;
	frame->status = s->active ? (s->fault ?: -EAGAIN) : -ENODATA;
	applied = &s->applied[s->sequence % EXPOSURE_HISTORY];
	if (s->active && !s->fault && applied->valid &&
	    applied->sequence == s->sequence) {
		frame->applied = *applied;
		frame->status = 0;
	}
	/* Stock camera2 writes active request N+1 at returned frame N's SOF.
	 * The MCU samples these levels on the following FSIN falling edge.
	 * Use this frame's applied phase, not the worker's two-frame target.
	 */
	exposure_controller_phase(s, frame->status ? 2 :
				  (frame->applied.phase + 1) % 3);
	if (s->active && !s->fault) {
		s->source_sequence = s->sequence;
		s->source_sof = sof;
		/* Timestamp the IRQ's dispatch decision, before queue_work below. */
		s->queue_qtimer = arch_timer_read_counter();
		s->worker_cpu = -1;
		s->worker_start_qtimer = 0;
		s->bus_done_qtimer = 0;
		s->worker_ret = 0;
		s->pending = true;
		queue = true;
	}
unlock:
	spin_unlock_irqrestore(&s->lock, flags);
	if (queue && !kthread_queue_work(s->worker, &s->work)) {
		spin_lock_irqsave(&s->lock, flags);
		if (!s->fault)
			exposure_fault(s, -EBUSY, "queue-busy");
		spin_unlock_irqrestore(&s->lock, flags);
	}
	exposure_report_fault(s);
}

void camss_exposure_lookup(struct csid_device *csid, u64 sof,
			   struct qcom_camss_frame_timing *record)
{
	struct camss_exposure *s = csid->exposure;
	struct exposure_frame *frame;
	unsigned long flags;
	unsigned int i;

	record->exposure_phase = U32_MAX;
	record->exposure_status = -ENODATA;
	spin_lock_irqsave(&s->lock, flags);
	if (s->active)
		record->flags |= QCOM_CAMSS_TIMING_PATTERN_ACTIVE;
	for (i = 0; i < EXPOSURE_HISTORY; i++) {
		frame = &s->frames[i];
		if (!sof || frame->sof != sof)
			continue;
		record->sensor_sequence = frame->sequence;
		record->phase_anchor_sof_qtimer = frame->anchor_sof;
		record->exposure_status = frame->status;
		if (!frame->active)
			break;
		record->flags |= QCOM_CAMSS_TIMING_PATTERN_ACTIVE;
		if (frame->status || !frame->applied.valid ||
		    !(record->flags & QCOM_CAMSS_TIMING_VALID))
			break;
		record->flags |= QCOM_CAMSS_TIMING_EXPOSURE_VALID;
		record->exposure_phase = frame->applied.phase;
		record->exposure_lines = frame->applied.step.exposure_lines;
		record->analogue_gain = frame->applied.step.analogue_gain;
		record->frame_length_lines = frame->applied.step.frame_length_lines;
		record->exposure_ns = frame->applied.exposure_ns;
		record->control_apply_sof_qtimer = frame->applied.source_sof;
		record->control_applied_boottime_ns = frame->applied.completed_boot;
		break;
	}
	spin_unlock_irqrestore(&s->lock, flags);
}

/* Called under the IRQ lock. Use the current stream's actual SOFs, not a
 * caller-supplied interval that could hide a dropped interrupt as one frame.
 */
static int exposure_measured_period(struct camss_exposure *s, u32 requested,
				    u32 *measured)
{
	u32 intervals[8], tmp;
	struct exposure_frame *a, *b;
	u64 seq, delta;
	unsigned int i, j;

	if (s->sequence < ARRAY_SIZE(intervals))
		return -EAGAIN;
	for (i = 0; i < ARRAY_SIZE(intervals); i++) {
		seq = s->sequence - i;
		a = &s->frames[seq % EXPOSURE_HISTORY];
		b = &s->frames[(seq - 1) % EXPOSURE_HISTORY];
		if (!a->sof || !b->sof || a->sequence != seq ||
		    b->sequence != seq - 1 || a->sof <= b->sof)
			return -EAGAIN;
		delta = a->sof - b->sof;
		if (delta > U32_MAX)
			return -ERANGE;
		intervals[i] = delta;
	}
	for (i = 1; i < ARRAY_SIZE(intervals); i++)
		for (j = i; j && intervals[j] < intervals[j - 1]; j--) {
			tmp = intervals[j];
			intervals[j] = intervals[j - 1];
			intervals[j - 1] = tmp;
		}
	*measured = intervals[ARRAY_SIZE(intervals) / 2];
	/* Reject an unstable pre-arm history and an inconsistent userspace
	 * sample. Scheduling uses the kernel median after this check.
	 */
	if (intervals[0] < *measured - *measured / 20 ||
	    intervals[ARRAY_SIZE(intervals) - 1] > *measured + *measured / 20)
		return -EAGAIN;
	if (requested < *measured - *measured / 20 ||
	    requested > *measured + *measured / 20)
		return -ERANGE;
	return 0;
}

long camss_exposure_configure(struct csid_device *csid,
			     const struct qcom_camss_exposure_pattern *pattern)
{
	struct camss_exposure *s = csid->exposure;
	struct ov7251_frame_group group = { .action = OV7251_FRAME_RELEASE };
	unsigned long flags;
	u64 anchor_sequence = 0;
	u32 measured_period = 0;
	bool found = false;
	unsigned int i;
	int ret = 0;

	static_assert(sizeof(*pattern) == 88);
	if (pattern->version != QCOM_CAMSS_EXPOSURE_PATTERN_VERSION ||
	    pattern->enable > 1 || memchr_inv(pattern->reserved, 0, sizeof(pattern->reserved)))
		return -EINVAL;
	if (csid->camss->res->version != CAMSS_8250 ||
	    csid->res->hw_ops != &csid_ops_gen2)
		return -EOPNOTSUPP;
	if (pattern->enable) {
		if (!pattern->anchor_sof_qtimer || pattern->phase_at_anchor > 2 ||
		    pattern->period_qtimer < 80000 || pattern->period_qtimer > 19200000)
			return -EINVAL;
		for (i = 0; i < 3; i++) {
			const struct qcom_camss_exposure_step *step = &pattern->steps[i];

			if (step->reserved || step->frame_length_lines != pattern->steps[0].frame_length_lines ||
			    step->frame_length_lines < 481 || step->frame_length_lines > 65535 ||
			    !step->exposure_lines || step->exposure_lines > step->frame_length_lines - 20 ||
			    step->analogue_gain < 16 || step->analogue_gain > 1023)
				return -ERANGE;
		}
	}
	mutex_lock(&s->config_lock);
	spin_lock_irqsave(&s->lock, flags);
	if (pattern->enable && (!s->streaming || !s->sensor || hweight8(csid->phy.en_vc) != 1)) {
		ret = -EPIPE;
		goto unlock;
	}
	if (pattern->enable) {
		ret = exposure_measured_period(s, pattern->period_qtimer, &measured_period);
		if (ret)
			goto unlock;
		for (i = 0; i < EXPOSURE_HISTORY; i++)
			if (s->frames[i].sof == pattern->anchor_sof_qtimer) {
				anchor_sequence = s->frames[i].sequence;
				found = true;
				break;
			}
		if (!found) {
			ret = -ESTALE;
			goto unlock;
		}
	}
	s->active = false;
	s->epoch++;
	exposure_controller_phase(s, 2);
	spin_unlock_irqrestore(&s->lock, flags);
	/* Drain any IRQ that has selected a work item but not queued it yet. */
	synchronize_irq(csid->irq);
	kthread_cancel_work_sync(&s->work);
	if (s->owned)
		v4l2_subdev_call(s->sensor, core, command, OV7251_SUBDEV_FRAME_GROUP, &group);
	s->owned = false;
	if (pattern->enable) {
		group.action = OV7251_FRAME_ACQUIRE;
		ret = v4l2_subdev_call(s->sensor, core, command, OV7251_SUBDEV_FRAME_GROUP, &group);
		if (ret)
			goto out;
		s->owned = true;
	}
	spin_lock_irqsave(&s->lock, flags);
	s->pattern = *pattern;
	s->pattern.period_qtimer = measured_period;
	s->anchor_sequence = anchor_sequence;
	s->pending = false;
	s->fault = 0;
	s->fault_reported = false;
	s->fault_reason = NULL;
	s->source_sof = 0;
	s->source_sequence = 0;
	s->queue_qtimer = 0;
	s->worker_cpu = -1;
	s->worker_start_qtimer = 0;
	s->bus_done_qtimer = 0;
	s->worker_ret = 0;
	memset(s->applied, 0, sizeof(s->applied));
	for (i = 0; i < EXPOSURE_HISTORY; i++) {
		s->frames[i].active = false;
		s->frames[i].status = -ENODATA;
		s->frames[i].applied.valid = false;
	}
	s->active = pattern->enable;
unlock:
	spin_unlock_irqrestore(&s->lock, flags);
out:
	mutex_unlock(&s->config_lock);
	return ret;
}
