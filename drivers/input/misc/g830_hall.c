/* drivers/input/misc/g830_hall.c - light and proxmity sensors driver
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


struct g830_hall_info {
	struct input_dev	*idev;
	struct switch_dev sdev;
	struct workqueue_struct *g830_hall_wq;
	struct delayed_work work;
	struct wake_lock wake_lock;
	int last_value;			// 1, for normal state(gpio high); 0, for active state(gio low)
	struct device		*dev;
	int	irq;

	struct regulator *vdd;	
	struct regulator *vio;	
	int irq_gpio;	
	unsigned int irq_gpio_flags;
	bool hall_enable;
};

#define GN_HALL_KEY_OPEN    111   
#define GN_HALL_KEY_CLOSE   112             

struct g830_hall_info *hall_data;

static int sensor_parse_dt(struct device *dev, struct g830_hall_info *hall)
{	
	struct device_node *np = dev->of_node;

	pr_err("%s: here\n", __func__);
	/* irq gpio */
	hall->irq_gpio = of_get_named_gpio_flags(np, "hall,irq-gpio",						 
			0, &hall->irq_gpio_flags);
	
	printk("%s: irq_gpio %d\n", __func__, hall->irq_gpio);
	if (hall->irq_gpio < 0)
		return hall->irq_gpio;
	return 0;
}


static void g830_hall_work_func(struct work_struct *work){
	
	int ret = 0;
	struct g830_hall_info *info = container_of(work, struct g830_hall_info, work.work);
	ret = gpio_get_value(info->irq_gpio);
	printk("g830 hall, g830_hall_work_func \n");
	
	if (info->hall_enable == 0)
	{
		return ;
	}
	
	if (ret){
		printk("g830 hall out high \n");
//		input_report_key(info->idev, KEY_CP200_IGNITION_ON, 1);
//		input_sync(info->idev);
//		input_report_key(info->idev, KEY_CP200_IGNITION_ON, 0);
//		input_sync(info->idev);
	}else{
		printk("g830 hall out low \n");
//		input_report_key(info->idev, KEY_CP200_IGNITION_OFF, 1);
//		input_sync(info->idev);
//		input_report_key(info->idev, KEY_CP200_IGNITION_OFF, 0);
//		input_sync(info->idev);
	}
	
	printk("g830 hall, wake unlocked ign: %d \n",ret);
	wake_unlock(&info->wake_lock);
}

/* handler method for IRQ trigger  */
static irqreturn_t g830_hall_handler(int irq, void *data)
{
	struct g830_hall_info *info = data;
	int ret = 0;

	wake_lock(&info->wake_lock);
	ret = gpio_get_value_cansleep(info->irq_gpio);
	printk("g830_hall, wake lock requested enable %d,gpio value %d\n", info->last_value, ret);

	if (info->last_value == ret) {
		return IRQ_HANDLED;
	}
/*
	disable_irq(info->irq);
	free_irq(info->irq, info);
	info->irq = gpio_to_irq(info->irq_gpio);
	printk("g830_hall,  info->irq %d\n", info->irq);
*/
	
	if (info->last_value) {
/*		ret = request_threaded_irq(info->irq,
			 NULL, g830_hall_handler,
			 IRQF_TRIGGER_HIGH | IRQF_ONESHOT,
			 "g830-hall", info);*/
//		input_sync(key);		// .. lcd off
		info->last_value = 0;
	} else {
/*		ret = request_threaded_irq(info->irq,
			 NULL, g830_hall_handler,
			 IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			 "g830-hall", info);*/
//		input_sync(key);		// .. lcd on 
		info->last_value = 1;
	}

	/*
	if (!delayed_work_pending(&info->work)){
		printk("g830_hall no work is pending, run ignition thread \n");
//		info->last_value = gpio_get_value(info->irq_gpio);
		queue_delayed_work(info->g830_hall_wq,&info->work,2*HZ);
	}
	* */
	wake_unlock(&info->wake_lock);
	/* Enable 8-second long onkey detection */
	return IRQ_HANDLED;
}

static ssize_t g830_hall_print_name(struct switch_dev *sdev, char *buf)
{
	int ret = 0;
//	ret = gpio_get_value(20);
	if (ret == 0){
		return sprintf(buf, "Ignition-Off\n");
	}else if(ret ==1){
		return sprintf(buf, "Ignition-On\n");
	}else
		return -EINVAL;
}

static ssize_t hall_ls_operationmode_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct g830_hall_info *hada = hall_data;
	long *tmp = (long *)buf;

	pr_err("%s: enter\n", __func__);
	tmp[0] = hada->hall_enable;

	return snprintf(buf, PAGE_SIZE, "%d\n", hada->hall_enable);
}


static ssize_t hall_ls_operationmode_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	uint16_t mode = 0;
	struct g830_hall_info *hada = hall_data;

	sscanf(buf, "%hu", &mode);
	pr_info("==>[operation mode]=%d\n", mode);

	if (mode == 0) {
		hada->hall_enable = 0;
		disable_irq(hada->irq);
	} else if (mode == 1) {
		hada->hall_enable = 1;
		hada->last_value  = 1;
		enable_irq(hada->irq);
	} else {
		pr_err("0: none\n1: als only\n2: ps only\n3: interleaving");
	}

	return count;
}

static struct device_attribute hall_attr_group[] = {
	__ATTR(enable_hall, 0666, hall_ls_operationmode_show, hall_ls_operationmode_store),
};

static int add_sysfs_interfaces(struct device *dev,
	struct device_attribute *a, int size)
{
	int i;
	for (i = 0; i < size; i++)
		if (device_create_file(dev, a + i))
			goto undo;
	return 0;
undo:
	for (; i >= 0; i--)
		device_remove_file(dev, a + i);
	dev_err(dev, "%s: failed to create sysfs interface\n", __func__);

	return -ENODEV;
}

static void remove_sysfs_interfaces(struct device *dev,
	struct device_attribute *a, int size)
{
	int i;
	for (i = 0; i < size; i++)
		device_remove_file(dev, a + i);
}

static int g830_hall_probe(struct platform_device *pdev)
{
	//Configuration of the ignition gpio i.e, number, key-code ..
//	struct g830_hall_data *data = pdev->dev.platform_data;
	//Linux legal dev information
	struct g830_hall_info *info;
	
	int ret;

	printk("%s: enter\n", __func__);
	
	info = kzalloc(sizeof(struct g830_hall_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = &pdev->dev;

	info->sdev.name	= "hall";
	info->sdev.print_name = g830_hall_print_name;
	info->hall_enable = 1;		// for test
	info->last_value  = 1;		// for test

	printk("%s: info->sdev.name=%s\n", __func__,info->sdev.name);
	ret = switch_dev_register(&info->sdev);
	if (ret)
		goto out;
	printk("%s: switch_dev_register\n", __func__);
	ret = sensor_parse_dt(&pdev->dev, info);
	if (ret) {
		printk("%s: sensor_parse_dt() err\n", __func__);
		return ret;
	}
	printk("%s: sensor_parse_dt() success\n", __func__);
	info->g830_hall_wq = create_singlethread_workqueue("g830_hall_wq");
	if (!info->g830_hall_wq) {
		ret = -ENOMEM;
		printk("%s: could not create workqueue\n", __func__);
		goto out;
	}
	
	/* this is the thread function we run on the work queue */
	INIT_DELAYED_WORK(&info->work, g830_hall_work_func);

	info->idev = input_allocate_device();
	if (!info->idev) {
		dev_err(info->dev, "Failed to allocate input dev\n");
		ret = -ENOMEM;
		goto out;
	}

	info->idev->name = pdev->name;
	
	info->idev->evbit[0] = BIT_MASK(EV_KEY);
//	set_bit(KEY_CP200_IGNITION_ON,info->idev->keybit);
//	set_bit(KEY_CP200_IGNITION_OFF,info->idev->keybit);
	ret = input_register_device(info->idev);
	if (ret) {
		dev_err(&pdev->dev, "Can't register input device: %d\n", ret);
		goto out_reg;
	}

	wake_lock_init(&info->wake_lock, WAKE_LOCK_SUSPEND,"g830-hall-wake-lock");
		
	/* request the GPIOs */
	if (gpio_is_valid(info->irq_gpio)) {
		ret = gpio_request(info->irq_gpio, "g830_hall_gpio_int");
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

//	ret = request_irq(info->irq, g830_hall_handler,
//			  IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
//			  "g830-hall", info);
	ret = request_threaded_irq(info->irq,
			 NULL, g830_hall_handler,
//			 IRQF_TRIGGER_HIGH | 
			 IRQF_TRIGGER_LOW | IRQF_ONESHOT,
			 "g830-hall", info);
	if (ret < 0) {
		dev_err(info->dev, "Failed to request IRQ: #%d: %d\n",
			info->irq, ret);
		goto out_irq;
	}
//	disable_irq(hada->irq);		// del it for test.
	
	ret = add_sysfs_interfaces(&info->idev->dev,
			hall_attr_group, ARRAY_SIZE(hall_attr_group));
	if (ret != 0) {
		dev_err(info->dev, "%s:create sysfs group error", __func__);
		goto exit_remove_sysfs_group;
	}
	ret = gpio_get_value_cansleep(info->irq_gpio);

	pr_err("%s: out gpio %d\n", __func__, ret);

	platform_set_drvdata(pdev, info);
	return 0;
	
exit_remove_sysfs_group:
	remove_sysfs_interfaces(&info->idev->dev,hall_attr_group, ARRAY_SIZE(hall_attr_group));
out_irq:
	input_unregister_device(info->idev);
	kfree(info);
	return ret;

out_reg:
	input_free_device(info->idev);
out:
	kfree(info);
	return ret;
}

static int g830_hall_remove(struct platform_device *pdev)
{
	struct g830_hall_info *info = platform_get_drvdata(pdev);

	wake_lock_destroy(&info->wake_lock);
	free_irq(info->irq, info);
	remove_sysfs_interfaces(&info->idev->dev,hall_attr_group, ARRAY_SIZE(hall_attr_group));
	input_unregister_device(info->idev);
	kfree(info);
	return 0;
}

static struct of_device_id g830_hall_match_table[] = {
	{.compatible = "rohs,bu5002",},	
	{},
};

static struct platform_driver g830_hall_driver = {
	.driver		= {
		.name	= "g830-hall",
		.owner	= THIS_MODULE,
		.of_match_table = g830_hall_match_table,
	},
	.probe		= g830_hall_probe,
	.remove		= g830_hall_remove,
};

static int __init g830_hall_init(void)
{
	return platform_driver_register(&g830_hall_driver);
}
module_init(g830_hall_init);

static void __exit g830_hall_exit(void)
{
	platform_driver_unregister(&g830_hall_driver);
}
module_exit(g830_hall_exit);

MODULE_DESCRIPTION("G830 hall driver");
MODULE_AUTHOR("Lee Quanqiao <liqq@ym-tek.com>");
MODULE_LICENSE("GPL");
