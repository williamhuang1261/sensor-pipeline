// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_i2c: character-device skeleton for the embedded sensor pipeline.
 *
 * Step 1 has no I2C logic yet. It exists to prove the build/load loop
 * (Kbuild against a real kernel's headers, insmod, /dev node creation,
 * rmmod) works end to end before any sensor code is added on top of it.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/errno.h>

#define DEVICE_NAME "sensor0"
#define CLASS_NAME  "sensor_i2c"

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

	pr_info("sensor_i2c: loaded, /dev/%s ready (major %d)\n", DEVICE_NAME,
		MAJOR(dev_num));
	return 0;

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
MODULE_DESCRIPTION("Character-device skeleton for the embedded sensor pipeline");
