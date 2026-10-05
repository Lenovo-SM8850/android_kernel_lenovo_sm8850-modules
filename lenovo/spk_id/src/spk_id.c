// SPDX-License-Identifier: GPL-2.0-only
/*
 * Speaker ID GPIO driver with sysfs interface (Lenovo TB323FU).
 *
 * Rewritten from the disassembly of the stock spk_id_dlkm.ko (no source is available; original
 * author wangjie46@lenovo.com): the GPIO named "spk-id" of the "lenovo,speaker_id" node is claimed,
 * its level is read once at probe and published as /sys/kernel/spk_id_gpio_level/spk_id_level.
 * Names, log messages and behaviour follow the stock module.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/delay.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

static int spk_id_gpio;
static bool gpio_requested;
static int spk_id_level = 2;	/* stock initial value: neither 0 nor 1 until probe has read the pin */
static struct kobject *spk_id_kobj;

/* the level read at probe (or last written); the pin is not read again */
static ssize_t spk_id_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", spk_id_level);
}

static ssize_t spk_id_level_store(struct kobject *kobj, struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	int val = 0;
	int ret;

	ret = kstrtoint(buf, 10, &val);
	if (ret < 0) {
		pr_err("spk_id: Failed to convert input to integer\n");
		return -EINVAL;
	}

	if (val < 0 || val > 1) {
		pr_err("spk_id: Input level out of range (must be 0 or 1)\n");
		return -EINVAL;
	}

	spk_id_level = val;
	pr_info("spk_id: Level updated to %d via sysfs\n", val);
	return count;
}

static struct kobj_attribute spk_id_level_attr =
	__ATTR(spk_id_level, 0664, spk_id_level_show, spk_id_level_store);

static int spk_id_probe(struct platform_device *pdev)
{
	int ret, i;
	int level;

	spk_id_gpio = of_get_named_gpio(pdev->dev.of_node, "spk-id", 0);
	if (spk_id_gpio < 0) {
		pr_err("spk_id: Failed to get GPIO number from DTS, error=%d\n", spk_id_gpio);
		return spk_id_gpio;
	}
	pr_info("spk_id: Got GPIO number %d from DTS\n", spk_id_gpio);

	/* -EBUSY (another driver still holds the line) is retried, 3 attempts, 500 ms apart */
	for (i = 0; i < 3; i++) {
		ret = gpio_request_one(spk_id_gpio, GPIOF_IN, "spk-id-gpio");
		if (!ret) {
			gpio_requested = true;
			break;
		}
		if (ret != -EBUSY) {
			pr_err("spk_id: GPIO request failed with error %d, no retry\n", ret);
			break;
		}
		pr_warn("spk_id: GPIO request failed with error %d,  retry\n", ret);
		msleep(500);
	}
	/* a failed request is not fatal: the level is read anyway and the sysfs node is created */

	level = gpiod_get_raw_value(gpio_to_desc(spk_id_gpio));
	spk_id_level = level;
	pr_info("spk_id: GPIO%d initial level: %d\n", spk_id_gpio, level);

	spk_id_kobj = kobject_create_and_add("spk_id_gpio_level", kernel_kobj);
	if (!spk_id_kobj) {
		pr_err("spk_id: Failed to create sysfs node\n");
		ret = -ENOMEM;
		goto err_gpio;
	}

	ret = sysfs_create_file(spk_id_kobj, &spk_id_level_attr.attr);
	if (ret) {
		pr_err("spk_id: Failed to create sysfs attribute, error=%d\n", ret);
		kobject_put(spk_id_kobj);
		spk_id_kobj = NULL;
		goto err_gpio;
	}

	pr_info("spk_id: Driver probed successfully\n");
	return 0;

err_gpio:
	if (gpio_requested) {
		gpio_free(spk_id_gpio);
		gpio_requested = false;
	}
	return ret;
}

static void spk_id_remove(struct platform_device *pdev)
{
	pr_info("spk_id: Driver remove called\n");

	if (spk_id_kobj) {
		sysfs_remove_file(spk_id_kobj, &spk_id_level_attr.attr);
		kobject_put(spk_id_kobj);
		spk_id_kobj = NULL;
	}

	if (gpio_requested && spk_id_gpio != -1) {
		gpio_free(spk_id_gpio);
		spk_id_gpio = -1;
		gpio_requested = false;
		pr_info("spk_id: GPIO released in remove\n");
	}
}

static void spk_id_shutdown(struct platform_device *pdev)
{
	pr_info("spk_id: System shutting down, releasing GPIO...\n");

	if (gpio_requested && spk_id_gpio != -1) {
		gpio_free(spk_id_gpio);
		spk_id_gpio = -1;
		gpio_requested = false;
		pr_info("spk_id: GPIO released during shutdown\n");
	}

	if (spk_id_kobj) {
		sysfs_remove_file(spk_id_kobj, &spk_id_level_attr.attr);
		kobject_put(spk_id_kobj);
		spk_id_kobj = NULL;
	}
}

static const struct of_device_id spk_id_match[] = {
	{ .compatible = "lenovo,speaker_id" },
	{ },
};
MODULE_DEVICE_TABLE(of, spk_id_match);

static struct platform_driver spk_id_driver = {
	.probe = spk_id_probe,
	.remove = spk_id_remove,
	.shutdown = spk_id_shutdown,
	.driver = {
		.name = "spk-id",
		.of_match_table = spk_id_match,
	},
};

module_platform_driver(spk_id_driver);

MODULE_AUTHOR("wangjie46@lenovo.com");
MODULE_DESCRIPTION("Speaker ID GPIO Driver with sysfs Interface");
MODULE_LICENSE("GPL");
