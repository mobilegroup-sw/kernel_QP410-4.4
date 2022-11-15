/* drivers/input/misc/kionix_accel.c - Kionix accelerometer driver
 *
 * Copyright (C) 2012-2014 Kionix, Inc.
 * Written by Kuching Tan <kuchingtan@kionix.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/regulator/consumer.h>
#include <linux/sensors.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif /* CONFIG_HAS_EARLYSUSPEND */

#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif /* CONFIG_OF */

/* POWER SUPPLY VOLTAGE RANGE */
#define ICN6201_VDD_MIN_UV  2000000
#define ICN6201_VDD_MAX_UV  3300000
#define ICN6201_VIO_MIN_UV  1750000
#define ICN6201_VIO_MAX_UV  1950000

#define ICN6201_MIPITOLVDS_NAME  "mipitolvds"

/* Debug Message Flags */

#define ICN6201_KMSG_ERR	1	/* Print kernel debug message for error */
#define ICN6201_KMSG_INF	0	/* Print kernel debug message for info */

#if ICN6201_KMSG_ERR
#define ICN6201_KMSGERR(format, ...)	\
		dev_err(format, ## __VA_ARGS__)
#else
#define ICN6201_KMSGERR(format, ...)
#endif

#if ICN6201_KMSG_INF
#define ICN6201_KMSGINF(format, ...)	\
		dev_info(format, ## __VA_ARGS__)
#else
#define ICN6201_KMSGINF(format, ...)
#endif

struct icn6201_mipitolvds_driver {
	/* regulator data */
	bool power_on;
	struct regulator *vdd;
	struct regulator *vio;

	struct i2c_client *client;
	struct sensors_classdev cdev;
	int en_gpio;
	int rst_gpio;
	int first_gpio;
	int lcd5v_gpio;
	int lcdpwm_gpio;
	unsigned int en_gpio_flags;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend;
#endif				/* CONFIG_HAS_EARLYSUSPEND */
};

struct icn6201_i2c_reg_array {
	uint16_t reg_addr;
	uint16_t reg_data;
};

static struct icn6201_i2c_reg_array icn6201_register_init[] = {
/*	{0x20, 0xE0},  // 480*800
	{0x21, 0x20},
	{0x22, 0x31},
	{0x23, 0x64},
	{0x24, 0x00},
	{0x25, 0x5E},
	{0x26, 0x00},
	{0x27, 0x06},
	{0x28, 0x00},
	{0x29, 0x04},
	{0x34, 0x80},
	{0x36, 0x64},
	{0xB5, 0xA0},
	{0x5C, 0xFF},
	{0x13, 0x47},
	{0x56, 0x90},
	{0x6B, 0x21},
	{0x69, 0x25},
	{0x10, 0x47},
	{0x2A, 0x41},
	{0xB6, 0x20},
	{0x51, 0x20},
	{0x09, 0x10},
	*/
/*	{0x20,0x00},	// 2
	{0x21,0x20},
	{0x22,0x35},
	{0x23,0x50},
	{0x24,0x00},
	{0x25,0x50},
	{0x26,0x00},
	{0x27,0x04},
	{0x28,0x00},
	{0x29,0x08},
	{0x34,0x80},
	{0x36,0x50},
	{0xB5,0xA0},
	{0x5C,0xFF},
	{0x13,0x47},
	{0x87,0x17},
	{0x56,0x90},
	{0x6B,0x21},
	{0x69,0x25},
	{0x10,0x47},
	{0x2A,0x41},
	{0xB6,0x20},
	{0x51,0x20},
	{0x09,0x10},
*/
/*	{0x20, 0x00},	// 3
	{0x21, 0x20},
	{0x22, 0x35},
	{0x23, 0x50},
	{0x24, 0x14},
	{0x25, 0x50},
	{0x26, 0x00},
	{0x27, 0x14},
	{0x28, 0x0A},
	{0x29, 0x14},
	{0x34, 0x80},
	{0x36, 0x50},
	{0xB5, 0xA0},
	{0x5C, 0xFF},
	{0x56, 0x90},
	{0x6B, 0x21},
	{0x69, 0x25},
	{0x10, 0x47},
	{0x2A, 0x41},
	{0xB6, 0x20},
	{0x51, 0x20},
	{0x09, 0x10},	
*/
/*	{0x20, 0x20},	// 4
	{0x21, 0xB0},
	{0x22, 0x43},
	{0x23, 0x10},
	{0x24, 0x00},
	{0x25, 0x00},
	{0x26, 0x00},
	{0x27, 0xA8},
	{0x28, 0x00},
	{0x29, 0x00},
	{0x34, 0x80},
	{0x36, 0x10},
	{0x86, 0x2A},
	{0xB5, 0xA0},
	{0x5C, 0xFF},
	{0x13, 0x47},
	{0x87, 0x17},
	{0x56, 0x90},
	{0x6B, 0x21},
	{0x69, 0x25},
//	{0x10, 0x47},
//	{0x2A, 0x41},
	{0xB6, 0x20},
	{0x51, 0x20},
	{0x09, 0x10},
	*/
	{0x20, 0x00},
	{0x21, 0x20},
	{0x22, 0x35},
	{0x23, 0x50},
	{0x24, 0x08},
	{0x25, 0x50},
	{0x26, 0x00},
	{0x27, 0x04},
	{0x28, 0x04},
	{0x29, 0x08},
	{0x34, 0x80},
	{0x36, 0x50},
	{0xB5, 0xA0},
	{0x5C, 0xFF},
//	{0x13, 0x47},	// lvds 3 lines
//	{0x13, 0x20},	// JEITA mode
	{0x56, 0x90},
	{0x6B, 0x21},
	{0x69, 0x25},
	{0xB6, 0x20},
	{0x51, 0x20},
	{0x09, 0x10},
};

/*
 * Global data
 */
static struct icn6201_mipitolvds_driver *icn6201_data;
/*
static int icn6201_i2c_read(struct i2c_client *client, char *writebuf,
			   int writelen, char *readbuf, int readlen)
{
	int ret;

	if (writelen > 0) {
		struct i2c_msg msgs[] = {
			{
				 .addr = client->addr,
				 .flags = 0,
				 .len = writelen,
				 .buf = writebuf,
			 },
			{
				 .addr = client->addr,
				 .flags = I2C_M_RD,
				 .len = readlen,
				 .buf = readbuf,
			 },
		};
		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret < 0)
			dev_err(&client->dev, "%s: i2c read error.\n",
				__func__);
	} else {
		struct i2c_msg msgs[] = {
			{
				 .addr = client->addr,
				 .flags = I2C_M_RD,
				 .len = readlen,
				 .buf = readbuf,
			 },
		};
		ret = i2c_transfer(client->adapter, msgs, 1);
		if (ret < 0)
			dev_err(&client->dev, "%s:i2c read error.\n", __func__);
	}
	return ret;
}

static int icn6201_i2c_write(struct i2c_client *client, char *writebuf,
			    int writelen)
{
	int ret;

	struct i2c_msg msgs[] = {
		{
			 .addr = client->addr,
			 .flags = 0,
			 .len = writelen,
			 .buf = writebuf,
		 },
	};
	ret = i2c_transfer(client->adapter, msgs, 1);
	if (ret < 0)
		dev_err(&client->dev, "%s: i2c write error.\n", __func__);

	return ret;
}

static int icn6201_write_reg(struct i2c_client *client, u8 addr, const u8 val)
{
	u8 buf[2] = {0};

	buf[0] = addr;
	buf[1] = val;

	return icn6201_i2c_write(client, buf, sizeof(buf));
}

static int icn6201_read_reg(struct i2c_client *client, u8 addr, u8 *val)
{
	return icn6201_i2c_read(client, &addr, 1, val, 1);
}*/

static int icn6201_mipitolvds_regulator_configure(
	struct icn6201_mipitolvds_driver *data, bool on)
{
	int rc;

	ICN6201_KMSGERR(&data->client->dev, "icn6201_mipitolvds_regulator_configure %d\n", on);
	if (!on) {
		if (regulator_count_voltages(data->vdd) > 0)
			regulator_set_voltage(data->vdd, 0,
				ICN6201_VDD_MAX_UV);

		regulator_put(data->vdd);

		if (regulator_count_voltages(data->vio) > 0)
			regulator_set_voltage(data->vio, 0,
				ICN6201_VIO_MAX_UV);

		regulator_put(data->vio);
	} else {
		data->vdd = regulator_get(&data->client->dev, "vdd");
		if (IS_ERR(data->vdd)) {
			rc = PTR_ERR(data->vdd);
			dev_err(&data->client->dev,
				"Regulator get failed vdd rc=%d\n", rc);
			return rc;
		}

		if (regulator_count_voltages(data->vdd) > 0) {
			rc = regulator_set_voltage(data->vdd,
				ICN6201_VDD_MIN_UV, ICN6201_VDD_MAX_UV);
			if (rc) {
				dev_err(&data->client->dev,
					"Regulator set failed vdd rc=%d\n",
					rc);
				goto reg_vdd_put;
			}
		}

		data->vio = regulator_get(&data->client->dev, "vio");
		if (IS_ERR(data->vio)) {
			rc = PTR_ERR(data->vio);
			dev_err(&data->client->dev,
				"Regulator get failed vio rc=%d\n", rc);
			goto reg_vdd_set;
		}

		if (regulator_count_voltages(data->vio) > 0) {
			rc = regulator_set_voltage(data->vio,
				ICN6201_VIO_MIN_UV, ICN6201_VIO_MAX_UV);
			if (rc) {
				dev_err(&data->client->dev,
				"Regulator set failed vio rc=%d\n", rc);
				goto reg_vio_put;
			}
		}
	}
	ICN6201_KMSGERR(&data->client->dev, "icn6201_mipitolvds_regulator_configure exit\n");

	return 0;
reg_vio_put:
	regulator_put(data->vio);

reg_vdd_set:
	if (regulator_count_voltages(data->vdd) > 0)
		regulator_set_voltage(data->vdd, 0, ICN6201_VDD_MAX_UV);
reg_vdd_put:
	regulator_put(data->vdd);
	return rc;
}

static int icn6201_mipitolvds_parse_dt(struct device *dev, struct icn6201_mipitolvds_driver *hall)
{	
	struct device_node *np = dev->of_node;

	pr_err("%s: here\n", __func__);
	/* en gpio */
	hall->en_gpio = of_get_named_gpio_flags(np, "icn6201,en-gpio",						 
			0, &hall->en_gpio_flags);
	
	printk("%s: irq_gpio %d\n", __func__, hall->en_gpio);
	
	hall->rst_gpio = of_get_named_gpio_flags(np, "icn6201,rst-gpio",
			0, NULL);
			
	hall->first_gpio = of_get_named_gpio_flags(np, "icn6201,first-gpio",
			0, NULL);
	
	hall->lcd5v_gpio = of_get_named_gpio_flags(np, "icn6201,lcd5v-gpio",
			0, NULL);
	
	hall->lcdpwm_gpio = of_get_named_gpio_flags(np, "icn6201,lcdpwm-gpio",
			0, NULL);
			
	if (hall->en_gpio < 0)
		return hall->en_gpio;
	return 0;
}

static int icn6201_mipitolvds_regulator_power_on(
	struct icn6201_mipitolvds_driver *data, bool on)
{
	int rc = 0;

	ICN6201_KMSGERR(&data->client->dev, "icn6201_mipitolvds_regulator_power_on %d\n", on);

	if (!on) {
		gpio_set_value(data->first_gpio, 0);
		gpio_set_value(data->en_gpio, 0);
		gpio_set_value(data->lcd5v_gpio, 0);
		gpio_set_value(data->lcdpwm_gpio, 0);
		gpio_set_value(data->rst_gpio, 0);
		
		rc = regulator_disable(data->vdd);
		if (rc) {
			dev_err(&data->client->dev,
				"Regulator vdd disable failed rc=%d\n", rc);
			return rc;
		}

		rc = regulator_disable(data->vio);
		if (rc) {
			dev_err(&data->client->dev,
				"Regulator vio disable failed rc=%d\n", rc);
			rc = regulator_enable(data->vdd);
			dev_err(&data->client->dev,
					"Regulator vio re-enabled rc=%d\n", rc);
			/*
			 * Successfully re-enable regulator.
			 * Enter poweron delay and returns error.
			 */
			if (!rc) {
				rc = -EBUSY;
				goto enable_delay;
			}
		}
		return rc;
	} else {
		gpio_set_value(data->first_gpio, 1);
//		gpio_set_value(data->rst_gpio, 0);
//		udelay (1000);
		gpio_set_value(data->en_gpio, 1);
//		udelay (1000);
		gpio_set_value(data->rst_gpio, 1);
		
		gpio_set_value(data->lcd5v_gpio, 0);
		gpio_set_value(data->lcdpwm_gpio, 1);

		rc = regulator_enable(data->vdd);
		if (rc) {
			dev_err(&data->client->dev,
				"Regulator vdd enable failed rc=%d\n", rc);
			return rc;
		}

		rc = regulator_enable(data->vio);
		if (rc) {
			dev_err(&data->client->dev,
				"Regulator vio enable failed rc=%d\n", rc);
			regulator_disable(data->vdd);
			return rc;
		}
	}
	ICN6201_KMSGERR(&data->client->dev, "icn6201_mipitolvds_regulator_power_on exit\n");

enable_delay:
	msleep(130);
	dev_dbg(&data->client->dev,
		"Sensor regulator power on =%d\n", on);
	return rc;
}

static int icn6201_mipitolvds_platform_hw_init(void)
{
	struct i2c_client *client;
	struct icn6201_mipitolvds_driver *data;
	int error;

	if (icn6201_data == NULL)
		return -ENODEV;

	data = icn6201_data;
	client = data->client;
	ICN6201_KMSGERR(&client->dev, "icn6201_mipitolvds_platform_hw_init\n");
	error = icn6201_mipitolvds_regulator_configure(data, true);
	if (error < 0) {
		dev_err(&client->dev, "unable to configure regulator\n");
		return error;
	}
	ICN6201_KMSGERR(&client->dev, "icn6201_mipitolvds_platform_hw_init exit\n");
	return 0;
}


static int icn6201_register_power_on_init(struct icn6201_mipitolvds_driver *acceld)
{
	int err;
	int tmp;
	int register_len = 0;
	int32_t value = 0;
	ICN6201_KMSGERR(&acceld->client->dev, "icn6201_register_power_on_init\n");
	register_len = ARRAY_SIZE(icn6201_register_init);

//address write
		value = i2c_smbus_read_byte_data(acceld->client,0x00);
		ICN6201_KMSGERR(&acceld->client->dev, "i2c_smbus_read_byte_data 0x00:%d \n",value);



	for (tmp = 0; tmp < register_len; tmp++) {
		/* ensure that PC1 is cleared before updating control registers */
		err = i2c_smbus_write_byte_data(acceld->client,
						icn6201_register_init[tmp].reg_addr, icn6201_register_init[tmp].reg_data);

		if (err < 0)
			return err;
	}
	ICN6201_KMSGERR(&acceld->client->dev, "icn6201_register_power_on_init exit\n");
	return 0;
}

static int icn6201_mipitolvds_probe(struct i2c_client *client,
			      const struct i2c_device_id *id)
{
	struct icn6201_mipitolvds_driver *acceld;
	int err;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE_DATA)) {
		ICN6201_KMSGERR(&client->dev, "client is not i2c capable. Abort.\n");
		return -ENXIO;
	}
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {  //0430
		dev_err(&client->dev, "I2C not supported\n");
		return -ENODEV;
	}

	acceld = kzalloc(sizeof(*acceld), GFP_KERNEL);
	if (acceld == NULL) {
		ICN6201_KMSGERR(&client->dev,
			"failed to allocate memory for module data. Abort.\n");
		goto err_free_mem;
	}
	icn6201_data = acceld;
	acceld->client = client;

	i2c_set_clientdata(client, acceld);
	ICN6201_KMSGERR(&client->dev, "icn6201_mipitolvds_probe entry\n");

	icn6201_mipitolvds_parse_dt(&client->dev, acceld);
	err = gpio_request(acceld->en_gpio, "ICN6201_EN_PM");
	gpio_direction_output(acceld->en_gpio, 1);
	err = gpio_request(acceld->rst_gpio, "ICN6201_RST_PM");
	gpio_direction_output(acceld->rst_gpio, 1);
	err = gpio_request(acceld->first_gpio, "ICN6201_FIRST_GPIO");
	gpio_direction_output(acceld->first_gpio, 1);
	err = gpio_request(acceld->lcd5v_gpio, "ICN6201_LCD5V_GPIO");
	gpio_direction_output(acceld->lcd5v_gpio, 0);
	err = gpio_request(acceld->lcdpwm_gpio, "ICN6201_LCDPWM_EN");
	gpio_direction_output(acceld->lcdpwm_gpio, 1);
	
	/* h/w initialization */
	icn6201_mipitolvds_platform_hw_init();
	icn6201_mipitolvds_regulator_power_on(acceld, 1);
	err = icn6201_register_power_on_init(acceld);
	if (err < 0 ) {
		ICN6201_KMSGERR(&client->dev, "icn6201_register_power_on_init failed\n");
	}


#ifdef CONFIG_HAS_EARLYSUSPEND
	acceld->early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 50;
	acceld->early_suspend.suspend = icn6201_mipitolvds_earlysuspend_suspend;
	acceld->early_suspend.resume = icn6201_mipitolvds_earlysuspend_resume;
	register_early_suspend(&acceld->early_suspend);
#endif /* CONFIG_HAS_EARLYSUSPEND */

	/* Register to sensors class */
//	acceld->cdev = sensors_cdev;
//	acceld->cdev.sensors_enable = icn6201_accel_cdev_enable;
//	acceld->cdev.sensors_poll_delay = icn6201_accel_cdev_poll_delay;
//	err = sensors_classdev_register(&client->dev, &acceld->cdev);
//	if (err) {
//		dev_err(&client->dev, "create class device file failed\n");
//		err = -EINVAL;
//		goto exit_remove_sysfs_group;
//	}

	ICN6201_KMSGERR(&client->dev, "icn6201_mipitolvds_probe finish\n");
	return 0;

/*exit_unregister_acc_class:
	sensors_classdev_unregister(&acceld->cdev);*/
err_free_mem:
	icn6201_data = NULL;
	kfree(acceld);
	return err;
}

static int icn6201_mipitolvds_remove(struct i2c_client *client)
{
	struct icn6201_mipitolvds_driver *acceld = i2c_get_clientdata(client);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&acceld->early_suspend);
#endif /* CONFIG_HAS_EARLYSUSPEND */

	icn6201_mipitolvds_regulator_power_on(acceld, 0);
	
	icn6201_data = NULL;
	kfree(acceld);

	return 0;
}

#ifdef CONFIG_PM
static int icn6201_mipitolvds_suspend(struct i2c_client *client, pm_message_t mesg)
{
	struct icn6201_mipitolvds_driver *acceld = i2c_get_clientdata(client);

	icn6201_mipitolvds_regulator_power_on(acceld, 0);
	return 0;
}

static int icn6201_mipitolvds_resume(struct i2c_client *client)
{
	struct icn6201_mipitolvds_driver *acceld = i2c_get_clientdata(client);

	icn6201_mipitolvds_regulator_power_on(acceld, 1);
	icn6201_register_power_on_init(acceld);

	return 0;
}

#else

#define icn6201_mipitolvds_suspend      NULL
#define icn6201_mipitolvds_resume       NULL

#endif /* CONFIG_PM */

static const struct i2c_device_id icn6201_mipitolvds_id[] = {
	{ICN6201_MIPITOLVDS_NAME, 0},
	{},
};

static struct of_device_id icn6201_mipitolvds_match_table[] = {
	{.compatible = "icn6201,mipitolvds-6201",},
	{},
};

MODULE_DEVICE_TABLE(i2c, icn6201_mipitolvds_id);

static struct i2c_driver icn6201_mipitolvds_driver = {
	.driver = {
		   .name = ICN6201_MIPITOLVDS_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = icn6201_mipitolvds_match_table,
		   },
	.suspend    = icn6201_mipitolvds_suspend,
	.resume     = icn6201_mipitolvds_resume,
	.probe = icn6201_mipitolvds_probe,
	.remove = icn6201_mipitolvds_remove,
	.id_table = icn6201_mipitolvds_id,
};

static int __init icn6201_mipitolvds_init(void)
{
	return i2c_add_driver(&icn6201_mipitolvds_driver);
}

module_init(icn6201_mipitolvds_init);

static void __exit icn6201_mipitolvds_exit(void)
{
	i2c_del_driver(&icn6201_mipitolvds_driver);
}

module_exit(icn6201_mipitolvds_exit);

MODULE_DESCRIPTION("icn6201 mipitolvds driver");
MODULE_AUTHOR("Quanqiao Lee <liqq@ym-tek.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
