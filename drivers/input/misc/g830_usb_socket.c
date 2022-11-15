/* drivers/input/misc/g830_usb_socket.c - light and proxmity sensors driver
 * Copyright (C) 2014 ym-tek Corporation.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/switch.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/unistd.h>
#include <linux/pm.h>
#include <linux/of_gpio.h>
#include <linux/sensors.h>

#define USB_SOCKET_DEFAULT_STATE 0
#define USB_SOCKET_ACTIVE_STATE  1

struct g830_usb_socket_info {
	struct input_dev	*idev;
	struct switch_dev sdev;
	struct workqueue_struct *g830_usb_socket_wq;
	struct delayed_work work;
	struct wake_lock wake_lock;
	int last_value;			// 1, for normal state(gpio 69 high); 0, for active state(gpio 69 low)
	struct device		*dev;
//	int	irq;

//	int irq_gpio;
	int usb_id_ctl_gpio;
	int usb_sel_gpio;
};

struct g830_usb_socket_info *usb_socket_data;

static int sensor_parse_dt(struct device *dev, struct g830_usb_socket_info *info)
{
	struct device_node *np = dev->of_node;

	pr_err("%s: here\n", __func__);
	/* irq gpio */
/*	info->irq_gpio = of_get_named_gpio_flags(np, "usbs,irq-gpio",						 
			0, NULL);
			
	pr_err("%s: irq_gpio %d\n", __func__, info->irq_gpio);
	if (info->irq_gpio < 0)
		return info->irq_gpio;
*/
	info->usb_sel_gpio = of_get_named_gpio_flags(np,
				"usbs,usb-sel-gpio", 0, NULL);
	if (!gpio_is_valid(info->usb_sel_gpio))
		return -EINVAL;

	info->usb_id_ctl_gpio = of_get_named_gpio_flags(np,
				"usbs,usb-id-ctl-gpio", 0, NULL);
	if (!gpio_is_valid(info->usb_id_ctl_gpio))
		return -EINVAL;

	return 0;
}

static void g830_usb_socket_work_func(struct work_struct *work){
	
	int ret = 0;
	struct g830_usb_socket_info *info = container_of(work, struct g830_usb_socket_info, work.work);
//	ret = gpio_get_value(info->irq_gpio);
	printk("g830 usb_socket, g830_usb_socket_work_func \n");
	
	if (ret){
		printk("g830 usb_socket out high \n");
//		input_report_key(info->idev, KEY_CP200_IGNITION_ON, 1);
//		input_sync(info->idev);
//		input_report_key(info->idev, KEY_CP200_IGNITION_ON, 0);
//		input_sync(info->idev);
	}else{
		printk("g830 usb_socket out low \n");
//		input_report_key(info->idev, KEY_CP200_IGNITION_OFF, 1);
//		input_sync(info->idev);
//		input_report_key(info->idev, KEY_CP200_IGNITION_OFF, 0);
//		input_sync(info->idev);
	}
	
	printk("g830 usb_socket, wake unlocked ign: %d \n",ret);
	wake_unlock(&info->wake_lock);
}
static void g830_usb_socket_set_gpio(struct g830_usb_socket_info *info, int val)
{
	printk("g830_usb_socket, g830_usb_socket_set_gpio %d\n", val);
	if (val) {
		gpio_set_value(info->usb_sel_gpio, 1);
		gpio_set_value(info->usb_id_ctl_gpio, 1);
	} else {
		gpio_set_value(info->usb_sel_gpio, 0);
		gpio_set_value(info->usb_id_ctl_gpio, 0);
	}
}

/* handler method for IRQ trigger  */ 
/*static irqreturn_t g830_usb_socket_handler(int irq, void *data)
{
	struct g830_usb_socket_info *info = data;
	int ret = 0;

//	wake_lock(&info->wake_lock);
	ret = gpio_get_value_cansleep(info->irq_gpio);
	printk("g830_usb_socket, wake lock requested enable %d,gpio value %d\n", info->last_value, ret);
	
	if (info->last_value == ret) {
		return IRQ_HANDLED;
	}

//	// this codes maybe not right.
	disable_irq(info->irq);
	free_irq(info->irq, info);
	info->irq = gpio_to_irq(info->irq_gpio);
	printk("g830_usb_socket,  info->irq %d\n", info->irq);
//
	if (info->last_value) {					// Active state, connectting the usb socket.
//		ret = request_threaded_irq(info->irq,
			 NULL, g830_usb_socket_handler,
			 IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
			 "g830-usb-socket-high", info);
//
		g830_usb_socket_set_gpio(info, USB_SOCKET_ACTIVE_STATE);
		info->last_value = 0;
	} else {									// default state, disconnectting the usb socket.
/		ret = request_threaded_irq(info->irq,
			 NULL, g830_usb_socket_handler,
			 IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			 "g830-usb-socket-low", info);
/
		g830_usb_socket_set_gpio(info, USB_SOCKET_DEFAULT_STATE);
		info->last_value = 1;
	}

	// Enable 8-second long onkey detection 
	return IRQ_HANDLED;
}*/

static ssize_t g830_usb_socket_print_name(struct switch_dev *sdev, char *buf)
{
	int ret = 0;
//	ret = gpio_get_value(20);
	if (ret == 0){
		return sprintf(buf, "G830 usb socket\n");
	}else if(ret ==1){
		return sprintf(buf, "G830 usb socket\n");
	}else
		return -EINVAL;
}

static int g830_usb_socket_probe(struct platform_device *pdev)
{
	struct g830_usb_socket_info *info;
	
	int ret;

	pr_err("%s: enter\n", __func__);
	
	info = kzalloc(sizeof(struct g830_usb_socket_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &pdev->dev;

	info->sdev.name	= "usb_socket";
	info->sdev.print_name = g830_usb_socket_print_name;

	ret = switch_dev_register(&info->sdev);
	if (ret)
		goto out;

	ret = sensor_parse_dt(&pdev->dev, info);
	if (ret) {
		pr_err("%s: sensor_parse_dt() err\n", __func__);
		return ret;
	}

	info->g830_usb_socket_wq = create_singlethread_workqueue("g830_usb_socket_wq");
	if (!info->g830_usb_socket_wq) {
		ret = -ENOMEM;
		pr_err("%s: could not create workqueue\n", __func__);
		goto out;
	}
	
	/* this is the thread function we run on the work queue */
	INIT_DELAYED_WORK(&info->work, g830_usb_socket_work_func);

	info->idev = input_allocate_device();
	if (!info->idev) {
		dev_err(info->dev, "Failed to allocate input dev\n");
		ret = -ENOMEM;
		goto out;
	}

	info->idev->name = pdev->name;
	
//	info->idev->evbit[0] = BIT_MASK(EV_KEY);
//	set_bit(KEY_CP200_IGNITION_ON,info->idev->keybit);
//	set_bit(KEY_CP200_IGNITION_OFF,info->idev->keybit);
	ret = input_register_device(info->idev);
	if (ret) {
		dev_err(&pdev->dev, "Can't register input device: %d\n", ret);
		goto out_reg;
	}

//	wake_lock_init(&info->wake_lock, WAKE_LOCK_SUSPEND,"g830-usb-socket-wake-lock");
	
	if (gpio_is_valid(info->usb_sel_gpio)) {
		ret = gpio_request(info->usb_sel_gpio, "G830_USB_SEL_GPIO");
		gpio_direction_output(info->usb_sel_gpio, 0);
	}
	
	if (gpio_is_valid(info->usb_id_ctl_gpio)) {
		ret = gpio_request(info->usb_id_ctl_gpio, "G830_USB_ID_CTL_GPIO");
		gpio_direction_output(info->usb_id_ctl_gpio, 0);
	}
	g830_usb_socket_set_gpio(info, USB_SOCKET_DEFAULT_STATE);
	/* request the GPIOs */
/*	if (gpio_is_valid(info->irq_gpio)) {
		ret = gpio_request(info->irq_gpio, "G830_USB_SOCKET_GPIO_INT");
		if (ret) {
			dev_err(&pdev->dev, "unable to request GPIO %d\n",
				info->irq_gpio);
			goto out_irq;
		}
		ret = gpio_direction_input(info->irq_gpio);
		if (ret) {
			dev_err(&pdev->dev, "set_direction for irq gpio failed\n");
			goto out_irq;
		}
	}
	info->irq = gpio_to_irq(info->irq_gpio);
	pr_err("info->irq %d\n",info->irq);

	ret = gpio_get_value_cansleep(info->irq_gpio);
	if (ret ) {
		g830_usb_socket_set_gpio(info, USB_SOCKET_DEFAULT_STATE);
		info->last_value  = 1;
	} else {
		g830_usb_socket_set_gpio(info, USB_SOCKET_ACTIVE_STATE);
		info->last_value  = 0;
	}
	
//	ret = request_irq(info->irq, g830_usb_socket_handler,
//			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
//			  "g830-usb_socket", info);
	ret = request_threaded_irq(info->irq,
			 NULL, g830_usb_socket_handler,
			 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
//			 IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			 "g830-usb-socket", info);
	if (ret < 0) {
		dev_err(info->dev, "Failed to request IRQ: #%d: %d\n",
			info->irq, ret);
		goto out_irq;
	}
*/
	pr_err("%s: out gpio %d\n", __func__, ret);

	platform_set_drvdata(pdev, info);
	return 0;
	
/*out_irq:
	input_unregister_device(info->idev);
	kfree(info);
	return ret;
*/
out_reg:
	input_free_device(info->idev);
out:
	kfree(info);
	return ret;
}

static int g830_usb_socket_remove(struct platform_device *pdev)
{
	struct g830_usb_socket_info *info = platform_get_drvdata(pdev);

	//wake_lock_destroy(&info->wake_lock);
//	free_irq(info->irq, info);
	input_unregister_device(info->idev);
	kfree(info);
	return 0;
}

static int g830_usb_socket_suspend(struct device *dev)
{
//	struct g830_usb_socket_info *info = dev_get_drvdata(dev);
//	dev_err(info->dev, "suspended\n");
//	disable_irq(info->irq);
	return 0;
}

static int g830_usb_socket_resume(struct device *dev)
{
//	struct g830_usb_socket_info *info = dev_get_drvdata(dev);
//	int ret;
//	dev_err(info->dev, "resumed\n");
/*	
	ret = gpio_get_value_cansleep(info->irq_gpio);
	if (ret ) {
		g830_usb_socket_set_gpio(info, USB_SOCKET_DEFAULT_STATE);
		info->last_value  = 1;
	} else {
		g830_usb_socket_set_gpio(info, USB_SOCKET_ACTIVE_STATE);
		info->last_value  = 0;
	}
	
	enable_irq(info->irq);*/
	return 0;
}

static const struct dev_pm_ops g830_usb_socket_pm_ops = {
	.suspend	= g830_usb_socket_suspend,
	.resume		= g830_usb_socket_resume,
};

static struct of_device_id g830_usb_socket_match_table[] = {
	{.compatible = "usb,socket",},	
	{},
};

static struct platform_driver g830_usb_socket_driver = {
	.driver		= {
		.name	= "g830-usb-socket",
		.owner	= THIS_MODULE,
		.of_match_table = g830_usb_socket_match_table,
		.pm		= &g830_usb_socket_pm_ops,
	},
	.probe		= g830_usb_socket_probe,
	.remove		= g830_usb_socket_remove,
};

static int __init g830_usb_socket_init(void)
{
	return platform_driver_register(&g830_usb_socket_driver);
}
module_init(g830_usb_socket_init);

static void __exit g830_usb_socket_exit(void)
{
	platform_driver_unregister(&g830_usb_socket_driver);
}
module_exit(g830_usb_socket_exit);

MODULE_DESCRIPTION("G830 usb socket driver");
MODULE_AUTHOR("Lee Quanqiao <liqq@ym-tek.com>");
MODULE_LICENSE("GPL");
