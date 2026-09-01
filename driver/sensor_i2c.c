// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_i2c: a character-device driver bound to a BME280-like I2C sensor.
 *
 * No physical Raspberry Pi or BME280 exists on the machine this was built
 * on. Development and verification instead use `i2c-stub`
 * (drivers/i2c/i2c-stub.c), the kernel's own in-tree tool for exercising an
 * I2C client driver without real hardware: it presents a virtual adapter
 * that answers real SMBus transactions from a plain byte-addressable
 * register file. `scripts/seed_stub.sh` seeds that register file and
 * instantiates this driver's client on the stub adapter; see the README's
 * "Hardware re-scoping" section for the full explanation.
 *
 * A kernel thread polls the sensor's measurement register on a fixed
 * interval and pushes each reading into a kfifo ring buffer; userspace
 * read()s block until a sample is queued and drain it from there, so a
 * burst of readings between reads is never dropped and a slow reader can
 * never stall the sampling thread.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/kfifo.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/ktime.h>

#define DEVICE_NAME "sensor0"
#define CLASS_NAME  "sensor_i2c"
#define DRIVER_NAME "bme280_stub"

/* Register layout mirrors a real BME280 closely enough to be a faithful
 * stand-in: a fixed chip-id register, a small calibration block, and one
 * measurement register the kthread re-reads every poll. A real BME280
 * packs temperature/pressure/humidity across several burst-read registers;
 * this keeps one byte to keep the ring-buffer plumbing the focus.
 */
#define REG_CHIP_ID      0x00
#define CHIP_ID_VALUE    0x60 /* the real BME280's chip-id value */
#define REG_CALIB_BASE   0x01
#define CALIB_LEN        4
#define REG_MEASUREMENT  0x05

#define POLL_INTERVAL_MS 500
#define FIFO_CAPACITY     32 /* samples; must be a power of two for kfifo */

struct sample {
	u64 seq;
	u64 ts_ns;
	u8 raw;
};

struct sensor_priv {
	struct i2c_client *client;
	u8 calib[CALIB_LEN];
	struct task_struct *poll_task;
};

static DEFINE_KFIFO(sample_fifo, struct sample, FIFO_CAPACITY);
static DEFINE_SPINLOCK(fifo_lock);
static DECLARE_WAIT_QUEUE_HEAD(fifo_wait);
static u64 sample_seq;

static dev_t dev_num;
static struct cdev sensor_cdev;
static struct class *sensor_class;
static struct device *sensor_device;

static int sensor_poll_thread(void *data)
{
	struct sensor_priv *priv = data;
	int val;
	struct sample s;

	while (!kthread_should_stop()) {
		val = i2c_smbus_read_byte_data(priv->client, REG_MEASUREMENT);
		if (val < 0) {
			dev_warn(&priv->client->dev,
				 "measurement read failed: %d\n", val);
		} else {
			s.seq = ++sample_seq;
			s.ts_ns = ktime_get_ns();
			s.raw = (u8)val;

			spin_lock(&fifo_lock);
			if (kfifo_is_full(&sample_fifo))
				kfifo_skip(&sample_fifo); /* drop oldest, keep sampling live */
			kfifo_in(&sample_fifo, &s, 1);
			spin_unlock(&fifo_lock);

			wake_up_interruptible(&fifo_wait);
		}

		msleep_interruptible(POLL_INTERVAL_MS);
	}

	return 0;
}

static ssize_t sensor_read(struct file *filp, char __user *buf, size_t count,
			    loff_t *offp)
{
	struct sample s;
	char line[64];
	int len;
	unsigned int copied;
	int ret;

	ret = wait_event_interruptible(fifo_wait, !kfifo_is_empty(&sample_fifo));
	if (ret)
		return ret;

	spin_lock(&fifo_lock);
	ret = kfifo_out(&sample_fifo, &s, 1);
	spin_unlock(&fifo_lock);

	if (!ret)
		return -EAGAIN; /* raced with another reader; caller may retry */

	len = scnprintf(line, sizeof(line), "seq=%llu ts_ns=%llu raw=0x%02x\n",
			 s.seq, s.ts_ns, s.raw);

	if (count < (size_t)len)
		return -EINVAL;

	if (copy_to_user(buf, line, len))
		return -EFAULT;

	copied = len;
	return copied;
}

static int sensor_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int sensor_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static const struct file_operations sensor_fops = {
	.owner = THIS_MODULE,
	.open = sensor_open,
	.release = sensor_release,
	.read = sensor_read,
};

static int sensor_i2c_probe(struct i2c_client *client)
{
	struct sensor_priv *priv;
	int chip_id;
	int i, val;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	i2c_set_clientdata(client, priv);

	chip_id = i2c_smbus_read_byte_data(client, REG_CHIP_ID);
	if (chip_id < 0) {
		dev_err(&client->dev, "failed to read chip id: %d\n", chip_id);
		return chip_id;
	}

	if (chip_id != CHIP_ID_VALUE)
		dev_warn(&client->dev,
			 "unexpected chip id 0x%02x (expected 0x%02x) -- fine against i2c-stub with a different seed, would be a real mismatch against real silicon\n",
			 chip_id, CHIP_ID_VALUE);
	else
		dev_info(&client->dev, "chip id 0x%02x confirmed\n", chip_id);

	for (i = 0; i < CALIB_LEN; i++) {
		val = i2c_smbus_read_byte_data(client, REG_CALIB_BASE + i);
		if (val < 0) {
			dev_err(&client->dev,
				"failed to read calibration register 0x%02x: %d\n",
				REG_CALIB_BASE + i, val);
			return val;
		}
		priv->calib[i] = (u8)val;
	}

	dev_info(&client->dev,
		 "probed at 0x%02x, calibration bytes: %02x %02x %02x %02x\n",
		 client->addr, priv->calib[0], priv->calib[1], priv->calib[2],
		 priv->calib[3]);

	priv->poll_task = kthread_run(sensor_poll_thread, priv,
				       "sensor_i2c_poll");
	if (IS_ERR(priv->poll_task)) {
		int ret = PTR_ERR(priv->poll_task);

		dev_err(&client->dev, "failed to start poll thread: %d\n", ret);
		return ret;
	}

	dev_info(&client->dev, "poll thread started, interval %d ms\n",
		 POLL_INTERVAL_MS);

	return 0;
}

static void sensor_i2c_remove(struct i2c_client *client)
{
	struct sensor_priv *priv = i2c_get_clientdata(client);

	if (priv->poll_task)
		kthread_stop(priv->poll_task);

	dev_info(&client->dev, "removed\n");
}

static const struct i2c_device_id sensor_i2c_id[] = {
	{ DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sensor_i2c_id);

static struct i2c_driver sensor_i2c_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = sensor_i2c_probe,
	.remove = sensor_i2c_remove,
	.id_table = sensor_i2c_id,
};

static int __init sensor_i2c_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_err("sensor_i2c: alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&sensor_cdev, &sensor_fops);
	sensor_cdev.owner = THIS_MODULE;

	ret = cdev_add(&sensor_cdev, dev_num, 1);
	if (ret < 0) {
		pr_err("sensor_i2c: cdev_add failed: %d\n", ret);
		goto err_unregister;
	}

	sensor_class = class_create(CLASS_NAME);
	if (IS_ERR(sensor_class)) {
		ret = PTR_ERR(sensor_class);
		pr_err("sensor_i2c: class_create failed: %d\n", ret);
		goto err_cdev;
	}

	sensor_device = device_create(sensor_class, NULL, dev_num, NULL,
				       DEVICE_NAME);
	if (IS_ERR(sensor_device)) {
		ret = PTR_ERR(sensor_device);
		pr_err("sensor_i2c: device_create failed: %d\n", ret);
		goto err_class;
	}

	ret = i2c_add_driver(&sensor_i2c_driver);
	if (ret < 0) {
		pr_err("sensor_i2c: i2c_add_driver failed: %d\n", ret);
		goto err_device;
	}

	pr_info("sensor_i2c: loaded, /dev/%s ready (major %d), i2c driver '%s' registered\n",
		DEVICE_NAME, MAJOR(dev_num), DRIVER_NAME);
	return 0;

err_device:
	device_destroy(sensor_class, dev_num);
err_class:
	class_destroy(sensor_class);
err_cdev:
	cdev_del(&sensor_cdev);
err_unregister:
	unregister_chrdev_region(dev_num, 1);
	return ret;
}

static void __exit sensor_i2c_exit(void)
{
	i2c_del_driver(&sensor_i2c_driver);
	device_destroy(sensor_class, dev_num);
	class_destroy(sensor_class);
	cdev_del(&sensor_cdev);
	unregister_chrdev_region(dev_num, 1);
	pr_info("sensor_i2c: unloaded\n");
}

module_init(sensor_i2c_init);
module_exit(sensor_i2c_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("William Huang");
MODULE_DESCRIPTION("Character-device driver bound to a BME280-like I2C sensor, kfifo-buffered");
