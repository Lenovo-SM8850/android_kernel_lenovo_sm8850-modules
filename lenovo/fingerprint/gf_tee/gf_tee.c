// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/uaccess.h>
#include <net/sock.h>
#include "panel_notifier.h"

#define GF_NETLINK 29
struct goodix_gf {
	struct device *dev;
	struct miscdevice misc;
	struct input_dev *input;
	struct regulator *vdd;
	struct gpio_desc *reset;
	struct sock *netlink;
	struct mutex lock;
	struct kref ref;
	struct delayed_work panel_work;
	void *panel_cookie;
	u32 portid;
	int irq;
	bool irq_requested, irq_enabled, wake_enabled, powered, dead, initialized;
	bool nav_enabled, suspended;
};
static struct goodix_gf *gf_device;
static DEFINE_MUTEX(gf_netlink_lock);
static void gf_receive(struct sk_buff *skb)
{
	mutex_lock(&gf_netlink_lock);
	if (gf_device && nlmsg_ok(nlmsg_hdr(skb), skb->len))
		WRITE_ONCE(gf_device->portid, NETLINK_CB(skb).portid);
	mutex_unlock(&gf_netlink_lock);
}
static void gf_notify(struct goodix_gf *gf, u8 command)
{
	struct sk_buff *skb;
	struct nlmsghdr *header;
	u32 portid = READ_ONCE(gf->portid);

	if (!portid || !gf->netlink)
		return;
	skb = nlmsg_new(16, GFP_KERNEL);
	if (!skb)
		return;
	header = nlmsg_put(skb, 0, 0, 0, 16, 0);
	if (!header) {
		kfree_skb(skb);
		return;
	}
	memset(nlmsg_data(header), 0, 16);
	*(u8 *)nlmsg_data(header) = command;
	netlink_unicast(gf->netlink, skb, portid, MSG_DONTWAIT);
}
static ssize_t fp_nav_switch_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct goodix_gf *gf = container_of(dev_get_drvdata(dev),
					 struct goodix_gf, misc);

	return sysfs_emit(buf, "%s\n", READ_ONCE(gf->nav_enabled) ? "true" : "false");
}

static ssize_t fp_nav_switch_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct goodix_gf *gf = container_of(dev_get_drvdata(dev),
					 struct goodix_gf, misc);

	mutex_lock(&gf->lock);
	if (!gf->dead && count && (buf[0] == '1' || buf[0] == '0')) {
		gf->nav_enabled = buf[0] == '1';
		gf_notify(gf, gf->nav_enabled ? 4 : 5);
	}
	mutex_unlock(&gf->lock);
	return count;
}
static DEVICE_ATTR_RW(fp_nav_switch);

static ssize_t fp_suspend_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct goodix_gf *gf = container_of(dev_get_drvdata(dev),
					 struct goodix_gf, misc);

	return sysfs_emit(buf, "%s\n", READ_ONCE(gf->suspended) ? "true" : "false");
}

static ssize_t fp_suspend_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct goodix_gf *gf = container_of(dev_get_drvdata(dev),
					 struct goodix_gf, misc);

	mutex_lock(&gf->lock);
	if (!gf->dead && count && (buf[0] == '1' || buf[0] == '0')) {
		gf->suspended = buf[0] == '1';
		gf_notify(gf, gf->suspended ? 3 : 2);
	}
	mutex_unlock(&gf->lock);
	return count;
}
static DEVICE_ATTR_RW(fp_suspend);

static struct attribute *gf_attrs[] = {
	&dev_attr_fp_nav_switch.attr,
	&dev_attr_fp_suspend.attr,
	NULL,
};
static const struct attribute_group gf_group = {
	.name = "goodix",
	.attrs = gf_attrs,
};
static const struct attribute_group *gf_groups[] = {
	&gf_group,
	NULL,
};

static irqreturn_t gf_irq(int irq, void *data)
{
	struct goodix_gf *gf = data;

	pm_wakeup_event(gf->dev, 1000);
	gf_notify(gf, 1);
	return IRQ_HANDLED;
}
static void gf_panel_event(enum panel_event_notifier_tag tag,
		struct panel_event_notification *event, void *data)
{
	struct goodix_gf *gf = data;

	if (READ_ONCE(gf->dead) || !event->notif_data.early_trigger)
		return;
	if (event->notif_type == DRM_PANEL_EVENT_BLANK)
		gf_notify(gf, 2);
	else if (event->notif_type == DRM_PANEL_EVENT_UNBLANK)
		gf_notify(gf, 3);
}
static void gf_panel_work(struct work_struct *work)
{
	struct goodix_gf *gf = container_of(to_delayed_work(work),
					struct goodix_gf, panel_work);
	struct drm_panel *panel = lenovo_find_panel(gf->dev->of_node);

	if (!IS_ERR(panel))
		/* Wuji has one touch panel; the unused secondary-touch slot is free. */
		gf->panel_cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_SECONDARY_TOUCH,
			panel, gf_panel_event, gf);
	if (IS_ERR_OR_NULL(gf->panel_cookie) && !READ_ONCE(gf->dead) &&
	    READ_ONCE(gf->initialized))
		schedule_delayed_work(&gf->panel_work, HZ);
}
static void gf_reset(struct goodix_gf *gf, unsigned int delay_ms)
{
	gpiod_set_raw_value_cansleep(gf->reset, 0);
	usleep_range(3000, 3500);
	gpiod_set_raw_value_cansleep(gf->reset, 1);
	usleep_range(delay_ms * 1000, delay_ms * 1000 + 500);
}
static int gf_power(struct goodix_gf *gf, bool on)
{
	int ret;

	if (on == gf->powered)
		return 0;
	if (!on) {
		ret = regulator_disable(gf->vdd);
		if (!ret)
			gf->powered = false;
		return ret;
	}
	ret = regulator_set_load(gf->vdd, 600000);
	if (ret < 0)
		return ret;
	if (regulator_count_voltages(gf->vdd) > 0) {
		ret = regulator_set_voltage(gf->vdd, 3000000, 3000000);
		if (ret)
			return ret;
	}
	ret = regulator_enable(gf->vdd);
	if (!ret) {
		gf->powered = true;
		usleep_range(10000, 11000);
		gf_reset(gf, 3);
	}
	return ret;
}
static void gf_irq_off(struct goodix_gf *gf)
{
	if (gf->irq_enabled) {
		disable_irq(gf->irq);
		gf->irq_enabled = false;
	}
}
static void gf_exit(struct goodix_gf *gf)
{
	gf->initialized = false;
	cancel_delayed_work_sync(&gf->panel_work);
	panel_event_notifier_unregister(gf->panel_cookie);
	gf->panel_cookie = NULL;
	gf_irq_off(gf);
	if (gf->wake_enabled) {
		disable_irq_wake(gf->irq);
		gf->wake_enabled = false;
	}
	if (gf->irq_requested) {
		free_irq(gf->irq, gf);
		gf->irq_requested = false;
	}
}
static void gf_free(struct kref *ref)
{
	kfree(container_of(ref, struct goodix_gf, ref));
}
static int gf_open(struct inode *inode, struct file *file)
{
	struct goodix_gf *gf = container_of(file->private_data, struct goodix_gf, misc);

	if (READ_ONCE(gf->dead))
		return -ENODEV;
	kref_get(&gf->ref);
	file->private_data = gf;
	return 0;
}
static int gf_release(struct inode *inode, struct file *file)
{
	kref_put(&((struct goodix_gf *)file->private_data)->ref, gf_free);
	return 0;
}
static long gf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct goodix_gf *gf = file->private_data;
	u32 key[2];
	u8 byte;
	int ret = 0, code;

	if (_IOC_TYPE(cmd) != 'g')
		return -EINVAL;
	mutex_lock(&gf->lock);
	if (gf->dead) {
		ret = -ENODEV;
		goto unlock;
	}
	switch (cmd) {
	case 0x80016700:
		byte = GF_NETLINK;
		if (copy_to_user((void __user *)arg, &byte, 1)) {
			ret = -EFAULT;
			break;
		}
		if (gf->initialized)
			break;
		ret = request_threaded_irq(gf->irq, NULL, gf_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_RISING, "gf", gf);
		if (ret)
			break;
		gf->irq_requested = true;
		gf->irq_enabled = true;
		gf->wake_enabled = !enable_irq_wake(gf->irq);
		gf_irq_off(gf);
		gf->initialized = true;
		schedule_delayed_work(&gf->panel_work, 0);
		break;
	case 0x6701:
		gf_exit(gf);
		break;
	case 0x6702:
		gf_reset(gf, 10);
		break;
	case 0x6703:
		if (gf->irq_requested && !gf->irq_enabled) {
			enable_irq(gf->irq);
			gf->irq_enabled = true;
		}
		break;
	case 0x6704:
		gf_irq_off(gf);
		break;
	case 0x40046705:
		pm_wakeup_event(gf->dev, 1000);
		break;
	case 0x6706:
	case 0x670a:
	case 0x4004670e:
		break;
	case 0x6707:
		ret = gf_power(gf, true);
		break;
	case 0x6708:
		ret = gf_power(gf, false);
		break;
	case 0x40086708:
		if (copy_from_user(key, (void __user *)arg, sizeof(key))) {
			ret = -EFAULT;
			break;
		}
		code = key[0] == 1 ? KEY_HOMEPAGE : key[0] == 2 ? KEY_POWER : KEY_CAMERA;
		if (key[0] == 1) {
			input_report_key(gf->input, code, !!key[1]);
			input_sync(gf->input);
		} else if ((key[0] == 2 || key[0] == 5) && key[1] == 1) {
			input_report_key(gf->input, code, 1);
			input_sync(gf->input);
			input_report_key(gf->input, code, 0);
			input_sync(gf->input);
		}
		break;
	case 0x8001670b:
		byte = 0;
		ret = copy_to_user((void __user *)arg, &byte, 1) ? -EFAULT : 0;
		break;
	case 0x4008670d:
		ret = copy_from_user(key, (void __user *)arg, sizeof(key)) ? -EFAULT : 0;
		break;
	case 0x670c:
		/* Keep open-file storage alive while deactivating the hardware. */
		gf_exit(gf);
		ret = gf_power(gf, false);
		break;
	default:
		/* The stock driver accepts unknown commands with the correct magic. */
		break;
	}
unlock:
	mutex_unlock(&gf->lock);
	return ret;
}
static const struct file_operations gf_fops = {
	.owner = THIS_MODULE, .open = gf_open, .release = gf_release,
	.unlocked_ioctl = gf_ioctl, .compat_ioctl = gf_ioctl,
};
static int gf_probe(struct platform_device *pdev)
{
	struct netlink_kernel_cfg cfg = { .input = gf_receive };
	struct device *dev = &pdev->dev;
	struct goodix_gf *gf;
	int gpio, ret;

	gf = kzalloc(sizeof(*gf), GFP_KERNEL);
	if (!gf)
		return -ENOMEM;
	gf->dev = dev;
	kref_init(&gf->ref);
	mutex_init(&gf->lock);
	INIT_DELAYED_WORK(&gf->panel_work, gf_panel_work);
	gpio = of_get_named_gpio(dev->of_node, "gfrst-gpios", 0);
	if (gpio < 0) {
		ret = gpio;
		goto free;
	}
	ret = devm_gpio_request_one(dev, gpio, GPIOF_OUT_INIT_LOW, "gf-reset");
	if (ret)
		goto free;
	gf->reset = gpio_to_desc(gpio);
	gpio = of_get_named_gpio(dev->of_node, "gfint-gpios", 0);
	if (gpio < 0) {
		ret = gpio;
		goto free;
	}
	ret = devm_gpio_request_one(dev, gpio, GPIOF_IN, "gf-irq");
	if (ret)
		goto free;
	gf->irq = gpio_to_irq(gpio);
	if (gf->irq < 0) {
		ret = gf->irq;
		goto free;
	}
	gf->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(gf->vdd)) {
		ret = PTR_ERR(gf->vdd);
		goto free;
	}
	ret = gf_power(gf, true);
	if (ret)
		goto free;
	gf->input = devm_input_allocate_device(dev);
	if (!gf->input) {
		ret = -ENOMEM;
		goto power_off;
	}
	gf->input->name = "gf-keys";
	input_set_capability(gf->input, EV_KEY, KEY_HOMEPAGE);
	input_set_capability(gf->input, EV_KEY, KEY_POWER);
	input_set_capability(gf->input, EV_KEY, KEY_CAMERA);
	ret = input_register_device(gf->input);
	if (ret)
		goto power_off;
	mutex_lock(&gf_netlink_lock);
	if (gf_device) {
		ret = -EBUSY;
	} else {
		gf->netlink = netlink_kernel_create(&init_net, GF_NETLINK, &cfg);
		if (!gf->netlink)
			ret = -EADDRINUSE;
		else
			gf_device = gf;
	}
	mutex_unlock(&gf_netlink_lock);
	if (ret)
		goto power_off;
	gf->misc = (struct miscdevice) { .minor = MISC_DYNAMIC_MINOR,
		.name = "goodix_fp", .fops = &gf_fops, .parent = dev,
		.groups = gf_groups };
	ret = misc_register(&gf->misc);
	if (ret) {
		mutex_lock(&gf_netlink_lock);
		gf_device = NULL;
		mutex_unlock(&gf_netlink_lock);
		netlink_kernel_release(gf->netlink);
		goto power_off;
	}
	device_init_wakeup(dev, true);
	platform_set_drvdata(pdev, gf);
	return 0;
power_off:
	gf_power(gf, false);
free:
	kref_put(&gf->ref, gf_free);
	return ret;
}
static void gf_remove(struct platform_device *pdev)
{
	struct goodix_gf *gf = platform_get_drvdata(pdev);

	misc_deregister(&gf->misc);
	mutex_lock(&gf->lock);
	gf->dead = true;
	gf_exit(gf);
	mutex_lock(&gf_netlink_lock);
	gf_device = NULL;
	mutex_unlock(&gf_netlink_lock);
	netlink_kernel_release(gf->netlink);
	gf->netlink = NULL;
	gf_power(gf, false);
	mutex_unlock(&gf->lock);
	device_init_wakeup(gf->dev, false);
	kref_put(&gf->ref, gf_free);
}
static const struct of_device_id gf_of_match[] = {
	{ .compatible = "goodix,plat-driver" }, { }
};
MODULE_DEVICE_TABLE(of, gf_of_match);
static struct platform_driver gf_driver = {
	.driver = { .name = "gf_tee", .of_match_table = gf_of_match,
		.suppress_bind_attrs = true }, .probe = gf_probe, .remove = gf_remove,
};
module_platform_driver(gf_driver);
MODULE_DESCRIPTION("Lenovo Wuji Goodix fingerprint TEE GPIO transport");
MODULE_LICENSE("GPL");
