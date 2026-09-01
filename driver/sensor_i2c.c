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

#define DEVICE_NAME "sensor0"
#define CLASS_NAME  "sensor_i2c"
#define DRIVER_NAME "bme280_stub"

/* Register layout mirrors a real BME280 closely enough to be a faithful
 * stand-in: a fixed chip-id register plus a small calibration block. A real
 * BME280 has a much larger calibration table (0x88-0xA1); this keeps only
 * enough of it to prove the probe path reads real, seeded register bytes.
 */
#define REG_CHIP_ID    0x00
#define CHIP_ID_VALUE  0x60 /* the real BME280's chip-id value */
#define REG_CALIB_BASE 0x01
#define CALIB_LEN      4

struct sensor_priv {
	struct i2c_client *client;
	u8 calib[CALIB_LEN];
};

static const char fixed_pattern[] = "sensor_i2c: skeleton, no sensor bound yet\n";

static dev_t dev_num;
static struct cdev sensor_cdev;
static struct class *sensor_class;
static struct device *sensor_device;

static ssize_t sensor_read(struct file *filp, char __user *buf, size_t count,
			    loff_t *offp)
{
	size_t len = sizeof(fixed_pattern) - 1;

	if (*offp >= len)
		return 0; /* EOF: one read returns the whole pattern, like /proc files do */

	if (count > len - *offp)
		count = len - *offp;

	if (copy_to_user(buf, fixed_pattern + *offp, count))
		return -EFAULT;

	*offp += count;
	return count;
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

	return 0;
}

static void sensor_i2c_remove(struct i2c_client *client)
{
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
MODULE_DESCRIPTION("Character-device driver bound to a BME280-like I2C sensor");
