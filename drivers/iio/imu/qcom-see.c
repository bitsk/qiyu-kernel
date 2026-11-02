// SPDX-License-Identifier: GPL-2.0-only
/* Qualcomm SEE sensor streams over QMI service 400 v1.
 *
 * The sensor bus and interrupt belong to SSC. Linux discovers sensor UIDs
 * through SEE and consumes its standard SI-unit, timestamped events.
 */
#include <linux/device.h>
#include <linux/completion.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/platform_device.h>
#include <linux/qrtr.h>
#include <linux/slab.h>
#include <linux/soc/qcom/qmi.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/sysfs.h>
#include <clocksource/arm_arch_timer.h>

#define SEE_SERVICE 400
#define SEE_REQUEST 0x20
#define SEE_SMALL_REPORT 0x21
#define SEE_LARGE_REPORT 0x22
#define SEE_MAX_REPORT 62016
#define SEE_UID_REQUEST 512
#define SEE_CONFIG_REQUEST 513
#define SEE_ON_CHANGE_REQUEST 514
#define SEE_DISABLE_REQUEST 10
#define SEE_UID_EVENT 768
#define SEE_SAMPLE_EVENT 1025
#define SEE_PROXIMITY_EVENT 769
#define SEE_ATTR_REQUEST 1
#define SEE_FLUSH_REQUEST 2
#define SEE_ATTR_EVENT 128
#define SEE_FLUSH_EVENT 129
#define SEE_ERROR_EVENT 130
#define SEE_MAX_RATES 32

static bool timestamp_debug;
module_param(timestamp_debug, bool, 0600);
MODULE_PARM_DESC(timestamp_debug, "Log bounded SEE/AP counter samples for clock diagnosis");

struct see_uid { u64 low, high; };
static const struct see_uid lookup_uid = {
	0xababababababababULL, 0xababababababababULL,
};

struct see_request {
	u32 len;
	u8 data[128];
	u8 report_valid;
	u8 report;
};

struct see_response {
	struct qmi_response_type_v01 result;
	u8 status_valid;
	u32 status;
};

struct see_report {
	u64 client_id;
	u32 len;
	u8 data[SEE_MAX_REPORT];
};

static const struct qmi_elem_info see_request_ei[] = {
	{ QMI_DATA_LEN, 1, sizeof(u16), NO_ARRAY, 1,
	  offsetof(struct see_request, len) },
	{ QMI_UNSIGNED_1_BYTE, 128, sizeof(u8), VAR_LEN_ARRAY, 1,
	  offsetof(struct see_request, data) },
	{ QMI_OPT_FLAG, 1, sizeof(u8), NO_ARRAY, 0x10,
	  offsetof(struct see_request, report_valid) },
	{ QMI_UNSIGNED_1_BYTE, 1, sizeof(u8), NO_ARRAY, 0x10,
	  offsetof(struct see_request, report) },
	{}
};

static const struct qmi_elem_info see_response_ei[] = {
	{ QMI_STRUCT, 1, sizeof(struct qmi_response_type_v01), NO_ARRAY, 2,
	  offsetof(struct see_response, result), qmi_response_type_v01_ei },
	{ QMI_OPT_FLAG, 1, sizeof(u8), NO_ARRAY, 0x11,
	  offsetof(struct see_response, status_valid) },
	{ QMI_UNSIGNED_4_BYTE, 1, sizeof(u32), NO_ARRAY, 0x11,
	  offsetof(struct see_response, status) },
	{}
};

static const struct qmi_elem_info see_report_ei[] = {
	{ QMI_UNSIGNED_8_BYTE, 1, sizeof(u64), NO_ARRAY, 1,
	  offsetof(struct see_report, client_id) },
	{ QMI_DATA_LEN, 1, sizeof(u16), NO_ARRAY, 2,
	  offsetof(struct see_report, len) },
	{ QMI_UNSIGNED_1_BYTE, SEE_MAX_REPORT, sizeof(u8), VAR_LEN_ARRAY, 2,
	  offsetof(struct see_report, data) },
	{}
};

/* Bounded protobuf cursor. Unknown fields with standard wire types skip. */
struct see_pb {
	const u8 *p;
	size_t len;
};

struct see_field {
	u32 number, wire;
	u64 value;
	struct see_pb bytes;
};

static int see_varint(struct see_pb *pb, u64 *value)
{
	unsigned int shift;
	u8 byte;

	*value = 0;
	for (shift = 0; shift < 64; shift += 7) {
		if (!pb->len)
			return -EBADMSG;
		byte = *pb->p++;
		pb->len--;
		if (shift == 63 && byte > 1)
			return -EBADMSG;
		*value |= (u64)(byte & 0x7f) << shift;
		if (!(byte & 0x80))
			return 0;
	}
	return -EBADMSG;
}

static int see_field(struct see_pb *pb, struct see_field *field)
{
	u64 tag, size;
	int ret;

	if (!pb->len)
		return 0;
	ret = see_varint(pb, &tag);
	if (ret || !(tag >> 3) || tag >> 3 > U32_MAX)
		return -EBADMSG;
	field->number = tag >> 3;
	field->wire = tag & 7;
	field->bytes = (struct see_pb){};
	if (!field->wire)
		return see_varint(pb, &field->value) ? -EBADMSG : 1;
	if (field->wire == 1)
		size = 8;
	else if (field->wire == 5)
		size = 4;
	else if (field->wire == 2) {
		if (see_varint(pb, &size))
			return -EBADMSG;
	} else {
		return -EBADMSG;
	}
	if (size > pb->len)
		return -EBADMSG;
	field->bytes = (struct see_pb){ pb->p, size };
	if (field->wire == 1)
		field->value = get_unaligned_le64(pb->p);
	else if (field->wire == 5)
		field->value = get_unaligned_le32(pb->p);
	pb->p += size;
	pb->len -= size;
	return 1;
}

static int see_parse_uid(struct see_pb pb, struct see_uid *uid)
{
	struct see_field f;
	unsigned int present = 0;
	int ret;

	while ((ret = see_field(&pb, &f)) > 0) {
		if (f.number == 1 && f.wire == 1) {
			uid->low = f.value;
			present |= 1;
		} else if (f.number == 2 && f.wire == 1) {
			uid->high = f.value;
			present |= 2;
		}
	}
	return ret < 0 || present != 3 ? -EBADMSG : 0;
}

struct see_client;
enum see_kind { SEE_ACCEL, SEE_GYRO, SEE_PROXIMITY, SEE_NUM_SENSORS };

struct see_sensor {
	struct see_client *client;
	struct iio_dev *indio;
	const char *type;
	enum see_kind kind;
	struct see_uid uid;
	bool ready, registered, streaming, timestamp_valid;
	bool direct_read;
	s32 direct_values[2];
	u64 last_ticks;
	u32 rate_micro;
	u32 requested_rate, rate_bits;
	int rates[SEE_MAX_RATES * 2];
	u32 rates_bits[SEE_MAX_RATES];
	unsigned int num_rates, fifo_size;
	u32 batch_us;
	u32 accuracy;
	u64 sample_count, flush_count;
	struct completion attributes_done, flush_done, sample_done;
	struct mutex flush_lock;
};

struct see_client {
	struct device *dev;
	struct qmi_handle qmi;
	struct work_struct work;
	struct mutex request_lock;
	spinlock_t uid_lock;
	bool stopping, lookup_sent;
	struct see_sensor sensor[SEE_NUM_SENSORS];
};

static bool see_on_change(const struct see_sensor *s)
{
	return s->kind == SEE_PROXIMITY;
}

static int see_send_batch(struct see_client *c, const struct see_uid *uid,
			  u32 message, const u8 *payload, size_t len, u32 batch_us)
{
	struct see_request req = { .report_valid = 1, .report = 1 };
	struct see_response resp = {};
	struct qmi_txn txn;
	u8 *p = req.data, *size;
	u32 value;
	int ret;

	/* All requests here fit the one-byte protobuf length encoding. */
	if (len > 80)
		return -E2BIG;
	*p++ = 0x0a;
	*p++ = 18;
	*p++ = 0x09;
	put_unaligned_le64(uid->low, p);
	p += 8;
	*p++ = 0x11;
	put_unaligned_le64(uid->high, p);
	p += 8;
	*p++ = 0x15;
	put_unaligned_le32(message, p);
	p += 4;
	/* APSS processor, wakeup delivery. */
	memcpy(p, "\x1a\x04\x08\x01\x10\x00", 6);
	p += 6;
	*p++ = 0x22;
	size = p++;
	/* sns_std_request.batching.batch_period, in microseconds. */
	if (message == SEE_CONFIG_REQUEST || message == SEE_ON_CHANGE_REQUEST) {
		u8 *batch_size;

		*p++ = 0x0a;
		batch_size = p++;
		*p++ = 0x08;
		value = batch_us;
		do {
			*p++ = (value & 0x7f) | (value > 0x7f ? 0x80 : 0);
			value >>= 7;
		} while (value);
		*batch_size = p - batch_size - 1;
	}
	*p++ = 0x12;
	*p++ = len;
	if (len)
		memcpy(p, payload, len);
	p += len;
	*size = p - size - 1;
	req.len = p - req.data;

	mutex_lock(&c->request_lock);
	if (READ_ONCE(c->stopping)) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = qmi_txn_init(&c->qmi, &txn, see_response_ei, &resp);
	if (ret < 0)
		goto unlock;
	ret = qmi_send_request(&c->qmi, NULL, &txn, SEE_REQUEST,
			       140, see_request_ei, &req);
	if (ret) {
		qmi_txn_cancel(&txn);
		goto unlock;
	}
	ret = qmi_txn_wait(&txn, msecs_to_jiffies(2000));
	if (ret >= 0)
		ret = resp.result.result || (resp.status_valid && resp.status) ?
			-EREMOTEIO : 0;
unlock:
	mutex_unlock(&c->request_lock);
	return ret;
}

static int see_send(struct see_client *c, const struct see_uid *uid,
		    u32 message, const u8 *payload, size_t len)
{
	return see_send_batch(c, uid, message, payload, len, 0);
}

/* Convert finite IEEE754 binary32 to fixed point without kernel FP. */
static int see_float_scaled(u32 bits, u32 scale, s32 *out)
{
	u32 exponent = (bits >> 23) & 0xff;
	u64 magnitude = bits & GENMASK(22, 0);
	int shift;

	if (exponent == 255)
		return -ERANGE;
	if (exponent)
		magnitude |= BIT(23);
	magnitude *= scale;
	shift = (exponent ? exponent : 1) - 150;
	if (shift >= 0) {
		if (shift >= 32 || magnitude > ((u64)S32_MAX >> shift))
			return -ERANGE;
		magnitude <<= shift;
	} else {
		magnitude = -shift >= 64 ? 0 : magnitude >> -shift;
	}
	if (magnitude > S32_MAX)
		return -ERANGE;
	*out = bits & BIT(31) ? -(s32)magnitude : (s32)magnitude;
	return 0;
}

static int see_float_micro(u32 bits, s32 *out)
{
	return see_float_scaled(bits, 1000000, out);
}

static void see_attributes(struct see_sensor *s, struct see_pb pb)
{
	struct see_field attr, field, entry, value;
	unsigned int count = 0;
	int id, ret, stream_type = -1;
	s32 rate;

	/* Attribute responses are immutable after initial discovery. */
	if (completion_done(&s->attributes_done))
		return;
	while ((ret = see_field(&pb, &attr)) > 0) {
		struct see_pb values = {};

		if (attr.number != 1 || attr.wire != 2)
			continue;
		id = -1;
		while ((ret = see_field(&attr.bytes, &field)) > 0) {
			if (field.number == 1 && field.wire == 0)
				id = field.value;
			else if (field.number == 2 && field.wire == 2)
				values = field.bytes;
		}
		if (ret < 0)
			return;
		while ((ret = see_field(&values, &entry)) > 0) {
			if (entry.number != 1 || entry.wire != 2)
				continue;
			while ((ret = see_field(&entry.bytes, &value)) > 0) {
				/* Ordinary rates (6), not low-latency-only rates (25). */
				if (id == 6 && value.number == 3 && value.wire == 5) {
					if (see_float_micro(value.value, &rate) || rate <= 0 ||
					    count == SEE_MAX_RATES)
						return;
					s->rates[2 * count] = rate / 1000000;
					s->rates[2 * count + 1] = rate % 1000000;
					s->rates_bits[count++] = value.value;
				} else if (id == 8 && value.number == 4 && value.wire == 1 &&
					   value.value <= U32_MAX) {
					s->fifo_size = value.value;
				} else if (id == 16 && value.number == 4 && value.wire == 1 &&
					   value.value <= 2) {
					stream_type = value.value;
				}
			}
			if (ret < 0)
				return;
		}
		if (ret < 0)
			return;
	}
	if (ret < 0 || stream_type != see_on_change(s) ||
	    (!see_on_change(s) && !count))
		return;
	s->num_rates = count;
	complete(&s->attributes_done);
}

struct see_scan {
	s32 data[3];
	s64 timestamp __aligned(8);
};

static void see_publish(struct see_sensor *s, struct see_scan *scan,
			u32 accuracy, u64 ticks)
{
	u64 before, counter, after;
	s64 timestamp;

	if (s->timestamp_valid && ticks <= s->last_ticks)
		return;
	/* A081 SEE and the AP counter share the 19.2 MHz QTimer epoch.
	 * Cross-timestamp at reception, then subtract the sample's age. Anchoring
	 * the oldest sample to the first packet's arrival shifts batched samples
	 * into the future and gives accel/gyro unrelated stream-start offsets.
	 * On-change initial reports may retain the last transition's timestamp.
	 * This maps SEE time, not the camera exposure start.
	 */
	before = iio_get_time_ns(s->indio);
	counter = arch_timer_read_counter();
	after = iio_get_time_ns(s->indio);
	if (counter < ticks)
		return;
	timestamp = before + (after - before) / 2 -
		mul_u64_u32_div(counter - ticks, 10000, 192);
	s->timestamp_valid = true;
	s->last_ticks = ticks;
	WRITE_ONCE(s->accuracy, accuracy);
	if (READ_ONCE(s->direct_read)) {
		if (!completion_done(&s->sample_done)) {
			s->direct_values[0] = scan->data[0];
			s->direct_values[1] = scan->data[1];
			complete(&s->sample_done);
		}
	} else {
		iio_push_to_buffers_with_ts(s->indio, scan, sizeof(*scan), timestamp);
	}
	s->sample_count++;
	if (READ_ONCE(timestamp_debug) &&
	    (s->sample_count <= 4 || !(s->sample_count % 128))) {
		dev_info(s->client->dev,
			 "%s sample=%llu see=%llu ap=%llu before=%llu after=%llu iio=%lld batch_us=%u accuracy=%u\n",
			 s->type, s->sample_count, ticks, counter, before, after,
			 timestamp, s->batch_us, s->accuracy);
	}
}

static void see_sample(struct see_sensor *s, struct see_pb pb, u64 ticks)
{
	struct see_scan scan = {};
	struct see_field f;
	unsigned int n = 0;
	u32 accuracy = 0;
	int ret;

	if (!READ_ONCE(s->streaming))
		return;
	while ((ret = see_field(&pb, &f)) > 0) {
		if (f.number == 2 && f.wire == 0) {
			if (f.value > 3)
				return;
			accuracy = f.value;
			continue;
		}
		if (f.number != 1)
			continue;
		if (f.wire == 5) {
			if (n == 3 || see_float_micro(f.value, &scan.data[n]))
				return;
			n++;
		} else if (f.wire == 2) {
			if (f.bytes.len % 4)
				return;
			while (f.bytes.len) {
				if (n == 3 ||
				    see_float_micro(get_unaligned_le32(f.bytes.p),
						     &scan.data[n]))
					return;
				n++;
				f.bytes.p += 4;
				f.bytes.len -= 4;
			}
		}
	}
	if (ret < 0 || n != 3)
		return;
	see_publish(s, &scan, accuracy, ticks);
}

static void see_proximity(struct see_sensor *s, struct see_pb pb, u64 ticks)
{
	struct see_scan scan = {};
	struct see_field f;
	u32 accuracy = 0, present = 0;
	int ret;

	if (!READ_ONCE(s->streaming))
		return;
	while ((ret = see_field(&pb, &f)) > 0) {
		if (f.wire != 0)
			continue;
		switch (f.number) {
		case 1: /* sns_proximity_event_type: FAR=0, NEAR=1 */
			if (f.value > 1)
				return;
			scan.data[0] = f.value;
			present |= BIT(0);
			break;
		case 2: /* Raw ADC count, not a distance in centimetres. */
			if (f.value > S32_MAX)
				return;
			scan.data[1] = f.value;
			present |= BIT(1);
			break;
		case 3:
			if (f.value > 3)
				return;
			accuracy = f.value;
			present |= BIT(2);
			break;
		}
	}
	if (ret < 0 || present != GENMASK(2, 0))
		return;
	see_publish(s, &scan, accuracy, ticks);
}

static void see_lookup_event(struct see_client *c, struct see_pb pb)
{
	struct see_field f;
	struct see_pb name = {};
	struct see_uid uid = {};
	bool found = false;
	unsigned long flags;
	int i, ret;

	while ((ret = see_field(&pb, &f)) > 0) {
		if (f.number == 1 && f.wire == 2)
			name = f.bytes;
		else if (f.number == 2 && f.wire == 2 && !found)
			found = !see_parse_uid(f.bytes, &uid);
	}
	if (ret < 0 || !found || !name.p)
		return;
	for (i = 0; i < ARRAY_SIZE(c->sensor); i++) {
		struct see_sensor *s = &c->sensor[i];

		if (name.len != strlen(s->type) || memcmp(name.p, s->type, name.len))
			continue;
		spin_lock_irqsave(&c->uid_lock, flags);
		if (!s->ready) {
			s->uid = uid;
			s->ready = true;
		}
		spin_unlock_irqrestore(&c->uid_lock, flags);
		schedule_work(&c->work);
	}
}

static void see_event(struct see_client *c, const struct see_uid *uid,
		      struct see_pb pb)
{
	struct see_field f;
	struct see_pb payload = {};
	u32 message = 0;
	u64 ticks = 0;
	int ret, i;

	while ((ret = see_field(&pb, &f)) > 0) {
		if (f.number == 1 && f.wire == 5)
			message = f.value;
		else if (f.number == 2 && f.wire == 1)
			ticks = f.value;
		else if (f.number == 3 && f.wire == 2)
			payload = f.bytes;
	}
	if (ret < 0)
		return;
	if (uid->low == lookup_uid.low && uid->high == lookup_uid.high) {
		if (message == SEE_UID_EVENT)
			see_lookup_event(c, payload);
		return;
	}
	for (i = 0; i < ARRAY_SIZE(c->sensor); i++) {
		struct see_sensor *s = &c->sensor[i];
		s32 rate;

		if (!READ_ONCE(s->ready) || uid->low != s->uid.low ||
		    uid->high != s->uid.high)
			continue;
		if (message == SEE_ATTR_EVENT) {
			see_attributes(s, payload);
			continue;
		}
		if (!READ_ONCE(s->registered))
			continue;
		if (message == SEE_PROXIMITY_EVENT && s->kind == SEE_PROXIMITY)
			see_proximity(s, payload, ticks);
		else if (message == SEE_SAMPLE_EVENT && s->kind != SEE_PROXIMITY)
			see_sample(s, payload, ticks);
		else if (message == 768) {
			while (see_field(&payload, &f) > 0)
				if (f.number == 1 && f.wire == 5 &&
				    !see_float_micro(f.value, &rate) && rate > 0)
					WRITE_ONCE(s->rate_micro, rate);
		} else if (message == SEE_FLUSH_EVENT) {
			s->flush_count++;
			complete(&s->flush_done);
		} else if (message == SEE_ERROR_EVENT) {
			dev_warn_ratelimited(c->dev, "%s SEE error event\n", s->type);
		}
	}
}

static void see_receive(struct qmi_handle *qmi, struct sockaddr_qrtr *sq,
			struct qmi_txn *txn, const void *data)
{
	struct see_client *c = container_of(qmi, struct see_client, qmi);
	const struct see_report *report = data;
	struct see_pb pb = { report->data, report->len }, events = pb;
	struct see_field f;
	struct see_uid uid = {};
	bool found = false;
	int ret;

	if (READ_ONCE(c->stopping))
		return;
	while ((ret = see_field(&pb, &f)) > 0)
		if (f.number == 1 && f.wire == 2)
			found = !see_parse_uid(f.bytes, &uid);
	if (ret < 0 || !found)
		return;
	while (see_field(&events, &f) > 0)
		if (f.number == 2 && f.wire == 2)
			see_event(c, &uid, f.bytes);
}

static const struct qmi_msg_handler see_handlers[] = {
	{ QMI_INDICATION, SEE_SMALL_REPORT, see_report_ei, sizeof(struct see_report), see_receive },
	{ QMI_INDICATION, SEE_LARGE_REPORT, see_report_ei, sizeof(struct see_report), see_receive },
	{}
};

/* Called with flush_lock held, before either a buffer or direct read. */
static int see_start(struct see_sensor *s, bool direct)
{
	u8 config[5] = { 0x0d };
	int ret;

	put_unaligned_le32(s->rate_bits, config + 1);
	s->timestamp_valid = false;
	s->sample_count = 0;
	s->accuracy = 0;
	s->rate_micro = 0;
	reinit_completion(&s->sample_done);
	WRITE_ONCE(s->direct_read, direct);
	WRITE_ONCE(s->streaming, true);
	if (see_on_change(s))
		ret = see_send_batch(s->client, &s->uid, SEE_ON_CHANGE_REQUEST,
				     NULL, 0, direct ? 0 : s->batch_us);
	else
		ret = see_send_batch(s->client, &s->uid, SEE_CONFIG_REQUEST,
				     config, sizeof(config), s->batch_us);
	if (ret)
		WRITE_ONCE(s->streaming, false);
	return ret;
}

static int see_read_single(struct see_sensor *s, unsigned int index, int *val)
{
	int ret, stop_ret;

	if (!iio_device_claim_direct(s->indio))
		return -EBUSY;
	mutex_lock(&s->flush_lock);
	ret = see_start(s, true);
	if (!ret) {
		if (!wait_for_completion_timeout(&s->sample_done, msecs_to_jiffies(2000)))
			ret = -ETIMEDOUT;
		else
			*val = s->direct_values[index];
		WRITE_ONCE(s->streaming, false);
		stop_ret = see_send(s->client, &s->uid, SEE_DISABLE_REQUEST, NULL, 0);
		if (!ret)
			ret = stop_ret;
	}
	WRITE_ONCE(s->direct_read, false);
	mutex_unlock(&s->flush_lock);
	iio_device_release_direct(s->indio);
	return ret ?: IIO_VAL_INT;
}

static int see_read_raw(struct iio_dev *indio, const struct iio_chan_spec *ch,
			int *val, int *val2, long mask)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	u32 rate;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!see_on_change(s))
			return -EINVAL;
		return see_read_single(s, ch->scan_index, val);
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 1;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		rate = READ_ONCE(s->requested_rate);
		*val = rate / 1000000;
		*val2 = rate % 1000000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int see_read_avail(struct iio_dev *indio, const struct iio_chan_spec *ch,
			  const int **values, int *type, int *length, long mask)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;
	*values = s->rates;
	*type = IIO_VAL_INT_PLUS_MICRO;
	*length = s->num_rates * 2;
	return IIO_AVAIL_LIST;
}

static int see_write_raw(struct iio_dev *indio, const struct iio_chan_spec *ch,
			 int val, int val2, long mask)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	unsigned int i;
	int ret = -EINVAL;

	if (mask != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;
	if (!iio_device_claim_direct(indio))
		return -EBUSY;
	for (i = 0; i < s->num_rates; i++) {
		if (s->rates[2 * i] != val || s->rates[2 * i + 1] != val2)
			continue;
		s->requested_rate = val * 1000000 + val2;
		s->rate_bits = s->rates_bits[i];
		ret = 0;
		break;
	}
	iio_device_release_direct(indio);
	return ret;
}

enum see_info { SEE_ID, SEE_REPORTED_RATE, SEE_ACCURACY };

static ssize_t see_info_read(struct iio_dev *indio, uintptr_t info,
			     const struct iio_chan_spec *ch, char *buf)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	u32 value;

	switch (info) {
	case SEE_ID:
		return sysfs_emit(buf, "%016llx%016llx\n", s->uid.high, s->uid.low);
	case SEE_REPORTED_RATE:
		value = READ_ONCE(s->rate_micro);
		return sysfs_emit(buf, "%u.%06u\n", value / 1000000, value % 1000000);
	case SEE_ACCURACY:
		return sysfs_emit(buf, "%u\n", READ_ONCE(s->accuracy));
	default:
		return -EINVAL;
	}
}

static const struct iio_chan_spec_ext_info see_ext_info[] = {
	{ .name = "sensor_id", .shared = IIO_SHARED_BY_TYPE,
	  .read = see_info_read, .private = SEE_ID },
	{ .name = "sampling_frequency_reported", .shared = IIO_SHARED_BY_TYPE,
	  .read = see_info_read, .private = SEE_REPORTED_RATE },
	{ .name = "accuracy", .shared = IIO_SHARED_BY_TYPE,
	  .read = see_info_read, .private = SEE_ACCURACY },
	{}
};

static const struct iio_chan_spec_ext_info see_on_change_ext_info[] = {
	{ .name = "sensor_id", .shared = IIO_SHARED_BY_TYPE,
	  .read = see_info_read, .private = SEE_ID },
	{ .name = "accuracy", .shared = IIO_SHARED_BY_TYPE,
	  .read = see_info_read, .private = SEE_ACCURACY },
	{}
};

static const struct iio_info see_iio_info = {
	.read_raw = see_read_raw,
	.write_raw = see_write_raw,
	.read_avail = see_read_avail,
};

static ssize_t see_timeout_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(dev_to_iio_dev(dev));
	u32 us = READ_ONCE(s->batch_us);

	return sysfs_emit(buf, "%u.%06u\n", us / 1000000, us % 1000000);
}

static ssize_t see_timeout_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t len)
{
	struct iio_dev *indio = dev_to_iio_dev(dev);
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	int whole, fraction, ret;
	u64 us;

	ret = iio_str_to_fixpoint(buf, 100000, &whole, &fraction);
	if (ret || whole < 0 || fraction < 0)
		return ret ?: -EINVAL;
	us = (u64)whole * 1000000 + fraction;
	if (us > U32_MAX)
		return -ERANGE;
	if (!iio_device_claim_direct(indio))
		return -EBUSY;
	s->batch_us = us;
	iio_device_release_direct(indio);
	return len;
}

static ssize_t see_flush_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t len)
{
	struct iio_dev *indio = dev_to_iio_dev(dev);
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	bool request;
	int ret;

	ret = kstrtobool(buf, &request);
	if (ret || !request)
		return ret ?: -EINVAL;
	/* Serialize with buffer setup/teardown and other flush requests. */
	mutex_lock(&s->flush_lock);
	if (!READ_ONCE(s->streaming)) {
		ret = -EINVAL;
		goto unlock;
	}
	reinit_completion(&s->flush_done);
	ret = see_send(s->client, &s->uid, SEE_FLUSH_REQUEST, NULL, 0);
	if (!ret && !wait_for_completion_timeout(&s->flush_done, msecs_to_jiffies(2000)))
		ret = -ETIMEDOUT;
unlock:
	mutex_unlock(&s->flush_lock);
	return ret ?: len;
}

static ssize_t see_flush_count_show(struct device *dev, struct device_attribute *attr,
				    char *buf)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit(buf, "%llu\n", READ_ONCE(s->flush_count));
}

static IIO_DEVICE_ATTR(hwfifo_timeout, 0644, see_timeout_show, see_timeout_store, 0);
static IIO_DEVICE_ATTR(flush, 0200, NULL, see_flush_store, 0);
static IIO_DEVICE_ATTR(flush_count, 0444, see_flush_count_show, NULL, 0);

static const struct iio_dev_attr *see_buffer_attrs[] = {
	&iio_dev_attr_hwfifo_timeout,
	&iio_dev_attr_flush,
	&iio_dev_attr_flush_count,
	NULL,
};

static int see_buffer_enable(struct iio_dev *indio)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	int ret;

	mutex_lock(&s->flush_lock);
	ret = see_start(s, false);
	mutex_unlock(&s->flush_lock);
	return ret;
}

static int see_buffer_disable(struct iio_dev *indio)
{
	struct see_sensor *s = *(struct see_sensor **)iio_priv(indio);
	int ret = 0;

	mutex_lock(&s->flush_lock);
	WRITE_ONCE(s->streaming, false);
	if (!READ_ONCE(s->client->stopping))
		ret = see_send(s->client, &s->uid, SEE_DISABLE_REQUEST, NULL, 0);
	mutex_unlock(&s->flush_lock);
	return ret;
}

static const struct iio_buffer_setup_ops see_buffer_ops = {
	.postenable = see_buffer_enable,
	.predisable = see_buffer_disable,
};

#define SEE_AXIS(_type, _mod, _idx) { \
	.type = _type, .modified = 1, .channel2 = IIO_MOD_##_mod, \
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) | BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.info_mask_shared_by_type_available = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.ext_info = see_ext_info, \
	.scan_index = _idx, \
	.scan_type = { .sign = 's', .realbits = 32, .storagebits = 32, .endianness = IIO_CPU }, \
}

static const struct iio_chan_spec see_accel_channels[] = {
	SEE_AXIS(IIO_ACCEL, X, 0), SEE_AXIS(IIO_ACCEL, Y, 1), SEE_AXIS(IIO_ACCEL, Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};
static const struct iio_chan_spec see_gyro_channels[] = {
	SEE_AXIS(IIO_ANGL_VEL, X, 0), SEE_AXIS(IIO_ANGL_VEL, Y, 1), SEE_AXIS(IIO_ANGL_VEL, Z, 2),
	IIO_CHAN_SOFT_TIMESTAMP(3),
};
static const unsigned long see_scan_masks[] = { GENMASK(2, 0), 0 };
static const unsigned long see_scalar_scan_masks[] = { GENMASK(1, 0), 0 };

static const struct iio_chan_spec see_proximity_channels[] = {
	{
		.type = IIO_PROXIMITY, .indexed = 1, .channel = 0,
		.extend_name = "near", .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = see_on_change_ext_info, .scan_index = 0,
		.scan_type = { .sign = 's', .realbits = 32, .storagebits = 32,
			       .endianness = IIO_CPU },
	}, {
		.type = IIO_PROXIMITY, .indexed = 1, .channel = 1,
		.extend_name = "adc", .info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = see_on_change_ext_info, .scan_index = 1,
		.scan_type = { .sign = 's', .realbits = 32, .storagebits = 32,
			       .endianness = IIO_CPU },
	},
	IIO_CHAN_SOFT_TIMESTAMP(2),
};

static const struct {
	const char *type, *name;
	const struct iio_chan_spec *channels;
	unsigned int num_channels;
} see_types[] = {
	[SEE_ACCEL] = { "accel", "qcom-see-accel", see_accel_channels,
			ARRAY_SIZE(see_accel_channels) },
	[SEE_GYRO] = { "gyro", "qcom-see-gyro", see_gyro_channels,
		       ARRAY_SIZE(see_gyro_channels) },
	[SEE_PROXIMITY] = { "proximity", "qcom-see-proximity", see_proximity_channels,
			    ARRAY_SIZE(see_proximity_channels) },

};

static int see_register_sensor(struct see_sensor *s, unsigned int index)
{
	struct device *dev = s->client->dev;
	struct iio_dev *indio;
	int ret;

	indio = devm_iio_device_alloc(dev, sizeof(struct see_sensor *));
	if (!indio)
		return -ENOMEM;
	*(struct see_sensor **)iio_priv(indio) = s;
	indio->name = see_types[index].name;
	indio->info = &see_iio_info;
	indio->channels = see_types[index].channels;
	indio->num_channels = see_types[index].num_channels;
	indio->available_scan_masks = see_on_change(s) ? see_scalar_scan_masks : see_scan_masks;
	ret = devm_iio_kfifo_buffer_setup_ext(dev, indio, &see_buffer_ops, see_buffer_attrs);
	if (ret)
		return ret;
	s->indio = indio;
	ret = iio_device_register(indio);
	if (ret)
		return ret;
	WRITE_ONCE(s->registered, true);
	dev_info(dev, "%s registered through SEE\n", indio->name);
	return 0;
}

static void see_work(struct work_struct *work)
{
	struct see_client *c = container_of(work, struct see_client, work);
	unsigned long flags;
	bool ready;
	int i, ret;

	if (READ_ONCE(c->stopping))
		return;
	if (!c->lookup_sent) {
		c->lookup_sent = true;
		for (i = 0; i < ARRAY_SIZE(c->sensor); i++) {
			const char *name = c->sensor[i].type;
			u8 request[32] = { 0x0a, strlen(name) };
			size_t n = strlen(name);

			memcpy(request + 2, name, n);
			/* Receive late registry discovery; select the default sensor. */
			memcpy(request + 2 + n, "\x10\x01\x18\x01", 4);
			ret = see_send(c, &lookup_uid, SEE_UID_REQUEST, request, n + 6);
			if (ret)
				dev_warn(c->dev, "%s lookup failed: %d\n", name, ret);
		}
	}
	for (i = 0; i < ARRAY_SIZE(c->sensor); i++) {
		spin_lock_irqsave(&c->uid_lock, flags);
		ready = c->sensor[i].ready;
		spin_unlock_irqrestore(&c->uid_lock, flags);
		if (ready && !c->sensor[i].registered) {
			struct see_sensor *s = &c->sensor[i];
			unsigned int j;

			ret = see_send(c, &s->uid, SEE_ATTR_REQUEST, NULL, 0);
			if (ret || !wait_for_completion_timeout(&s->attributes_done,
							       msecs_to_jiffies(2000))) {
				dev_err(c->dev, "%s attributes unavailable: %d\n", s->type, ret);
				continue;
			}
			/* Prefer the existing 100 Hz default when advertised. */
			s->requested_rate = s->rates[0] * 1000000 + s->rates[1];
			s->rate_bits = s->rates_bits[0];
			for (j = 0; j < s->num_rates; j++) {
				if (s->rates[2 * j] == 100 && !s->rates[2 * j + 1]) {
					s->requested_rate = 100000000;
					s->rate_bits = s->rates_bits[j];
					break;
				}
			}
			ret = see_register_sensor(&c->sensor[i], i);
			if (ret)
				dev_err(c->dev, "%s IIO registration failed: %d\n",
					c->sensor[i].type, ret);
		}
	}
}

static int see_probe(struct platform_device *pdev)
{
	struct sockaddr_qrtr *sq = dev_get_platdata(&pdev->dev);
	struct see_client *c;
	int i, ret;

	c = devm_kzalloc(&pdev->dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = &pdev->dev;
	mutex_init(&c->request_lock);
	spin_lock_init(&c->uid_lock);
	INIT_WORK(&c->work, see_work);
	for (i = 0; i < ARRAY_SIZE(c->sensor); i++) {
		c->sensor[i].client = c;
		c->sensor[i].kind = i;
		c->sensor[i].type = see_types[i].type;
		init_completion(&c->sensor[i].attributes_done);
		init_completion(&c->sensor[i].flush_done);
		init_completion(&c->sensor[i].sample_done);
		mutex_init(&c->sensor[i].flush_lock);
	}
	ret = qmi_handle_init(&c->qmi, SEE_MAX_REPORT + 32, NULL, see_handlers);
	if (ret)
		return ret;
	ret = kernel_connect(c->qmi.sock, (struct sockaddr_unsized *)sq, sizeof(*sq), 0);
	if (ret) {
		qmi_handle_release(&c->qmi);
		return ret;
	}
	platform_set_drvdata(pdev, c);
	schedule_work(&c->work);
	return 0;
}

static void see_remove(struct platform_device *pdev)
{
	struct see_client *c = platform_get_drvdata(pdev);
	int i;

	WRITE_ONCE(c->stopping, true);
	cancel_work_sync(&c->work);
	mutex_lock(&c->request_lock);
	qmi_handle_release(&c->qmi);
	mutex_unlock(&c->request_lock);
	for (i = 0; i < ARRAY_SIZE(c->sensor); i++)
		if (c->sensor[i].registered)
			iio_device_unregister(c->sensor[i].indio);
}

static struct platform_driver see_driver = {
	.probe = see_probe,
	.remove = see_remove,
	.driver.name = "qcom-see",
};

static int see_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct sockaddr_qrtr sq = { AF_QIPCRTR, service->node, service->port };
	struct platform_device *pdev;

	pdev = platform_device_register_data(NULL, "qcom-see", PLATFORM_DEVID_AUTO,
					     &sq, sizeof(sq));
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	service->priv = pdev;
	return 0;
}

static void see_del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	platform_device_unregister(service->priv);
}

static const struct qmi_ops see_lookup_ops = {
	.new_server = see_new_server,
	.del_server = see_del_server,
};
static struct qmi_handle see_lookup;

static int __init see_init(void)
{
	int ret;

	ret = platform_driver_register(&see_driver);
	if (ret)
		return ret;
	ret = qmi_handle_init(&see_lookup, 0, &see_lookup_ops, NULL);
	if (ret)
		goto unregister;
	ret = qmi_add_lookup(&see_lookup, SEE_SERVICE, 1, 0);
	if (!ret)
		return 0;
	qmi_handle_release(&see_lookup);
unregister:
	platform_driver_unregister(&see_driver);
	return ret;
}

static void __exit see_exit(void)
{
	qmi_handle_release(&see_lookup);
	platform_driver_unregister(&see_driver);
}
module_init(see_init);
module_exit(see_exit);

MODULE_DESCRIPTION("Qualcomm SEE inertial and proximity IIO client");
MODULE_LICENSE("GPL");
