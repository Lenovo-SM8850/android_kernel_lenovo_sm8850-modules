// SPDX-License-Identifier: GPL-2.0-only
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>

static DEFINE_MUTEX(control_lock);
static unsigned int control;
static struct kobject *thermal_kobj;

static ssize_t control_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	ssize_t ret;

	mutex_lock(&control_lock);
	ret = sysfs_emit(buf, "%u\n", control);
	mutex_unlock(&control_lock);

	return ret;
}

static ssize_t control_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned int value;

	if (kstrtouint(buf, 10, &value))
		return -EINVAL;

	mutex_lock(&control_lock);
	if (control != value) {
		control = value;
		sysfs_notify(kobj, NULL, "control");
	}
	mutex_unlock(&control_lock);

	return count;
}

static struct kobj_attribute control_attr =
	__ATTR(control, 0644, control_show, control_store);

static int __init lenovo_thermal_control_init(void)
{
	struct device *cpu_dev = get_cpu_device(0);
	int ret;

	if (!cpu_dev)
		return -ENODEV;

	thermal_kobj = kobject_create_and_add("thermal", &cpu_dev->kobj);
	if (!thermal_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(thermal_kobj, &control_attr.attr);
	if (ret) {
		kobject_put(thermal_kobj);
		thermal_kobj = NULL;
	}

	return ret;
}

static void __exit lenovo_thermal_control_exit(void)
{
	sysfs_remove_file(thermal_kobj, &control_attr.attr);
	kobject_put(thermal_kobj);
}

module_init(lenovo_thermal_control_init);
module_exit(lenovo_thermal_control_exit);

MODULE_DESCRIPTION("Lenovo thermal case control");
MODULE_LICENSE("GPL");
