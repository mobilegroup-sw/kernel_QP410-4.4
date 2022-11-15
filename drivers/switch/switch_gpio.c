/*
 *  drivers/switch/switch_gpio.c
 *
 * Copyright (C) 2008 Google, Inc.
 * Author: Mike Lockwood <lockwood@android.com>
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/switch.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/input.h>
struct gpio_switch_data {
	struct switch_dev sdev;
	unsigned gpio;
	const char *name_on;
	const char *name_off;
	const char *state_on;
	const char *state_off;
	int irq;
	struct work_struct work;
	int last_value;
};

#define GN_HALL_KEY_OPEN    111   
#define GN_HALL_KEY_CLOSE   112             


#define GPIO_SWITCH_KEY

#ifdef GPIO_SWITCH_KEY
static struct input_dev *gpio_switch_dev;
static void gpio_switch_key_init(void)
{
	struct input_dev *idev;
	int rc = 0;
	idev = input_allocate_device();
	if(!idev){
		return;
	}
	gpio_switch_dev = idev;
	idev->name = "gpio_switch";
	idev->id.bustype = BUS_I2C;
	input_set_capability(idev,EV_KEY,GN_HALL_KEY_OPEN);
	input_set_capability(idev,EV_KEY,GN_HALL_KEY_CLOSE);

	rc = input_register_device(idev);
	if(rc){
		input_free_device(idev);
		gpio_switch_dev = NULL;
	}
}

static void gpio_switch_key_report(int state)
{
	printk("gpio_switch_key_report state=%d\n",state);

	if(state==1)
	{
		input_report_key(gpio_switch_dev,GN_HALL_KEY_OPEN,1);
		input_sync(gpio_switch_dev);
		input_report_key(gpio_switch_dev,GN_HALL_KEY_OPEN,0);
		input_sync(gpio_switch_dev);
	}
	else
	{
		input_report_key(gpio_switch_dev,GN_HALL_KEY_CLOSE,1);
		input_sync(gpio_switch_dev);
		input_report_key(gpio_switch_dev,GN_HALL_KEY_CLOSE,0);
		input_sync(gpio_switch_dev);
	
	}
}
#else
static void gpio_switch_key_init(void)
{
}
static void gpio_switch_key_report(int state)
{
}
#endif
static void gpio_switch_work(struct work_struct *work)
{
	int state;
	struct gpio_switch_data	*data =
		container_of(work, struct gpio_switch_data, work);

	state = gpio_get_value(data->gpio);
	switch_set_state(&data->sdev, state);
	
	printk("%s , state=%d\n",__func__,state);
}

static irqreturn_t gpio_irq_handler(int irq, void *dev_id)
{
	struct gpio_switch_data *switch_data =
	    (struct gpio_switch_data *)dev_id;
#ifdef GPIO_SWITCH_KEY
	int ret = 0;
	int states = 0;
	ret = gpio_get_value(switch_data->gpio);
	if (switch_data->last_value == ret) {
		return IRQ_HANDLED;
	}
	if (switch_data->last_value) {
		states = switch_data->last_value = 0;
		gpio_switch_key_report(states);
		
	} else {
		states = switch_data->last_value = 1;
		gpio_switch_key_report(states);
	}
#endif
	schedule_work(&switch_data->work);
	return IRQ_HANDLED;
}

static ssize_t switch_gpio_print_state(struct switch_dev *sdev, char *buf)
{
	struct gpio_switch_data	*switch_data =
		container_of(sdev, struct gpio_switch_data, sdev);
	const char *state;
	if (switch_get_state(sdev))
		state = switch_data->state_on;
	else
		state = switch_data->state_off;

	if (state)
		return sprintf(buf, "%s\n", state);
	return -1;
}

static int gpio_switch_parse_dt(struct device *dev,
			struct gpio_switch_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;
	u32 irq_gpio_flags;
	
	rc = of_property_read_string(np, "switch,gpio_name",
						&pdata->name);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "switch,gpio_name.");
		return -EINVAL;
	}


	pdata->gpio = of_get_named_gpio_flags(np, "switch,gpio",
				0, &irq_gpio_flags);
	if (pdata->gpio < 0)
		return pdata->gpio;



	rc = of_property_read_string(np, "switch,name_on",
						&pdata->name_on);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Failed to parse switch,name_on.\n");
		return -EINVAL;
	}

	rc = of_property_read_string(np, "switch,name_off",
						&pdata->name_off);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Failed to parse switch,name_off.\n");
		return -EINVAL;
	}


	rc = of_property_read_string(np, "switch,state_on",
						&pdata->state_on);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Failed to parse switch,state_on.\n");
		return -EINVAL;
	}	

	rc = of_property_read_string(np, "switch,state_off",
						&pdata->state_off);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Failed to parse switch,state_off.\n");
		return -EINVAL;
	}
	
	

	return 0;
}


static int gpio_switch_probe(struct platform_device *pdev)
{
	struct gpio_switch_platform_data *pdata = pdev->dev.platform_data;
	struct gpio_switch_data *switch_data;
	struct gpio_switch_platform_data switch_pdata;
	struct device *dev = &pdev->dev;
	
	int ret = 0;

	if (!pdata) {
		ret = gpio_switch_parse_dt(dev, &switch_pdata);
		if (ret)
			return -EBUSY;
		pdata = &switch_pdata;
	}
	
	switch_data = kzalloc(sizeof(struct gpio_switch_data), GFP_KERNEL);
	if (!switch_data)
		return -ENOMEM;

	switch_data->sdev.name = pdata->name;
	switch_data->gpio = pdata->gpio;
	switch_data->name_on = pdata->name_on;
	switch_data->name_off = pdata->name_off;
	switch_data->state_on = pdata->state_on;
	switch_data->state_off = pdata->state_off;
	switch_data->sdev.print_state = switch_gpio_print_state;
#ifdef GPIO_SWITCH_KEY
	switch_data->last_value = 1;
#endif
	printk("pdata->name=%s\n",pdata->name);
	printk("pdata->gpio=%d\n",pdata->gpio);
	printk("pdata->name_on=%s\n",pdata->name_on);
	printk("pdata->name_off=%s\n",pdata->name_off);
	printk("pdata->state_on=%s\n",pdata->state_on);
	printk("pdata->state_off=%s\n",pdata->state_off);

    ret = switch_dev_register(&switch_data->sdev);
	if (ret < 0)
		goto err_switch_dev_register;

	ret = gpio_request(switch_data->gpio, pdev->name);
	if (ret < 0)
		goto err_request_gpio;

	ret = gpio_direction_input(switch_data->gpio);
	if (ret < 0)
		goto err_set_gpio_input;

	INIT_WORK(&switch_data->work, gpio_switch_work);

	switch_data->irq = gpio_to_irq(switch_data->gpio);
	if (switch_data->irq < 0) {
		ret = switch_data->irq;
		goto err_detect_irq_num_failed;
	}
	gpio_switch_key_init();
	ret = request_irq(switch_data->irq, gpio_irq_handler,
			  IRQF_TRIGGER_MASK, pdev->name, switch_data);
	if (ret < 0)
		goto err_request_irq;
#ifdef GPIO_SWITCH_KEY
	ret = enable_irq_wake(switch_data->irq); 
	if (ret < 0) { 
		printk(" ========== Couldn't enable gpio switch irq wake=============\n"); 
	}  
#endif
	/* Perform initial detection */
	gpio_switch_work(&switch_data->work);

	return 0;

err_request_irq:
err_detect_irq_num_failed:
err_set_gpio_input:
	gpio_free(switch_data->gpio);
err_request_gpio:
	switch_dev_unregister(&switch_data->sdev);
err_switch_dev_register:
	kfree(switch_data);

	return ret;
}

static int gpio_switch_remove(struct platform_device *pdev)
{
	struct gpio_switch_data *switch_data = platform_get_drvdata(pdev);

	cancel_work_sync(&switch_data->work);
	gpio_free(switch_data->gpio);
	switch_dev_unregister(&switch_data->sdev);
	kfree(switch_data);

	return 0;
}
static struct of_device_id gpio_switch_of_match[] = {
	{ .compatible = "qcom,switch-gpio", },
	{ },
};
static struct platform_driver gpio_switch_driver = {
	.probe		= gpio_switch_probe,
	.remove		= gpio_switch_remove,
	.driver		= {
		.name	= "switch-gpio",
		.owner	= THIS_MODULE,
		.of_match_table = gpio_switch_of_match,
	},
};

static int __init gpio_switch_init(void)
{
	return platform_driver_register(&gpio_switch_driver);
}

static void __exit gpio_switch_exit(void)
{
	platform_driver_unregister(&gpio_switch_driver);
}

module_init(gpio_switch_init);
module_exit(gpio_switch_exit);

MODULE_AUTHOR("Mike Lockwood <lockwood@android.com>");
MODULE_DESCRIPTION("GPIO Switch driver");
MODULE_LICENSE("GPL");
