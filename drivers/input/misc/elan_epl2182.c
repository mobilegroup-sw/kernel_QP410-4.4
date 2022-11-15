/* drivers/input/misc/elan_epl2182.c - light and proxmity sensors driver
 * Copyright (C) 2011-2014 ELAN Corporation.
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
#include <linux/debugfs.h>
#include <linux/wakelock.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/input.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/unistd.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/of_gpio.h>
#include <linux/sensors.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#include <linux/input/elan_interface.h>
/*********************************************************
 * configuration
*********************************************************/
/* 0 is polling mode, 1 is interrupt mode*/
#define PS_INTERRUPT_MODE		0				// lee add, 20150105, for used polling mode.

#define PS_POLLING_RATE		100
#define ALS_POLLING_RATE		1000	/* msec */

#define LUX_PER_COUNT			1100
#undef  DEBUG
#define DEBUG 0
#ifdef DEBUG
#undef pr_debug
#define pr_debug pr_err
#define epl_info(fmt, ...) \
	pr_err(pr_fmt(fmt), ##__VA_ARGS__)
#else
#define epl_info(fmt, ...) \
	no_printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#endif

#define LOG_FUN(f)		epl_info("%s  %d\n", __func__, __LINE__)
/**********************************************************
 * configuration
**********************************************************/
enum CMC_MODE {
	CMC_MODE_ALS = 0x00,
	CMC_MODE_PS = 0x10,
};
#define TXBYTES				2
#define RXBYTES				2

#define PS_DELAY			55
#define ALS_DELAY			55

#define PACKAGE_SIZE			2
#define I2C_RETRY_COUNT			10
#define P_INTT				1

#define PS_INTT				4
#define ALS_INTT			5

#define PS_H_THRESHOLD 2000
#define PS_L_THRESHOLD 1000

#define PS_AUTO_ENABLE	1
#if PS_AUTO_ENABLE
#define DYN_L_OFFSET	100
#define DYN_H_OFFSET	400
#define DYN_CONDITION	30000
#endif

static int set_psensor_intr_threshold(uint16_t low_thd, uint16_t high_thd);

#if PS_INTERRUPT_MODE
static void epl_sensor_irq_do_work(struct work_struct *work);
static DECLARE_WORK(epl_sensor_irq_work, epl_sensor_irq_do_work);
#endif

static void report_polling_do_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(report_polling_work, report_polling_do_work);
static void polling_do_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(polling_work, polling_do_work);

/* primitive raw data from I2C */
struct epl_raw_data {
	u8 raw_bytes[PACKAGE_SIZE];
	u16 ps_state;
	u16 ps_int_state;
	u16 ps_ch1_raw;
	u16 als_ch1_raw;
	u16 ps_sta;

#if PS_AUTO_ENABLE
	u16 ps_min_raw;
	u16 ps_condition;
	u16 ps_cal_h;
	u16 ps_cal_l;
#endif
};
/* static int sus_res = 0; */
struct elan_epl_data {
	struct i2c_client *client;
	/* regulator data */
	bool power_on;
	struct regulator *vdd;
	struct regulator *vio;

	/* pinctrl data*/
	struct pinctrl *pinctrl;
	struct pinctrl_state *pin_default;
	struct pinctrl_state *pin_sleep;

	unsigned int als_poll_delay;
	unsigned int ps_poll_delay;
	struct input_dev *als_input_dev;
	struct input_dev *ps_input_dev;

	struct sensors_classdev als_cdev;
	struct sensors_classdev ps_cdev;

	struct workqueue_struct *epl_wq;
	int intr_pin;
	int (*power) (int on);

	int ps_opened;
	int als_opened;

	unsigned int ps_th_l;	/* P_SENSOR_LTHD */
	unsigned int ps_th_h;	/* P_SENSOR_HTHD */
	int enable_pflag;
	int enable_lflag;
	/* save sensor enabling state for resume */
	unsigned int als_enable_state;

	int read_flag;
	int irq_gpio;
	unsigned int irq_gpio_flags;
};

int als_level[15]	= {20, 45, 70, 90, 150, 300, 500, 700, 1150, 2250, 4500, 8000, 15000, 30000, 50000};
int als_value[16]	= {10, 30, 60, 80, 100, 200, 400, 600, 800, 1500, 3000, 6000, 10000, 20000, 40000, 60000};

static struct wake_lock g_ps_wlock;
struct elan_epl_data *epl_data;
static struct epl_raw_data gRawData;
static int dual_count;

static const char ElanPsensorName[] = "proximity";
static const char ElanALsensorName[] = "light";

static int psensor_mode_suspend;

/*
 * Global data
 */
struct elan_epl_data *sensor_info;
static struct sensors_classdev sensors_light_cdev = {
	.name = "light",
	.vendor = "elan",
	.version = 1,
	.handle = SENSORS_LIGHT_HANDLE,
	.type = SENSOR_TYPE_LIGHT,
	.max_range = "30000",
	.resolution = "0.0125",
	.sensor_power = "0.20",
	.min_delay = 1000,	/* in microseconds */
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.enabled = 0,
	.delay_msec = 100,
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
};

static struct sensors_classdev sensors_proximity_cdev = {
	.name = "proximity",
	.vendor = "elan",
	.version = 1,
	.handle = SENSORS_PROXIMITY_HANDLE,
	.type = SENSOR_TYPE_PROXIMITY,
	.max_range = "5",
	.resolution = "5.0",
	.sensor_power = "3",
	.min_delay = 1000,	/* in microseconds */
	.fifo_reserved_event_count = 0,
	.fifo_max_event_count = 0,
	.enabled = 0,
	.delay_msec = 100,
	.sensors_enable = NULL,
	.sensors_poll_delay = NULL,
};

/*
//====================I2C write operation===============//
//regaddr: ELAN Register Address.
//bytecount: How many bytes to be written to register via i2c bus.
//txbyte: I2C bus transmit byte(s).
//Single byte(0X01) transmit only slave address.
//data: setting value.
//
// Example: If you want to write single byte
//to 0x1D register address, show below
//elan_sensor_I2C_Write(client,0x1D,	0x01,		0X02,	0xff);
//
//
*/
static int elan_sensor_I2C_Write(struct i2c_client *client, uint8_t regaddr,
				 uint8_t bytecount, uint8_t txbyte,
				 uint8_t data)
{
	uint8_t buffer[2];
	int ret = 0;
	int retry;

	buffer[0] = (regaddr << 3) | bytecount;
	buffer[1] = data;

//	epl_info("%s\n", __func__);
	for (retry = 0; retry < I2C_RETRY_COUNT; retry++) {
		ret = i2c_master_send(client, buffer, txbyte);

		if (ret == txbyte)
			break;
		msleep(25);
	}

	if (retry >= I2C_RETRY_COUNT) {
		pr_err(KERN_ERR "i2c write retry over %d\n", I2C_RETRY_COUNT);
		return -EINVAL;
	}

	return ret;
}

static int elan_sensor_I2C_Read(struct i2c_client *client)
{
	uint8_t buffer[RXBYTES];
	int ret = 0, i = 0;
	int retry;

//	epl_info("%s\n", __func__);
	for (retry = 0; retry < I2C_RETRY_COUNT; retry++) {

		ret = i2c_master_recv(client, buffer, RXBYTES);
		if (ret == RXBYTES)
			break;
		msleep(25);
	}

	if (retry >= I2C_RETRY_COUNT) {
		pr_err("i2c read retry over %d\n", I2C_RETRY_COUNT);
		return -EINVAL;
	}

	for (i = 0; i < PACKAGE_SIZE; i++)
		gRawData.raw_bytes[i] = buffer[i];

	return ret;
}

static void elan_sensor_stop_work(void)
{
//	struct elan_epl_data *epld = epl_data;
//	epl_info("%s\n", __func__);
	cancel_delayed_work(&polling_work);
	cancel_delayed_work(&report_polling_work);
//	queue_delayed_work(epld->epl_wq, &polling_work, msecs_to_jiffies(10));
}

static void elan_sensor_restart_work(void)
{
	struct elan_epl_data *epld = epl_data;
//	epl_info("%s\n", __func__);
	cancel_delayed_work(&polling_work);
	cancel_delayed_work(&report_polling_work);
	queue_delayed_work(epld->epl_wq, &polling_work, msecs_to_jiffies(10));
}

#if PS_AUTO_ENABLE
static void dyn_ps_cal(struct elan_epl_data *epld)
{
	if((gRawData.ps_ch1_raw < gRawData.ps_min_raw)
	&& (gRawData.ps_sta != 1)
	&& (gRawData.ps_condition <= DYN_CONDITION))
	{
		gRawData.ps_min_raw = gRawData.ps_ch1_raw;
		epld->ps_th_l = gRawData.ps_ch1_raw + DYN_L_OFFSET;
		epld->ps_th_h = gRawData.ps_ch1_raw + DYN_H_OFFSET;
		set_psensor_intr_threshold(epld->ps_th_l, epld->ps_th_h);
		epl_info("dyn ps raw = %d, min = %d, condition = %d\n dyn h_thre = %d, l_thre = %d, ps_state = %d",
		gRawData.ps_ch1_raw, gRawData.ps_min_raw, gRawData.ps_condition,epld->ps_th_h, epld->ps_th_l, gRawData.ps_state);
	}
}
#endif

static int elan_sensor_psensor_enable(struct elan_epl_data *epld)
{
	int ret;
	uint8_t regdata = 0;
	struct i2c_client *client = epld->client;
	LOG_FUN();
	epl_info("--- Proximity sensor Enable ---\n");

	ret =
	    elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE, 0x02,
				  EPL_INT_DISABLE);

	regdata = EPL_SENSING_2_TIME | EPL_PS_MODE | EPL_L_GAIN;
	regdata =
	    regdata | (PS_INTERRUPT_MODE ? EPL_C_SENSING_MODE :
		       EPL_S_SENSING_MODE);
	ret =
	    elan_sensor_I2C_Write(client, REG_0, W_SINGLE_BYTE, 0X02, regdata);

	regdata = PS_INTT << 4 | EPL_PST_1_TIME | EPL_10BIT_ADC;
	ret =
	    elan_sensor_I2C_Write(client, REG_1, W_SINGLE_BYTE, 0X02, regdata);

	set_psensor_intr_threshold(epld->ps_th_l, epld->ps_th_h);

	ret =
	    elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0X02,
				  EPL_C_RESET);
	ret =
	    elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0x02,
				  EPL_C_START_RUN);
        msleep(PS_DELAY);

		elan_sensor_I2C_Write(client, REG_13, R_SINGLE_BYTE, 0x01, 0);
		elan_sensor_I2C_Read(client);
		gRawData.ps_state = !((gRawData.raw_bytes[0] & 0x04) >> 2);
		gRawData.ps_sta = ((gRawData.raw_bytes[0]&0x02)>>1);

#if PS_AUTO_ENABLE
		elan_sensor_I2C_Write(client, REG_16, R_TWO_BYTE, 0x01, 0x00);
		elan_sensor_I2C_Read(client);
		gRawData.ps_ch1_raw = (gRawData.raw_bytes[1] << 8) | gRawData.raw_bytes[0];

	    	elan_sensor_I2C_Write(client,REG_14,R_TWO_BYTE,0x01,0x00);
	    	elan_sensor_I2C_Read(client);
		gRawData.ps_condition= ((gRawData.raw_bytes[1]<<8) | gRawData.raw_bytes[0]);

		dyn_ps_cal(epld);
#endif

#if PS_INTERRUPT_MODE
	if (epld->enable_pflag) {		

		if (gRawData.ps_state != gRawData.ps_int_state) {
			elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE,
					      0x02, EPL_INT_FRAME_ENABLE);
		} else {
			elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE,
					      0x02, EPL_INT_ACTIVE_LOW);
		}

	} else {
		elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE, 0x02,
				      EPL_INT_ACTIVE_LOW);
	}
#endif

	if (ret != 0x02)
		epl_info("P-sensor i2c err\n");

	return ret;
}

static int elan_sensor_lsensor_enable(struct elan_epl_data *epld)
{
	int ret;
	uint8_t regdata = 0;
	struct i2c_client *client = epld->client;
	LOG_FUN();
	epl_info("--- ALS sensor Enable --\n");

	regdata = EPL_INT_DISABLE;
	ret =
	    elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE, 0x02, regdata);

	regdata =
	    EPL_S_SENSING_MODE | EPL_SENSING_8_TIME | EPL_ALS_MODE |
	    EPL_AUTO_GAIN;
	ret =
	    elan_sensor_I2C_Write(client, REG_0, W_SINGLE_BYTE, 0X02, regdata);

	regdata = ALS_INTT << 4 | EPL_PST_1_TIME | EPL_10BIT_ADC;
	ret =
	    elan_sensor_I2C_Write(client, REG_1, W_SINGLE_BYTE, 0X02, regdata);

	ret =
	    elan_sensor_I2C_Write(client, REG_10, W_SINGLE_BYTE, 0x02,
				  EPL_GO_MID);
	ret =
	    elan_sensor_I2C_Write(client, REG_11, W_SINGLE_BYTE, 0x02,
				  EPL_GO_LOW);

	ret =
	    elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0X02,
				  EPL_C_RESET);
	ret =
	    elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0x02,
				  EPL_C_START_RUN);

	if (ret != 0x02)
		epl_info(" ALS-sensor i2c err\n");

	return ret;

}

/*
//====================elan_epl_ps_poll_rawdata===============//
//polling method for proximity sensor detect.
//Report proximity sensor raw data.
//Report "ABS_DISTANCE" event to HAL layer.
//Variable "value" 0 and 1 to represent
//which distance from psensor to target(human's face..etc).
//value: 0 represent near.
//value: 1 represent far.
*/
static void elan_epl_ps_poll_rawdata(void)
{
	struct elan_epl_data *epld = epl_data;
	struct i2c_client *client = epld->client;

//	epl_info("%s\n", __func__);
	elan_sensor_I2C_Write(epld->client, REG_7, W_SINGLE_BYTE, 0x02,
			      EPL_DATA_LOCK);

	elan_sensor_I2C_Write(client, REG_13, R_SINGLE_BYTE, 0x01, 0);
	elan_sensor_I2C_Read(client);
	gRawData.ps_state = !((gRawData.raw_bytes[0] & 0x04) >> 2);
	elan_sensor_I2C_Write(client, REG_16, R_TWO_BYTE, 0x01, 0x00);
	elan_sensor_I2C_Read(client);
	gRawData.ps_ch1_raw =
	    (gRawData.raw_bytes[1] << 8) | gRawData.raw_bytes[0];

	elan_sensor_I2C_Write(epld->client, REG_7, W_SINGLE_BYTE, 0x02,
			      EPL_DATA_UNLOCK);

	epl_info("### ps_ch1_raw_data  (%d), value(%d) ###\n\n",
		 gRawData.ps_ch1_raw, gRawData.ps_state);

	input_report_abs(epld->ps_input_dev, ABS_DISTANCE, gRawData.ps_state);
	input_sync(epld->ps_input_dev);
}

static void elan_epl_als_rawdata(void)
{
	struct elan_epl_data *epld = epl_data;
	struct i2c_client *client = epld->client;
	int als_level_num = 16;
	uint32_t lux;
	int idx;
	elan_sensor_I2C_Write(client, REG_16, R_TWO_BYTE, 0x01, 0x00);
	elan_sensor_I2C_Read(client);
	gRawData.als_ch1_raw =
	    (gRawData.raw_bytes[1] << 8) | gRawData.raw_bytes[0];

	//lux = (gRawData.als_ch1_raw * LUX_PER_COUNT) / 1000 * 15 / 100;
	lux = (gRawData.als_ch1_raw * LUX_PER_COUNT) / 1000;
	
	//if (lux > 20000)
		//lux = 20000;
		

		for(idx = 0; idx < als_level_num; idx++)
    {
        if(lux < als_level[idx])
        {
            break;
        }
    }

    if(idx >= als_level_num)
    {
      //  APS_ERR("exceed range\n");
        idx = als_level_num - 1;
    }   

	epl_info("-------------------  ALS raw = %d, lux = %d\n\n",
		 gRawData.als_ch1_raw,  als_value[idx]);

	input_report_abs(epld->als_input_dev, ABS_MISC,  als_value[idx]);
	input_sync(epld->als_input_dev);
}

/*
//====================set_psensor_intr_threshold===============//
//low_thd: The value is psensor interrupt low threshold.
//high_thd:	The value is psensor interrupt hihg threshold.
//When psensor rawdata > hihg_threshold, interrupt pin will be pulled low.
//After interrupt occur, psensor rawdata < low_threshold,
//interrupt pin will be pulled high.
*/
static int set_psensor_intr_threshold(uint16_t low_thd, uint16_t high_thd)
{
	int ret = 0;
	struct elan_epl_data *epld = epl_data;
	struct i2c_client *client = epld->client;

	uint8_t high_msb, high_lsb, low_msb, low_lsb;

//	epl_info("%s\n", __func__);
	high_msb = (uint8_t) (high_thd >> 8);
	high_lsb = (uint8_t) (high_thd & 0x00ff);
	low_msb = (uint8_t) (low_thd >> 8);
	low_lsb = (uint8_t) (low_thd & 0x00ff);

	elan_sensor_I2C_Write(client, REG_2, W_SINGLE_BYTE, 0x02, high_lsb);
	elan_sensor_I2C_Write(client, REG_3, W_SINGLE_BYTE, 0x02, high_msb);
	elan_sensor_I2C_Write(client, REG_4, W_SINGLE_BYTE, 0x02, low_lsb);
	elan_sensor_I2C_Write(client, REG_5, W_SINGLE_BYTE, 0x02, low_msb);

	return ret;
}

#if PS_INTERRUPT_MODE
static void epl_sensor_irq_do_work(struct work_struct *work)
{
	struct elan_epl_data *epld = epl_data;
	struct i2c_client *client = epld->client;
	int mode = 0;
	LOG_FUN();

	elan_sensor_I2C_Write(epld->client, REG_7, W_SINGLE_BYTE, 0x02,
			      EPL_DATA_LOCK);
	elan_sensor_I2C_Write(client, REG_13, R_SINGLE_BYTE, 0x01, 0);
	elan_sensor_I2C_Read(client);
	mode = gRawData.raw_bytes[0] & (3 << 4);

	if (mode == CMC_MODE_PS && epld->enable_pflag) {
		gRawData.ps_int_state = !((gRawData.raw_bytes[0] & 0x04) >> 2);
		elan_epl_ps_poll_rawdata();
	} else {
		epl_info("error: interrupt in als\n");
	}

	elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE, 0x02,
			      EPL_INT_ACTIVE_LOW);

	elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0x02,
			      EPL_DATA_UNLOCK);

	enable_irq(client->irq);
}

static irqreturn_t elan_sensor_irq_handler(int irqNo, void *handle)
{
	struct i2c_client *client = (struct i2c_client *)handle;
	struct elan_epl_data *epld = i2c_get_clientdata(client);

	disable_irq_nosync(client->irq);
	queue_work(epld->epl_wq, &epl_sensor_irq_work);

	return IRQ_HANDLED;
}
#endif

static void report_polling_do_work(struct work_struct *work)
{
	struct elan_epl_data *epld = epl_data;

//	epl_info("%s: dual_count %d, enable_pflag %d\n", __func__, dual_count, epld->enable_pflag);
	if (dual_count == CMC_MODE_PS)
		elan_epl_ps_poll_rawdata();
	else if (dual_count == CMC_MODE_ALS)
		elan_epl_als_rawdata();

	if (epld->enable_pflag) {
		elan_sensor_psensor_enable(epld);

		if (PS_INTERRUPT_MODE == 0) {
			dual_count = CMC_MODE_PS;
			queue_delayed_work(epld->epl_wq, &report_polling_work,
					   msecs_to_jiffies
					   (epld->ps_poll_delay));
		}
	}
}

static void polling_do_work(struct work_struct *work)
{
	struct elan_epl_data *epld = epl_data;
	struct i2c_client *client = epld->client;

	bool isInterleaving = epld->enable_pflag == 1
	    && epld->enable_lflag == 1;
	bool isAlsOnly = epld->enable_pflag == 0 && epld->enable_lflag == 1;
	bool isPsOnly = epld->enable_pflag == 1 && epld->enable_lflag == 0;

	cancel_delayed_work(&polling_work);
	cancel_delayed_work(&report_polling_work);

	epl_info("enable_pflag = %d, enable_lflag = %d\n", epld->enable_pflag,
		 epld->enable_lflag);

	if (isAlsOnly || isInterleaving) {
		elan_sensor_lsensor_enable(epld);
		dual_count = CMC_MODE_ALS;

		queue_delayed_work(epld->epl_wq, &report_polling_work,
				   msecs_to_jiffies(ALS_DELAY));
		queue_delayed_work(epld->epl_wq, &polling_work,
				   msecs_to_jiffies(epld->als_poll_delay));
//	} else if (isPsOnly || PS_AUTO_ENABLE) {
	} else if (isPsOnly) {
		elan_sensor_psensor_enable(epld);
		dual_count = CMC_MODE_PS;

		if (PS_INTERRUPT_MODE) {
			/* do nothing */
		} else {
			queue_delayed_work(epld->epl_wq, &report_polling_work,
					   msecs_to_jiffies(PS_DELAY));
			queue_delayed_work(epld->epl_wq, &polling_work,
					   msecs_to_jiffies
					   (epld->ps_poll_delay));
		}
	} else {
		elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE, 0x02,
				      EPL_INT_DISABLE);
		elan_sensor_I2C_Write(client, REG_0, W_SINGLE_BYTE, 0X02,
				      EPL_S_SENSING_MODE);
	}
}

static ssize_t elan_ls_operationmode_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	uint16_t mode = 0;
	/* struct elan_epl_data *epld = epl_data; */
	struct i2c_client *client = to_i2c_client(dev);
	struct elan_epl_data *epld = i2c_get_clientdata(client);

	sscanf(buf, "%hu", &mode);
	epl_info("==>[operation mode]=%d\n", mode);

	if (mode == 0) {
		epld->enable_lflag = 0;
		epld->enable_pflag = 0;
	} else if (mode == 1) {
		epld->enable_lflag = 1;
		epld->enable_pflag = 0;
	} else if (mode == 2) {
		epld->enable_lflag = 0;
		epld->enable_pflag = 1;
	} else if (mode == 3) {
		epld->enable_lflag = 1;
		epld->enable_pflag = 1;
	} else {
		epl_info("0: none\n1: als only\n2: ps only\n3: interleaving");
	}

	elan_sensor_restart_work();
	return count;
}

static ssize_t elan_ls_operationmode_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct elan_epl_data *epld = epl_data;
	long *tmp = (long *)buf;
	uint16_t mode = 0;
	LOG_FUN();

	if (epld->enable_pflag == 0 && epld->enable_lflag == 0)
		mode = 0;
	else if (epld->enable_pflag == 0 && epld->enable_lflag == 1)
		mode = 1;
	else if (epld->enable_pflag == 1 && epld->enable_lflag == 0)
		mode = 2;
	else if (epld->enable_pflag == 1 && epld->enable_lflag == 1)
		mode = 3;

	tmp[0] = mode;

	/*return sprintf(buf, "%d\n", mode); */
	return snprintf(buf, PAGE_SIZE, "%d\n", mode);
}

static ssize_t elan_ls_status_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct elan_epl_data *epld = epl_data;
	u16 ch1;

	if (!epl_data) {
		epl_info("epl_data is null!!\n");
		return 0;
	}
	elan_sensor_I2C_Write(epld->client, REG_7, W_SINGLE_BYTE, 0x02,
			      EPL_DATA_LOCK);

	elan_sensor_I2C_Write(epld->client, REG_16, R_TWO_BYTE, 0x01, 0x00);
	elan_sensor_I2C_Read(epld->client);
	ch1 = (gRawData.raw_bytes[1] << 8) | gRawData.raw_bytes[0];
	epl_info("ch1 raw_data = %d\n", ch1);

	elan_sensor_I2C_Write(epld->client, REG_7, W_SINGLE_BYTE, 0x02,
			      EPL_DATA_UNLOCK);

	/* return sprintf(buf, "%d\n", ch1); */
	return snprintf(buf, PAGE_SIZE, "%d\n", ch1);
}

static DEVICE_ATTR(elan_ls_operationmode, S_IROTH | S_IWOTH,
		   elan_ls_operationmode_show, elan_ls_operationmode_store);
static DEVICE_ATTR(elan_ls_status, S_IROTH | S_IWOTH, elan_ls_status_show,
		   NULL);

static struct attribute *ets_attributes[] = {
	&dev_attr_elan_ls_operationmode.attr,
	&dev_attr_elan_ls_status.attr,
	NULL,
};

static struct attribute_group ets_attr_group = {
	.attrs = ets_attributes,
};

static int elan_als_open(struct inode *inode, struct file *file)
{
	struct elan_epl_data *epld = epl_data;

	LOG_FUN();

	if (epld->als_opened)
		return -EBUSY;
	epld->als_opened = 1;

	return 0;
}

static int elan_als_read(struct file *file, char __user *buffer, size_t count,
			 loff_t *ppos)
{
	struct elan_epl_data *epld = epl_data;
	int buf[1];
	if (epld->read_flag == 1) {
		buf[0] = gRawData.als_ch1_raw;
		if (copy_to_user(buffer, &buf, sizeof(buf)))
			return 0;
		epld->read_flag = 0;
		return 12;
	} else {
		return 0;
	}
}

static int elan_als_release(struct inode *inode, struct file *file)
{
	struct elan_epl_data *epld = epl_data;

	LOG_FUN();

	epld->als_opened = 0;

	return 0;
}

static long elan_als_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	int flag;
	unsigned long buf[1];
	struct elan_epl_data *epld = epl_data;

	void __user *argp = (void __user *)arg;

	epl_info("als io ctrl cmd %d\n", _IOC_NR(cmd));

	switch (cmd) {
	case ELAN_EPL6800_IOCTL_GET_LFLAG:

		epl_info("elan ambient-light IOCTL Sensor get lflag\n");
		flag = epld->enable_lflag;
		if (copy_to_user(argp, &flag, sizeof(flag)))
			return -EFAULT;

		epl_info("elan ambient-light Sensor get lflag %d\n", flag);
		break;

	case ELAN_EPL6800_IOCTL_ENABLE_LFLAG:

		epl_info("elan ambient-light IOCTL Sensor set lflag\n");
		if (copy_from_user(&flag, argp, sizeof(flag)))
			return -EFAULT;
		if (flag < 0 || flag > 1)
			return -EINVAL;

		epld->enable_lflag = flag;
		elan_sensor_restart_work();

		epl_info("elan ambient-light Sensor set lflag %d\n", flag);
		break;

	case ELAN_EPL6800_IOCTL_GETDATA:
		buf[0] = (unsigned long)gRawData.als_ch1_raw;
		if (copy_to_user(argp, &buf, sizeof(buf)))
			return -EFAULT;

		break;

	default:
		pr_err("invalid cmd %d\n", _IOC_NR(cmd));
		return -EINVAL;
	}

	return 0;

}

static const struct file_operations elan_als_fops = {
	.owner = THIS_MODULE,
	.open = elan_als_open,
	.read = elan_als_read,
	.release = elan_als_release,
	.unlocked_ioctl = elan_als_ioctl,
};

static struct miscdevice elan_als_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "elan_als",
	.fops = &elan_als_fops,
};

static int elan_ps_open(struct inode *inode, struct file *file)
{
	struct elan_epl_data *epld = epl_data;

	LOG_FUN();

	if (epld->ps_opened)
		return -EBUSY;

	epld->ps_opened = 1;

	return 0;
}

static int elan_ps_release(struct inode *inode, struct file *file)
{
	struct elan_epl_data *epld = epl_data;

	LOG_FUN();

	epld->ps_opened = 0;

	psensor_mode_suspend = 0;

	return 0;
}

static long elan_ps_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	int value;
	int flag;
	struct elan_epl_data *epld = epl_data;

	void __user *argp = (void __user *)arg;

//	epl_info("ps io ctrl cmd %d\n", _IOC_NR(cmd));

	switch (cmd) {
	case ELAN_EPL6800_IOCTL_GET_PFLAG:

//		epl_info("elan Proximity Sensor IOCTL get pflag\n");
		flag = epld->enable_pflag;
		if (copy_to_user(argp, &flag, sizeof(flag)))
			return -EFAULT;

//		epl_info("elan Proximity Sensor get pflag %d\n", flag);
		break;

	case ELAN_EPL6800_IOCTL_ENABLE_PFLAG:
//		epl_info("elan Proximity IOCTL Sensor set pflag\n");
		if (copy_from_user(&flag, argp, sizeof(flag)))
			return -EFAULT;
		if (flag < 0 || flag > 1)
			return -EINVAL;

		epld->enable_pflag = flag;
		elan_sensor_restart_work();

//		epl_info("elan Proximity Sensor set pflag %d\n", flag);
		break;

	case ELAN_EPL6800_IOCTL_GETDATA:

		value = gRawData.ps_ch1_raw;
		if (copy_to_user(argp, &value, sizeof(value)))
			return -EFAULT;

//		epl_info("elan proximity Sensor get data (%d)\n", value);
		break;

	default:
		pr_err("invalid cmd %d\n", _IOC_NR(cmd));
		return -EINVAL;
	}

	return 0;
}

static const struct file_operations elan_ps_fops = {
	.owner = THIS_MODULE,
	.open = elan_ps_open,
	.release = elan_ps_release,
	.unlocked_ioctl = elan_ps_ioctl
};

static struct miscdevice elan_ps_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "elan_ps",
	.fops = &elan_ps_fops
};

static int initial_sensor(struct elan_epl_data *epld)
{
	struct i2c_client *client = epld->client;

	int ret = 0;

//	epl_info("initial_sensor enter!\n");

	ret = elan_sensor_I2C_Read(client);

	if (ret < 0)
		return -EINVAL;

	elan_sensor_I2C_Write(client, REG_0, W_SINGLE_BYTE, 0x02,
			      EPL_S_SENSING_MODE);
	elan_sensor_I2C_Write(client, REG_9, W_SINGLE_BYTE, 0x02,
			      EPL_INT_DISABLE);
	set_psensor_intr_threshold(epld->ps_th_l, epld->ps_th_h);

	msleep(20);

	epld->enable_lflag = 0;
	epld->enable_pflag = 0;

	return ret;
}

/*
static int elan_power_on(struct elan_epl_data *epld, bool on)
{
	int rc;

	LOG_FUN();

	if (!on)
		goto power_off;

	rc = regulator_enable(epld->vdd);
	if (rc) {
		dev_err(&epld->client->dev,
			"Regulator vdd enable failed rc=%d\n", rc);
		return rc;
	}

	rc = regulator_enable(epld->vio);
	if (rc) {
		dev_err(&epld->client->dev,
			"Regulator vio enable failed rc=%d\n", rc);
		regulator_disable(epld->vdd);
	}

	return rc;

power_off:
	rc = regulator_disable(epld->vdd);
	if (rc) {
		dev_err(&epld->client->dev,
			"Regulator vdd disable failed rc=%d\n", rc);
		return rc;
	}

	rc = regulator_disable(epld->vio);
	if (rc) {
		dev_err(&epld->client->dev,
			"Regulator vio disable failed rc=%d\n", rc);
	}

	return rc;
}
*/
static int elan_power_on(struct elan_epl_data *epld, bool on)
{
	int rc = 0;

	if (!on) {
		rc = regulator_disable(epld->vdd);
		if (rc) {
			dev_err(&epld->client->dev,
				"Regulator vdd disable failed rc=%d\n", rc);
			return rc;
		}

		rc = regulator_disable(epld->vio);
		if (rc) {
			dev_err(&epld->client->dev,
				"Regulator vio disable failed rc=%d\n", rc);
			rc = regulator_enable(epld->vdd);
			dev_err(&epld->client->dev,
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
		rc = regulator_enable(epld->vdd);
		if (rc) {
			dev_err(&epld->client->dev,
				"Regulator vdd enable failed rc=%d\n", rc);
			return rc;
		}

		rc = regulator_enable(epld->vio);
		if (rc) {
			dev_err(&epld->client->dev,
				"Regulator vio enable failed rc=%d\n", rc);
			regulator_disable(epld->vdd);
			return rc;
		}
	}

enable_delay:
	msleep(130);
	dev_dbg(&epld->client->dev,
		"Sensor regulator power on =%d\n", on);
	return rc;
}

static int sensor_platform_hw_power_on(bool on)
{
	struct elan_epl_data *data;
	int err = 0;

	/* sensor_info is global pointer to struct ltr553_data */
	if (sensor_info == NULL)
		return -ENODEV;

//	epl_info("%s\n", __func__);
	data = sensor_info;

	if (data->power_on != on) {
/*		if (!IS_ERR_OR_NULL(data->pinctrl)) {
			if (on)
				err = pinctrl_select_state(data->pinctrl,
					data->pin_default);
			else
				err = pinctrl_select_state(data->pinctrl,
					data->pin_sleep);
			if (err)
				dev_err(&data->client->dev,
					"Can't select pinctrl state\n");
		}
*/
		err = elan_power_on(data, on);
		if (err)
			dev_err(&data->client->dev,
					"Can't configure regulator!\n");
		else
			data->power_on = on;
	}

	return err;
}

static int elan_power_init(struct elan_epl_data *epld, bool on)
{
	int rc;

	LOG_FUN();

	if (!on)
		goto pwr_deinit;

	epld->vdd = regulator_get(&epld->client->dev, "vdd");
	if (IS_ERR(epld->vdd)) {
		rc = PTR_ERR(epld->vdd);
		dev_err(&epld->client->dev,
			"Regulator get failed vdd rc=%d\n", rc);
		return rc;
	}

	if (regulator_count_voltages(epld->vdd) > 0) {
		rc = regulator_set_voltage(epld->vdd, EPL2182_VDD_MIN_UV,
					   EPL2182_VDD_MAX_UV);
		if (rc) {
			dev_err(&epld->client->dev,
				"Regulator set_vtg failed vdd rc=%d\n", rc);
			goto reg_vdd_put;
		}
	}

	epld->vio = regulator_get(&epld->client->dev, "vio");
	if (IS_ERR(epld->vio)) {
		rc = PTR_ERR(epld->vio);
		dev_err(&epld->client->dev,
			"Regulator get failed vio rc=%d\n", rc);
		goto reg_vdd_set_vtg;
	}

	if (regulator_count_voltages(epld->vio) > 0) {
		rc = regulator_set_voltage(epld->vio, EPL2182_VIO_MIN_UV,
					   EPL2182_VIO_MAX_UV);
		if (rc) {
			dev_err(&epld->client->dev,
				"Regulator set_vtg failed vio rc=%d\n", rc);
			goto reg_vio_put;
		}
	}

	return 0;

reg_vio_put:
	regulator_put(epld->vio);
reg_vdd_set_vtg:
	if (regulator_count_voltages(epld->vdd) > 0)
		regulator_set_voltage(epld->vdd, 0, EPL2182_VDD_MAX_UV);
reg_vdd_put:
	regulator_put(epld->vdd);
	return rc;

pwr_deinit:
	if (regulator_count_voltages(epld->vdd) > 0)
		regulator_set_voltage(epld->vdd, 0, EPL2182_VDD_MAX_UV);

	regulator_put(epld->vdd);

	if (regulator_count_voltages(epld->vio) > 0)
		regulator_set_voltage(epld->vio, 0, EPL2182_VIO_MAX_UV);

	regulator_put(epld->vio);
	return 0;
}

static int lightsensor_setup(struct elan_epl_data *epld)
{
	int err = 0;
//	epl_info("lightsensor_setup enter.\n");

	epld->als_input_dev = input_allocate_device();
	if (!epld->als_input_dev) {
		pr_err("could not allocate ls input device\n");
		return -ENOMEM;
	}
	epld->als_input_dev->name = ElanALsensorName;
	set_bit(EV_ABS, epld->als_input_dev->evbit);
	input_set_abs_params(epld->als_input_dev, ABS_MISC, 0, 9, 0, 0);

	err = input_register_device(epld->als_input_dev);
	if (err < 0) {
		pr_err("can not register ls input device\n");
		goto err_free_ls_input_device;
	}

	err = misc_register(&elan_als_device);
	if (err < 0) {
		pr_err("can not register ls misc device\n");
		goto err_unregister_ls_input_device;
	}

	return err;

err_unregister_ls_input_device:
	input_unregister_device(epld->als_input_dev);
err_free_ls_input_device:
	input_free_device(epld->als_input_dev);
	return err;
}

static int psensor_setup(struct elan_epl_data *epld)
{
	int err = 0;
//	epl_info("psensor_setup enter.\n");

	epld->ps_input_dev = input_allocate_device();
	if (!epld->ps_input_dev) {
		pr_err("could not allocate ps input device\n");
		return -ENOMEM;
	}
	epld->ps_input_dev->name = ElanPsensorName;

	set_bit(EV_ABS, epld->ps_input_dev->evbit);
	input_set_abs_params(epld->ps_input_dev, ABS_DISTANCE, 0, 1, 0, 0);

	err = input_register_device(epld->ps_input_dev);
	if (err < 0) {
		pr_err("could not register ps input device\n");
		goto err_free_ps_input_device;
	}

	err = misc_register(&elan_ps_device);
	if (err < 0) {
		pr_err("could not register ps misc device\n");
		goto err_unregister_ps_input_device;
	}

	return err;

err_unregister_ps_input_device:
	input_unregister_device(epld->ps_input_dev);
err_free_ps_input_device:
	input_free_device(epld->ps_input_dev);
	return err;
}

#if PS_INTERRUPT_MODE
static int setup_interrupt(struct elan_epl_data *epld)
{
	struct i2c_client *client = epld->client;

	int err = 0;
	msleep(20);
	LOG_FUN();
	if (gpio_is_valid(epld->irq_gpio)) {
		err = gpio_request(epld->irq_gpio, "elan_irq_gpio");
		if (err)
			dev_err(&client->dev, "irq gpio request failed");

		err = gpio_direction_input(epld->irq_gpio);
		if (err) {
			dev_err(&client->dev,
				"set_direction for irq gpio failed\n");
		}
	}

	err =
	    request_irq(client->irq, elan_sensor_irq_handler,
			IRQF_TRIGGER_FALLING, "EPL2182", (void *)client);
	if (err < 0) {
		pr_err("request irq pin %d fail for gpio\n", err);
		goto exit_free_irq;
	}

	irq_set_irq_wake(client->irq, 1);

	return err;

exit_free_irq:
	free_irq(client->irq, client);
	return err;
}
#endif

static int elan_enable_ps_sensor(struct i2c_client *client, int val)
{
	struct elan_epl_data *epld = i2c_get_clientdata(client);
	LOG_FUN();
	pr_debug("epl2182 enable PS sensor -> %d\n", val);

	if ((val != 0) && (val != 1)) {
		pr_debug("%s:store unvalid value=%d\n", __func__, val);
		return -EINVAL;
	}

	if (val == 1) {
		if ((epld->enable_pflag == 0) && (epld->enable_lflag == 0))
			sensor_platform_hw_power_on(true);

		if (epld->enable_pflag == 0)
			epld->enable_pflag = 1;

		enable_irq(client->irq);
	} else {
		if (epld->enable_pflag == 1)
			epld->enable_pflag = 0;

		disable_irq(client->irq);
	}

	elan_sensor_restart_work();

	/* Vote off  regulators if both light and prox sensor are off */
	if ((epld->enable_pflag == 0) && (epld->enable_lflag == 0))
		sensor_platform_hw_power_on(false);

	return 0;
}

static int elan_enable_als_sensor(struct i2c_client *client, int val)
{
	struct elan_epl_data *epld = i2c_get_clientdata(client);

	LOG_FUN();
	pr_debug("%s: val=%d, enable_lflag %d\n", __func__, val, epld->enable_lflag);

	if ((val != 0) && (val != 1)) {
		pr_err("%s: invalid value (val = %d)\n", __func__, val);
		return -EINVAL;
	}

	if (val == 1) {

		if ((epld->enable_pflag == 0) && (epld->enable_lflag == 0))
			sensor_platform_hw_power_on(true);

		if (epld->enable_lflag == 0)
			epld->enable_lflag = 1;
		elan_sensor_restart_work();

	} else {
		if (epld->enable_lflag == 1)
			epld->enable_lflag = 0;
		elan_sensor_stop_work();
	}

	/* Vote off  regulators if both light and prox sensor are off */
	if ((epld->enable_pflag == 0) && (epld->enable_lflag == 0))
		sensor_platform_hw_power_on(false);

	return 0;
}

static int elan_als_set_enable(struct sensors_classdev *sensors_cdev,
			       unsigned int enable)
{
	struct elan_epl_data *epld =
	    container_of(sensors_cdev, struct elan_epl_data, als_cdev);

	if ((enable != 0) && (enable != 1)) {
		pr_err("%s: invalid value(%d)\n", __func__, enable);
		return -EINVAL;
	}

	return elan_enable_als_sensor(epld->client, enable);
}

static int elan_ps_set_enable(struct sensors_classdev *sensors_cdev,
			      unsigned int enable)
{
	struct elan_epl_data *epld =
	    container_of(sensors_cdev, struct elan_epl_data, ps_cdev);

	if ((enable != 0) && (enable != 1)) {
		pr_err("%s: invalid value(%d)\n", __func__, enable);
		return -EINVAL;
	}

#if PS_AUTO_ENABLE	
	if(enable == 1){
		gRawData.ps_min_raw = 0xffff;
	}
#endif

	return elan_enable_ps_sensor(epld->client, enable);
}

/*at least 500ms, "echo A > poll_delay" means poll A msec! */
static ssize_t elan_als_poll_delay(struct sensors_classdev *sensors_cdev,
				   unsigned int delay_msec)
{
	struct elan_epl_data *epld =
	    container_of(sensors_cdev, struct elan_epl_data, als_cdev);

//	epl_info("%s\n", __func__);
	if (delay_msec < ALS_POLLING_RATE / 2)	/*at least 500 ms */
		delay_msec = ALS_POLLING_RATE / 2;

	epld->als_poll_delay = delay_msec;	/* convert us => ms */

	if (epld->enable_lflag == 1) {
		cancel_delayed_work(&polling_work);
		cancel_delayed_work(&report_polling_work);
		queue_delayed_work(epld->epl_wq, &polling_work,
				   msecs_to_jiffies(10));
	}
	return 0;
}

/*at least 500ms, "echo A > poll_delay" means poll A msec! */
static ssize_t elan_ps_poll_delay(struct sensors_classdev *sensors_cdev,
				  unsigned int delay_msec)
{
	struct elan_epl_data *epld =
	    container_of(sensors_cdev, struct elan_epl_data, ps_cdev);

//	epl_info("%s\n", __func__);
	if (delay_msec < PS_POLLING_RATE)	/* at least 500 ms */
		delay_msec = PS_POLLING_RATE;

	epld->ps_poll_delay = delay_msec;	/* convert us => ms */

	if (epld->enable_pflag == 1) {
		cancel_delayed_work(&polling_work);
		cancel_delayed_work(&report_polling_work);
		queue_delayed_work(epld->epl_wq, &polling_work,
				   msecs_to_jiffies(10));
	}
	return 0;
}
#ifdef CONFIG_OLDPLATFORM
static int elan_sensor_suspend(struct i2c_client *client, pm_message_t mesg)
{
	LOG_FUN();

	if (epl_data->enable_pflag == 0) {
		elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0x02,
				      EPL_C_P_DOWN);
		cancel_delayed_work(&polling_work);
	}
	return 0;
}

static int elan_sensor_resume(struct i2c_client *client)
{
	struct elan_epl_data *epld = epl_data;
	LOG_FUN();

	if (epld->enable_pflag | epld->enable_lflag) {
		elan_sensor_I2C_Write(client, REG_7, W_SINGLE_BYTE, 0x02,
				      EPL_C_P_UP);
	}

	if (epld->enable_pflag)
		elan_sensor_restart_work();

	return 0;
}
#else
static int elan_sensor_suspend(struct device *dev)
{
	struct elan_epl_data *data = epl_data;
	struct i2c_client *client = data->client;

//	LOG_FUN();
	/*
	  * Save sensor state and disable them,
	  * this is to ensure internal state flags are set correctly.
	*/
	data->als_enable_state = data->enable_lflag;
	
//	pr_err("als_enable_state %d, enable_pflag %d\n",data->als_enable_state, data->enable_pflag);
	
	if (data->als_enable_state)
		elan_enable_als_sensor(client, 0);

	if (data->enable_pflag)
		irq_set_irq_wake(client->irq, 1);

	return 0;
}

static int elan_sensor_resume(struct device *dev)
{
	struct elan_epl_data *data = epl_data;
	struct i2c_client *client = data->client;

//	LOG_FUN();
	/* Resume L sensor state as P sensor does not disable */
//	pr_err("als_enable_state %d\n",data->als_enable_state);
	if (data->als_enable_state)
		elan_enable_als_sensor(client, 1);

	return 0;
}
#endif
/*
static int elan_pinctrl_init(struct elan_epl_data *data)
{
	struct i2c_client *client = data->client;

	data->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR_OR_NULL(data->pinctrl)) {
		dev_err(&client->dev, "Failed to get pinctrl\n");
		return PTR_ERR(data->pinctrl);
	}

	data->pin_default =
		pinctrl_lookup_state(data->pinctrl, "default");
	if (IS_ERR_OR_NULL(data->pin_default)) {
		dev_err(&client->dev, "Failed to look up default state\n");
		return PTR_ERR(data->pin_default);
	}

	data->pin_sleep =
		pinctrl_lookup_state(data->pinctrl, "sleep");
	if (IS_ERR_OR_NULL(data->pin_sleep)) {
		dev_err(&client->dev, "Failed to look up sleep state\n");
		return PTR_ERR(data->pin_sleep);
	}

	return 0;
}
*/
static int sensor_parse_dt(struct device *dev, struct elan_epl_data *epld)
{
	struct device_node *np = dev->of_node;
	struct i2c_client *client;
	unsigned int tmp;
	int rc = 0;

	client = epld->client;

	LOG_FUN();
	/* irq gpio */
//	epld->irq_gpio = of_get_named_gpio_flags(np, "epl2182,irq-gpio",
//						 0, &epld->irq_gpio_flags);
//	if (epld->irq_gpio < 0)
//		return epld->irq_gpio;

	/* ps tuning data */
	rc = of_property_read_u32(np, "epl2182,prox_th_min", &tmp);
	if (rc) {
		dev_err(dev, "Unable to read prox_th_min\n");
		return rc;
	}

	epld->ps_th_l = tmp;

	rc = of_property_read_u32(np, "epl2182,prox_th_max", &tmp);
	if (rc) {
		dev_err(dev, "Unable to read prox_th_max\n");
		return rc;
	}

	epld->ps_th_h = tmp;

	return 0;
}
static int i2c_reg_check(struct i2c_client *client)
{
	int last;
	epl_info("chip id REG 0x00 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x00));
	epl_info("chip id REG 0x01 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x08));
	epl_info("chip id REG 0x02 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x10));
	epl_info("chip id REG 0x03 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x18));
	epl_info("chip id REG 0x04 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x20));
	epl_info("chip id REG 0x05 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x28));
	epl_info("chip id REG 0x06 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x30));
	epl_info("chip id REG 0x07 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x38));
	epl_info("chip id REG 0x09 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x48));
	epl_info("chip id REG 0x0D value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x68));
	epl_info("chip id REG 0x0E value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x70));
	epl_info("chip id REG 0x0F value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x71));
	epl_info("chip id REG 0x10 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x80));
	epl_info("chip id REG 0x11 value = %8x\n",
		 i2c_smbus_read_byte_data(client, 0x88));
	last =  i2c_smbus_read_byte_data(client, 0x98);
	epl_info("chip id REG 0x13 value = %8x\n", last);

	return last < 0 ? last : 0;
}

static int elan_sensor_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	int err = 0, ret = 0;
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct elan_epl_data *epld;

//	epl_info("elan sensor probe enter.\n");

	epld = kzalloc(sizeof(struct elan_epl_data), GFP_KERNEL);
	if (!epld)
		return -ENOMEM;

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"No supported i2c func what we need?!!\n");
		err = -ENOTSUPP;
		goto i2c_fail;
	}
	epld->als_poll_delay = ALS_POLLING_RATE;
	epld->ps_poll_delay = PS_POLLING_RATE;
	epld->client = client;
	epld->ps_th_h = PS_H_THRESHOLD;
	epld->ps_th_l = PS_L_THRESHOLD;

	i2c_set_clientdata(client, epld);

	epl_data = epld;

	err = sensor_parse_dt(&client->dev, epld);
	if (err) {
		pr_err("%s: sensor_parse_dt() err\n", __func__);
		return err;
	}

	/* Global pointer for this device */
	sensor_info = epld;
	epld->enable_pflag = 0;
	epld->enable_lflag = 0;

	/* initialize pinctrl */
/*	err = elan_pinctrl_init(epld);
	if (err) {
		dev_err(&client->dev, "Can't initialize pinctrl\n");
			goto exit_pinctrl;
	}
	err = pinctrl_select_state(epld->pinctrl, epld->pin_default);
	if (err) {
		dev_err(&client->dev,
			"Can't select pinctrl default state\n");
		goto exit_pinctrl;
	}*/

	err = elan_power_init(epld, true);
	if (err)
		dev_err(&client->dev, "power init failed");

	err = sensor_platform_hw_power_on(true);
	if (err)
		dev_err(&client->dev, "power on failed");
	/*check i2c func and register value*/
	err = i2c_reg_check(client);
	if (err) {
		sensor_platform_hw_power_on(false);
		regulator_put(epld->vio);
		regulator_put(epld->vdd);
		goto i2c_fail;
	}

	epld->epl_wq = create_singlethread_workqueue("elan_sensor_wq");
	if (!epld->epl_wq) {
		pr_err("can't create workqueue\n");
		err = -ENOMEM;
		goto err_create_singlethread_workqueue;
	}

	err = lightsensor_setup(epld);
	if (err < 0) {
		pr_err("lightsensor_setup error!!\n");
		goto err_lightsensor_setup;
	}

	err = psensor_setup(epld);
	if (err < 0) {
		pr_err("psensor_setup error!!\n");
		goto err_psensor_setup;
	}

	err = initial_sensor(epld);
	if (err < 0) {
		pr_err("fail to initial sensor (%d)\n", err);
		goto err_sensor_setup;
	}

#if PS_INTERRUPT_MODE
	err = setup_interrupt(epld);
	if (err < 0) {
		pr_err("setup error!\n");
		goto err_sensor_setup;
	}
#endif
	wake_lock_init(&g_ps_wlock, WAKE_LOCK_SUSPEND, "ps_wakelock");
	err = sysfs_create_group(&client->dev.kobj, &ets_attr_group);
	if (err != 0) {
		dev_err(&client->dev, "%s:create sysfs group error", __func__);
		goto err_fail;
	}

	/* Register to sensors class */
	epld->als_cdev = sensors_light_cdev;
	epld->als_cdev.sensors_enable = elan_als_set_enable;
	epld->als_cdev.sensors_poll_delay = elan_als_poll_delay;

	epld->ps_cdev = sensors_proximity_cdev;
	epld->ps_cdev.sensors_enable = elan_ps_set_enable;
	epld->ps_cdev.sensors_poll_delay = elan_ps_poll_delay;

	ret = sensors_classdev_register(&client->dev, &epld->als_cdev);
	if (ret) {
		pr_err("%s: Unable to register to sensors class: %d\n",
		       __func__, ret);
		goto exit_remove_sysfs_group;
	}
	ret = sensors_classdev_register(&client->dev, &epld->ps_cdev);
	if (ret) {
		pr_err("%s: Unable to register to sensors class: %d\n",
		       __func__, ret);
		goto exit_create_class_sysfs;
	}

	epl_info("sensor probe success.\n");

	return err;

exit_create_class_sysfs:
	sensors_classdev_unregister(&epld->als_cdev);
exit_remove_sysfs_group:
	sysfs_remove_group(&client->dev.kobj, &ets_attr_group);
err_fail:
	free_irq(client->irq, client);
err_sensor_setup:
	misc_deregister(&elan_ps_device);
	input_unregister_device(epld->ps_input_dev);
	input_free_device(epld->ps_input_dev);
err_psensor_setup:
	input_unregister_device(epld->als_input_dev);
	input_free_device(epld->als_input_dev);
	misc_deregister(&elan_als_device);
err_lightsensor_setup:
	destroy_workqueue(epld->epl_wq);
err_create_singlethread_workqueue:
//exit_pinctrl:
i2c_fail:
	kfree(epld);
	sensor_info = NULL;

	return err;
}

static int elan_sensor_remove(struct i2c_client *client)
{
	struct elan_epl_data *epld = i2c_get_clientdata(client);

	dev_dbg(&client->dev, "%s: enter.\n", __func__);

	input_unregister_device(epld->als_input_dev);
	input_unregister_device(epld->ps_input_dev);
	input_free_device(epld->als_input_dev);
	input_free_device(epld->ps_input_dev);
	misc_deregister(&elan_ps_device);
	misc_deregister(&elan_als_device);
//	free_irq(epld->irq_gpio, epld);
	destroy_workqueue(epld->epl_wq);
	kfree(epld);
	return 0;
}

static const struct i2c_device_id elan_sensor_id[] = {
	{ELAN_LS_2182, 0},
	{}
};

static struct of_device_id elan_match_table[] = {
	{.compatible = "elan,epl2182",},
	{},
};

static const struct dev_pm_ops elan_pm_ops = {
	.suspend	= elan_sensor_suspend,
	.resume		= elan_sensor_resume,
};

static struct i2c_driver elan_sensor_driver = {
	.probe = elan_sensor_probe,
	.remove = elan_sensor_remove,
	.driver = {
		   .name = ELAN_LS_2182,
		   .owner = THIS_MODULE,
		   .of_match_table = elan_match_table,
		   .pm = &elan_pm_ops,
		   },
	.id_table = elan_sensor_id,
};

static int __init elan_sensor_init(void)
{
	return i2c_add_driver(&elan_sensor_driver);
}

static void __exit elan_sensor_exit(void)
{
	i2c_del_driver(&elan_sensor_driver);
}

module_init(elan_sensor_init);
module_exit(elan_sensor_exit);

MODULE_AUTHOR("Renato Pan <renato.pan@eminent-tek.com>");
MODULE_DESCRIPTION("ELAN epl2182 driver");
MODULE_LICENSE("GPL");
