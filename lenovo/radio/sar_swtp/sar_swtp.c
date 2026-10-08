// SPDX-License-Identifier: GPL-2.0
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define SWTP_KEY_LOW 249
#define SWTP_KEY_HIGH 250

struct sar_swtp {
	struct gpio_desc *gpio;
	struct input_dev *input;
	struct proc_dir_entry *proc;
};

static irqreturn_t sar_swtp_irq(int irq, void *data)
{
	struct sar_swtp *swtp = data;
	int level = gpiod_get_raw_value_cansleep(swtp->gpio);
	unsigned int code;

	if (level < 0)
		return IRQ_HANDLED;
	code = level ? SWTP_KEY_HIGH : SWTP_KEY_LOW;
	input_report_key(swtp->input, code, 1);
	input_report_key(swtp->input, code, 0);
	input_sync(swtp->input);

	/* Stock alternates level triggers after sampling the GPIO. */
	irq_set_irq_type(irq, level ? IRQ_TYPE_LEVEL_LOW : IRQ_TYPE_LEVEL_HIGH);
	return IRQ_HANDLED;
}

static int sar_swtp_gpio_show(struct seq_file *seq, void *unused)
{
	struct sar_swtp *swtp = seq->private;
	int level = gpiod_get_raw_value_cansleep(swtp->gpio);

	if (level < 0)
		return level;
	seq_printf(seq, "%d\n", level);
	return 0;
}

static int sar_swtp_gpio_open(struct inode *inode, struct file *file)
{
	return single_open(file, sar_swtp_gpio_show, pde_data(inode));
}

static const struct proc_ops sar_swtp_gpio_ops = {
	.proc_open = sar_swtp_gpio_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static void sar_swtp_proc_remove(void *data)
{
	struct sar_swtp *swtp = data;

	proc_remove(swtp->proc);
}

static int sar_swtp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sar_swtp *swtp;
	int irq, ret;

	swtp = devm_kzalloc(dev, sizeof(*swtp), GFP_KERNEL);
	if (!swtp)
		return -ENOMEM;
	/* The stock binding uses the singular property irq-gpio. */
	swtp->gpio = devm_gpiod_get(dev, "irq", GPIOD_IN);
	if (IS_ERR(swtp->gpio))
		return PTR_ERR(swtp->gpio);
	irq = gpiod_to_irq(swtp->gpio);
	if (irq < 0)
		return irq;

	swtp->input = devm_input_allocate_device(dev);
	if (!swtp->input)
		return -ENOMEM;
	swtp->input->name = "sar-swtp";
	input_set_capability(swtp->input, EV_KEY, SWTP_KEY_LOW);
	input_set_capability(swtp->input, EV_KEY, SWTP_KEY_HIGH);
	ret = input_register_device(swtp->input);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, irq, NULL, sar_swtp_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					"sar-swtp", swtp);
	if (ret)
		return ret;

	swtp->proc = proc_create_data("sar_swtp_gpio_status", 0644, NULL,
				     &sar_swtp_gpio_ops, swtp);
	if (!swtp->proc)
		return -ENOMEM;
	return devm_add_action_or_reset(dev, sar_swtp_proc_remove, swtp);
}

static const struct of_device_id sar_swtp_match[] = {
	{ .compatible = "virtualsar,swtp-eint" },
	{ }
};
MODULE_DEVICE_TABLE(of, sar_swtp_match);

static struct platform_driver sar_swtp_driver = {
	.probe = sar_swtp_probe,
	.driver = {
		.name = "sar_swtp",
		.of_match_table = sar_swtp_match,
	},
};
module_platform_driver(sar_swtp_driver);

MODULE_DESCRIPTION("Sar_Swtp Driver");
MODULE_LICENSE("GPL");
