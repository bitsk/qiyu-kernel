// SPDX-License-Identifier: GPL-2.0-only
/* A081 Nordic radio bridge: transport only, no controller protocol decoding. */
#include <linux/compat.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/kstrtox.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <uapi/linux/a081-nordic.h>

struct a081_nordic {
	struct spi_device *spi;
	struct miscdevice misc;
	struct gpio_desc *enable, *reset;
	struct mutex lock;
	spinlock_t irq_lock;
	wait_queue_head_t wait;
	struct kref ref;
	u64 irq_sequence, irq_timestamp, transfers;
	u8 *tx, *rx;
	bool opened;
};

struct a081_nordic_file {
	struct a081_nordic *radio;
	u64 consumed;
};

static void a081_nordic_free(struct kref *ref)
{
	kfree(container_of(ref, struct a081_nordic, ref));
}

static void a081_nordic_put(void *data)
{
	struct a081_nordic *radio = data;

	kref_put(&radio->ref, a081_nordic_free);
}

static void a081_nordic_power_off(void *data)
{
	struct a081_nordic *radio = data;

	gpiod_set_value_cansleep(radio->reset, 1);
	gpiod_set_value_cansleep(radio->enable, 0);
}

static irqreturn_t a081_nordic_irq(int irq, void *data)
{
	struct a081_nordic *radio = data;
	unsigned long flags;

	spin_lock_irqsave(&radio->irq_lock, flags);
	radio->irq_timestamp = ktime_get_ns();
	radio->irq_sequence++;
	spin_unlock_irqrestore(&radio->irq_lock, flags);
	wake_up_interruptible(&radio->wait);
	return IRQ_HANDLED;
}

static int a081_nordic_open(struct inode *inode, struct file *file)
{
	struct a081_nordic *radio = container_of(file->private_data,
						       struct a081_nordic, misc);
	struct a081_nordic_file *client;
	int ret = 0;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	mutex_lock(&radio->lock);
	if (!radio->spi)
		ret = -ENODEV;
	else if (radio->opened)
		ret = -EBUSY;
	else {
		kref_get(&radio->ref);
		radio->opened = true;
		client->radio = radio;
		file->private_data = client;
	}
	mutex_unlock(&radio->lock);
	if (ret)
		kfree(client);
	return ret;
}

static int a081_nordic_release(struct inode *inode, struct file *file)
{
	struct a081_nordic_file *client = file->private_data;
	struct a081_nordic *radio = client->radio;

	mutex_lock(&radio->lock);
	radio->opened = false;
	mutex_unlock(&radio->lock);
	a081_nordic_put(radio);
	kfree(client);
	return 0;
}

static __poll_t a081_nordic_poll(struct file *file, poll_table *wait)
{
	struct a081_nordic_file *client = file->private_data;
	struct a081_nordic *radio = client->radio;

	poll_wait(file, &radio->wait, wait);
	if (!READ_ONCE(radio->spi))
		return EPOLLERR | EPOLLHUP;
	if (READ_ONCE(radio->irq_sequence) != READ_ONCE(client->consumed))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static long a081_nordic_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct a081_nordic_file *client = file->private_data;
	struct a081_nordic *radio = client->radio;
	struct a081_nordic_dual_transfer result = {};
	struct spi_transfer transfer = {
		.tx_buf = radio->tx,
		.rx_buf = radio->rx,
		.len = A081_NORDIC_DUAL_FRAME_SIZE,
		.bits_per_word = 8,
	};
	void __user *user = (void __user *)arg;
	unsigned long flags;
	int ret;

	if (cmd != A081_NORDIC_IOC_DUAL_TRANSFER)
		return -ENOTTY;
	if (copy_from_user(result.tx, user, transfer.len))
		return -EFAULT;
	ret = mutex_lock_interruptible(&radio->lock);
	if (ret)
		return ret;
	if (!radio->spi) {
		ret = -ENODEV;
		goto unlock;
	}
	spin_lock_irqsave(&radio->irq_lock, flags);
	result.irq_sequence = radio->irq_sequence;
	result.irq_timestamp_ns = radio->irq_timestamp;
	spin_unlock_irqrestore(&radio->irq_lock, flags);
	if (result.irq_sequence == client->consumed) {
		ret = -EAGAIN;
		goto unlock;
	}
	memcpy(radio->tx, result.tx, transfer.len);
	ret = spi_sync_transfer(radio->spi, &transfer, 1);
	if (ret)
		goto unlock;
	result.completion_timestamp_ns = ktime_get_ns();
	memcpy(result.rx, radio->rx, transfer.len);
	WRITE_ONCE(client->consumed, result.irq_sequence);
	radio->transfers++;
	/* A newer IRQ during the exchange remains visible to poll(). */
	if (copy_to_user(user, &result, sizeof(result)))
		ret = -EFAULT;
unlock:
	mutex_unlock(&radio->lock);
	return ret;
}

static const struct file_operations a081_nordic_fops = {
	.owner = THIS_MODULE,
	.open = a081_nordic_open,
	.release = a081_nordic_release,
	.poll = a081_nordic_poll,
	.unlocked_ioctl = a081_nordic_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static ssize_t irq_count_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct a081_nordic *radio = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%llu\n", READ_ONCE(radio->irq_sequence));
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t transfer_count_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct a081_nordic *radio = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%llu\n", READ_ONCE(radio->transfers));
}
static DEVICE_ATTR_RO(transfer_count);

static ssize_t reset_asserted_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct a081_nordic *radio = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&radio->lock);
	ret = radio->spi ? gpiod_get_value_cansleep(radio->reset) : -ENODEV;
	mutex_unlock(&radio->lock);

	return ret < 0 ? ret : sysfs_emit(buf, "%d\n", ret);
}

static ssize_t reset_asserted_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct a081_nordic *radio = dev_get_drvdata(dev);
	bool asserted;
	int ret;

	ret = kstrtobool(buf, &asserted);
	if (ret)
		return ret;
	ret = mutex_lock_interruptible(&radio->lock);
	if (ret)
		return ret;
	ret = radio->spi ? gpiod_set_value_cansleep(radio->reset, asserted) : -ENODEV;
	mutex_unlock(&radio->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(reset_asserted);

static struct attribute *a081_nordic_attrs[] = {
	&dev_attr_irq_count.attr,
	&dev_attr_transfer_count.attr,
	&dev_attr_reset_asserted.attr,
	NULL,
};
ATTRIBUTE_GROUPS(a081_nordic);

static int a081_nordic_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct a081_nordic *radio;
	int ret;

	if (spi->irq <= 0 || spi->mode != SPI_MODE_0)
		return -EINVAL;
	radio = kzalloc(sizeof(*radio), GFP_KERNEL);
	if (!radio)
		return -ENOMEM;
	kref_init(&radio->ref);
	ret = devm_add_action_or_reset(dev, a081_nordic_put, radio);
	if (ret)
		return ret;
	mutex_init(&radio->lock);
	spin_lock_init(&radio->irq_lock);
	init_waitqueue_head(&radio->wait);
	radio->spi = spi;
	spi_set_drvdata(spi, radio);
	radio->tx = devm_kmalloc(dev, A081_NORDIC_DUAL_FRAME_SIZE, GFP_KERNEL);
	radio->rx = devm_kmalloc(dev, A081_NORDIC_DUAL_FRAME_SIZE, GFP_KERNEL);
	if (!radio->tx || !radio->rx)
		return -ENOMEM;
	radio->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(radio->enable))
		return dev_err_probe(dev, PTR_ERR(radio->enable), "power GPIO\n");
	radio->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(radio->reset))
		return dev_err_probe(dev, PTR_ERR(radio->reset), "reset GPIO\n");
	ret = devm_add_action_or_reset(dev, a081_nordic_power_off, radio);
	if (ret)
		return ret;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return ret;
	ret = devm_request_irq(dev, spi->irq, a081_nordic_irq, 0,
			       dev_name(dev), radio);
	if (ret)
		return dev_err_probe(dev, ret, "data-ready IRQ\n");
	/* Match OEM probe: power on, keep reset asserted until userspace starts. */
	ret = gpiod_set_value_cansleep(radio->enable, 1);
	if (ret)
		return ret;
	radio->misc.minor = MISC_DYNAMIC_MINOR;
	radio->misc.name = "a081-nordic";
	radio->misc.fops = &a081_nordic_fops;
	radio->misc.parent = dev;
	radio->misc.mode = 0600;
	ret = misc_register(&radio->misc);
	if (!ret)
		dev_info(dev, "%u-byte radio transport, %u Hz, IRQ %d, reset held\n",
			 A081_NORDIC_DUAL_FRAME_SIZE, spi->max_speed_hz, spi->irq);
	return ret;
}

static void a081_nordic_stop(struct spi_device *spi)
{
	struct a081_nordic *radio = spi_get_drvdata(spi);

	disable_irq(spi->irq);
	mutex_lock(&radio->lock);
	WRITE_ONCE(radio->spi, NULL);
	mutex_unlock(&radio->lock);
	wake_up_interruptible(&radio->wait);
}

static void a081_nordic_remove(struct spi_device *spi)
{
	struct a081_nordic *radio = spi_get_drvdata(spi);

	misc_deregister(&radio->misc);
	a081_nordic_stop(spi);
}

static void a081_nordic_shutdown(struct spi_device *spi)
{
	a081_nordic_stop(spi);
	a081_nordic_power_off(spi_get_drvdata(spi));
}

static const struct of_device_id a081_nordic_of_match[] = {
	{ .compatible = "iqiyi,a081-controller" },
	{}
};
MODULE_DEVICE_TABLE(of, a081_nordic_of_match);

static const struct spi_device_id a081_nordic_ids[] = {
	{ "a081-controller" },
	{}
};
MODULE_DEVICE_TABLE(spi, a081_nordic_ids);

static struct spi_driver a081_nordic_driver = {
	.driver = {
		.name = "a081-nordic",
		.of_match_table = a081_nordic_of_match,
		.dev_groups = a081_nordic_groups,
	},
	.probe = a081_nordic_probe,
	.remove = a081_nordic_remove,
	.shutdown = a081_nordic_shutdown,
	.id_table = a081_nordic_ids,
};
module_spi_driver(a081_nordic_driver);

MODULE_DESCRIPTION("iQIYI A081 Nordic controller radio transport");
MODULE_LICENSE("GPL");
