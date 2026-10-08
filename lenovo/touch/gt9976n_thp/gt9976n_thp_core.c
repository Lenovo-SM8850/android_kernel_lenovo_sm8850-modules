// SPDX-License-Identifier: GPL-2.0
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/input/mt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/proc_fs.h>
#include <linux/power_supply.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/pinctrl/consumer.h>
#include <linux/seq_file.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <net/genetlink.h>
#include "panel_notifier.h"

#define THP_SLOT_SIZE 0x2800
#define THP_SLOTS 20
#define THP_RING_SIZE (THP_SLOT_SIZE * THP_SLOTS)
#define THP_FRAME_MAX (THP_SLOT_SIZE - sizeof(u64))
#define THP_REPORT_SIZE 504

static bool allow_fw_update;
module_param(allow_fw_update, bool, 0444);
MODULE_PARM_DESC(allow_fw_update, "Allow touch firmware and config flash updates");

struct thp_spi_transfer {
	__u64 tx;
	__u64 rx;
	__u32 size;
	__u32 reserved;
};
struct thp_host_message {
	__u32 command;
	__u32 size;
	u8 data[128];
};
static_assert(sizeof(struct thp_spi_transfer) == 24);
static_assert(sizeof(struct thp_host_message) == 136);
static_assert(THP_RING_SIZE == 0x32000);
struct goodix_thp {
	struct spi_device *spi;
	struct miscdevice touch, agent, penraw;
	struct platform_device *pen_info;
	u8 pen_mac[6], pen_identity[2];
	u8 pen_samples[32][40], pen_write, pen_count, pen_sequence;
	struct input_dev *fingers, *pen;
	struct regulator *iovdd, *avdd;
	struct gpio_desc *reset;
	struct kref ref;
	struct mutex control_lock, bus_lock, ring_lock, input_lock;
	struct pinctrl *pins;
	struct pinctrl_state *active_pins, *sleep_pins;
	atomic_t request_id;
	wait_queue_head_t frames;
	u8 *ring, *tx, *rx;
	u32 read_slot, write_slot, count, frame_address, frame_length;
	u32 command_address, gesture_address, status_address, gesture_mask;
	u32 portid, sequence, state, state_changed;
	int timeout_ms;
	int irq;
	bool dead, ready, irq_enabled, hal_ready, powered, wake_enabled;
	bool iovdd_enabled, avdd_enabled, esd_enabled, irq_seen;
	bool restore_modes, pm_suspended, irq_pending, panel_blank;
	int charger_sent;
	struct notifier_block charger_notifier;
	struct delayed_work panel_work, esd_work, mode_work;
	struct delayed_work suspend_work, resume_work;
	void *panel_cookie;
};
struct thp_control {
	struct goodix_thp *thp;
	struct proc_dir_entry *entry;
	const char *name;
	u8 command;
	int value;
	bool valid;
};
static int thp_send_command(struct goodix_thp *thp, u8 cmd, u16 data);
static int thp_sleep(struct goodix_thp *thp, bool suspend);
static struct thp_control thp_controls[] = {
	{ .name = "gesture_mode", .command = 0xa6 },
	{ .name = "pen_wakeup_mode", .command = 0xa7 },
	{ .name = "panel_direction", .command = 15 },
	{ .name = "game_mode", .command = 14 },
	{ .name = "game_edge", .command = 17 },
	{ .name = "pen_type", .command = 19 },
	{ .name = "support_pen", .command = 12 },
	{ .name = "HighReportRate", .command = 0xa4 },
};
static struct goodix_thp *thp_device;
static DEFINE_MUTEX(thp_netlink_lock);
static struct genl_family thp_family;
static const struct nla_policy thp_policy[6] = {
	[1] = { .type = NLA_U32 }, [2] = { .type = NLA_U8 },
	[3] = { .type = NLA_S32 }, [4] = { .type = NLA_U8 },
	[5] = { .type = NLA_BINARY, .len = THP_SLOT_SIZE },
};
static int thp_enter(struct sk_buff *skb, struct genl_info *info)
{
	int ret = -ENODEV;

	mutex_lock(&thp_netlink_lock);
	if (thp_device) {
		WRITE_ONCE(thp_device->portid, info->snd_portid);
		ret = 0;
	}
	mutex_unlock(&thp_netlink_lock);
	return ret;
}
static int thp_response(struct sk_buff *skb, struct genl_info *info)
{
	/* Runtime requests are asynchronous. */
	return info->attrs[1] ? 0 : -EINVAL;
}
static const struct genl_small_ops thp_ops[] = {
	{ .cmd = 1, .doit = thp_enter },
	{ .cmd = 3, .doit = thp_response },
};
static struct genl_family thp_family = {
	.name = "thp_nlmsg", .version = 1, .maxattr = 5,
	.module = THIS_MODULE, .policy = thp_policy,
	.small_ops = thp_ops, .n_small_ops = ARRAY_SIZE(thp_ops),
};
static int thp_notify(struct goodix_thp *thp, u8 type, const void *data, int size)
{
	struct sk_buff *skb;
	void *header;
	u32 portid = READ_ONCE(thp->portid);

	if (!portid)
		return -ENOTCONN;
	skb = genlmsg_new(NLMSG_DEFAULT_SIZE + size, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	header = genlmsg_put(skb, 0, 0, &thp_family, 0, 2);
	if (!header || nla_put_u32(skb, 1, atomic_inc_return(&thp->request_id)) ||
	    nla_put_u8(skb, 2, 0) || nla_put_u8(skb, 4, type) ||
	    nla_put(skb, 5, size, data)) {
		nlmsg_free(skb);
		return -EMSGSIZE;
	}
	genlmsg_end(skb, header);
	return genlmsg_unicast(&init_net, skb, portid);
}
/* Stock screen_show reads the physical driver state, not the last request. */
static ssize_t screen_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct goodix_thp *thp = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&thp->bus_lock);
	ret = thp->dead ? -ENODEV :
		sysfs_emit(buf, "%s\n", thp->state ? "off" : "on");
	mutex_unlock(&thp->bus_lock);
	return ret;
}

static ssize_t screen_state_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct goodix_thp *thp = dev_get_drvdata(dev);
	u8 on;
	int value, ret;

	ret = kstrtoint(buf, 10, &value);
	if (ret)
		return ret;
	if (value != 0 && value != 1)
		return -EINVAL;
	on = value;
	mutex_lock(&thp->bus_lock);
	if (thp->dead)
		ret = -ENODEV;
	else if (!thp->ready || !thp->hal_ready)
		ret = -EAGAIN;
	else
		/* Reconcile the host only; never suspend/reset the controller. */
		ret = thp_notify(thp, 5, &on, sizeof(on));
	mutex_unlock(&thp->bus_lock);
	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_RW(screen_state);

static int thp_control_send(struct thp_control *control, int value)
{
	u8 command[8] = { 0, 0, 6, 0xce, control->command, 0, 0, 0 };
	int size = sizeof(command);
	u16 checksum;

	if (control->command == 0xa4) {
		size = 7;
		command[2] = 4;
		command[3] = 0xa4;
		command[4] = !value;
		command[5] = 0xa9 + !value;
	} else {
		command[5] = control->command == 12 ? value != 2 : value;
		checksum = command[2] + command[3] + command[4] + command[5];
		put_unaligned_le16(checksum, command + 6);
	}
	return thp_notify(control->thp, 1, command, size);
}
/* Stock charge_mode_work/first_charger_mode use usb ONLINE and AF packets. */
static void thp_mode_work(struct work_struct *work)
{
	struct goodix_thp *thp = container_of(to_delayed_work(work),
					struct goodix_thp, mode_work);
	struct power_supply *usb;
	union power_supply_propval online;
	u8 packet[7] = { 0, 0, 5, 0xaf, 0, 0xb4, 0 };
	bool retry = false, replay;
	int i, ret;

	mutex_lock(&thp->bus_lock);
	if (thp->dead || thp->pm_suspended || !thp->ready ||
	    !thp->hal_ready || thp->state)
		goto unlock;
	replay = thp->restore_modes;
	if (replay) {
		for (i = 0; i < ARRAY_SIZE(thp_controls); i++) {
			struct thp_control *control = &thp_controls[i];

			if (!control->valid || control->command == 0xa6 ||
			    control->command == 0xa7)
				continue;
			if (thp_control_send(control, control->value))
				retry = true;
		}
		thp->restore_modes = retry;
		thp->charger_sent = -1;
	}
	usb = power_supply_get_by_name("usb");
	if (!usb) {
		retry = true;
		goto unlock;
	}
	ret = power_supply_get_property(usb, POWER_SUPPLY_PROP_ONLINE, &online);
	power_supply_put(usb);
	if (ret) {
		retry = true;
		goto unlock;
	}
	online.intval = !!online.intval;
	if (thp->charger_sent != online.intval) {
		packet[4] = online.intval;
		packet[5] += online.intval;
		if (thp_notify(thp, 1, packet, sizeof(packet)))
			retry = true;
		else
			thp->charger_sent = online.intval;
	}
unlock:
	if (retry && !thp->dead && !thp->pm_suspended)
		mod_delayed_work(system_wq, &thp->mode_work, HZ);
	mutex_unlock(&thp->bus_lock);
}
static int thp_charger_event(struct notifier_block *notifier,
			    unsigned long event, void *data)
{
	struct goodix_thp *thp = container_of(notifier, struct goodix_thp,
					 charger_notifier);
	struct power_supply *supply = data;

	if (event == PSY_EVENT_PROP_CHANGED && supply &&
	    !strcmp(supply->desc->name, "usb") && READ_ONCE(thp->ready) &&
	    !READ_ONCE(thp->dead) && !READ_ONCE(thp->pm_suspended))
		mod_delayed_work(system_wq, &thp->mode_work, 0);
	return NOTIFY_OK;
}
static int thp_control_show(struct seq_file *seq, void *unused)
{
	struct thp_control *control = seq->private;

	seq_printf(seq, "%d\n", READ_ONCE(control->value));
	return 0;
}
static int thp_control_open(struct inode *inode, struct file *file)
{
	return single_open(file, thp_control_show, pde_data(inode));
}
static ssize_t thp_control_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *offset)
{
	struct thp_control *control = pde_data(file_inode(file));
	struct goodix_thp *thp = control->thp;
	char text[8] = { 0 };
	int value, ret;

	if (!count || count >= sizeof(text))
		return -EINVAL;
	if (copy_from_user(text, buffer, count))
		return -EFAULT;
	ret = kstrtoint(strim(text), 10, &value);
	if (ret)
		return ret;
	mutex_lock(&thp->bus_lock);
	if (thp->dead || !thp->ready) {
		ret = -ENODEV;
	} else if (thp->pm_suspended) {
		ret = -EBUSY;
	} else if (control->command == 0xa6 || control->command == 0xa7) {
		u32 mask = control->command == 0xa6 ? BIT(7) : BIT(14);

		if (thp->state && !thp->wake_enabled) {
			ret = -ENODEV;
		} else {
			if (control->command == 0xa7 ? value == 1 : !!value)
				thp->gesture_mask |= mask;
			else
				thp->gesture_mask &= ~mask;
			ret = thp->wake_enabled ?
				thp_send_command(thp, 0xa6, ~thp->gesture_mask) : 0;
		}
	} else {
		ret = thp_control_send(control, value);
	}
	if (!ret) {
		WRITE_ONCE(control->value, value);
		control->valid = true;
	}
	mutex_unlock(&thp->bus_lock);
	return ret ?: count;
}
static const struct proc_ops thp_control_ops = {
	.proc_open = thp_control_open, .proc_read = seq_read,
	.proc_lseek = seq_lseek, .proc_release = single_release,
	.proc_write = thp_control_write,
};
static void thp_controls_remove(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(thp_controls); i++) {
		proc_remove(thp_controls[i].entry);
		thp_controls[i].entry = NULL;
	}
}
static int thp_controls_create(struct goodix_thp *thp)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(thp_controls); i++) {
		thp_controls[i].thp = thp;
		thp_controls[i].valid = false;
		thp_controls[i].value = 0;
		thp_controls[i].entry = proc_create_data(thp_controls[i].name,
			0660, NULL, &thp_control_ops, &thp_controls[i]);
		if (!thp_controls[i].entry) {
			thp_controls_remove();
			return -ENOMEM;
		}
	}
	return 0;
}
/* Suspend the controller before notifying the host of panel blank. */
static void thp_suspend_work(struct work_struct *work)
{
	struct goodix_thp *thp = container_of(to_delayed_work(work),
					struct goodix_thp, suspend_work);
	u8 on = 0;
	int ret;

	if (READ_ONCE(thp->dead) || READ_ONCE(thp->pm_suspended))
		return;
	ret = thp_sleep(thp, true);
	if (ret)
		dev_err(&thp->spi->dev, "THP gesture suspend failed: %d\n", ret);
	thp_notify(thp, 5, &on, sizeof(on));
}
/* Notify the host before resuming the controller. */
static void thp_resume_work(struct work_struct *work)
{
	struct goodix_thp *thp = container_of(to_delayed_work(work),
					struct goodix_thp, resume_work);
	u8 on = 1;
	int ret;

	if (READ_ONCE(thp->dead) || READ_ONCE(thp->pm_suspended))
		return;
	thp_notify(thp, 5, &on, sizeof(on));
	if (READ_ONCE(thp->state)) {
		ret = thp_sleep(thp, false);
		if (ret)
			dev_err(&thp->spi->dev, "THP gesture resume failed: %d\n", ret);
	}
}
static void thp_panel_event(enum panel_event_notifier_tag tag,
		struct panel_event_notification *event, void *data)
{
	struct goodix_thp *thp = data;

	if (!READ_ONCE(thp->hal_ready) || READ_ONCE(thp->dead))
		return;
	/* Cancel the opposite transition; keep normal reporting during doze. */
	if (event->notif_type == DRM_PANEL_EVENT_BLANK &&
	    event->notif_data.early_trigger) {
		WRITE_ONCE(thp->panel_blank, true);
		cancel_delayed_work_sync(&thp->resume_work);
		schedule_delayed_work(&thp->suspend_work, 0);
	} else if ((event->notif_type == DRM_PANEL_EVENT_UNBLANK ||
		    event->notif_type == DRM_PANEL_EVENT_BLANK_LP) &&
		   !event->notif_data.early_trigger) {
		WRITE_ONCE(thp->panel_blank, false);
		cancel_delayed_work_sync(&thp->suspend_work);
		schedule_delayed_work(&thp->resume_work, 0);
	}
}
static void thp_panel_work(struct work_struct *work)
{
	struct goodix_thp *thp = container_of(to_delayed_work(work),
					struct goodix_thp, panel_work);
	struct drm_panel *panel = lenovo_find_panel(thp->spi->dev.of_node);

	if (!IS_ERR(panel))
		thp->panel_cookie = panel_event_notifier_register(
			PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_TOUCH,
			panel, thp_panel_event, thp);
	if (IS_ERR_OR_NULL(thp->panel_cookie) && !READ_ONCE(thp->dead))
		schedule_delayed_work(&thp->panel_work, HZ);
}
static void thp_free(struct kref *ref)
{
	struct goodix_thp *thp = container_of(ref, struct goodix_thp, ref);

	kfree(thp->rx);
	kfree(thp->tx);
	if (thp->ring)
		free_pages_exact(thp->ring, THP_RING_SIZE);
	kfree(thp);
}
/* Caller holds bus_lock. SPI command addresses are big-endian. */
static int thp_read_register(struct goodix_thp *thp, u32 address,
			     u8 *buffer, u32 size)
{
	int ret;
	struct spi_transfer transfer = {
		.tx_buf = thp->tx, .rx_buf = thp->rx, .len = size + 8,
	};

	if (thp->pm_suspended || !thp->powered)
		return -EHOSTDOWN;
	if (!size || size > THP_SLOT_SIZE)
		return -EINVAL;
	memset(thp->tx, 0xff, size + 8);
	thp->tx[0] = 0xf1;
	put_unaligned_be32(address, thp->tx + 1);
	ret = spi_sync_transfer(thp->spi, &transfer, 1);
	if (!ret)
		memcpy(buffer, thp->rx + 8, size);
	return ret;
}
/* Caller holds bus_lock, which owns the shared SPI transfer buffers. */
static int thp_write_register(struct goodix_thp *thp, u32 address,
			      const u8 *data, u32 size)
{
	struct spi_transfer transfer = { .tx_buf = thp->tx, .len = size + 5 };

	if (thp->pm_suspended || !thp->powered)
		return -EHOSTDOWN;
	if (!size || size > THP_SLOT_SIZE)
		return -EINVAL;
	thp->tx[0] = 0xf0;
	put_unaligned_be32(address, thp->tx + 1);
	memcpy(thp->tx + 5, data, size);
	return spi_sync_transfer(thp->spi, &transfer, 1);
}
static int thp_send_command(struct goodix_thp *thp, u8 cmd, u16 data)
{
	u8 packet[8] = { 0, 0, 6, cmd, data, data >> 8 }, reply[8];
	int attempt, poll, ret;

	if (!thp->command_address)
		return -EINVAL;
	put_unaligned_le16(6 + cmd + (data & 0xff) + (data >> 8), packet + 6);
	for (attempt = 0; attempt < 6; attempt++) {
		ret = thp_write_register(thp, thp->command_address, packet, sizeof(packet));
		if (ret)
			return ret;
		for (poll = 0; poll < 5; poll++) {
			ret = thp_read_register(thp, thp->command_address, reply, sizeof(reply));
			if (ret)
				break;
			if (reply[1] == 0x80) {
				usleep_range(10000, 11000);
				return 0;
			}
			if (reply[1] == 1)
				break;
			usleep_range(10000, 11000);
		}
	}
	return -ETIMEDOUT;
}
static void thp_gesture_irq(struct goodix_thp *thp)
{
	u8 event[42], clear = 0;
	u16 sum;
	int ret, key = 0;

	if (!thp->gesture_address)
		return;
	msleep(50);
	ret = thp_read_register(thp, thp->gesture_address, event, sizeof(event));
	if (!ret && (event[0] & BIT(5))) {
		sum = event[0] + event[1] + event[2] + event[3] + event[4] + event[5];
		if (sum == get_unaligned_le16(event + 6)) {
			if (event[4] == 0xcc)
				key = KEY_WAKEUP;
			else if (event[4] == 0xdd)
				key = KEY_POWER;
		}
	}
	if (key) {
		struct input_dev *input = key == KEY_WAKEUP ? thp->fingers : thp->pen;

		pm_wakeup_event(&thp->spi->dev, 1000);
		mutex_lock(&thp->input_lock);
		if (key == KEY_POWER)
			input_event(input, EV_MSC, MSC_SCAN, 0xc0604);
		input_report_key(input, key, 1);
		input_sync(input);
		if (key == KEY_POWER)
			input_event(input, EV_MSC, MSC_SCAN, 0xc0604);
		input_report_key(input, key, 0);
		input_sync(input);
		mutex_unlock(&thp->input_lock);
	} else {
		thp_send_command(thp, 0xa6, ~thp->gesture_mask);
	}
	thp_write_register(thp, thp->gesture_address, &clear, 1);
}
static bool thp_checksum(const u8 *data, u32 size)
{
	u16 sum = 0;
	u32 i;

	for (i = 0; i < size - 2; i++)
		sum += data[i];
	return sum == get_unaligned_le16(data + size - 2);
}
/* The shipping updater uses 512-byte headers and ten-byte subsystem records. */
static bool thp_version_valid(const u8 *version)
{
	return memchr_inv(version, 0, 28) && memchr_inv(version, 0xff, 28) &&
		thp_checksum(version, 28);
}
static bool thp_firmware_valid(const struct firmware *fw, bool config)
{
	u32 sum = 0;
	size_t offset;

	if (fw->size < 512 || fw->size > U32_MAX || (fw->size & 1) ||
	    get_unaligned_le32(fw->data) != fw->size - (config ? 0 : 8))
		return false;
	for (offset = 8; offset < fw->size; offset += 2)
		sum += get_unaligned_le16(fw->data + offset);
	return sum == get_unaligned_le32(fw->data + 4);
}
static int thp_write_confirm(struct goodix_thp *thp, u32 address,
			     const u8 *data, u32 size)
{
	u8 *check;
	u32 chunk;
	int ret = 0, attempt;

	check = kmalloc(4096, GFP_KERNEL);
	if (!check)
		return -ENOMEM;
	while (size) {
		chunk = min_t(u32, size, 4096);
		for (attempt = 0; attempt < 3; attempt++) {
			ret = thp_write_register(thp, address, data, chunk);
			if (!ret)
				ret = thp_read_register(thp, address, check, chunk);
			if (!ret && !memcmp(check, data, chunk))
				break;
			ret = ret ?: -EIO;
		}
		if (ret)
			break;
		address += chunk;
		data += chunk;
		size -= chunk;
	}
	kfree(check);
	return ret;
}
static void thp_reset_controller(struct goodix_thp *thp, int delay_ms)
{
	gpiod_set_raw_value_cansleep(thp->reset, 0);
	usleep_range(5000, 5500);
	gpiod_set_raw_value_cansleep(thp->reset, 1);
	msleep(delay_ms);
}
static int thp_start_isp(struct goodix_thp *thp, const u8 *data, u32 size)
{
	static const u32 registers[] = { 0xd800, 0xd804, 0xd000, 0xc500, 0xc4a0 };
	static const u32 values[] = { 0x26011002, 0xffffffff, 0, 0, 0x032ab903 };
	u8 buffer[28], stable[12], hold[2] = { 1, 0 };
	int ret, attempt, i;

	if (!allow_fw_update)
		return -EPERM;
	thp_reset_controller(thp, 20);
	for (attempt = 0; attempt < 100; attempt++) {
		ret = thp_write_register(thp, 2, hold, sizeof(hold));
		for (i = 0; !ret && i < ARRAY_SIZE(registers); i++) {
			put_unaligned_le32(values[i], buffer);
			ret = thp_write_register(thp, registers[i], buffer,
				(i == 2 || i == 3) ? 1 : 4);
		}
		for (i = 0; !ret && i < 3; i++)
			ret = thp_read_register(thp, 0x2000, stable + i * 4, 4);
		if (!ret && !memcmp(stable, stable + 4, 4) &&
		    !memcmp(stable, stable + 8, 4))
			break;
		usleep_range(1000, 1100);
	}
	if (attempt == 100)
		return ret ?: -ETIMEDOUT;
	ret = thp_write_confirm(thp, 0x3e800, data, size);
	if (ret)
		return ret;
	memset(buffer, 0x55, 8);
	ret = thp_write_confirm(thp, 0x10000, buffer, 8);
	if (ret)
		return ret;
	thp_reset_controller(thp, 100);
	ret = thp_read_register(thp, 0x10014, buffer, sizeof(buffer));
	if (!ret && memcmp(buffer + 13, "ISP", 3))
		ret = -EIO;
	return ret;
}
static int thp_flash_packet(struct goodix_thp *thp, u8 type, u32 address,
			    const u8 *data, u32 size)
{
	u8 command[16] = { 0, 0, 11, 0xbb, type }, response[16];
	u8 *packet;
	u32 sum = 0, i;
	int ret, attempt, poll;

	if (!allow_fw_update)
		return -EPERM;
	/* Firmware sections are aligned; never read past an odd final word. */
	if (!size || size > 4096 || (size & 1))
		return -EINVAL;
	packet = kmalloc(size + 4, GFP_KERNEL);
	if (!packet)
		return -ENOMEM;
	memcpy(packet, data, size);
	for (i = 0; i < size; i += 2)
		sum += get_unaligned_le16(data + i);
	put_unaligned_le32(sum, packet + size);
	put_unaligned_le16(size + 4, command + 5);
	put_unaligned_le32(address, command + 7);
	sum = 0;
	for (i = 2; i < 11; i++)
		sum += command[i];
	put_unaligned_le16(sum, command + 11);
	for (attempt = 0; attempt < 2; attempt++) {
		ret = thp_write_register(thp, 0x14810, packet, size + 4);
		if (!ret)
			ret = thp_write_confirm(thp, 0x14800, command, 2);
		if (!ret)
			ret = thp_write_register(thp, 0x14802, command + 2, 14);
		if (ret)
			continue;
		for (poll = 0; poll < 6; poll++) {
			ret = thp_read_register(thp, 0x14800, response, sizeof(response));
			if (!ret && response[1] == 0xee)
				break;
			usleep_range(5000, 5100);
		}
		if (poll == 6) {
			ret = ret ?: -ETIMEDOUT;
			continue;
		}
		msleep(50);
		for (poll = 0; poll < 20; poll++) {
			ret = thp_read_register(thp, 0x14800, response, sizeof(response));
			if (!ret && response[0] == 0xee && response[1] == 0xee)
				break;
			usleep_range(10000, 11000);
		}
		if (poll < 20)
			break;
		ret = ret ?: -ETIMEDOUT;
	}
	kfree(packet);
	return ret;
}
static int thp_flash_section(struct goodix_thp *thp, u8 type, u32 address,
			     const u8 *data, u32 size)
{
	u32 chunk;
	int ret;

	if (size & 1 || size > U32_MAX - address)
		return -EINVAL;
	while (size) {
		chunk = min_t(u32, size, 4096);
		ret = thp_flash_packet(thp, type, address, data, chunk);
		if (ret)
			return ret;
		address += chunk;
		data += chunk;
		size -= chunk;
	}
	return 0;
}
static int thp_config_id(struct goodix_thp *thp, u32 *id)
{
	/* Vendor AFE reads the complete 512-byte version-2 IC information. */
	u8 info[512];
	u16 length;
	u32 descriptor;
	int ret, offset = 12, size;

	ret = thp_read_register(thp, 0x10070, info, sizeof(info));
	if (ret)
		return ret;
	length = get_unaligned_le16(info);
	if (length < 14 || length > sizeof(info) || !thp_checksum(info, length))
		return -EBADMSG;
	while (offset + 4 <= length - 2) {
		descriptor = get_unaligned_le32(info + offset);
		size = descriptor & 0xff;
		if (size < 4 || size > length - 2 - offset)
			return -EBADMSG;
		if (!(descriptor >> 20)) {
			if (size < 8)
				return -EBADMSG;
			*id = get_unaligned_le32(info + offset + 4);
			return 0;
		}
		offset += size;
	}
	return -ENODATA;
}
static int thp_expected_config_id(const struct firmware *config, u32 *id)
{
	u32 offset, size;
	int i;

	if (config->data[12] > (config->size - 64) / 10)
		return -EBADMSG;
	for (i = 0; i < config->data[12]; i++) {
		const u8 *record = config->data + 64 + i * 10;

		if (get_unaligned_le16(record))
			continue;
		offset = get_unaligned_le32(record + 2);
		size = get_unaligned_le32(record + 6);
		if (offset > config->size || size > config->size - offset || size < 34)
			return -EBADMSG;
		*id = get_unaligned_le32(config->data + offset + 30);
		return 0;
	}
	return -ENODATA;
}
/* Probe is unpublished and IRQ is not registered while flash is programmed. */
static int thp_prepare_firmware(struct goodix_thp *thp)
{
	const struct firmware *fw = NULL, *config = NULL;
	u8 version[28] = { 0 }, count;
	u32 size, address, resident_config, expected_config;
	bool version_matches;
	size_t offset = 512;
	int ret, i;

	/*
	 * Stock compares firmware in delayed update work after publishing THP.
	 * Retry boot-time reads without changing SPI framing or checksum rules.
	 */
	for (i = 0; i < 3; i++) {
		ret = thp_read_register(thp, 0x10014, version, sizeof(version));
		if (!ret && thp_version_valid(version))
			break;
		ret = ret ?: -EBADMSG;
		if (i < 2)
			msleep(20);
	}
	if (ret)
		dev_warn(&thp->spi->dev,
			 "resident version read failed: %d; raw %28phN\n",
			 ret, version);
	if (!allow_fw_update) {
		/*
		 * A bad resident version/config must not remove the host ABI.
		 * Stock update work ignores updater errors; thpd supplies registers.
		 * No ISP or flash operation is reachable in this mode.
		 */
		if (ret)
			return 0;
		ret = thp_config_id(thp, &resident_config);
		if (ret)
			dev_warn(&thp->spi->dev,
				 "resident config read failed: %d; flash updates disabled\n",
				 ret);
		else
			dev_info(&thp->spi->dev,
				 "resident PID %8phN VID %4phN config %#x; flash updates disabled\n",
				 version + 10, version + 18, resident_config);
		return 0;
	}
	/* Missing update files are acceptable only with valid resident firmware. */
	if (request_firmware(&fw, "goodix_firmware.bin", &thp->spi->dev))
		return ret;
	if (!thp_firmware_valid(fw, false)) {
		ret = -EBADMSG;
		goto out;
	}
	count = fw->data[29];
	if (!count || count > (512 - 42) / 10) {
		ret = -EINVAL;
		goto out;
	}
	/* Validate every payload before modifying any controller register. */
	for (i = 0; i < count; i++) {
		size = get_unaligned_le32(fw->data + 43 + i * 10);
		address = get_unaligned_le32(fw->data + 47 + i * 10);
		if (size > fw->size - offset || (size & 1) ||
		    (i && size > U32_MAX - address)) {
			ret = -EINVAL;
			goto out;
		}
		offset += size;
	}
	if (offset != fw->size || fw->data[42] != 1) {
		ret = -EINVAL;
		goto out;
	}
	version_matches = !ret && !memcmp(version + 10, fw->data + 17, 8) &&
		!memcmp(version + 18, fw->data + 25, 4);
	ret = request_firmware(&config, "goodix_config.bin", &thp->spi->dev);
	if (ret) {
		config = NULL;
		if (version_matches) {
			ret = 0;
			goto out;
		}
	} else {
		if (!thp_firmware_valid(config, true)) {
			ret = -EBADMSG;
			goto out;
		}
		ret = thp_expected_config_id(config, &expected_config);
		if (ret)
			goto out;
		if (version_matches && !thp_config_id(thp, &resident_config) &&
		    resident_config == expected_config) {
			ret = 0;
			goto out;
		}
	}
	ret = thp_start_isp(thp, fw->data + 512, get_unaligned_le32(fw->data + 43));
	if (ret)
		goto reset;
	if (config) {
		ret = thp_flash_section(thp, 4, 0x5c000, config->data, config->size);
		if (ret)
			goto reset;
	}
	offset = 512 + get_unaligned_le32(fw->data + 43);
	for (i = 1; i < count; i++) {
		size = get_unaligned_le32(fw->data + 43 + i * 10);
		address = get_unaligned_le32(fw->data + 47 + i * 10);
		ret = thp_flash_section(thp, fw->data[42 + i * 10], address,
					fw->data + offset, size);
		if (ret)
			break;
		offset += size;
	}
reset:
	thp_reset_controller(thp, 200);
	if (!ret) {
		ret = thp_read_register(thp, 0x10014, version, sizeof(version));
		if (!ret && (!thp_version_valid(version) ||
		    memcmp(version + 10, fw->data + 17, 8) ||
		    memcmp(version + 18, fw->data + 25, 4)))
			ret = -EBADMSG;
		if (!ret && config) {
			ret = thp_config_id(thp, &resident_config);
			if (!ret && resident_config != expected_config)
				ret = -EBADMSG;
		}
	}
out:
	release_firmware(config);
	release_firmware(fw);
	return ret;
}
static irqreturn_t thp_irq(int irq, void *data)
{
	struct goodix_thp *thp = data;
	u8 header[16] = { 0 }, *slot;
	u32 length;
	int ret;

	mutex_lock(&thp->bus_lock);
	if (thp->dead)
		goto unlock;
	if (thp->pm_suspended) {
		/* Do not block suspend_device_irqs() in synchronize_irq(). */
		thp->irq_pending = true;
		pm_wakeup_event(&thp->spi->dev, 0);
		goto unlock;
	}
	if (thp->state) {
		if (thp->wake_enabled)
			thp_gesture_irq(thp);
		goto unlock;
	}
	thp->irq_seen = true;
	if (!thp->frame_address)
		goto unlock;
	ret = thp_read_register(thp, thp->frame_address, header, sizeof(header));
	length = get_unaligned_le16(header + 3);
	if (ret || !thp_checksum(header, sizeof(header)) ||
	    length < sizeof(header) || length > THP_FRAME_MAX)
		goto unlock;
	mutex_lock(&thp->ring_lock);
	/* Do not overwrite a frame still mapped by the host. */
	if (thp->count < THP_SLOTS) {
		slot = thp->ring + thp->write_slot * THP_SLOT_SIZE;
		ret = thp_read_register(thp, thp->frame_address,
					slot + sizeof(u64), length);
		if (!ret) {
			put_unaligned_le32(++thp->sequence, slot);
			put_unaligned_le32(length, slot + 4);
			thp->write_slot = (thp->write_slot + 1) % THP_SLOTS;
			thp->count++;
			wake_up_interruptible(&thp->frames);
		}
	}
	mutex_unlock(&thp->ring_lock);
unlock:
	mutex_unlock(&thp->bus_lock);
	return IRQ_HANDLED;
}
static int thp_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct goodix_thp *thp = container_of(misc, struct goodix_thp, touch);

	if (!READ_ONCE(thp->ready) || READ_ONCE(thp->dead))
		return -ENODEV;
	kref_get(&thp->ref);
	file->private_data = thp;
	return 0;
}
static int thp_agent_open(struct inode *inode, struct file *file)
{
	struct goodix_thp *thp = container_of(file->private_data,
					 struct goodix_thp, agent);

	if (!READ_ONCE(thp->ready) || READ_ONCE(thp->dead))
		return -ENODEV;
	kref_get(&thp->ref);
	file->private_data = thp;
	return 0;
}
static int thp_release(struct inode *inode, struct file *file)
{
	kref_put(&((struct goodix_thp *)file->private_data)->ref, thp_free);
	return 0;
}
static int thp_penraw_open(struct inode *inode, struct file *file)
{
	struct goodix_thp *thp = container_of(file->private_data,
					 struct goodix_thp, penraw);

	if (!READ_ONCE(thp->ready) || READ_ONCE(thp->dead))
		return -ENODEV;
	kref_get(&thp->ref);
	file->private_data = thp;
	return 0;
}
static long thp_penraw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct goodix_thp *thp = file->private_data;
	u8 snapshot[648] = { 0 };
	int i, start;

	if (cmd != 0x82885000)
		return -EFAULT;
	mutex_lock(&thp->input_lock);
	if (thp->dead) {
		mutex_unlock(&thp->input_lock);
		return -ENODEV;
	}
	snapshot[0] = thp->pen_count;
	start = thp->pen_count == 16 ? (thp->pen_write + 16) % 32 : 0;
	for (i = 0; i < thp->pen_count; i++)
		memcpy(snapshot + 8 + i * 40, thp->pen_samples[(start + i) % 32], 40);
	mutex_unlock(&thp->input_lock);
	return copy_to_user((void __user *)arg, snapshot, sizeof(snapshot)) ? -EFAULT : 0;
}
/* Caller holds bus_lock. Preserve the stock framework's exact DEVPATH/env ABI. */
static void thp_pen_uevent(struct goodix_thp *thp)
{
	char info[64], mac[32];
	char *env[5] = { "TOUCH_INFORMATION=08", "UEVENT_TO=PEN_FRAMEWORK TYPE=TP",
		info, mac, NULL };

	snprintf(info, sizeof(info), "INFO=%d;%d;0xffff;0xffff;0x3fffff",
		 thp->pen_identity[0], thp->pen_identity[1]);
	snprintf(mac, sizeof(mac), "MAC=%02x:%02x:%02x:%02x:%02x:%02x",
		 thp->pen_mac[5], thp->pen_mac[4], thp->pen_mac[3],
		 thp->pen_mac[2], thp->pen_mac[1], thp->pen_mac[0]);
	kobject_uevent_env(&thp->pen_info->dev.kobj, KOBJ_CHANGE, env);
}
static int thp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct goodix_thp *thp = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (thp->dead)
		return -ENODEV;
	if (vma->vm_pgoff || size != THP_RING_SIZE)
		return -EINVAL;
	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP);
	return remap_pfn_range(vma, vma->vm_start,
		page_to_pfn(virt_to_page(thp->ring)), size, vma->vm_page_prot);
}
static void thp_clear_frames(struct goodix_thp *thp)
{
	mutex_lock(&thp->ring_lock);
	thp->read_slot = thp->write_slot = thp->count = 0;
	mutex_unlock(&thp->ring_lock);
}
static void thp_release_inputs(struct goodix_thp *thp)
{
	int i;

	mutex_lock(&thp->input_lock);
	for (i = 0; i < 10; i++) {
		input_mt_slot(thp->fingers, i);
		input_mt_report_slot_state(thp->fingers, MT_TOOL_FINGER, false);
	}
	input_report_key(thp->fingers, BTN_TOUCH, 0);
	input_report_key(thp->fingers, BTN_TOOL_FINGER, 0);
	input_mt_sync_frame(thp->fingers);
	input_sync(thp->fingers);
	input_report_key(thp->pen, BTN_TOUCH, 0);
	input_report_key(thp->pen, BTN_TOOL_PEN, 0);
	input_report_key(thp->pen, BTN_TOOL_RUBBER, 0);
	input_report_key(thp->pen, BTN_STYLUS, 0);
	input_report_key(thp->pen, BTN_STYLUS2, 0);
	input_report_key(thp->pen, BTN_STYLUS3, 0);
	input_sync(thp->pen);
	mutex_unlock(&thp->input_lock);
}
/* Track each rail independently so a partial failure cannot unbalance refs. */
static int thp_power(struct goodix_thp *thp, bool on)
{
	int ret, first = 0;

	if (on) {
		if (!thp->iovdd_enabled) {
			ret = regulator_enable(thp->iovdd);
			if (ret)
				return ret;
			thp->iovdd_enabled = true;
		}
		if (!IS_ERR(thp->avdd) && !thp->avdd_enabled) {
			ret = regulator_enable(thp->avdd);
			if (ret)
				return ret;
			thp->avdd_enabled = true;
		}
		thp->powered = true;
		return 0;
	}
	thp->powered = false;
	if (thp->avdd_enabled) {
		ret = regulator_disable(thp->avdd);
		if (ret)
			first = ret;
		else
			thp->avdd_enabled = false;
	}
	if (thp->iovdd_enabled) {
		ret = regulator_disable(thp->iovdd);
		if (ret && !first)
			first = ret;
		else if (!ret)
			thp->iovdd_enabled = false;
	}
	return first;
}
static int thp_reinit(struct goodix_thp *thp)
{
	int ret;

	mutex_lock(&thp->control_lock);
	if (READ_ONCE(thp->dead)) {
		ret = -ENODEV;
		goto out;
	}
	if (READ_ONCE(thp->pm_suspended)) {
		ret = -EBUSY;
		goto out;
	}
	if (thp->irq_enabled) {
		disable_irq(thp->irq);
		thp->irq_enabled = false;
	}
	mutex_lock(&thp->bus_lock);
	if (thp->wake_enabled) {
		disable_irq_wake(thp->irq);
		thp->wake_enabled = false;
	}
	ret = thp_power(thp, true);
	if (!ret)
		ret = pinctrl_select_state(thp->pins, thp->active_pins);
	if (!ret) {
		gpiod_set_raw_value_cansleep(thp->reset, 0);
		usleep_range(5000, 5500);
		gpiod_set_raw_value_cansleep(thp->reset, 1);
		msleep(200);
		thp->hal_ready = false;
		thp->restore_modes = true;
		thp->state = thp->state_changed = 0;
		thp_clear_frames(thp);
		thp_release_inputs(thp);
	}
	mutex_unlock(&thp->bus_lock);
out:
	mutex_unlock(&thp->control_lock);
	return ret;
}
/* The control_lock serializes IRQ transitions; bus_lock excludes SPI during power. */
static int thp_sleep_locked(struct goodix_thp *thp, bool suspend)
{
	int ret = 0;

	if (READ_ONCE(thp->dead)) {
		ret = -ENODEV;
		goto out;
	}
	if (READ_ONCE(thp->pm_suspended)) {
		ret = -EBUSY;
		goto out;
	}
	if (!!READ_ONCE(thp->state) == suspend)
		goto out;
	if (thp->irq_enabled) {
		disable_irq(thp->irq);
		thp->irq_enabled = false;
	}
	mutex_lock(&thp->bus_lock);
	if (suspend && thp->gesture_mask) {
		ret = thp_send_command(thp, 0xa6, ~thp->gesture_mask);
		if (!ret) {
			ret = enable_irq_wake(thp->irq);
			if (!ret) {
				thp->wake_enabled = true;
				thp->state = 1;
			}
		}
	} else if (suspend) {
		thp->state = 1;
		gpiod_set_raw_value_cansleep(thp->reset, 0);
		usleep_range(2000, 2100);
		ret = pinctrl_select_state(thp->pins, thp->sleep_pins);
		if (!ret)
			ret = thp_power(thp, false);
	} else {
		if (thp->wake_enabled) {
			disable_irq_wake(thp->irq);
			thp->wake_enabled = false;
			gpiod_set_raw_value_cansleep(thp->reset, 0);
			usleep_range(5000, 5500);
		}
		ret = thp_power(thp, true);
		if (ret)
			goto unlock;
		ret = pinctrl_select_state(thp->pins, thp->active_pins);
		if (ret)
			goto unlock;
		usleep_range(15000, 16000);
		gpiod_set_raw_value_cansleep(thp->reset, 1);
		msleep(200);
		thp_clear_frames(thp);
		thp->state = 0;
		thp->restore_modes = true;
		mod_delayed_work(system_wq, &thp->mode_work, 0);
	}
	thp->state_changed = 1;
unlock:
	mutex_unlock(&thp->bus_lock);
	if (suspend)
		thp_release_inputs(thp);
	if ((!suspend && !ret) || thp->wake_enabled || !thp->state) {
		enable_irq(thp->irq);
		thp->irq_enabled = true;
	}
out:
	return ret;
}
static int thp_sleep(struct goodix_thp *thp, bool suspend)
{
	int ret;

	mutex_lock(&thp->control_lock);
	ret = thp_sleep_locked(thp, suspend);
	mutex_unlock(&thp->control_lock);
	return ret;
}
/* Serialize panel transitions with system suspend and resume. */
static int thp_pm_suspend(struct device *dev)
{
	struct goodix_thp *thp = dev_get_drvdata(dev);
	u8 on = 0;
	int ret;

	cancel_delayed_work_sync(&thp->resume_work);
	cancel_delayed_work_sync(&thp->suspend_work);
	mutex_lock(&thp->control_lock);
	ret = thp_sleep_locked(thp, true);
	if (!ret) {
		mutex_lock(&thp->bus_lock);
		thp->pm_suspended = true;
		thp_notify(thp, 5, &on, sizeof(on));
		mutex_unlock(&thp->bus_lock);
	}
	mutex_unlock(&thp->control_lock);
	if (ret)
		return ret;
	cancel_delayed_work_sync(&thp->esd_work);
	cancel_delayed_work_sync(&thp->mode_work);
	return 0;
}
static int thp_pm_resume(struct device *dev)
{
	struct goodix_thp *thp = dev_get_drvdata(dev);
	u8 on = 1;
	int ret = 0;

	mutex_lock(&thp->control_lock);
	mutex_lock(&thp->bus_lock);
	thp->pm_suspended = false;
	/* The SPI parent is now awake; decode before resetting the controller. */
	if (thp->irq_pending) {
		thp->irq_pending = false;
		if (thp->wake_enabled)
			thp_gesture_irq(thp);
	}
	mutex_unlock(&thp->bus_lock);
	if (!READ_ONCE(thp->panel_blank)) {
		/* Preserve normal reporting for an ON or BLANK_LP/AOD panel. */
		thp_notify(thp, 5, &on, sizeof(on));
		ret = thp_sleep_locked(thp, false);
	}
	mutex_unlock(&thp->control_lock);
	mod_delayed_work(system_wq, &thp->esd_work, 3 * HZ);
	return ret;
}
static const struct dev_pm_ops thp_pm_ops = {
	.suspend = thp_pm_suspend,
	.resume = thp_pm_resume,
};
static void thp_esd_work(struct work_struct *work)
{
	struct goodix_thp *thp = container_of(to_delayed_work(work),
					struct goodix_thp, esd_work);
	u8 status = 0, feed = 0xaa;
	bool restart_irq;
	int ret;

	mutex_lock(&thp->control_lock);
	mutex_lock(&thp->bus_lock);
	if (thp->dead || thp->pm_suspended || !thp->esd_enabled || !thp->hal_ready ||
	    thp->state || !thp->powered || !thp->status_address || thp->irq_seen)
		goto unlock;
	ret = thp_read_register(thp, thp->status_address, &status, 1);
	if (ret)
		goto unlock;
	if (status != 0xaa) {
		thp_write_register(thp, thp->status_address, &feed, 1);
		goto unlock;
	}
	/* A stale feed byte means firmware stopped consuming the watchdog. */
	restart_irq = thp->irq_enabled;
	mutex_unlock(&thp->bus_lock);
	if (restart_irq) {
		disable_irq(thp->irq);
		thp->irq_enabled = false;
	}
	mutex_lock(&thp->bus_lock);
	gpiod_set_raw_value_cansleep(thp->reset, 0);
	ret = thp_power(thp, false);
	msleep(200);
	if (!ret)
		ret = thp_power(thp, true);
	if (!ret)
		thp_reset_controller(thp, 200);
	thp_clear_frames(thp);
	thp_release_inputs(thp);
	thp->state_changed = 1;
	if (ret)
		dev_err(&thp->spi->dev, "THP ESD power recovery failed: %d\n", ret);
	else if (restart_irq) {
		thp->restore_modes = true;
		mod_delayed_work(system_wq, &thp->mode_work, 0);
		enable_irq(thp->irq);
		thp->irq_enabled = true;
	}
unlock:
	thp->irq_seen = false;
	if (!thp->dead && !thp->pm_suspended)
		schedule_delayed_work(&thp->esd_work, 3 * HZ);
	mutex_unlock(&thp->bus_lock);
	mutex_unlock(&thp->control_lock);
}
static long thp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct goodix_thp *thp = file->private_data;
	void __user *user = (void __user *)arg;
	struct thp_spi_transfer xfer;
	struct thp_host_message message;
	u32 value;
	int ret = 0;

	if (READ_ONCE(thp->dead))
		return -ENODEV;
	switch (cmd) {
	case 0x8004b801:
		if (READ_ONCE(thp->timeout_ms) < 0) {
			ret = wait_event_interruptible(thp->frames,
				READ_ONCE(thp->count) || READ_ONCE(thp->dead));
			if (!ret)
				ret = 1;
		} else {
			ret = wait_event_interruptible_timeout(thp->frames,
				READ_ONCE(thp->count) || READ_ONCE(thp->dead),
				msecs_to_jiffies(READ_ONCE(thp->timeout_ms)));
		}
		if (ret < 0)
			return ret;
		if (thp->dead)
			return -ENODEV;
		if (!ret)
			return -ETIMEDOUT;
		mutex_lock(&thp->ring_lock);
		value = thp->read_slot * THP_SLOT_SIZE;
		ret = thp->count ? put_user(value, (u32 __user *)user) : -ENODATA;
		mutex_unlock(&thp->ring_lock);
		return ret;
	case 0x4004b803:
		WRITE_ONCE(thp->timeout_ms, (int)arg < 0 ? -1 : min_t(int, arg, 5000));
		return 0;
	case 0xb805:
		mutex_lock(&thp->ring_lock);
		if (thp->count) {
			thp->read_slot = (thp->read_slot + 1) % THP_SLOTS;
			thp->count--;
		}
		mutex_unlock(&thp->ring_lock);
		return 0;
	case 0xb809:
		thp_clear_frames(thp);
		return 0;
	case 0x8004b80a:
		return put_user(READ_ONCE(thp->state), (u32 __user *)user);
	case 0x8004b80b:
		return put_user(READ_ONCE(thp->state_changed), (u32 __user *)user);
	case 0x4004b80c:
		WRITE_ONCE(thp->state_changed, !!arg);
		return 0;
	case 0x8004b812:
		return put_user(3, (u32 __user *)user);
	case 0x4004b814:
		value = arg;
		mutex_lock(&thp->bus_lock);
		ret = thp->dead ? -ENODEV : thp_notify(thp, 6, (u8 *)&value, 1);
		mutex_unlock(&thp->bus_lock);
		return ret;
	case 0xb806:
		return thp_reinit(thp);
	case 0x4004b808:
	case 0xc018b80e:
		return 0;
	case 0x4088b811:
		if (copy_from_user(&message, user, sizeof(message)))
			return -EFAULT;
		if (message.size > sizeof(message.data))
			return -EINVAL;
		mutex_lock(&thp->bus_lock);
		if (thp->dead) {
			mutex_unlock(&thp->bus_lock);
			return -ENODEV;
		}
		if (message.command == 100) {
			if (message.size < 16) {
				ret = -EINVAL;
			} else {
				thp->frame_address = get_unaligned_le32(message.data);
				thp->command_address = get_unaligned_le32(message.data + 4);
				thp->gesture_address = get_unaligned_le32(message.data + 8);
				thp->status_address = get_unaligned_le32(message.data + 12);
			}
		} else if (message.command == 101) {
			thp->hal_ready = true;
			thp->restore_modes = true;
			mod_delayed_work(system_wq, &thp->mode_work, 0);
		} else if (message.command == 102) {
			if (message.size < sizeof(thp->pen_mac))
				ret = -EINVAL;
			else {
				memcpy(thp->pen_mac, message.data, sizeof(thp->pen_mac));
				thp_pen_uevent(thp);
			}
		} else if (message.command == 105) {
			if (message.size < 9)
				ret = -EINVAL;
			else {
				memcpy(thp->pen_identity, message.data, 2);
				thp_pen_uevent(thp);
			}
		}
		mutex_unlock(&thp->bus_lock);
		return ret;
	case 0xb80f:
	case 0xb810:
		return thp_sleep(thp, cmd == 0xb80f);
	case 0x4004b807:
		mutex_lock(&thp->control_lock);
		if (READ_ONCE(thp->dead)) {
			mutex_unlock(&thp->control_lock);
			return -ENODEV;
		}
		if (READ_ONCE(thp->pm_suspended)) {
			mutex_unlock(&thp->control_lock);
			return -EBUSY;
		}
		/* Calling disable_irq() must not wait while holding the threaded IRQ mutex. */
		if (arg && !thp->irq_enabled) {
			thp->irq_enabled = true;
			enable_irq(thp->irq);
		} else if (!arg && thp->irq_enabled) {
			thp->irq_enabled = false;
			disable_irq(thp->irq);
		}
		mutex_unlock(&thp->control_lock);
		return 0;
	default:
		break;
	}
	mutex_lock(&thp->bus_lock);
	if (thp->dead) {
		ret = -ENODEV;
		goto unlock;
	}
	if (thp->pm_suspended || !thp->powered) {
		ret = -EHOSTDOWN;
		goto unlock;
	}
	switch (cmd) {
	case 0x4004b802:
		gpiod_set_raw_value_cansleep(thp->reset, !!arg);
		break;
	case 0x4004b80d:
		if (!arg || arg > 30000000) {
			ret = -EINVAL;
			break;
		}
		thp->spi->max_speed_hz = arg;
		ret = spi_setup(thp->spi);
		break;
	case 0xc018b804:
		if (copy_from_user(&xfer, user, sizeof(xfer))) {
			ret = -EFAULT;
			break;
		}
		if (!xfer.size || xfer.size > THP_SLOT_SIZE + 8) {
			ret = -EINVAL;
			break;
		}
		if (xfer.tx && copy_from_user(thp->tx, u64_to_user_ptr(xfer.tx), xfer.size)) {
			ret = -EFAULT;
			break;
		}
		{
			/* Stock permits write-only and read-only host transfers. */
			struct spi_transfer transfer = {
				.tx_buf = xfer.tx ? thp->tx : NULL,
				.rx_buf = xfer.rx ? thp->rx : NULL,
				.len = xfer.size,
			};

			ret = spi_sync_transfer(thp->spi, &transfer, 1);
		}
		if (!ret && xfer.rx &&
		    copy_to_user(u64_to_user_ptr(xfer.rx), thp->rx, xfer.size))
			ret = -EFAULT;
		break;
	default:
		ret = -ENOTTY;
	}
unlock:
	mutex_unlock(&thp->bus_lock);
	return ret;
}
static long thp_agent_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct goodix_thp *thp = file->private_data;
	u8 report[THP_REPORT_SIZE];
	const u8 *point;
	bool touch = false, pen;
	u8 buttons;
	int i;

	if (READ_ONCE(thp->dead))
		return -ENODEV;
	if (cmd >= 0x8004b902 && cmd <= 0x8004b906)
		return 0;
	if (cmd != 0xc1f8b901)
		return -EINVAL;
	if (copy_from_user(report, (void __user *)arg, sizeof(report)))
		return -EFAULT;
	mutex_lock(&thp->input_lock);
	if (thp->dead) {
		mutex_unlock(&thp->input_lock);
		return -ENODEV;
	}
	point = report + 4 + 10 * 44;
	pen = point[1] == 1;
	if (pen || !report[0x1f3]) {
		for (i = 0; i < 10; i++) {
			const u8 *finger = report + 4 + i * 44;
			bool active = !pen && finger[1];

			input_mt_slot(thp->fingers, i);
			input_mt_report_slot_state(thp->fingers, MT_TOOL_FINGER, active);
			if (active) {
				touch = true;
				input_report_abs(thp->fingers, ABS_MT_POSITION_X,
						 get_unaligned_le32(finger + 4));
				input_report_abs(thp->fingers, ABS_MT_POSITION_Y,
						 get_unaligned_le32(finger + 8));
				input_report_abs(thp->fingers, ABS_MT_TOUCH_MAJOR,
						 get_unaligned_le32(finger + 32));
				input_report_abs(thp->fingers, ABS_MT_TOUCH_MINOR,
						 get_unaligned_le32(finger + 36));
			}
		}
		if (!pen && !report[0x1f3])
			touch = (s32)get_unaligned_le32(report + 0x1e8) > 0;
		input_report_key(thp->fingers, BTN_TOUCH, touch);
		input_report_key(thp->fingers, BTN_TOOL_FINGER, touch);
		input_mt_sync_frame(thp->fingers);
		input_sync(thp->fingers);
	}
	buttons = report[0x1f4];
	input_report_key(thp->pen, BTN_TOOL_PEN, pen && !(buttons & BIT(0)));
	input_report_key(thp->pen, BTN_TOOL_RUBBER, pen && (buttons & BIT(0)));
	input_report_key(thp->pen, BTN_TOUCH, pen && !report[0x1f1]);
	input_report_key(thp->pen, BTN_STYLUS, pen && (buttons & BIT(1)));
	input_report_key(thp->pen, BTN_STYLUS2, pen && (buttons & BIT(2)));
	input_report_key(thp->pen, BTN_STYLUS3, pen && (buttons & BIT(3)));
	if (pen) {
		u8 *sample = thp->pen_samples[thp->pen_write];

		memset(sample, 0, 40);
		sample[0] = thp->pen_sequence++;
		put_unaligned_le16(jiffies_to_msecs(jiffies), sample + 2);
		sample[8] = 1;
		sample[10] = get_unaligned_le32(point + 16);
		sample[11] = get_unaligned_le32(point + 20);
		put_unaligned_le64((s64)(s32)get_unaligned_le32(point + 4) / 10, sample + 16);
		put_unaligned_le64((s64)(s32)get_unaligned_le32(point + 8) / 10, sample + 24);
		put_unaligned_le64((s64)(s32)get_unaligned_le32(point + 12), sample + 32);
		thp->pen_write = (thp->pen_write + 1) % 32;
		thp->pen_count = min_t(int, thp->pen_count + 1, 16);
		input_report_abs(thp->pen, ABS_X, get_unaligned_le32(point + 4));
		input_report_abs(thp->pen, ABS_Y, get_unaligned_le32(point + 8));
		input_report_abs(thp->pen, ABS_PRESSURE, get_unaligned_le32(point + 12));
		input_report_abs(thp->pen, ABS_TILT_X, get_unaligned_le32(point + 16));
		input_report_abs(thp->pen, ABS_TILT_Y, get_unaligned_le32(point + 20));
		input_report_abs(thp->pen, ABS_DISTANCE, get_unaligned_le32(point + 28));
	}
	input_sync(thp->pen);
	mutex_unlock(&thp->input_lock);
	return 0;
}
static const struct file_operations thp_fops = {
	.owner = THIS_MODULE, .open = thp_open, .release = thp_release,
	.unlocked_ioctl = thp_ioctl, .compat_ioctl = thp_ioctl, .mmap = thp_mmap,
};
static const struct file_operations thp_penraw_fops = {
	.owner = THIS_MODULE, .open = thp_penraw_open, .release = thp_release,
	.unlocked_ioctl = thp_penraw_ioctl, .compat_ioctl = thp_penraw_ioctl,
};
static const struct file_operations thp_agent_fops = {
	.owner = THIS_MODULE, .open = thp_agent_open, .release = thp_release,
	.unlocked_ioctl = thp_agent_ioctl, .compat_ioctl = thp_agent_ioctl,
};
static int thp_inputs(struct goodix_thp *thp)
{
	u32 x = 16000, y = 25600, width = 256, pressure = 4096;
	struct device *dev = &thp->spi->dev;
	int ret;

	of_property_read_u32(dev->of_node, "goodix,panel-max-x", &x);
	of_property_read_u32(dev->of_node, "goodix,panel-max-y", &y);
	of_property_read_u32(dev->of_node, "goodix,panel-max-w", &width);
	of_property_read_u32(dev->of_node, "goodix,panel-max-p", &pressure);
	thp->fingers = devm_input_allocate_device(dev);
	thp->pen = devm_input_allocate_device(dev);
	if (!thp->fingers || !thp->pen)
		return -ENOMEM;
	thp->fingers->name = "goodix_ts_0";
	thp->pen->name = "goodix_ts_0_pen";
	thp->fingers->id.bustype = thp->pen->id.bustype = BUS_SPI;
	input_set_capability(thp->fingers, EV_KEY, KEY_WAKEUP);
	input_set_capability(thp->pen, EV_KEY, KEY_POWER);
	input_set_capability(thp->pen, EV_MSC, MSC_SCAN);
	input_set_capability(thp->fingers, EV_KEY, BTN_TOUCH);
	input_set_capability(thp->fingers, EV_KEY, BTN_TOOL_FINGER);
	input_set_abs_params(thp->fingers, ABS_MT_POSITION_X, 0, x - 1, 0, 0);
	input_set_abs_params(thp->fingers, ABS_MT_POSITION_Y, 0, y - 1, 0, 0);
	input_set_abs_params(thp->fingers, ABS_MT_TOUCH_MAJOR, 0, width, 0, 0);
	input_set_abs_params(thp->fingers, ABS_MT_TOUCH_MINOR, 0, width, 0, 0);
	ret = input_mt_init_slots(thp->fingers, 10, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	input_set_capability(thp->pen, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(thp->pen, EV_KEY, BTN_TOOL_RUBBER);
	input_set_capability(thp->pen, EV_KEY, BTN_TOUCH);
	input_set_capability(thp->pen, EV_KEY, BTN_STYLUS);
	input_set_capability(thp->pen, EV_KEY, BTN_STYLUS2);
	input_set_capability(thp->pen, EV_KEY, BTN_STYLUS3);
	input_set_abs_params(thp->pen, ABS_X, 0, x - 1, 0, 0);
	input_set_abs_params(thp->pen, ABS_Y, 0, y - 1, 0, 0);
	input_set_abs_params(thp->pen, ABS_PRESSURE, 0, pressure - 1, 0, 0);
	input_set_abs_params(thp->pen, ABS_TILT_X, -90, 90, 0, 0);
	input_set_abs_params(thp->pen, ABS_TILT_Y, -90, 90, 0, 0);
	input_set_abs_params(thp->pen, ABS_DISTANCE, 0, 255, 0, 0);
	__set_bit(INPUT_PROP_DIRECT, thp->pen->propbit);
	ret = input_register_device(thp->fingers);
	return ret ?: input_register_device(thp->pen);
}
static int thp_probe(struct spi_device *spi)
{
	struct goodix_thp *thp;
	int gpio, ret;

	thp = kzalloc(sizeof(*thp), GFP_KERNEL);
	if (!thp)
		return -ENOMEM;
	kref_init(&thp->ref);
	thp->ring = alloc_pages_exact(THP_RING_SIZE, GFP_KERNEL | __GFP_ZERO);
	thp->tx = kmalloc(THP_SLOT_SIZE + 8, GFP_KERNEL);
	thp->rx = kmalloc(THP_SLOT_SIZE + 8, GFP_KERNEL);
	if (!thp->ring || !thp->tx || !thp->rx) {
		ret = -ENOMEM;
		goto free;
	}
	thp->spi = spi;
	thp->timeout_ms = 3000;
	mutex_init(&thp->control_lock);
	mutex_init(&thp->bus_lock);
	mutex_init(&thp->ring_lock);
	mutex_init(&thp->input_lock);
	init_waitqueue_head(&thp->frames);
	INIT_DELAYED_WORK(&thp->panel_work, thp_panel_work);
	INIT_DELAYED_WORK(&thp->suspend_work, thp_suspend_work);
	INIT_DELAYED_WORK(&thp->resume_work, thp_resume_work);
	INIT_DELAYED_WORK(&thp->esd_work, thp_esd_work);
	INIT_DELAYED_WORK(&thp->mode_work, thp_mode_work);
	thp->charger_sent = -1;
	thp->charger_notifier.notifier_call = thp_charger_event;
	thp->esd_enabled = of_property_read_bool(spi->dev.of_node, "goodix,esd-enable");
	gpio = of_get_named_gpio(spi->dev.of_node, "goodix,reset-gpio", 0);
	if (gpio < 0) {
		ret = gpio;
		goto free;
	}
	thp->reset = gpio_to_desc(gpio);
	ret = devm_gpio_request_one(&spi->dev, gpio, GPIOF_OUT_INIT_LOW, "thp-reset");
	if (ret)
		goto free;
	thp->pins = devm_pinctrl_get(&spi->dev);
	if (IS_ERR(thp->pins)) {
		ret = PTR_ERR(thp->pins);
		goto free;
	}
	thp->active_pins = pinctrl_lookup_state(thp->pins, "active");
	thp->sleep_pins = pinctrl_lookup_state(thp->pins, "sleep");
	if (IS_ERR(thp->active_pins) || IS_ERR(thp->sleep_pins)) {
		ret = -EINVAL;
		goto free;
	}
	ret = pinctrl_select_state(thp->pins, thp->active_pins);
	if (ret)
		goto free;
	thp->iovdd = devm_regulator_get(&spi->dev, "iovdd");
	if (IS_ERR(thp->iovdd)) {
		ret = PTR_ERR(thp->iovdd);
		goto free;
	}
	thp->avdd = devm_regulator_get_optional(&spi->dev, "avdd");
	if (IS_ERR(thp->avdd) && PTR_ERR(thp->avdd) != -ENODEV) {
		ret = PTR_ERR(thp->avdd);
		goto free;
	}
	ret = thp_power(thp, true);
	if (ret)
		goto power_off;
	usleep_range(5000, 5500);
	gpiod_set_raw_value_cansleep(thp->reset, 1);
	msleep(100);
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		goto power_off;
	ret = thp_prepare_firmware(thp);
	if (ret) {
		dev_err(&spi->dev, "THP firmware initialization failed: %d\n", ret);
		goto power_off;
	}
	thp->state_changed = 1;
	ret = thp_inputs(thp);
	if (ret)
		goto power_off;
	gpio = of_get_named_gpio(spi->dev.of_node, "goodix,irq-gpio", 0);
	if (gpio < 0) {
		ret = gpio;
		goto power_off;
	}
	ret = devm_gpio_request_one(&spi->dev, gpio, GPIOF_IN, "thp-irq");
	if (ret)
		goto power_off;
	thp->irq = gpio_to_irq(gpio);
	if (thp->irq < 0) {
		ret = thp->irq;
		goto power_off;
	}
	ret = request_threaded_irq(thp->irq, NULL, thp_irq,
			IRQF_ONESHOT | IRQF_TRIGGER_FALLING, "gt9976n_thp", thp);
	if (ret)
		goto power_off;
	disable_irq(thp->irq);
	thp->touch = (struct miscdevice) { .minor = MISC_DYNAMIC_MINOR,
		.name = "thp", .fops = &thp_fops, .parent = &spi->dev };
	thp->agent = (struct miscdevice) { .minor = MISC_DYNAMIC_MINOR,
		.name = "input_agent", .fops = &thp_agent_fops, .parent = &spi->dev };
	ret = misc_register(&thp->touch);
	if (ret)
		goto irq_free;
	ret = misc_register(&thp->agent);
	if (ret)
		goto misc_free;
	mutex_lock(&thp_netlink_lock);
	if (thp_device) {
		ret = -EBUSY;
	} else {
		ret = genl_register_family(&thp_family);
		if (!ret)
			thp_device = thp;
	}
	mutex_unlock(&thp_netlink_lock);
	if (ret) {
		misc_deregister(&thp->agent);
		goto misc_free;
	}
	ret = thp_controls_create(thp);
	if (ret) {
		mutex_lock(&thp_netlink_lock);
		thp_device = NULL;
		mutex_unlock(&thp_netlink_lock);
		genl_unregister_family(&thp_family);
		misc_deregister(&thp->agent);
		goto misc_free;
	}
	thp->pen_info = platform_device_register_simple("gt9976n_thp", 0, NULL, 0);
	if (IS_ERR(thp->pen_info)) {
		ret = PTR_ERR(thp->pen_info);
		goto controls_free;
	}
	platform_set_drvdata(thp->pen_info, thp);
	ret = device_create_file(&thp->pen_info->dev, &dev_attr_screen_state);
	if (ret) {
		platform_device_unregister(thp->pen_info);
		goto controls_free;
	}
	thp->penraw = (struct miscdevice) { .minor = MISC_DYNAMIC_MINOR,
		.name = "lenovo_penraw", .fops = &thp_penraw_fops, .parent = &spi->dev };
	ret = misc_register(&thp->penraw);
	if (ret) {
		device_remove_file(&thp->pen_info->dev, &dev_attr_screen_state);
		platform_device_unregister(thp->pen_info);
		goto controls_free;
	}
	ret = power_supply_reg_notifier(&thp->charger_notifier);
	if (ret) {
		misc_deregister(&thp->penraw);
		device_remove_file(&thp->pen_info->dev, &dev_attr_screen_state);
		platform_device_unregister(thp->pen_info);
		goto controls_free;
	}
	device_init_wakeup(&spi->dev, true);
	spi_set_drvdata(spi, thp);
	WRITE_ONCE(thp->ready, true);
	schedule_delayed_work(&thp->panel_work, 0);
	if (thp->esd_enabled)
		schedule_delayed_work(&thp->esd_work, 3 * HZ);
	return 0;
controls_free:
	thp_controls_remove();
	mutex_lock(&thp_netlink_lock);
	thp_device = NULL;
	mutex_unlock(&thp_netlink_lock);
	genl_unregister_family(&thp_family);
	misc_deregister(&thp->agent);
misc_free:
	misc_deregister(&thp->touch);
irq_free:
	free_irq(thp->irq, thp);
power_off:
	gpiod_set_raw_value_cansleep(thp->reset, 0);
	thp_power(thp, false);
free:
	kref_put(&thp->ref, thp_free);
	return ret;
}
static void thp_remove(struct spi_device *spi)
{
	struct goodix_thp *thp = spi_get_drvdata(spi);

	mutex_lock(&thp->control_lock);
	mutex_lock(&thp->bus_lock);
	WRITE_ONCE(thp->dead, true);
	mutex_unlock(&thp->bus_lock);
	mutex_unlock(&thp->control_lock);
	device_remove_file(&thp->pen_info->dev, &dev_attr_screen_state);
	power_supply_unreg_notifier(&thp->charger_notifier);
	cancel_delayed_work_sync(&thp->panel_work);
	panel_event_notifier_unregister(thp->panel_cookie);
	cancel_delayed_work_sync(&thp->suspend_work);
	cancel_delayed_work_sync(&thp->resume_work);
	cancel_delayed_work_sync(&thp->mode_work);
	cancel_delayed_work_sync(&thp->esd_work);
	mutex_lock(&thp->control_lock);
	wake_up_interruptible(&thp->frames);
	thp_controls_remove();
	misc_deregister(&thp->penraw);
	misc_deregister(&thp->agent);
	misc_deregister(&thp->touch);
	mutex_lock(&thp_netlink_lock);
	thp_device = NULL;
	mutex_unlock(&thp_netlink_lock);
	genl_unregister_family(&thp_family);
	if (thp->wake_enabled)
		disable_irq_wake(thp->irq);
	free_irq(thp->irq, thp);
	mutex_unlock(&thp->control_lock);
	mutex_lock(&thp->bus_lock);
	mutex_lock(&thp->input_lock);
	gpiod_set_raw_value_cansleep(thp->reset, 0);
	thp_power(thp, false);
	mutex_unlock(&thp->input_lock);
	platform_device_unregister(thp->pen_info);
	device_init_wakeup(&spi->dev, false);
	mutex_unlock(&thp->bus_lock);
	kref_put(&thp->ref, thp_free);
}
static const struct of_device_id thp_of_match[] = {
	{ .compatible = "gt9976n_thp" }, { }
};
MODULE_DEVICE_TABLE(of, thp_of_match);
static const struct spi_device_id thp_spi_ids[] = { { "gt9976n_thp", 0 }, { } };
MODULE_DEVICE_TABLE(spi, thp_spi_ids);
static struct spi_driver thp_driver = {
	.driver = { .name = "gt9976n_thp_core", .of_match_table = thp_of_match,
		.suppress_bind_attrs = true, .pm = &thp_pm_ops },
	.probe = thp_probe, .remove = thp_remove, .id_table = thp_spi_ids,
};
module_spi_driver(thp_driver);
MODULE_DESCRIPTION("Lenovo Wuji GT9976N host touch transport");
MODULE_LICENSE("GPL");
