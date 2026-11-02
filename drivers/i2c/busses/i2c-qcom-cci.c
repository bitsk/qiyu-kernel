// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2016, The Linux Foundation. All rights reserved.
// Copyright (c) 2017-2022 Linaro Limited.

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>

#define CCI_HW_VERSION				0x0
#define CCI_RESET_CMD				0x004
#define CCI_RESET_CMD_MASK			0x0f73f3f7
#define CCI_RESET_CMD_M0_MASK			0x000003f1
#define CCI_RESET_CMD_M1_MASK			0x0003f001
#define CCI_QUEUE_START				0x008
#define CCI_HALT_REQ				0x034
#define CCI_HALT_REQ_I2C_M0_Q0Q1		BIT(0)
#define CCI_HALT_REQ_I2C_M1_Q0Q1		BIT(1)

#define CCI_I2C_Mm_SCL_CTL(m)			(0x100 + 0x100 * (m))
#define CCI_I2C_Mm_SDA_CTL_0(m)			(0x104 + 0x100 * (m))
#define CCI_I2C_Mm_SDA_CTL_1(m)			(0x108 + 0x100 * (m))
#define CCI_I2C_Mm_SDA_CTL_2(m)			(0x10c + 0x100 * (m))
#define CCI_I2C_Mm_MISC_CTL(m)			(0x110 + 0x100 * (m))

#define CCI_I2C_Mm_READ_DATA(m)			(0x118 + 0x100 * (m))
#define CCI_I2C_Mm_READ_BUF_LEVEL(m)		(0x11c + 0x100 * (m))
#define CCI_I2C_Mm_Qn_EXEC_WORD_CNT(m, n)	(0x300 + 0x200 * (m) + 0x100 * (n))
#define CCI_I2C_Mm_Qn_CUR_WORD_CNT(m, n)	(0x304 + 0x200 * (m) + 0x100 * (n))
#define CCI_I2C_Mm_Qn_CUR_CMD(m, n)		(0x308 + 0x200 * (m) + 0x100 * (n))
#define CCI_I2C_Mm_Qn_REPORT_STATUS(m, n)	(0x30c + 0x200 * (m) + 0x100 * (n))
#define CCI_I2C_Mm_Qn_LOAD_DATA(m, n)		(0x310 + 0x200 * (m) + 0x100 * (n))

#define CCI_IRQ_GLOBAL_CLEAR_CMD		0xc00
#define CCI_IRQ_MASK_0				0xc04
#define CCI_IRQ_MASK_0_I2C_M0_RD_DONE		BIT(0)
#define CCI_IRQ_MASK_0_I2C_M0_Q0_REPORT		BIT(4)
#define CCI_IRQ_MASK_0_I2C_M0_Q1_REPORT		BIT(8)
#define CCI_IRQ_MASK_0_I2C_M1_RD_DONE		BIT(12)
#define CCI_IRQ_MASK_0_I2C_M1_Q0_REPORT		BIT(16)
#define CCI_IRQ_MASK_0_I2C_M1_Q1_REPORT		BIT(20)
#define CCI_IRQ_MASK_0_RST_DONE_ACK		BIT(24)
#define CCI_IRQ_MASK_0_I2C_M0_Q0Q1_HALT_ACK	BIT(25)
#define CCI_IRQ_MASK_0_I2C_M1_Q0Q1_HALT_ACK	BIT(26)
#define CCI_IRQ_MASK_0_I2C_M0_ERROR		0x18000ee6
#define CCI_IRQ_MASK_0_I2C_M1_ERROR		0x60ee6000
#define CCI_IRQ_CLEAR_0				0xc08
#define CCI_IRQ_STATUS_0			0xc0c
#define CCI_IRQ_STATUS_0_I2C_M0_RD_DONE		BIT(0)
#define CCI_IRQ_STATUS_0_I2C_M0_Q0_REPORT	BIT(4)
#define CCI_IRQ_STATUS_0_I2C_M0_Q1_REPORT	BIT(8)
#define CCI_IRQ_STATUS_0_I2C_M1_RD_DONE		BIT(12)
#define CCI_IRQ_STATUS_0_I2C_M1_Q0_REPORT	BIT(16)
#define CCI_IRQ_STATUS_0_I2C_M1_Q1_REPORT	BIT(20)
#define CCI_IRQ_STATUS_0_RST_DONE_ACK		BIT(24)
#define CCI_IRQ_STATUS_0_I2C_M0_Q0Q1_HALT_ACK	BIT(25)
#define CCI_IRQ_STATUS_0_I2C_M1_Q0Q1_HALT_ACK	BIT(26)
#define CCI_IRQ_STATUS_0_I2C_M0_Q0_NACK_ERR	BIT(27)
#define CCI_IRQ_STATUS_0_I2C_M0_Q1_NACK_ERR	BIT(28)
#define CCI_IRQ_STATUS_0_I2C_M1_Q0_NACK_ERR	BIT(29)
#define CCI_IRQ_STATUS_0_I2C_M1_Q1_NACK_ERR	BIT(30)
#define CCI_IRQ_STATUS_0_I2C_M0_ERROR		0x18000ee6
#define CCI_IRQ_STATUS_0_I2C_M1_ERROR		0x60ee6000

#define CCI_TIMEOUT	(msecs_to_jiffies(100))
#define NUM_MASTERS	2
#define NUM_QUEUES	2

#define CCI_I2C_SET_PARAM	1
#define CCI_I2C_REPORT		8
#define CCI_I2C_WRITE		9
#define CCI_I2C_READ		10

#define CCI_I2C_REPORT_IRQ_EN	BIT(8)

enum {
	I2C_MODE_STANDARD,
	I2C_MODE_FAST,
	I2C_MODE_FAST_PLUS,
};

enum cci_i2c_queue_t {
	QUEUE_0,
	QUEUE_1
};

struct hw_params {
	u16 thigh; /* HIGH period of the SCL clock in clock ticks */
	u16 tlow; /* LOW period of the SCL clock */
	u16 tsu_sto; /* set-up time for STOP condition */
	u16 tsu_sta; /* set-up time for a repeated START condition */
	u16 thd_dat; /* data hold time */
	u16 thd_sta; /* hold time (repeated) START condition */
	u16 tbuf; /* bus free time between a STOP and START condition */
	u8 scl_stretch_en;
	u16 trdhld;
	u16 tsp; /* pulse width of spikes suppressed by the input filter */
};

struct cci;

#define CCI_TIMING_RECORDS 16
#define CCI_SLOW_QUEUE_NS (4 * NSEC_PER_MSEC)

struct cci_queue_timing {
	u64 sequence;
	u64 start_ns;
	u64 irq_ns;
	u64 return_ns;
	u32 irq_status;
	u32 words;
	u32 queue;
	int result;
};

struct cci_master {
	struct i2c_adapter adap;
	u16 master;
	u8 mode;
	int status;
	struct completion irq_complete;
	struct cci *cci;
	u64 timing_sequence;
	u64 irq_ns;
	u32 irq_status;
	spinlock_t timing_lock;
	struct cci_queue_timing timing[CCI_TIMING_RECORDS];
	struct cci_queue_timing worst_timing;
	u64 timing_count;
};

struct cci_data {
	unsigned int num_masters;
	struct i2c_adapter_quirks quirks;
	u16 queue_size[NUM_QUEUES];
	struct hw_params params[3];
};

struct cci {
	struct device *dev;
	void __iomem *base;
	unsigned int irq;
	const struct cci_data *data;
	struct clk_bulk_data *clocks;
	int nclocks;
	struct cci_master master[NUM_MASTERS];
};

static void cci_timing_print(struct seq_file *s, unsigned int master,
			     const char *kind, const struct cci_queue_timing *t)
{
	seq_printf(s, "master=%u kind=%s sequence=%llu queue=%u words=%u start_ns=%llu irq_ns=%llu return_ns=%llu irq_status=%#x result=%d\n",
		   master, kind, t->sequence, t->queue, t->words, t->start_ns,
		   t->irq_ns, t->return_ns, t->irq_status, t->result);
}

static int cci_timing_show(struct seq_file *s, void *unused)
{
	struct cci *cci = s->private;
	struct cci_queue_timing records[CCI_TIMING_RECORDS], worst;
	unsigned long flags;
	u64 count, first, n;
	unsigned int i;

	seq_printf(s, "clock=MONOTONIC slow_queue_ns=%llu records_per_master=%u\n",
		   (u64)CCI_SLOW_QUEUE_NS, CCI_TIMING_RECORDS);
	for (i = 0; i < cci->data->num_masters; i++) {
		struct cci_master *m = &cci->master[i];

		if (!m->cci)
			continue;
		spin_lock_irqsave(&m->timing_lock, flags);
		count = m->timing_count;
		worst = m->worst_timing;
		memcpy(records, m->timing, sizeof(records));
		spin_unlock_irqrestore(&m->timing_lock, flags);
		seq_printf(s, "master=%u slow_or_error_count=%llu adapter=%d\n",
			   i, count, m->adap.nr);
		first = count > CCI_TIMING_RECORDS ? count - CCI_TIMING_RECORDS : 0;
		for (n = first; n < count; n++)
			cci_timing_print(s, i, "recent", &records[n % CCI_TIMING_RECORDS]);
		if (worst.sequence)
			cci_timing_print(s, i, "worst", &worst);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cci_timing);

static void cci_timing_remove(void *directory)
{
	debugfs_remove_recursive(directory);
}

static void cci_complete(struct cci_master *master, u32 status, u64 now)
{
	WRITE_ONCE(master->irq_ns, now);
	WRITE_ONCE(master->irq_status, READ_ONCE(master->irq_status) | status);
	complete(&master->irq_complete);
}

static irqreturn_t cci_isr(int irq, void *dev)
{
	struct cci *cci = dev;
	u64 irq_ns;
	u32 val, reset = 0;
	int ret = IRQ_NONE;

	val = readl(cci->base + CCI_IRQ_STATUS_0);
	irq_ns = ktime_get_ns();
	writel(val, cci->base + CCI_IRQ_CLEAR_0);
	writel(0x1, cci->base + CCI_IRQ_GLOBAL_CLEAR_CMD);

	if (val & CCI_IRQ_STATUS_0_RST_DONE_ACK) {
		cci_complete(&cci->master[0], val, irq_ns);
		if (cci->master[1].master)
			cci_complete(&cci->master[1], val, irq_ns);
		ret = IRQ_HANDLED;
	}

	/* An error completes only after the halt/reset path below. */
	if (!(val & CCI_IRQ_STATUS_0_I2C_M0_ERROR) &&
	    (val & CCI_IRQ_STATUS_0_I2C_M0_RD_DONE ||
	     val & CCI_IRQ_STATUS_0_I2C_M0_Q0_REPORT ||
	     val & CCI_IRQ_STATUS_0_I2C_M0_Q1_REPORT)) {
		cci->master[0].status = 0;
		cci_complete(&cci->master[0], val, irq_ns);
		ret = IRQ_HANDLED;
	}

	if (!(val & CCI_IRQ_STATUS_0_I2C_M1_ERROR) &&
	    (val & CCI_IRQ_STATUS_0_I2C_M1_RD_DONE ||
	     val & CCI_IRQ_STATUS_0_I2C_M1_Q0_REPORT ||
	     val & CCI_IRQ_STATUS_0_I2C_M1_Q1_REPORT)) {
		cci->master[1].status = 0;
		cci_complete(&cci->master[1], val, irq_ns);
		ret = IRQ_HANDLED;
	}

	if (unlikely(val & CCI_IRQ_STATUS_0_I2C_M0_Q0Q1_HALT_ACK)) {
		reset |= CCI_RESET_CMD_M0_MASK;
		ret = IRQ_HANDLED;
	}

	if (unlikely(val & CCI_IRQ_STATUS_0_I2C_M1_Q0Q1_HALT_ACK)) {
		reset |= CCI_RESET_CMD_M1_MASK;
		ret = IRQ_HANDLED;
	}

	if (unlikely(reset))
		writel(reset, cci->base + CCI_RESET_CMD);

	if (unlikely(val & CCI_IRQ_STATUS_0_I2C_M0_ERROR)) {
		WRITE_ONCE(cci->master[0].irq_status,
			   READ_ONCE(cci->master[0].irq_status) | val);
		if (val & CCI_IRQ_STATUS_0_I2C_M0_Q0_NACK_ERR ||
			val & CCI_IRQ_STATUS_0_I2C_M0_Q1_NACK_ERR)
			cci->master[0].status = -ENXIO;
		else
			cci->master[0].status = -EIO;

		writel(CCI_HALT_REQ_I2C_M0_Q0Q1, cci->base + CCI_HALT_REQ);
		ret = IRQ_HANDLED;
	}

	if (unlikely(val & CCI_IRQ_STATUS_0_I2C_M1_ERROR)) {
		WRITE_ONCE(cci->master[1].irq_status,
			   READ_ONCE(cci->master[1].irq_status) | val);
		if (val & CCI_IRQ_STATUS_0_I2C_M1_Q0_NACK_ERR ||
			val & CCI_IRQ_STATUS_0_I2C_M1_Q1_NACK_ERR)
			cci->master[1].status = -ENXIO;
		else
			cci->master[1].status = -EIO;

		writel(CCI_HALT_REQ_I2C_M1_Q0Q1, cci->base + CCI_HALT_REQ);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static int cci_halt(struct cci *cci, u8 master_num)
{
	struct cci_master *master;
	u32 val;

	if (master_num >= cci->data->num_masters) {
		dev_err(cci->dev, "Unsupported master idx (%u)\n", master_num);
		return -EINVAL;
	}

	val = BIT(master_num);
	master = &cci->master[master_num];

	reinit_completion(&master->irq_complete);
	writel(val, cci->base + CCI_HALT_REQ);

	if (!wait_for_completion_timeout(&master->irq_complete, CCI_TIMEOUT)) {
		dev_err(cci->dev, "CCI halt timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void cci_init(struct cci *cci)
{
	u32 val = CCI_IRQ_MASK_0_I2C_M0_RD_DONE |
			CCI_IRQ_MASK_0_I2C_M0_Q0_REPORT |
			CCI_IRQ_MASK_0_I2C_M0_Q1_REPORT |
			CCI_IRQ_MASK_0_I2C_M1_RD_DONE |
			CCI_IRQ_MASK_0_I2C_M1_Q0_REPORT |
			CCI_IRQ_MASK_0_I2C_M1_Q1_REPORT |
			CCI_IRQ_MASK_0_RST_DONE_ACK |
			CCI_IRQ_MASK_0_I2C_M0_Q0Q1_HALT_ACK |
			CCI_IRQ_MASK_0_I2C_M1_Q0Q1_HALT_ACK |
			CCI_IRQ_MASK_0_I2C_M0_ERROR |
			CCI_IRQ_MASK_0_I2C_M1_ERROR;
	int i;

	writel(val, cci->base + CCI_IRQ_MASK_0);

	for (i = 0; i < cci->data->num_masters; i++) {
		int mode = cci->master[i].mode;
		const struct hw_params *hw;

		if (!cci->master[i].cci)
			continue;

		hw = &cci->data->params[mode];

		val = hw->thigh << 16 | hw->tlow;
		writel(val, cci->base + CCI_I2C_Mm_SCL_CTL(i));

		val = hw->tsu_sto << 16 | hw->tsu_sta;
		writel(val, cci->base + CCI_I2C_Mm_SDA_CTL_0(i));

		val = hw->thd_dat << 16 | hw->thd_sta;
		writel(val, cci->base + CCI_I2C_Mm_SDA_CTL_1(i));

		val = hw->tbuf;
		writel(val, cci->base + CCI_I2C_Mm_SDA_CTL_2(i));

		val = hw->scl_stretch_en << 8 | hw->trdhld << 4 | hw->tsp;
		writel(val, cci->base + CCI_I2C_Mm_MISC_CTL(i));
	}
}

static int cci_reset(struct cci *cci)
{
	/*
	 * we reset the whole controller, here and for implicity use
	 * master[0].xxx for waiting on it.
	 */
	reinit_completion(&cci->master[0].irq_complete);
	writel(CCI_RESET_CMD_MASK, cci->base + CCI_RESET_CMD);

	if (!wait_for_completion_timeout(&cci->master[0].irq_complete,
					 CCI_TIMEOUT)) {
		dev_err(cci->dev, "CCI reset timeout\n");
		return -ETIMEDOUT;
	}

	cci_init(cci);

	return 0;
}

static int cci_run_queue(struct cci *cci, u8 master, u8 queue)
{
	struct cci_master *m = &cci->master[master];
	struct cci_queue_timing sample = { .queue = queue };
	unsigned long flags, completed;
	u32 val;

	val = readl(cci->base + CCI_I2C_Mm_Qn_CUR_WORD_CNT(master, queue));
	sample.words = val;
	writel(val, cci->base + CCI_I2C_Mm_Qn_EXEC_WORD_CNT(master, queue));

	reinit_completion(&cci->master[master].irq_complete);
	WRITE_ONCE(m->irq_ns, 0);
	WRITE_ONCE(m->irq_status, 0);
	sample.sequence = ++m->timing_sequence;
	val = BIT(master * 2 + queue);
	sample.start_ns = ktime_get_ns();
	writel(val, cci->base + CCI_QUEUE_START);

	completed = wait_for_completion_timeout(&m->irq_complete, CCI_TIMEOUT);
	sample.return_ns = ktime_get_ns();
	sample.irq_ns = READ_ONCE(m->irq_ns);
	sample.irq_status = READ_ONCE(m->irq_status);
	sample.result = completed ? m->status : -ETIMEDOUT;
	/* Retain bounded diagnostics only; printk would consume frame budget. */
	if (sample.result || sample.return_ns - sample.start_ns >= CCI_SLOW_QUEUE_NS) {
		spin_lock_irqsave(&m->timing_lock, flags);
		m->timing[m->timing_count++ % CCI_TIMING_RECORDS] = sample;
		if (sample.return_ns - sample.start_ns >
		    m->worst_timing.return_ns - m->worst_timing.start_ns)
			m->worst_timing = sample;
		spin_unlock_irqrestore(&m->timing_lock, flags);
	}
	if (!completed) {
		dev_err(cci->dev, "master %d queue %d timeout\n",
			master, queue);
		cci_reset(cci);
		return -ETIMEDOUT;
	}

	return sample.result;
}

static int cci_validate_queue(struct cci *cci, u8 master, u8 queue)
{
	u32 val;

	val = readl(cci->base + CCI_I2C_Mm_Qn_CUR_WORD_CNT(master, queue));
	if (val == cci->data->queue_size[queue])
		return -EINVAL;

	if (!val)
		return 0;

	val = CCI_I2C_REPORT | CCI_I2C_REPORT_IRQ_EN;
	writel(val, cci->base + CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));

	return cci_run_queue(cci, master, queue);
}

static int cci_i2c_read(struct cci *cci, u16 master,
			u16 addr, u8 *buf, u16 len)
{
	u32 val, words_read, words_exp;
	u8 queue = QUEUE_1;
	int i, index = 0, ret;
	bool first = true;

	/*
	 * Call validate queue to make sure queue is empty before starting.
	 * This is to avoid overflow / underflow of queue.
	 */
	ret = cci_validate_queue(cci, master, queue);
	if (ret < 0)
		return ret;

	val = CCI_I2C_SET_PARAM | (addr & 0x7f) << 4;
	writel(val, cci->base + CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));

	val = CCI_I2C_READ | len << 4;
	writel(val, cci->base + CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));

	ret = cci_run_queue(cci, master, queue);
	if (ret < 0)
		return ret;

	words_read = readl(cci->base + CCI_I2C_Mm_READ_BUF_LEVEL(master));
	words_exp = len / 4 + 1;
	if (words_read != words_exp) {
		dev_err(cci->dev, "words read = %d, words expected = %d\n",
			words_read, words_exp);
		return -EIO;
	}

	do {
		val = readl(cci->base + CCI_I2C_Mm_READ_DATA(master));

		for (i = 0; i < 4 && index < len; i++) {
			if (first) {
				/* The LS byte of this register represents the
				 * first byte read from the slave during a read
				 * access.
				 */
				first = false;
				continue;
			}
			buf[index++] = (val >> (i * 8)) & 0xff;
		}
	} while (--words_read);

	return 0;
}

static void cci_i2c_queue_write(struct cci *cci, u16 master,
				const struct i2c_msg *msg)
{
	u8 queue = QUEUE_0;
	u8 load[12] = { 0 };
	int i = 0, j;
	u32 val;

	val = CCI_I2C_SET_PARAM | (msg->addr & 0x7f) << 4;
	writel(val, cci->base + CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));

	load[i++] = CCI_I2C_WRITE | msg->len << 4;

	for (j = 0; j < msg->len; j++)
		load[i++] = msg->buf[j];

	for (j = 0; j < i; j += 4) {
		val = load[j];
		val |= load[j + 1] << 8;
		val |= load[j + 2] << 16;
		val |= load[j + 3] << 24;
		writel(val, cci->base + CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));
	}
}

static int cci_i2c_write(struct cci *cci, u16 master,
			struct i2c_msg *msgs, int num)
{
	u8 queue = QUEUE_0;
	u32 report = CCI_I2C_REPORT | CCI_I2C_REPORT_IRQ_EN;
	unsigned int queued = 0, words;
	int i, ret;

	ret = cci_validate_queue(cci, master, queue);
	if (ret < 0)
		return ret;

	for (i = 0; i < num; i++) {
		/* SET_PARAM, the padded WRITE command, and one final REPORT. */
		words = 1 + DIV_ROUND_UP(msgs[i].len + 1, 4);
		if (queued + words + 1 > cci->data->queue_size[queue]) {
			writel(report, cci->base +
			       CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));
			ret = cci_run_queue(cci, master, queue);
			if (ret < 0)
				return ret;
			queued = 0;
		}

		cci_i2c_queue_write(cci, master, &msgs[i]);
		queued += words;
	}

	writel(report, cci->base + CCI_I2C_Mm_Qn_LOAD_DATA(master, queue));

	return cci_run_queue(cci, master, queue);
}

static int cci_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct cci_master *cci_master = i2c_get_adapdata(adap);
	struct cci *cci = cci_master->cci;
	int i, count, ret;

	ret = pm_runtime_get_sync(cci->dev);
	if (ret < 0)
		goto err;

	for (i = 0; i < num; i += count) {
		count = 1;
		if (msgs[i].flags & I2C_M_RD) {
			ret = cci_i2c_read(cci, cci_master->master,
					   msgs[i].addr, msgs[i].buf,
					   msgs[i].len);
		} else {
			/* Keep reads and special-message flags on their old path. */
			if (!msgs[i].flags)
				while (i + count < num && !msgs[i + count].flags)
					count++;
			ret = cci_i2c_write(cci, cci_master->master,
					    &msgs[i], count);
		}

		if (ret < 0)
			break;
	}

	if (!ret)
		ret = num;

err:
	pm_runtime_put_autosuspend(cci->dev);

	return ret;
}

static u32 cci_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm cci_algo = {
	.xfer = cci_xfer,
	.functionality = cci_func,
};

static int cci_enable_clocks(struct cci *cci)
{
	return clk_bulk_prepare_enable(cci->nclocks, cci->clocks);
}

static void cci_disable_clocks(struct cci *cci)
{
	clk_bulk_disable_unprepare(cci->nclocks, cci->clocks);
}

static int __maybe_unused cci_suspend_runtime(struct device *dev)
{
	struct cci *cci = dev_get_drvdata(dev);

	cci_disable_clocks(cci);
	return 0;
}

static int __maybe_unused cci_resume_runtime(struct device *dev)
{
	struct cci *cci = dev_get_drvdata(dev);
	int ret;

	ret = cci_enable_clocks(cci);
	if (ret)
		return ret;

	cci_init(cci);
	return 0;
}

static const struct dev_pm_ops qcom_cci_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(cci_suspend_runtime, cci_resume_runtime, NULL)
};

static void cci_put_of_node(void *data)
{
	of_node_put(data);
}

static int cci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *r;
	struct cci *cci;
	struct dentry *timing;
	char *timing_name;
	int ret, i;
	u32 val;

	cci = devm_kzalloc(dev, sizeof(*cci), GFP_KERNEL);
	if (!cci)
		return -ENOMEM;

	cci->dev = dev;
	platform_set_drvdata(pdev, cci);
	cci->data = device_get_match_data(dev);
	if (!cci->data)
		return -ENOENT;

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		struct cci_master *master;
		u32 idx;

		ret = of_property_read_u32(child, "reg", &idx);
		if (ret) {
			dev_err(dev, "%pOF invalid 'reg' property", child);
			continue;
		}

		if (idx >= cci->data->num_masters) {
			dev_err(dev, "%pOF invalid 'reg' value: %u (max is %u)",
				child, idx, cci->data->num_masters - 1);
			continue;
		}

		master = &cci->master[idx];
		master->adap.quirks = &cci->data->quirks;
		master->adap.algo = &cci_algo;
		master->adap.dev.parent = dev;
		master->adap.dev.of_node = of_node_get(child);
		ret = devm_add_action_or_reset(dev, cci_put_of_node, child);
		if (ret)
			return ret;
		master->master = idx;
		master->cci = cci;

		i2c_set_adapdata(&master->adap, master);
		snprintf(master->adap.name, sizeof(master->adap.name), "Qualcomm-CCI");

		master->mode = I2C_MODE_STANDARD;
		ret = of_property_read_u32(child, "clock-frequency", &val);
		if (!ret) {
			if (val == I2C_MAX_FAST_MODE_FREQ)
				master->mode = I2C_MODE_FAST;
			else if (val == I2C_MAX_FAST_MODE_PLUS_FREQ)
				master->mode = I2C_MODE_FAST_PLUS;
		}

		init_completion(&master->irq_complete);
		spin_lock_init(&master->timing_lock);
	}

	/* Memory */

	cci->base = devm_platform_get_and_ioremap_resource(pdev, 0, &r);
	if (IS_ERR(cci->base))
		return PTR_ERR(cci->base);

	/* Clocks */

	ret = devm_clk_bulk_get_all(dev, &cci->clocks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	else if (!ret)
		return dev_err_probe(dev, -EINVAL, "not enough clocks in DT\n");
	cci->nclocks = ret;

	ret = cci_enable_clocks(cci);
	if (ret < 0)
		return ret;

	/* Interrupt */

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		goto disable_clocks;
	cci->irq = ret;

	ret = devm_request_irq(dev, cci->irq, cci_isr, 0, dev_name(dev), cci);
	if (ret < 0) {
		dev_err(dev, "request_irq failed, ret: %d\n", ret);
		goto disable_clocks;
	}

	val = readl(cci->base + CCI_HW_VERSION);
	dev_dbg(dev, "CCI HW version = 0x%08x", val);

	ret = cci_reset(cci);
	if (ret < 0)
		goto disable_clocks;

	pm_runtime_set_autosuspend_delay(dev, MSEC_PER_SEC);
	ret = devm_pm_runtime_set_active_enabled(dev);
	if (ret)
		goto disable_clocks;

	pm_runtime_use_autosuspend(dev);

	for (i = 0; i < cci->data->num_masters; i++) {
		if (!cci->master[i].cci)
			continue;

		ret = i2c_add_adapter(&cci->master[i].adap);
		if (ret < 0)
			goto error_i2c;
	}

	timing_name = devm_kasprintf(dev, GFP_KERNEL, "cci-timing-%s", dev_name(dev));
	if (timing_name) {
		timing = debugfs_create_dir(timing_name, NULL);
		if (!IS_ERR_OR_NULL(timing)) {
			debugfs_create_file("slow-queues", 0400, timing, cci, &cci_timing_fops);
			ret = devm_add_action_or_reset(dev, cci_timing_remove, timing);
			if (ret)
				dev_dbg(dev, "optional timing debugfs unavailable: %d\n", ret);
		}
	}

	return 0;

error_i2c:

	for (--i ; i >= 0; i--) {
		if (cci->master[i].cci)
			i2c_del_adapter(&cci->master[i].adap);
	}
disable_clocks:
	cci_disable_clocks(cci);

	return ret;
}

static void cci_remove(struct platform_device *pdev)
{
	struct cci *cci = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < cci->data->num_masters; i++) {
		if (cci->master[i].cci) {
			i2c_del_adapter(&cci->master[i].adap);
			cci_halt(cci, i);
		}
	}
}

static const struct cci_data cci_v1_data = {
	.num_masters = 1,
	.queue_size = { 64, 16 },
	.quirks = {
		.max_write_len = 10,
		.max_read_len = 12,
	},
	.params[I2C_MODE_STANDARD] = {
		.thigh = 78,
		.tlow = 114,
		.tsu_sto = 28,
		.tsu_sta = 28,
		.thd_dat = 10,
		.thd_sta = 77,
		.tbuf = 118,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 1
	},
	.params[I2C_MODE_FAST] = {
		.thigh = 20,
		.tlow = 28,
		.tsu_sto = 21,
		.tsu_sta = 21,
		.thd_dat = 13,
		.thd_sta = 18,
		.tbuf = 32,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 3
	},
};

static const struct cci_data cci_v1_5_data = {
	.num_masters = 2,
	.queue_size = { 64, 16 },
	.quirks = {
		.max_write_len = 10,
		.max_read_len = 12,
	},
	.params[I2C_MODE_STANDARD] = {
		.thigh = 78,
		.tlow = 114,
		.tsu_sto = 28,
		.tsu_sta = 28,
		.thd_dat = 10,
		.thd_sta = 77,
		.tbuf = 118,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 1
	},
	.params[I2C_MODE_FAST] = {
		.thigh = 20,
		.tlow = 28,
		.tsu_sto = 21,
		.tsu_sta = 21,
		.thd_dat = 13,
		.thd_sta = 18,
		.tbuf = 32,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 3
	},
};

static const struct cci_data cci_v2_data = {
	.num_masters = 2,
	.queue_size = { 64, 16 },
	.quirks = {
		.max_write_len = 11,
		.max_read_len = 12,
	},
	.params[I2C_MODE_STANDARD] = {
		.thigh = 201,
		.tlow = 174,
		.tsu_sto = 204,
		.tsu_sta = 231,
		.thd_dat = 22,
		.thd_sta = 162,
		.tbuf = 227,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 3
	},
	.params[I2C_MODE_FAST] = {
		.thigh = 38,
		.tlow = 56,
		.tsu_sto = 40,
		.tsu_sta = 40,
		.thd_dat = 22,
		.thd_sta = 35,
		.tbuf = 62,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 3
	},
	.params[I2C_MODE_FAST_PLUS] = {
		.thigh = 16,
		.tlow = 22,
		.tsu_sto = 17,
		.tsu_sta = 18,
		.thd_dat = 16,
		.thd_sta = 15,
		.tbuf = 24,
		.scl_stretch_en = 0,
		.trdhld = 3,
		.tsp = 3
	},
};

static const struct cci_data cci_msm8953_data = {
	.num_masters = 2,
	.queue_size = { 64, 16 },
	.quirks = {
		.max_write_len = 11,
		.max_read_len = 12,
	},
	.params[I2C_MODE_STANDARD] = {
		.thigh = 78,
		.tlow = 114,
		.tsu_sto = 28,
		.tsu_sta = 28,
		.thd_dat = 10,
		.thd_sta = 77,
		.tbuf = 118,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 1
	},
	.params[I2C_MODE_FAST] = {
		.thigh = 20,
		.tlow = 28,
		.tsu_sto = 21,
		.tsu_sta = 21,
		.thd_dat = 13,
		.thd_sta = 18,
		.tbuf = 32,
		.scl_stretch_en = 0,
		.trdhld = 6,
		.tsp = 3
	},
	.params[I2C_MODE_FAST_PLUS] = {
		.thigh = 16,
		.tlow = 22,
		.tsu_sto = 17,
		.tsu_sta = 18,
		.thd_dat = 16,
		.thd_sta = 15,
		.tbuf = 19,
		.scl_stretch_en = 1,
		.trdhld = 3,
		.tsp = 3
	},
};

static const struct of_device_id cci_dt_match[] = {
	{ .compatible = "qcom,msm8226-cci", .data = &cci_v1_data},
	{ .compatible = "qcom,msm8953-cci", .data = &cci_msm8953_data},
	{ .compatible = "qcom,msm8974-cci", .data = &cci_v1_5_data},
	{ .compatible = "qcom,msm8996-cci", .data = &cci_v2_data},


	/*
	 * Legacy compatibles kept for backwards compatibility.
	 * Do not add any new ones unless they introduce a new config
	 */
	{ .compatible = "qcom,msm8916-cci", .data = &cci_v1_data},
	{ .compatible = "qcom,sdm845-cci", .data = &cci_v2_data},
	{ .compatible = "qcom,sm8250-cci", .data = &cci_v2_data},
	{ .compatible = "qcom,sm8450-cci", .data = &cci_v2_data},
	{}
};
MODULE_DEVICE_TABLE(of, cci_dt_match);

static struct platform_driver qcom_cci_driver = {
	.probe  = cci_probe,
	.remove = cci_remove,
	.driver = {
		.name = "i2c-qcom-cci",
		.of_match_table = cci_dt_match,
		.pm = &qcom_cci_pm,
	},
};

module_platform_driver(qcom_cci_driver);

MODULE_DESCRIPTION("Qualcomm Camera Control Interface driver");
MODULE_AUTHOR("Todor Tomov <todor.tomov@linaro.org>");
MODULE_AUTHOR("Loic Poulain <loic.poulain@linaro.org>");
MODULE_LICENSE("GPL v2");
