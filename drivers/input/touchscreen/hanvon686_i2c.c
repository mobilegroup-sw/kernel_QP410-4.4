/*
 *
 * Electromagnetic Pen I2C Driver for Hanvon
 *
 * Copyright (C) 1999-2012  Hanvon Technology Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/pm.h>
#include <linux/of_gpio.h>
#include <linux/proc_fs.h>
 
 #if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>

#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
/* Early-suspend level */
#define FT_SUSPEND_LEVEL 1
#endif

#define CHIP_NAME	"Hanvon0868"
#define MAX_EVENTS	5
#define MAX_X		0x27de
#define MAX_Y		0x1cfe

/**define pen flags**/
#define PEN_ALL_LEAVE					0xe0
#define PEN_POINTER_DOWN				0xa1
#define PEN_POINTER_UP					0xa0
#define PEN_BUTTON_UP					0xa3
#define PEN_BUTTON_DOWN					0xa2
#define PEN_RUBBER_DOWN					0xa5
#define PEN_RUBBER_UP					0xa4

struct hanvon_pen_data
{
    u16 x;
    u16 y;
    u16 pressure;
    u8 flag;
};

#define PINCTRL_STATE_ACTIVE	"pmx_hts_active"
#define PINCTRL_STATE_SUSPEND	"pmx_hts_suspend"
#define PINCTRL_STATE_RELEASE	"pmx_hts_release"

struct hanvon_i2c_chip {
	unsigned char * chipname;
	struct workqueue_struct *ktouch_wq;
	struct work_struct work_irq;
	struct mutex mutex_wq;
	struct i2c_client *client;
	unsigned char work_state;
	struct input_dev *p_inputdev;
	u32 vdd_gpio;
	u32 irq_gpio;
	u32 rst_gpio;
	u32 slp_gpio;
	u32 irq_gpio_flags;
	u32 irqflags;
//	int irq;
	struct pinctrl *ts_pinctrl;
	struct pinctrl_state *pinctrl_state_active;
	struct pinctrl_state *pinctrl_state_suspend;
	struct pinctrl_state *pinctrl_state_release;
};

static struct i2c_device_id hanvon_i2c_idtable[] = {
	{ "hanvon_0868_i2c", 0 }, 
	{ } 
}; 

MODULE_DEVICE_TABLE(i2c, hanvon_i2c_idtable);

static struct input_dev * allocate_hanvon_input_dev(struct hanvon_i2c_chip *hidp, struct i2c_client * client)
{
	int ret;
	struct input_dev *p_inputdev=NULL;

	p_inputdev = input_allocate_device();
	if(p_inputdev == NULL)
	{
		return NULL;
	}

	p_inputdev->name = "Hanvon electromagnetic pen";
	p_inputdev->phys = "I2C";
	p_inputdev->id.bustype = BUS_I2C;
	p_inputdev->dev.parent = &client->dev;
	
	hidp->p_inputdev = p_inputdev;
	
	input_set_drvdata(p_inputdev, hidp);
	
    set_bit(EV_ABS, p_inputdev->evbit);
    __set_bit(INPUT_PROP_DIRECT, p_inputdev->propbit);
    __set_bit(EV_ABS, p_inputdev->evbit);
    __set_bit(EV_KEY, p_inputdev->evbit);
    __set_bit(BTN_TOUCH, p_inputdev->keybit);
    __set_bit(BTN_TOOL_PEN, p_inputdev->keybit);
    __set_bit(BTN_TOOL_RUBBER, p_inputdev->keybit);
    input_set_abs_params(p_inputdev, ABS_X, 0, MAX_X, 0, 0);
    input_set_abs_params(p_inputdev, ABS_Y, 0, MAX_Y, 0, 0);
//    input_set_abs_params(p_inputdev, ABS_MT_POSITION_X, 0, MAX_X, 0, 0);
//    input_set_abs_params(p_inputdev, ABS_MT_POSITION_Y, 0, MAX_Y, 0, 0);
    input_set_abs_params(p_inputdev, ABS_PRESSURE, 0, 1024, 0, 0);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,36)
	input_set_events_per_packet(p_inputdev, MAX_EVENTS);
#endif

	ret = input_register_device(p_inputdev);
	if(ret) 
	{
		printk(KERN_INFO "Unable to register input device.\n");
		input_free_device(p_inputdev);
		p_inputdev = NULL;
	}
	
	return p_inputdev;
}

static struct hanvon_pen_data hanvon_get_packet(struct hanvon_i2c_chip *phic)
{
	struct hanvon_pen_data data = {0};
	struct i2c_client *client = phic->client;
	u8 x_buf[7];
	int count;

	do{
		count = i2c_master_recv(client, x_buf, 7);
//		pr_err("hanvon_get_packet count %d, EAGAIN %d\n", count, EAGAIN);
	} while(count == EAGAIN);
//	printk(KERN_INFO "Reading data. %.2x %.2x %.2x %.2x %.2x %.2x %.2x\n", 
//				x_buf[0], x_buf[1], x_buf[2], x_buf[3], x_buf[4], x_buf[5], x_buf[6]);
//	pr_err("hanvon_get_packet Reading data. %.2x %.2x %.2x %.2x %.2x %.2x %.2x\n", 
//				x_buf[0], x_buf[1], x_buf[2], x_buf[3], x_buf[4], x_buf[5], x_buf[6]);
	data.x |= ((x_buf[1]&0x7f) << 9) | (x_buf[2] << 2) | (x_buf[6] >> 5); // x
	data.y |= ((x_buf[3]&0x7f) << 9) | (x_buf[4] << 2) | ((x_buf[6] >> 3)&0x03); // y
	data.pressure |= ((x_buf[6]&0x07) << 7) | (x_buf[5]);  // pressure
	data.flag = x_buf[0];
	return data;
}

static int hanvon_report_event(struct hanvon_i2c_chip *phic)
{
	struct hanvon_pen_data data = {0};
	static int last_status = 0;
	data = hanvon_get_packet(phic);
	//printk(KERN_INFO "hanvon x=%d, y=%d, pressure=%d, flag=%d\n", data.x, data.y, data.pressure, data.flag);
//	pr_err("hanvon x=%d, y=%d, pressure=%d, flag=%d\n", data.x, data.y, data.pressure, data.flag);
//	pr_err("hanvon_report_event data.flag %d\n", data.flag);
    switch(data.flag)
    {
        case PEN_RUBBER_DOWN:	//0xa5 165
        {   
			input_report_abs(phic->p_inputdev, ABS_X, data.x);
			input_report_abs(phic->p_inputdev, ABS_Y, data.y);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_X, data.x);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_Y, data.y);
			input_report_abs(phic->p_inputdev, ABS_PRESSURE, data.pressure);
			input_report_key(phic->p_inputdev, BTN_TOUCH, 1);
			input_report_key(phic->p_inputdev, BTN_TOOL_RUBBER, 1);
			last_status = data.flag;
	    	break;
        }
        case PEN_RUBBER_UP:		//0xa4 164
        {
			input_report_abs(phic->p_inputdev, ABS_X, data.x);
			input_report_abs(phic->p_inputdev, ABS_Y, data.y);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_X, data.x);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_Y, data.y);
			input_report_abs(phic->p_inputdev, ABS_PRESSURE, data.pressure);
			input_report_key(phic->p_inputdev, BTN_TOUCH, 0);
			input_report_key(phic->p_inputdev, BTN_TOOL_RUBBER, 0);
			last_status = 0;
			break;
        }
		case PEN_POINTER_DOWN:		//0xa1 161
		{
			input_report_abs(phic->p_inputdev, ABS_X, data.x);
			input_report_abs(phic->p_inputdev, ABS_Y, data.y);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_X, data.x);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_Y, data.y);
			input_report_abs(phic->p_inputdev, ABS_PRESSURE, data.pressure);
			input_report_key(phic->p_inputdev, BTN_TOUCH, 1);
			input_report_key(phic->p_inputdev, BTN_TOOL_PEN, 1);
			last_status = data.flag;
			break;
		}
		case PEN_POINTER_UP:		//0xa0 160
		{
			input_report_abs(phic->p_inputdev, ABS_X, data.x);
			input_report_abs(phic->p_inputdev, ABS_Y, data.y);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_X, data.x);
//			input_report_abs(phic->p_inputdev, ABS_MT_POSITION_Y, data.y);
			input_report_abs(phic->p_inputdev, ABS_PRESSURE, data.pressure);
			input_report_key(phic->p_inputdev, BTN_TOUCH, 0);
			input_report_key(phic->p_inputdev, BTN_TOOL_PEN, 0);
			last_status = 0;
			break;
		}
    }
    input_sync(phic->p_inputdev);
	return 0;
}

static void hanvon_i2c_wq(struct work_struct *work)
{
	struct hanvon_i2c_chip *phid = container_of(work, struct hanvon_i2c_chip, work_irq);
	struct i2c_client *client = phid->client;
	//int gpio = irq_to_gpio(client->irq);
	//printk(KERN_INFO "%s: queue work.gpio(%d)\n", __func__, gpio);
//	pr_err("%s: queue work.\n", __func__);
	mutex_lock(&phid->mutex_wq);
	hanvon_report_event(phid);
	schedule();
	mutex_unlock(&phid->mutex_wq);
	enable_irq(client->irq);
}

static irqreturn_t hanvon_i2c_interrupt(int irq, void *dev_id)
{
	struct hanvon_i2c_chip *phid = (struct hanvon_i2c_chip *)dev_id;

	disable_irq_nosync(irq);
	queue_work(phid->ktouch_wq, &phid->work_irq);
	//printk(KERN_INFO "%s:Interrupt handled.\n", __func__);
//	pr_err("%s: Interrupt handled.\n", __func__);
	return IRQ_HANDLED;
}

static void hanvon_i2c_on_opt(struct hanvon_i2c_chip *hidp, bool on)
{
	if (true == on) {
		gpio_set_value(hidp->vdd_gpio, 1);
		msleep(50);
		gpio_set_value(hidp->slp_gpio, 1);
		msleep(50);
		gpio_set_value(hidp->rst_gpio, 1);
	} else {
		gpio_set_value(hidp->rst_gpio, 0);
		gpio_set_value(hidp->slp_gpio, 0);
		gpio_set_value(hidp->vdd_gpio, 0);
	}
}

static void hanvon_i2c_rst_opt(struct hanvon_i2c_chip *hidp)
{
	int err;
	if (gpio_is_valid(hidp->vdd_gpio)) {
		err = gpio_request(hidp->vdd_gpio, "hanvon_i2c_vdd_gpio");
		if (err) {
			dev_err(&hidp->client->dev, "vdd gpio request failed");
			goto free_vdd_gpio;
		}

		err = gpio_direction_output(hidp->vdd_gpio, 1);
		if (err) {
			dev_err(&hidp->client->dev,
				"set_direction for vdd gpio failed\n");
			goto free_vdd_gpio;
		}
		gpio_set_value(hidp->vdd_gpio, 1);
	}
	if (gpio_is_valid(hidp->slp_gpio)) {
		err = gpio_request(hidp->slp_gpio, "hanvon_i2c_slp_gpio");
		if (err) {
			dev_err(&hidp->client->dev, "sleep gpio request failed");
			goto free_sleep_gpio;
		}

		err = gpio_direction_output(hidp->slp_gpio, 1);
		if (err) {
			dev_err(&hidp->client->dev,
				"set_direction for sleep gpio failed\n");
			goto free_sleep_gpio;
		}
		gpio_set_value(hidp->slp_gpio, 1);
	}
	if (gpio_is_valid(hidp->rst_gpio)) {
		err = gpio_request(hidp->rst_gpio, "hanvon_i2c_rst_gpio");
		if (err) {
			dev_err(&hidp->client->dev, "reset gpio request failed");
			goto free_reset_gpio;
		}

		err = gpio_direction_output(hidp->rst_gpio, 0);
		if (err) {
			dev_err(&hidp->client->dev,
				"set_direction for reset gpio failed\n");
			goto free_reset_gpio;
		}
//		msleep(50);
//		gpio_set_value_cansleep(hidp->rst_gpio, 0);
		msleep(50);
		gpio_set_value(hidp->rst_gpio, 1);
		dev_err(&hidp->client->dev, "set_direction for reset gpio success\n");
	}
free_vdd_gpio:
	if (gpio_is_valid(hidp->vdd_gpio))
		gpio_free(hidp->vdd_gpio);
free_reset_gpio:
	if (gpio_is_valid(hidp->rst_gpio))
		gpio_free(hidp->rst_gpio);
free_sleep_gpio:
	if (gpio_is_valid(hidp->slp_gpio))
		gpio_free(hidp->slp_gpio);
	
}


static int hanvon_i2c_pinctrl_init(struct hanvon_i2c_chip *ft5x06_data)
{
	int retval;

	/* Get pinctrl if target uses pinctrl */
	dev_err(&ft5x06_data->client->dev,
			"Target1\n");
	ft5x06_data->ts_pinctrl = devm_pinctrl_get(&(ft5x06_data->client->dev));
	if (IS_ERR_OR_NULL(ft5x06_data->ts_pinctrl)) {
		retval = PTR_ERR(ft5x06_data->ts_pinctrl);
		dev_err(&ft5x06_data->client->dev,
			"Target does not use pinctrl %d\n", retval);
		goto err_pinctrl_get;
	}
	dev_err(&ft5x06_data->client->dev,
			"Target1\n");
			
	ft5x06_data->pinctrl_state_active
		= pinctrl_lookup_state(ft5x06_data->ts_pinctrl,
				PINCTRL_STATE_ACTIVE);
	if (IS_ERR_OR_NULL(ft5x06_data->pinctrl_state_active)) {
		retval = PTR_ERR(ft5x06_data->pinctrl_state_active);
		dev_err(&ft5x06_data->client->dev,
			"Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_ACTIVE, retval);
		goto err_pinctrl_lookup;
	}
	dev_err(&ft5x06_data->client->dev,
			"Target2\n");
	ft5x06_data->pinctrl_state_suspend
		= pinctrl_lookup_state(ft5x06_data->ts_pinctrl,
			PINCTRL_STATE_SUSPEND);
	if (IS_ERR_OR_NULL(ft5x06_data->pinctrl_state_suspend)) {
		retval = PTR_ERR(ft5x06_data->pinctrl_state_suspend);
		dev_err(&ft5x06_data->client->dev,
			"Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_SUSPEND, retval);
		goto err_pinctrl_lookup;
	}
	dev_err(&ft5x06_data->client->dev,
			"Target3\n");
	ft5x06_data->pinctrl_state_release
		= pinctrl_lookup_state(ft5x06_data->ts_pinctrl,
			PINCTRL_STATE_RELEASE);
	if (IS_ERR_OR_NULL(ft5x06_data->pinctrl_state_release)) {
		retval = PTR_ERR(ft5x06_data->pinctrl_state_release);
		dev_err(&ft5x06_data->client->dev,
			"Can not lookup %s pinstate %d\n",
			PINCTRL_STATE_RELEASE, retval);
	}
	dev_err(&ft5x06_data->client->dev,
			"Target4\n");
	return 0;

err_pinctrl_lookup:
	devm_pinctrl_put(ft5x06_data->ts_pinctrl);
err_pinctrl_get:
	ft5x06_data->ts_pinctrl = NULL;
	return retval;
}

#ifdef CONFIG_OF
static int hanvon_i2c_parse_dt(struct device *dev,
			struct hanvon_i2c_chip *hidp)
{
	struct device_node *np = dev->of_node;

	hidp->vdd_gpio = of_get_named_gpio_flags(np, "hanvon,vdd-gpio",
				0, &hidp->irq_gpio_flags);
	if (hidp->vdd_gpio < 0)
		return hidp->vdd_gpio;
	
	hidp->irq_gpio = of_get_named_gpio_flags(np, "hanvon,irq-gpio",
				0, &hidp->irq_gpio_flags);
	if (hidp->irq_gpio < 0)
		return hidp->irq_gpio;
	
	hidp->rst_gpio = of_get_named_gpio_flags(np, "hanvon,reset-gpio",
				0, &hidp->irq_gpio_flags);
	if (hidp->rst_gpio < 0)
		return hidp->rst_gpio;
	
	hidp->slp_gpio = of_get_named_gpio_flags(np, "hanvon,sleep-gpio",
				0, &hidp->irq_gpio_flags);
	if (hidp->slp_gpio < 0)
		return hidp->slp_gpio;

	return 0;
}
#else
static int hanvon_i2c_parse_dt(struct device *dev,
			struct hanvon_i2c_chip *pdata)
{
	return -ENODEV;
}
#endif

/* Returns the enable state of device */
static ssize_t hanvon_i2c_get_enable(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct hanvon_i2c_chip *pdata = i2c_get_clientdata(client);
	int i = 0;

	for (;i<50;i++) {
		hanvon_report_event(pdata);
	}
	return snprintf(buf, sizeof(buf), "%d\n", 1);
}

static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP | S_IWOTH,
		   hanvon_i2c_get_enable, NULL);
		   
static struct attribute *hanvon_i2c_attributes[] = {
	&dev_attr_enable.attr,
	NULL
};

static struct attribute_group hanvon_i2c_attribute_group = {
	.attrs = hanvon_i2c_attributes
};

static int hanvon_i2c_probe(struct i2c_client * client, const struct i2c_device_id * idp)
{
	int result = -1;
//	int gpio;// = irq_to_gpio(client->irq);
	struct hanvon_i2c_chip *hidp = NULL;
	int err ;
//	int i = 0;
//	printk(KERN_INFO "starting %s, irq(%d).\n", __func__, gpio);
	
	hidp = (struct hanvon_i2c_chip*)kzalloc(sizeof(struct hanvon_i2c_chip), GFP_KERNEL);
	if(!hidp)
	{
		printk(KERN_INFO "request memory failed.\n");
		result = -ENOMEM;
		goto fail1;
	}
	result = hanvon_i2c_parse_dt(&client->dev, hidp);
	if (result) {
			dev_err(&client->dev, "DT parsing failed\n");
			goto fail1;
	}
	
	hidp->client = client;
	// setup input device.
	hidp->p_inputdev = allocate_hanvon_input_dev(hidp, client);

	err = hanvon_i2c_pinctrl_init(hidp);
	if (!err && hidp->ts_pinctrl) {
		err = pinctrl_select_state(hidp->ts_pinctrl,
					hidp->pinctrl_state_active);
		if (err < 0) {
			dev_err(&client->dev,
				"failed to select pin to active state");
			goto pinctrl_deinit;
		}
	} else {
		goto free_irq_gpio;
	}
	
//	gpio = hidp->irq_gpio;
	if (gpio_is_valid(hidp->irq_gpio)) {
		err = gpio_request(hidp->irq_gpio, "hanvon_i2c_irq_gpio");
		if (err) {
			dev_err(&client->dev, "irq gpio request failed");
			goto err_gpio_req;
		}
		err = gpio_direction_input(hidp->irq_gpio);
		if (err) {
			dev_err(&client->dev,
				"set_direction for irq gpio failed\n");
			goto free_irq_gpio;
		}
	}

	// setup work queue.
	
	hidp->ktouch_wq = create_singlethread_workqueue("hanvon0868");
	mutex_init(&hidp->mutex_wq);
	INIT_WORK(&hidp->work_irq, hanvon_i2c_wq);
	i2c_set_clientdata(client, hidp);
	// request irq.
//	result = request_irq(client->irq, hanvon_i2c_interrupt, IRQF_DISABLED | IRQF_TRIGGER_FALLING,
//		 client->name, hidp);IRQF_TRIGGER_HIGH
	client->irq = gpio_to_irq(hidp->irq_gpio);
//	printk(KERN_INFO "starting %s, irq(%d), irq_gpio %d.\n", __func__, gpio, hidp->irq_gpio);
	result = request_threaded_irq(client->irq, NULL,
				hanvon_i2c_interrupt,
				IRQF_TRIGGER_RISING | IRQF_ONESHOT,
				client->dev.driver->name, hidp);
	if(result)
	{
		printk(KERN_INFO " Request irq(%d) failed\n", client->irq);
		goto fail2;
	}
	err =
	    sysfs_create_group(&hidp->p_inputdev->dev.kobj,
			       &hanvon_i2c_attribute_group);
	hanvon_i2c_rst_opt(hidp);
				   
//	for (;i<50;i++) {
//		hanvon_report_event(hidp);
//	}
	printk(KERN_INFO "%s done.\n", __func__);
	return 0;
pinctrl_deinit:
	if (hidp->ts_pinctrl) {
		if (IS_ERR_OR_NULL(hidp->pinctrl_state_release)) {
			devm_pinctrl_put(hidp->ts_pinctrl);
			hidp->ts_pinctrl = NULL;
		} else {
			err = pinctrl_select_state(hidp->ts_pinctrl,
					hidp->pinctrl_state_release);
			if (err)
				pr_err("failed to select relase pinctrl state\n");
		}
	}
fail2:
	i2c_set_clientdata(client, NULL);
	destroy_workqueue(hidp->ktouch_wq);
	free_irq(client->irq, hidp);
	input_unregister_device(hidp->p_inputdev);
	hidp->p_inputdev = NULL;
fail1:
free_irq_gpio:
	if (gpio_is_valid(hidp->irq_gpio))
		gpio_free(hidp->irq_gpio);
err_gpio_req:
	kfree(hidp);
	hidp = NULL;
	return result;
}

static int hanvon_i2c_remove(struct i2c_client * client)
{
	return 0;
}


static const struct i2c_device_id hanvon_i2c_id[] = {
	{"hanvon_i2c", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, hanvon_i2c_id);

#ifdef CONFIG_OF
static struct of_device_id hanvon_i2c_match_table[] = {
	{ .compatible = "hanvon,0868",},
	{ },
};
#else
#define hanvon_i2c_match_table NULL
#endif

static int hanvon_i2c_suspend(struct device *dev)
{
	struct hanvon_i2c_chip *data = dev_get_drvdata(dev);
	pr_err("%s: work.\n", __func__);
	disable_irq(data->client->irq);
	hanvon_i2c_on_opt(data, false);
	return 0;
}

static int hanvon_i2c_resume(struct device *dev)
{
	struct hanvon_i2c_chip *data = dev_get_drvdata(dev);
	pr_err("%s: work.\n", __func__);
	hanvon_i2c_on_opt(data, true);
	enable_irq(data->client->irq);
	return 0;
}

static const struct dev_pm_ops hw0868_i2c_pm_ops = {
	.suspend = hanvon_i2c_suspend,
	.resume = hanvon_i2c_resume,
};

static struct i2c_driver hanvon_i2c_driver = {
	.driver = {
		.name 	= "hanvon_0868_i2c",
		.owner = THIS_MODULE,
		.of_match_table = hanvon_i2c_match_table,
		.pm = &hw0868_i2c_pm_ops,
	},
	.id_table	= hanvon_i2c_idtable,
	.probe		= hanvon_i2c_probe,
	.remove		= hanvon_i2c_remove,
};

static int hw0868_i2c_init(void)
{
	printk(KERN_INFO "hw0868 chip initializing ....\n");
	return i2c_add_driver(&hanvon_i2c_driver);
}

static void hw0868_i2c_exit(void)
{
	printk(KERN_INFO "hw0868 driver exit.\n");
	i2c_del_driver(&hanvon_i2c_driver);
}

module_init(hw0868_i2c_init);
module_exit(hw0868_i2c_exit);

MODULE_AUTHOR("zhang nian");
MODULE_DESCRIPTION("Electromagnetic Pen");
MODULE_LICENSE("GPL");

