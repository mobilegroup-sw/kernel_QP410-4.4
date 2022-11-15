#include <linux/jiffies.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>
#include <linux/input/mt.h>

#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/async.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <asm/irq.h>
#include <linux/io.h>
#include <mach/gpio.h>		/*kingshine niro */
#include <linux/regulator/consumer.h>

#include <linux/debugfs.h>

#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif

#include <mach/irqs.h>
#include <mach/hardware.h>
#include  <linux/gsl2680.h>	/* D&E series use different config array */

/* extern int  msm_vreg_config_power_onoff(int vreg_on); */

/************************************************
  All kinds of switch of function and define parameter;
 ************************************************/
#define REPORT_DATA_ANDROID_4_0	/* Report data for Android 4.0 edition */
#define GSLX680_I2C_NAME		"gslX680" /* Device name */
#define GSLX680_I2C_ADDR		0x40  /* I2C address */
#define GSLX680_ADAPTER_INDEX	1	/* Adapter index of platform; */
/*#define IRQ_PORT				13	// Interrupt GPIO */
/*#define RST_PORT				12	// Reset GPIO */
static int IRQ_PORT;
static int RST_PORT;

#define GSL_DATA_REG	0x80	/* Data address register; */
#define GSL_STATUS_REG	0xe0	/* Status register; */
#define GSL_PAGE_REG	0xf0	/* Page address register; */
#define DMA_TRANS_LEN	0x08	/* Direct memory access */

#define PRESS_MAX		255	/* Maximum pressure */
#define MAX_CONTACTS	10	/* The maximum quantity of touch points; */
#define MAX_KEY_NUM		3	/* The count of key; */

#define TPD_PROC_DEBUG  //Android Debug Brigde by Silead MFC; 
/*#define TPD_ESD_PROTECT //Setting Timer for Electronic Static Discharge; */
/*#define HAVE_TOUCH_KEY     // Virtual Touch Key; */
#define TPD_PHONE_MODE	   /* Function after phone call; */
#define GSL_ALG_ID		   /* New algorithm of Non-ID; */
/* #define GSL_DEBUG       //Print information; */
/* #define GSL_ANTIWATER   //Open waterproof function; */

//#define SCREEN_MAX_X 720
//#define SCREEN_MAX_Y 1280
#define MAX_FINGERS 10

#define GSL_DRIVER_VERSION	2

#define GSL_FW_NAME_MAX_LEN	50
#define GSL_INFO_MAX_LEN	512
//#define GSL_IC_VERSION		0xa082

#define GSL_STORE_TS_INFO(buf, name, ic_ver, max_tch, \
			fw_vkey_support, fw_name) \
			snprintf(buf, GSL_INFO_MAX_LEN, \
				"controller\t= gsl\n" \
				"name\t\t= %s\n" \
				"IC_version\t= 0x%x\n" \
				"max_touches\t= %d\n" \
				"drv_ver\t\t= 0x%x\n" \
				"fw_vkey_support\t= %s\n" \
				"fw_name\t\t= %s\n", \
				name, ic_ver, max_tch, GSL_DRIVER_VERSION, \
				fw_vkey_support, fw_name)

#define GSL_DEBUG_DIR_NAME	"ts_debug"

#ifdef HAVE_TOUCH_KEY
struct s_gsl_key_data {
	u16 x_min;
	u16 x_max;
	u16 y_min;
	u16 y_max;
	u16 key;
	char *key_name;
};

struct s_gsl_key_data gsl_key_data[MAX_KEY_NUM] = {
	{20, 50, 1300, 1340, KEY_MENU, ""},
	{380, 410, 1300, 1340, KEY_HOMEPAGE, ""},
	{600, 630, 1300, 1340, KEY_BACK, ""}
};
#endif

static int gsl_halt_flag;
static struct i2c_client *this_client;

#ifdef TPD_PHONE_MODE
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/ioctl.h>
#include <linux/uaccess.h>

#define GSLX680_TP_CALL_NAME "gsl1688_tp_call"

static int gsl_phone_flag;
static int gsl_phone_count;
static u8 gsl_phone_data[32] = { 0 };

static int gsl_phone_id_data;
#endif

/************************************************
  This Module connect with Silead MFC Android Debug Bridge tool;
 ************************************************/
#ifdef TPD_PROC_DEBUG
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/seq_file.h>
//static struct proc_dir_entry *gsl_config_proc;
#define GSL_CONFIG_PROC_FILE "gsl_config"
#define CONFIG_LEN 31
static char gsl_read[CONFIG_LEN];
static u8 gsl_data_proc[8] = { 0 };

static u8 gsl_proc_flag;
#endif

/************************************************
  This Module is prepared for Electronic Static Discharge Protect;
 ************************************************/
#ifdef TPD_ESD_PROTECT
/* #undef TPD_PROC_DEBUG */
#define TPD_ESD_CHECK_CIRCLE        200
static struct delayed_work gsl_esd_check_work;
static struct workqueue_struct *gsl_esd_check_workqueue;
static u32 gsl_timer_data;
/* static u8 gsl_data_1st[4] = {0}; */

/* 0:first test  1:second test  2:doing gsl_load_fw */
static int gsl_esd_flag;
#endif

/************************************************
  This Module would be used when the touchpanel with virtual key;
 ************************************************/
#ifdef HAVE_TOUCH_KEY
static u16 key;
static int key_state_flag;
#endif

struct gsl_ts_data {
	u8 x_index;
	u8 y_index;
	u8 z_index;
	u8 id_index;
	u8 touch_index;
	u8 data_reg;
	u8 status_reg;
	u8 data_size;
	u8 touch_bytes;
	u8 update_data;
	u8 touch_meta_data;
	u8 finger_size;
};

static struct gsl_ts_data devices[] = {
	{
	 .x_index = 6,
	 .y_index = 4,
	 .z_index = 5,
	 .id_index = 7,
	 .data_reg = GSL_DATA_REG,
	 .status_reg = GSL_STATUS_REG,
	 .update_data = 0x4,
	 .touch_bytes = 4,
	 .touch_meta_data = 4,
	 .finger_size = 70,
	 },
};

struct gsl_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct work;
	struct workqueue_struct *wq;
	struct gsl_ts_data *dd;
	u8 *touch_data;
	u8 device_id;
	bool is_suspended;
	bool int_pending;
	bool enable;
	struct mutex sus_lock;
	uint32_t irq_gpio;
	uint32_t reset_gpio;
	int irq;
	struct regulator *vdd;
	struct regulator *vcc_i2c;
	struct dentry *dir;
	u32 ic_ver;
	bool fw_vkey_support;
	char fw_name[GSL_FW_NAME_MAX_LEN];
	const char *fw_name_ptr;
	u16 addr;
	char *ts_info;
#if defined(CONFIG_FB)
	struct notifier_block fb_notif;
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	struct early_suspend early_suspend;
#endif

#ifdef GSL_TIMER
	struct timer_list gsl_timer;
#endif

};

#define GSL_INFO(fmt, args...)   \
	printk(fmt, ##args)

#ifdef GSL_DEBUG
#define print_info(fmt, args...)   \
	printk(fmt, ##args)
#else
#define print_info(fmt, args...)
#endif

static u32 id_sign[MAX_CONTACTS + 1] = { 0 };
static u8 id_state_flag[MAX_CONTACTS + 1] = { 0 };
static u8 id_state_old_flag[MAX_CONTACTS + 1] = { 0 };
static u16 x_old[MAX_CONTACTS + 1] = { 0 };
static u16 y_old[MAX_CONTACTS + 1] = { 0 };

static u16 x_new;
static u16 y_new;


/************************************************
  Description	: Put the shutdown(Reset) on Lower Voltage;
  Input			: None
  Return Value	: return 0
 ************************************************/
static int gslX680_shutdown_low(void)
{
	gpio_direction_output(RST_PORT, 0);
	return 0;
}

static int gslX680_shutdown_high(void)
{
	gpio_direction_output(RST_PORT, 1);
	return 0;
}

static inline u16 join_bytes(u8 a, u8 b)
{
	u16 ab = 0;
	ab = ab | a;
	ab = ab << 8 | b;
	return ab;
}


/************************************************
  Description	: Write data into register (Edition 1);
 ************************************************/
static u32 gsl_write_interfacexw(struct i2c_client *client, const u8 reg,
				 u8 *buf, u32 num)
{
	struct i2c_msg xfer_msg[1];
	u8 tmp_buf[num + 1];
	tmp_buf[0] = reg;
	memcpy(tmp_buf + 1, buf, num);
	xfer_msg[0].addr = client->addr;
	xfer_msg[0].len = num + 1;
	xfer_msg[0].flags = client->flags & I2C_M_TEN;
	xfer_msg[0].buf = tmp_buf;
	/* xfer_msg[0].timing = 400; */

	return i2c_transfer(client->adapter, xfer_msg, 1) == 1 ? 0 : -EFAULT;
}

/************************************************
  Description	: Write data into register (Edition 2);
 ************************************************/
static u32 gsl_write_interface(struct i2c_client *client, const u8 reg,
			       u8 *buf, u32 num)
{
	struct i2c_msg xfer_msg[1];

	buf[0] = reg;

	xfer_msg[0].addr = client->addr;
	xfer_msg[0].len = num + 1;
	xfer_msg[0].flags = client->flags & I2C_M_TEN;
	xfer_msg[0].buf = buf;

	return i2c_transfer(client->adapter, xfer_msg, 1) == 1 ? 0 : -EFAULT;
}

static void fw2buf(u8 *buf, const u32 *fw)
{
	u32 *u32_buf = (int *)buf;
	*u32_buf = *fw;
}

/************************************************
  Description	: Download the data array from .H file;
 ************************************************/
static void gsl_load_fw(struct i2c_client *client)
{
	u8 buf[DMA_TRANS_LEN * 4 + 1] = { 0 };
	u8 send_flag = 1;
	u8 *cur = buf + 1;
	u32 source_line = 0;
	u32 source_len = ARRAY_SIZE(GSLX680_FW);

	GSL_INFO(KERN_INFO "=============gsl_load_fw start==============\n");

	for (source_line = 0; source_line < source_len; source_line++) {
		/* init page trans, set the page val */
		if (GSL_PAGE_REG == GSLX680_FW[source_line].offset) {
			fw2buf(cur, &GSLX680_FW[source_line].val);
			gsl_write_interface(client, GSL_PAGE_REG, buf, 4);
			send_flag = 1;
		} else {
			if (1 ==
			    send_flag % (DMA_TRANS_LEN <
					 0x20 ? DMA_TRANS_LEN : 0x20))
				buf[0] = (u8) GSLX680_FW[source_line].offset;

			fw2buf(cur, &GSLX680_FW[source_line].val);
			cur += 4;

			if (0 ==
			    send_flag % (DMA_TRANS_LEN <
					 0x20 ? DMA_TRANS_LEN : 0x20)) {
				gsl_write_interface(client, buf[0], buf,
						    cur - buf - 1);
				cur = buf + 1;
			}

			send_flag++;
		}
	}

	GSL_INFO(KERN_INFO "=============gsl_load_fw end==============\n");

}


static int gsl_ts_write(struct i2c_client *client, u8 addr, u8 *pdata,
			int datalen)
{
	int ret = 0;
	u8 tmp_buf[128];
	unsigned int bytelen = 0;
	if (datalen > 125) {
		GSL_INFO(KERN_ERR "%s too big datalen = %d!\n",
							__func__, datalen);
		return -EPERM;
	}

	tmp_buf[0] = addr;
	bytelen++;

	if (datalen != 0 && pdata != NULL) {
		memcpy(&tmp_buf[bytelen], pdata, datalen);
		bytelen += datalen;
	}

	ret = i2c_master_send(client, tmp_buf, bytelen);
	return ret;
}


static int gsl_ts_read(struct i2c_client *client, u8 addr, u8 *pdata,
		       unsigned int datalen)
{
	int ret = 0;

	if (datalen > 126) {
		GSL_INFO(KERN_ERR "%s too big datalen = %d!\n",
				__func__, datalen);
		return -EPERM;
	}

	if (addr < 0x80) {
		gsl_ts_write(client, addr, NULL, 0);
		i2c_master_recv(client, pdata, datalen);
	}

	ret = gsl_ts_write(client, addr, NULL, 0);
	if (ret < 0) {
		GSL_INFO(KERN_ERR "%s set data address fail!\n", __func__);
		return ret;
	}

	return i2c_master_recv(client, pdata, datalen);
}


static int test_i2c(struct i2c_client *client)
{
	u8 read_buf = 0;
	u8 write_buf = 0x12;
	int ret, rc = 1;

	ret = gsl_ts_read(client, 0xf0, &read_buf, sizeof(read_buf));
	if (ret < 0)
		rc--;
	else
		GSL_INFO(KERN_INFO "I read reg 0xf0 is %x\n", read_buf);

	usleep_range(2000, 2000); /* msleep(2); */
	ret = gsl_ts_write(client, 0xf0, &write_buf, sizeof(write_buf));
	if (ret >= 0)
		GSL_INFO(KERN_INFO "I write reg 0xf0 0x12\n");

	usleep_range(2000, 2000); /* msleep(2); */
	ret = gsl_ts_read(client, 0xf0, &read_buf, sizeof(read_buf));
	if (ret < 0)
		rc--;
	else
		GSL_INFO(KERN_INFO "I read reg 0xf0 is 0x%x\n", read_buf);

	return rc;
}


/************************************************
  Description	: Startup chip follow the step;
  write (0x01fe1000) into ()
  Input			: struct i2c_client
  Return Value	: void
 ************************************************/
static void startup_chip(struct i2c_client *client)
{
	u8 buf[4] = { 0x00 };

#ifdef GSL_ALG_ID
	/* gsl_DataInit(gsl_config_data_id); */
#endif

	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_ts_write(client, 0xe0, buf, 4);
	usleep_range(10000, 10000); /* msleep(10); */
}

/************************************************
  Description	: Reset chip follow the step;
  write (0x88) into address (0xe0);
  write (0x04) into address (0xe4);
  write (0x00) into address (0xbc);
  Input			: struct i2c_client
  Return Value	: void
 ************************************************/
static void reset_chip(struct i2c_client *client)
{
	u8 buf[4] = { 0 };
	buf[0] = 0x88;
	gsl_ts_write(client, 0xe0, buf, 4);
	msleep(20);
	buf[0] = 0x04;
	gsl_ts_write(client, 0xe4, buf, 4);
	usleep_range(10000, 10000); /* msleep(10); */
	buf[0] = 0x00;
	gsl_ts_write(client, 0xbc, buf, 4);
	usleep_range(10000, 10000); /* msleep(10); */

}

/************************************************
  Description	: Clear the data of register follow the step;
  write (0x88) into address (0xe0);
  write (0x01) into address (0x80);
  write (0x04) into address (0xe4);
  write (0x00) into address (0xe0);
  Input			: struct i2c_client
  Return Value	: void
 ************************************************/
static void clr_reg(struct i2c_client *client)
{
	u8 write_buf[4] = { 0 };
	write_buf[0] = 0x88;
	gsl_ts_write(client, 0xe0, &write_buf[0], 1);
	msleep(20);
	write_buf[0] = 0x01;
	gsl_ts_write(client, 0x80, &write_buf[0], 1);
	usleep_range(5000, 5000); /* msleep(5); */
	write_buf[0] = 0x04;
	gsl_ts_write(client, 0xe4, &write_buf[0], 1);
	usleep_range(5000, 5000); /* msleep(5); */
	write_buf[0] = 0x00;
	gsl_ts_write(client, 0xe0, &write_buf[0], 1);
	msleep(20);
}


#ifdef TPD_PROC_DEBUG
static int char_to_int(char ch)
{
	int size;

	if (ch >= '0' && ch <= '9')
		size = (ch - '0');
	else
		size = (ch - 'a' + 10);

	return size;
}

//static int gsl_config_read_proc(char *page, char **start, off_t off, int count,
//				int *eof, void *data)
static int gsl_config_read_proc(struct seq_file *m,void *v)
{
//	char *ptr = page;
//	char size;
	char temp_data[5] = { 0 };
	unsigned int tmp = 0;

	if ('v' == gsl_read[0] && 's' == gsl_read[1]) {
#ifdef GSL_ALG_ID
		tmp = gsl_version_id();
#else
		tmp = 0x20121215;
#endif
		//ptr += snprintf(ptr, "version:%x\n", tmp, sizeof(tmp));
		//ptr += snprintf(m, sizeof(tmp), "version:%x\n", tmp );
		seq_printf(m,"version:%x\n",tmp);
	} else if ('r' == gsl_read[0] && 'e' == gsl_read[1]) {
		if ('i' == gsl_read[3]) {
#ifdef GSL_ALG_ID
			tmp = (gsl_data_proc[5] << 8) | gsl_data_proc[4];
		//	ptr += snprintf(m, sizeof(tmp),"gsl_config_data_id[%d] = ",
			//			tmp );
			seq_printf(m,"gsl_config_data_id[%d] = ",tmp);
						
			if (tmp >= 0 && tmp < 256)
			//	ptr += snprintf(m,sizeof(gsl_config_data_id[tmp]), "%d\n",
			//		gsl_config_data_id[tmp]
			//		);
				seq_printf(m,"%d\n",gsl_config_data_id[tmp]);		
#endif
		} else {
			gsl_write_interfacexw(this_client, 0xf0,
					      &gsl_data_proc[4], 4);
			gsl_ts_read(this_client, gsl_data_proc[0], temp_data,
				    4);
		//	ptr +=
		//	    snprintf(m, sizeof(gsl_data_proc[0]),"offset : {0x%02x,0x",
		//		    gsl_data_proc[0]);
					seq_printf(m,"offset : {0x%02x,0x", gsl_data_proc[0]);
	//		ptr += snprintf(m, sizeof(temp_data[3]),"%02x", temp_data[3],
	//				);
					seq_printf(m,"%02x", temp_data[3]);
	//		ptr += snprintf(m, temp_data[2],"%02x", temp_data[2]
		//			);
					seq_printf(m,"%02x", temp_data[2]);
		//	ptr += snprintf(m, temp_data[1],"%02x", temp_data[1],
		//			);
					seq_printf(m,"%02x", temp_data[1]);
		//	ptr += snprintf(m,temp_data[0], "%02x};\n", temp_data[0]
			//		);
					seq_printf(m,"%02x", temp_data[0]);
		}
	}
	//*eof = 1;
	//size = (ptr - page);
	return 0;
}
static ssize_t gsl_config_write_proc(struct file *file, const char __user *buffer, size_t count, loff_t * data)
//static int gsl_config_write_proc(struct file *file, const char *buffer, unsigned long count, void *data)
{
	u8 buf[8] = { 0 };
	int tmp = 0;
	int tmp1 = 0;

	print_info("[tp-gsl][%s]\n", __func__);

	if (count > CONFIG_LEN) {
		print_info("size not match [%d:%ld]\n", CONFIG_LEN, count);
		return -EFAULT;
	}

	if (copy_from_user
	    (gsl_read, buffer, (count < CONFIG_LEN ? count : CONFIG_LEN))) {
		print_info("copy from user fail\n");
		return -EFAULT;
	}
	print_info("[tp-gsl][%s][%s]\n", __func__, gsl_read);

	buf[3] = char_to_int(gsl_read[14]) << 4 | char_to_int(gsl_read[15]);
	buf[2] = char_to_int(gsl_read[16]) << 4 | char_to_int(gsl_read[17]);
	buf[1] = char_to_int(gsl_read[18]) << 4 | char_to_int(gsl_read[19]);
	buf[0] = char_to_int(gsl_read[20]) << 4 | char_to_int(gsl_read[21]);

	buf[7] = char_to_int(gsl_read[5]) << 4 | char_to_int(gsl_read[6]);
	buf[6] = char_to_int(gsl_read[7]) << 4 | char_to_int(gsl_read[8]);
	buf[5] = char_to_int(gsl_read[9]) << 4 | char_to_int(gsl_read[10]);
	buf[4] = char_to_int(gsl_read[11]) << 4 | char_to_int(gsl_read[12]);
	if ('v' == gsl_read[0] && 's' == gsl_read[1]) {	/* version //vs */
		GSL_INFO(KERN_INFO "gsl version\n");
	} else if ('s' == gsl_read[0] && 't' == gsl_read[1]) {	/* start //st */
		gsl_proc_flag = 1;
		reset_chip(this_client);
	} else if ('e' == gsl_read[0] && 'n' == gsl_read[1]) {	/* end //en */
		msleep(20);
		reset_chip(this_client);
		startup_chip(this_client);
#ifdef GSL_ALG_ID
		gsl_DataInit(gsl_config_data_id);
#endif
		gsl_proc_flag = 0;
	} else if ('r' == gsl_read[0] && 'e' == gsl_read[1])	/* read buf */
		memcpy(gsl_data_proc, buf, 8);
	 else if ('w' == gsl_read[0] && 'r' == gsl_read[1])	/* write buf */
		gsl_write_interfacexw(this_client, buf[4], buf, 4);
#ifdef GSL_ALG_ID
	else if ('i' == gsl_read[0] && 'd' == gsl_read[1]) { /*write id config*/
		tmp1 = (buf[7] << 24) | (buf[6] << 16) | (buf[5] << 8) | buf[4];
		tmp = (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | buf[0];
		if (tmp1 >= 0 && tmp1 < 256)
			gsl_config_data_id[tmp1] = tmp;
	}
#endif

	return count;
}
static int gsl_server_list_open(struct inode *inode,struct file *file)
{
	return single_open(file,gsl_config_read_proc,NULL);
}
static const struct file_operations gsl_seq_fops = {
	.open = gsl_server_list_open,
	.read = seq_read,
	.release = single_release,
	.write = gsl_config_write_proc,
	.owner = THIS_MODULE,
};
#endif

/************************************************
  Description	:Chip initialize
 ************************************************/
static void init_chip(struct i2c_client *client)
{
	static int gsl_init_chip_flag;
	/* static volatile int gsl_init_chip_flag = 0; */

	if (1 == gsl_init_chip_flag)
		return;

	gsl_init_chip_flag = 1;
	gslX680_shutdown_low();	/* Set the Shutdown on Lower Voltage */
	msleep(20);		/* sleep 20 millisecond; */
	gslX680_shutdown_high(); /* Set the Shutdown on Higher Voltage */
	msleep(20);		/* sleep 20 millisecond; */
	test_i2c(client);

	clr_reg(client);
	reset_chip(client);
	gsl_load_fw(client);	/* Download .H file; */
	startup_chip(client);
	reset_chip(client);
	startup_chip(client);

	gsl_init_chip_flag = 0;

}

/************************************************
  Description	: sensitivity of anit-water algorithm
  Input			: struct i2c_client *client
  Output		: void
  Return Value	: return 0
 ************************************************/
#ifdef GSL_ANTIWATER
static int waterproof(struct i2c_client *client)
{
	u8 addr_buf[4] = { 0 };
	u8 para_buf[4] = { 0 };

	int sense = 2;
	switch (sense) {
	case 1:
		para_buf[0] = 0xa;
		print_info("=== Waterproof sensitivity parameter is 0xa! ===");
		break;
	case 2:
		para_buf[0] = 0xb;
		print_info("=== Waterproof sensitivity parameter is 0xb! ===");
		break;
	case 3:
		para_buf[0] = 0xc;
		print_info("=== Waterproof sensitivity parameter is 0xc! ===");
		break;
	case 4:
		para_buf[0] = 0xd;
		print_info("=== Waterproof sensitivity parameter is 0xd! ===");
		break;
	case 5:
		para_buf[0] = 0xe;
		print_info("=== Waterproof sensitivity parameter is 0xe! ===");
		break;
	default:
			print_info("=== Waterproof module is malfunction! ===");
	}
	msleep(30);
	addr_buf[0] = 0x0f;
	gsl_write_interfacexw(client, 0xf0, addr_buf, sizeof(addr_buf));
	gsl_write_interfacexw(client, 0x64, para_buf, sizeof(para_buf));
	return 0;
}
#endif

/************************************************
  Description	: Electronic Static Discharge check function
  Input			: struct work_struct *work
  Return Value	: return 0
 ************************************************/
#ifdef TPD_ESD_PROTECT
static void gsl_esd_check_func(struct work_struct *work)
{
	u8 buf[4] = { 0 };
	u32 tmp;
	static int timer_count;
	if (gsl_halt_flag == 1)
		return;

	/* buf[0] = 0x9f; */
	/* gsl_write_interface(ddata->client, GSL_PAGE_REG, buf, 4); */
	/* gsl_ts_read(this_client, 0xb4, buf, 4); */
	gsl_ts_read(this_client, 0xb4, buf, 4);
	tmp = (buf[3] << 24) | (buf[2] << 16) | (buf[1] << 8) | (buf[0]);

	print_info("[pre] 0xb4 = %x\n", gsl_timer_data);
	print_info("[cur] 0xb4 = %x\n", tmp);
	print_info("gsl_esd_flag=%d\n", gsl_esd_flag);
	if (0 == gsl_esd_flag) {
		if (tmp == gsl_timer_data) {
			gsl_esd_flag = 1;
			if (0 == gsl_halt_flag) {
				queue_delayed_work(gsl_esd_check_workqueue,
						   &gsl_esd_check_work, 25);
			}
		} else {
			gsl_esd_flag = 0;
			timer_count = 0;
			if (0 == gsl_halt_flag) {
				queue_delayed_work(gsl_esd_check_workqueue,
						   &gsl_esd_check_work,
						   TPD_ESD_CHECK_CIRCLE);
			}
		}
	} else if (1 == gsl_esd_flag) {
		if (tmp == gsl_timer_data) {
			if (0 == gsl_halt_flag) {
				timer_count++;
				gsl_esd_flag = 2;
				init_chip(this_client);
				gsl_esd_flag = 1;
			}
			if (0 == gsl_halt_flag && timer_count < 20) {
				queue_delayed_work(gsl_esd_check_workqueue,
						   &gsl_esd_check_work,
						   TPD_ESD_CHECK_CIRCLE);
			}
		} else {
			timer_count = 0;
			if (0 == gsl_halt_flag && timer_count < 20) {
				queue_delayed_work(gsl_esd_check_workqueue,
						   &gsl_esd_check_work,
						   TPD_ESD_CHECK_CIRCLE);
			}
		}
		gsl_esd_flag = 0;
	}
	gsl_timer_data = tmp;
}
#endif

static ssize_t gsl_ts_info_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);

	return snprintf(buf, GSL_INFO_MAX_LEN, "%s\n", ts->ts_info);
}

static DEVICE_ATTR(ts_info, 0664, gsl_ts_info_show, NULL);

static ssize_t gsl_mt_protocol_type_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	return snprintf(buf, 16, "%s\n", "MT Protocol B");
}

static DEVICE_ATTR(mt_protocol_type, 0664, gsl_mt_protocol_type_show, NULL);

static void check_mem_data(struct i2c_client *client);
static void gsl1688_tp_call_open(void);
static void gsl1688_tp_call_release(void);

static ssize_t gsl_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);
	unsigned long val;
	int rc;

	if (size > 2)
		return -EINVAL;

	if (ts->is_suspended) {
		dev_info(&ts->client->dev, "Already in suspend state\n");
		goto no_operation;
	}

	rc = kstrtoul(buf, 10, &val);
	if (rc != 0)
		return rc;

	if (val) {
		gslX680_shutdown_high();
		msleep(20);
		reset_chip(ts->client);
		startup_chip(ts->client);
		check_mem_data(ts->client);
		enable_irq(ts->irq);
		gsl_halt_flag = 0;
		ts->enable = true;
#ifdef TPD_PHONE_MODE
		if (2 == gsl_phone_flag)
			gsl1688_tp_call_open();
		else if (3 == gsl_phone_flag)
			gsl1688_tp_call_release();
#endif
	} else {
		gsl_halt_flag = 1;
		disable_irq_nosync(ts->irq);
		cancel_work_sync(&ts->work);
		reset_chip(ts->client);
		gslX680_shutdown_low();
		ts->enable = false;
	}

no_operation:
	return size;
}

static ssize_t gsl_enable_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);

	if (ts->is_suspended) {
		dev_info(&ts->client->dev, "Already in suspend state\n");
		return snprintf(buf, 4, "%s\n", "0");
	}
	return snprintf(buf, 4, "%s\n", ts->enable ? "1" : "0");
}

static DEVICE_ATTR(enable, 0664, gsl_enable_show, gsl_enable_store);

static ssize_t gsl_update_fw_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t gsl_update_fw_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	return 0;
}

static DEVICE_ATTR(update_fw, 0664, gsl_update_fw_show, gsl_update_fw_store);

static ssize_t gsl_force_update_fw_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t size)
{
	return 0;
}

static DEVICE_ATTR(force_update_fw, 0644, gsl_update_fw_show,
		   gsl_force_update_fw_store);

static ssize_t gsl_fw_name_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);

	return snprintf(buf, GSL_FW_NAME_MAX_LEN - 1, "%s\n", ts->fw_name);
}

static ssize_t gsl_fw_name_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);

	if (size > GSL_FW_NAME_MAX_LEN - 1)
		return -EINVAL;

	strlcpy(ts->fw_name, buf, size);
	if (ts->fw_name[size - 1] == '\n')
		ts->fw_name[size - 1] = 0;

	return size;
}

static DEVICE_ATTR(fw_name, 0664, gsl_fw_name_show, gsl_fw_name_store);

static bool gsl_debug_addr_is_valid(int addr)
{
	if (addr < 0 || addr > 0xFF) {
		pr_err("GSL reg address is invalid: 0x%x\n", addr);
		return false;
	}

	return true;
}

static int gsl_debug_data_set(void *_data, u64 val)
{
	struct gsl_ts *ts = _data;

	mutex_lock(&ts->input->mutex);

	if (gsl_debug_addr_is_valid(ts->addr))
		dev_err(&ts->client->dev,
			"Writing into GSL registers not supported\n");

	mutex_unlock(&ts->input->mutex);

	return 0;
}

static int gsl_debug_data_get(void *_data, u64 *val)
{
	struct gsl_ts *ts = _data;
	int rc;
	u8 reg;

	mutex_lock(&ts->input->mutex);

	if (gsl_debug_addr_is_valid(ts->addr)) {
		rc = gsl_ts_read(ts->client, ts->addr, &reg, sizeof(reg));
		if (rc < 0)
			dev_err(&ts->client->dev,
				"GSL read register 0x%x failed (%d)\n",
				ts->addr, rc);
		else
			*val = reg;
	}

	mutex_unlock(&ts->input->mutex);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_data_fops, gsl_debug_data_get,
			gsl_debug_data_set, "0x%02llX\n");

static int gsl_debug_addr_set(void *_data, u64 val)
{
	struct gsl_ts *ts = _data;

	if (gsl_debug_addr_is_valid(val)) {
		mutex_lock(&ts->input->mutex);
		ts->addr = val;
		mutex_unlock(&ts->input->mutex);
	}

	return 0;
}

static int gsl_debug_addr_get(void *_data, u64 *val)
{
	struct gsl_ts *ts = _data;

	mutex_lock(&ts->input->mutex);

	if (gsl_debug_addr_is_valid(ts->addr))
		*val = ts->addr;

	mutex_unlock(&ts->input->mutex);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_addr_fops, gsl_debug_addr_get,
			gsl_debug_addr_set, "0x%02llX\n");

static int gsl_ts_suspend(struct device *dev);
static int gsl_ts_resume(struct device *dev);

static int gsl_debug_suspend_set(void *_data, u64 val)
{
	struct gsl_ts *ts = _data;

	mutex_lock(&ts->input->mutex);

	if (val)
		gsl_ts_suspend(&ts->client->dev);
	else
		gsl_ts_resume(&ts->client->dev);

	mutex_unlock(&ts->input->mutex);

	return 0;
}

static int gsl_debug_suspend_get(void *data, u64 *val)
{
	struct gsl_ts *ts = data;

	mutex_lock(&ts->input->mutex);
	*val = ts->is_suspended;
	mutex_unlock(&ts->input->mutex);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(debug_suspend_fops, gsl_debug_suspend_get,
			gsl_debug_suspend_set, "%lld\n");

static int gsl_debug_dump_info(struct seq_file *m, void *v)
{
	struct gsl_ts *ts = m->private;

	seq_printf(m, "%s\n", ts->ts_info);

	return 0;
}

static int debugfs_dump_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, gsl_debug_dump_info, inode->i_private);
}

static const struct file_operations debug_dump_info_fops = {
	.owner = THIS_MODULE,
	.open = debugfs_dump_info_open,
	.read = seq_read,
	.release = single_release,
};


/************************************************
  Description	:Check memory(RAM) data;
  The memery is malfunction when the data in register
  (0xb0) is not equal to {5a5a5a5a};
 ************************************************/
static void check_mem_data(struct i2c_client *client)
{
	u8 read_buf[4] = { 0 };

	msleep(30);
	gsl_ts_read(client, 0xb0, read_buf, sizeof(read_buf));

	if (read_buf[3] != 0x5a || read_buf[2] != 0x5a || read_buf[1] != 0x5a
	    || read_buf[0] != 0x5a) {
		GSL_INFO(KERN_INFO "###check mem read 0xb0 = %x %x %x %x ###\n",
		       read_buf[3], read_buf[2], read_buf[1], read_buf[0]);
		init_chip(client);
	}
}


static void record_point(u16 x, u16 y, u8 id)
{
	u16 x_err = 0;
	u16 y_err = 0;

	id_sign[id] = id_sign[id] + 1;

	if (id_sign[id] == 1) {
		x_old[id] = x;
		y_old[id] = y;
	}

	x = (x_old[id] + x) / 2;
	y = (y_old[id] + y) / 2;

	if (x > x_old[id])
		x_err = x - x_old[id];
	else
		x_err = x_old[id] - x;

	if (y > y_old[id])
		y_err = y - y_old[id];
	else
		y_err = y_old[id] - y;

	if ((x_err > 6 && y_err > 2) || (x_err > 2 && y_err > 6)) {
		x_new = x;
		x_old[id] = x;
		y_new = y;
		y_old[id] = y;
	} else {
		if (x_err > 6) {
			x_new = x;
			x_old[id] = x;
		} else
			x_new = x_old[id];
		if (y_err > 6) {
			y_new = y;
			y_old[id] = y;
		} else
			y_new = y_old[id];
	}

	if (id_sign[id] == 1) {
		x_new = x_old[id];
		y_new = y_old[id];
	}

}


#ifdef HAVE_TOUCH_KEY
static void report_key(struct gsl_ts *ts, u16 x, u16 y)
{
	u16 i = 0;

	for (i = 0; i < MAX_KEY_NUM; i++) {
		if ((gsl_key_data[i].x_min < x)
		    && (x < gsl_key_data[i].x_max)
		    && (gsl_key_data[i].y_min < y)
		    && (y < gsl_key_data[i].y_max)) {
			key = gsl_key_data[i].key;
			input_report_key(ts->input, key, 1);
			input_sync(ts->input);
			key_state_flag = 1;
			break;
		}
	}
}
#endif


static void report_data(struct gsl_ts *ts, u16 x, u16 y, u8 pressure, u8 id)
{
	if (x > SCREEN_MAX_X || y > SCREEN_MAX_Y) {
#ifdef HAVE_TOUCH_KEY
		report_key(ts, x, y);
#endif
	}
#ifdef REPORT_DATA_ANDROID_4_0
	input_mt_slot(ts->input, id);
	input_report_abs(ts->input, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, pressure);
	input_report_abs(ts->input, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, 1);
#else
	input_event(ts->input, EV_KEY, BTN_TOUCH,  1);
	input_report_abs(ts->input, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, pressure);
	input_report_abs(ts->input, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, 1);
	input_mt_sync(ts->input);
#endif
}

static void gsl_ts_xy_worker(struct work_struct *work)
{
	int rc, i;
	u8 id, touches;
	u16 x, y;

	struct gsl_ts *ts = container_of(work, struct gsl_ts,work);
	
#ifdef GSL_ALG_ID
	u32 tmp1;
	u8 buf[4] = {0};
	struct gsl_touch_info cinfo;
#endif
	print_info("=====gslX680_ts_worker=====\n");				 

#ifdef TPD_ESD_PROTECT
	if (2 == gsl_esd_flag)
		goto schedule;
#endif




	rc = gsl_ts_read(ts->client, 0x80, ts->touch_data, ts->dd->data_size);
	if (rc < 0) 
	{
		dev_err(&ts->client->dev, "read failed\n");
		goto schedule;
	}
		
	touches = ts->touch_data[ts->dd->touch_index];
	print_info("-----touches: %d -----\n", touches);		
#ifdef GSL_ALG_ID
	cinfo.finger_num = touches;
	print_info("tp-gsl  finger_num = %d\n",cinfo.finger_num);
	for(i = 0; i < (touches < MAX_CONTACTS ? touches : MAX_CONTACTS); i ++)
	{
		cinfo.x[i] = join_bytes( ( ts->touch_data[ts->dd->x_index  + 4 * i + 1] & 0xf),
				ts->touch_data[ts->dd->x_index + 4 * i]);
		cinfo.y[i] = join_bytes(ts->touch_data[ts->dd->y_index + 4 * i + 1],
				ts->touch_data[ts->dd->y_index + 4 * i ]);
		cinfo.id[i] = ((ts->touch_data[ts->dd->x_index  + 4 * i + 1] & 0xf0)>>4);
		print_info("tp-gsl  before: x[%d] = %d, y[%d] = %d, id[%d] = %d \n",i,cinfo.x[i],i,cinfo.y[i],i,cinfo.id[i]);
	}
	cinfo.finger_num=(ts->touch_data[3]<<24)|(ts->touch_data[2]<<16)
		|(ts->touch_data[1]<<8)|(ts->touch_data[0]);
	gsl_alg_id_main(&cinfo);
	tmp1=gsl_mask_tiaoping();
	print_info("[tp-gsl] tmp1=%x\n",tmp1);
	if(tmp1>0&&tmp1<0xffffffff)
	{
		buf[0]=0xa;buf[1]=0;buf[2]=0;buf[3]=0;
		gsl_write_interfacexw(ts->client,0xf0,buf,4);
		buf[0]=(u8)(tmp1 & 0xff);
		buf[1]=(u8)((tmp1>>8) & 0xff);
		buf[2]=(u8)((tmp1>>16) & 0xff);
		buf[3]=(u8)((tmp1>>24) & 0xff);
		print_info("tmp1=%08x,buf[0]=%02x,buf[1]=%02x,buf[2]=%02x,buf[3]=%02x\n",
			tmp1,buf[0],buf[1],buf[2],buf[3]);
		gsl_write_interfacexw(ts->client,0x8,buf,4);
	}
	touches = cinfo.finger_num;
#endif
	
	for(i = 1; i <= MAX_CONTACTS; i ++)
	{
		if(touches == 0)
			id_sign[i] = 0;	
		id_state_flag[i] = 0;
	}
	for(i= 0;i < (touches > MAX_FINGERS ? MAX_FINGERS : touches);i ++)
	{
	#ifdef GSL_ALG_ID
		id = cinfo.id[i];
		x =  cinfo.x[i];
		y =  cinfo.y[i];	
	#else
		x = join_bytes( ( ts->touch_data[ts->dd->x_index  + 4 * i + 1] & 0xf),
				ts->touch_data[ts->dd->x_index + 4 * i]);
		y = join_bytes(ts->touch_data[ts->dd->y_index + 4 * i + 1],
				ts->touch_data[ts->dd->y_index + 4 * i ]);
		id = ts->touch_data[ts->dd->id_index + 4 * i] >> 4;
	#endif

		if(1 <=id && id <= MAX_CONTACTS)
		{
		#if 1
		#ifdef FILTER_POINT
			filter_point(x, y ,id);
		#else
			record_point(x, y , id);
		#endif
		#endif
			report_data(ts, x_new, y_new, 10, id);		
			id_state_flag[id] = 1;
		}
	}
	for(i = 1; i <= MAX_CONTACTS; i ++)
	{	
		if( (0 == touches) || ((0 != id_state_old_flag[i]) && (0 == id_state_flag[i])) )
		{
		#ifdef REPORT_DATA_ANDROID_4_0
			input_mt_slot(ts->input, i);
			input_report_abs(ts->input, ABS_MT_TRACKING_ID, -1);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		#endif
			id_sign[i]=0;
		}
		id_state_old_flag[i] = id_state_flag[i];
	}
	if(0 == touches)
	{
#ifndef REPORT_DATA_ANDROID_4_0
		input_mt_sync(ts->input);
#endif
	#ifdef HAVE_TOUCH_KEY
		if(key_state_flag)
		{
        		input_report_key(ts->input, key, 0);
			input_sync(ts->input);
			key_state_flag = 0;
		}
	#endif			
	}	
	input_sync(ts->input);
	
schedule:
	enable_irq(ts->irq);

}

/************************************************
 Description	:gsl interrupt request
 ************************************************/
static irqreturn_t gsl_ts_irq(int irq, void *dev_id)
{
	struct gsl_ts *ts = dev_id;

	if (ts->is_suspended == true)
		return IRQ_HANDLED;
	print_info("===GSLX680 Interrupt===\n");

	disable_irq_nosync(ts->irq);

	if (!work_pending(&ts->work)) {
		queue_work(ts->wq, &ts->work);
		usleep_range(1000, 1000); /* msleep(1); */
	}

	return IRQ_HANDLED;

}


#ifdef GSL_TIMER
static void gsl_timer_handle(unsigned long data)
{
	struct gsl_ts *ts = (struct gsl_ts *)data;

#ifdef GSL_DEBUG
	GSL_INFO(KERN_DEBUG "---gsl_timer_handle---\n");
#endif

	disable_irq_nosync(ts->irq);
	check_mem_data(ts->client);
	ts->gsl_timer.expires = jiffies + 3 * HZ;
	add_timer(&ts->gsl_timer);
	enable_irq(ts->irq);

}
#endif

#ifdef CONFIG_OF
static int gsl_parse_dt(struct device *dev, struct gsl_ts *ts)
{
	u32 gpio_flags_tmp;
	struct device_node *np = dev->of_node;
	int rc;

	ts->reset_gpio = of_get_named_gpio_flags(np, "gsl,reset-gpio",
						 0, &gpio_flags_tmp);
	if (ts->reset_gpio < 0)
		return ts->reset_gpio;
	RST_PORT = ts->reset_gpio;

	ts->irq_gpio = of_get_named_gpio_flags(np, "gsl,irq-gpio",
					       0, &gpio_flags_tmp);
	if (ts->irq_gpio < 0)
		return ts->irq_gpio;
	IRQ_PORT = ts->irq_gpio;

	ts->fw_name_ptr = "gsl2680.h";
	rc = of_property_read_string(np, "gsl,fw-name", &ts->fw_name_ptr);
	if (rc && (rc != -EINVAL)) {
		dev_err(dev, "Unable to read fw name\n");
		return rc;
	}

	ts->fw_vkey_support = of_property_read_bool(np, "gsl,fw-vkey-support");

	return 0;
}
#else
static int gsl_parse_dt(struct device *dev, struct gsl_ts *ts)
{
	return -ENODEV;
}
#endif /*CONFIG_OF */

static int gsl_power_on(struct gsl_ts *data, bool on)
{
	int rc;

	if (!on)
		goto power_off;

	rc = regulator_enable(data->vdd);
	if (rc) {
		dev_err(&data->client->dev,
			"Regulator vdd enable failed rc=%d\n", rc);
		return rc;
	}

	rc = regulator_enable(data->vcc_i2c);
	if (rc) {
		dev_err(&data->client->dev,
			"Regulator vcc_i2c enable failed rc=%d\n", rc);
		regulator_disable(data->vdd);
	}

	return rc;

power_off:
	rc = regulator_disable(data->vdd);
	if (rc) {
		dev_err(&data->client->dev,
			"Regulator vdd disable failed rc=%d\n", rc);
		return rc;
	}

	rc = regulator_disable(data->vcc_i2c);
	if (rc) {
		dev_err(&data->client->dev,
			"Regulator vcc_i2c disable failed rc=%d\n", rc);
		rc = regulator_enable(data->vdd);
		if (rc)
			dev_err(&data->client->dev,
				"Regulator vcc_i2c enable failed rc=%d\n", rc);
	}

	return rc;
}

static int gsl_power_init(struct gsl_ts *data, bool on)
{
	int rc;

	if (!on)
		goto pwr_deinit;

	data->vdd = regulator_get(&data->client->dev, "vdd");
	if (IS_ERR(data->vdd)) {
		rc = PTR_ERR(data->vdd);
		dev_err(&data->client->dev,
			"Regulator get failed vdd rc=%d\n", rc);
		return rc;
	}

	if (regulator_count_voltages(data->vdd) > 0) {
		rc = regulator_set_voltage(data->vdd, 2700000, 3300000);
		if (rc) {
			dev_err(&data->client->dev,
				"Regulator set_vtg failed vdd rc=%d\n", rc);
			goto reg_vdd_put;
		}
	}

	data->vcc_i2c = regulator_get(&data->client->dev, "vcc_i2c");
	if (IS_ERR(data->vcc_i2c)) {
		rc = PTR_ERR(data->vcc_i2c);
		dev_err(&data->client->dev,
			"Regulator get failed vcc_i2c rc=%d\n", rc);
		goto reg_vdd_set_vtg;
	}

	if (regulator_count_voltages(data->vcc_i2c) > 0) {
		rc = regulator_set_voltage(data->vcc_i2c, 1800000, 1800000);
		if (rc) {
			dev_err(&data->client->dev,
				"Regulator set_vtg failed vcc_i2c rc=%d\n", rc);
			goto reg_vcc_i2c_put;
		}
	}

	return 0;

reg_vcc_i2c_put:
	regulator_put(data->vcc_i2c);
reg_vdd_set_vtg:
	if (regulator_count_voltages(data->vdd) > 0)
		regulator_set_voltage(data->vdd, 0, 3300000);
reg_vdd_put:
	regulator_put(data->vdd);
	return rc;

pwr_deinit:
	if (regulator_count_voltages(data->vdd) > 0)
		regulator_set_voltage(data->vdd, 0, 3300000);

	regulator_put(data->vdd);

	if (regulator_count_voltages(data->vcc_i2c) > 0)
		regulator_set_voltage(data->vcc_i2c, 0, 1800000);

	regulator_put(data->vcc_i2c);
	return 0;
}


static int gsl_ts_init_ts(struct i2c_client *client, struct gsl_ts *ts)
{
	struct input_dev *input_device;
	int rc = 0;

	GSL_INFO(KERN_INFO "[GSLX680] Enter %s\n", __func__);

	ts->dd = &devices[ts->device_id];

	if (ts->device_id == 0) {
		ts->dd->data_size =
		    MAX_FINGERS * ts->dd->touch_bytes + ts->dd->touch_meta_data;
		ts->dd->touch_index = 0;
	}

	ts->touch_data = kzalloc(ts->dd->data_size, GFP_KERNEL);
	if (!ts->touch_data) {
		pr_err("%s: Unable to allocate memory\n", __func__);
		return -ENOMEM;
	}
	
	input_device = input_allocate_device();
	if (!input_device) {
		rc = -ENOMEM;
		goto error_alloc_dev;
	}

	ts->input = input_device;
	input_device->name = GSLX680_I2C_NAME;
	input_device->id.bustype = BUS_I2C;
	input_device->dev.parent = &client->dev;
	input_set_drvdata(input_device, ts);

#ifdef REPORT_DATA_ANDROID_4_0

	input_set_abs_params(input_device, ABS_MT_TRACKING_ID, 0,
			     (MAX_CONTACTS + 1), 0, 0);

	__set_bit(EV_ABS, input_device->evbit);
	__set_bit(EV_KEY, input_device->evbit);
	__set_bit(EV_REP, input_device->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_device->propbit);
	input_mt_init_slots(input_device, (MAX_CONTACTS + 1), 0);
#else
	input_set_abs_params(input_device, ABS_MT_TRACKING_ID, 0,
			     (MAX_CONTACTS + 1), 0, 0);
	set_bit(EV_ABS, input_device->evbit);
	set_bit(EV_KEY, input_device->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_device->propbit);
	input_device->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);
#endif

#ifdef HAVE_TOUCH_KEY
	{
		int i;
		for (i = 0; i < MAX_KEY_NUM; i++)
			set_bit(gsl_key_data[i].key, input_device->keybit);
	}
#endif

	set_bit(ABS_MT_POSITION_X, input_device->absbit);
	set_bit(ABS_MT_POSITION_Y, input_device->absbit);
	set_bit(ABS_MT_TOUCH_MAJOR, input_device->absbit);
	set_bit(ABS_MT_WIDTH_MAJOR, input_device->absbit);

	input_set_abs_params(input_device, ABS_MT_POSITION_X, 0, SCREEN_MAX_X,
			     0, 0);
	input_set_abs_params(input_device, ABS_MT_POSITION_Y, 0, SCREEN_MAX_Y,
			     0, 0);
	input_set_abs_params(input_device, ABS_MT_TOUCH_MAJOR, 0, PRESS_MAX, 0,
			     0);
	input_set_abs_params(input_device, ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);

	client->irq = gpio_to_irq(ts->irq_gpio);
	ts->irq = client->irq;

	ts->wq = create_singlethread_workqueue("kworkqueue_ts");
	if (!ts->wq) {
		dev_err(&client->dev, "Could not create workqueue\n");
		goto error_wq_create;
	}
	flush_workqueue(ts->wq);

	INIT_WORK(&ts->work, gsl_ts_xy_worker);

	rc = input_register_device(input_device);
	if (rc)
		goto error_unreg_device;

	return 0;

error_unreg_device:
	destroy_workqueue(ts->wq);
error_wq_create:
	input_free_device(input_device);
error_alloc_dev:
	kfree(ts->touch_data);
	return rc;
}

/************************************************
  Description	:suspend (shutdown-pin into lower voltage)
 ************************************************/
static int gsl_ts_suspend(struct device *dev)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);

	GSL_INFO(KERN_DEBUG "I'am in gsl_ts_suspend() start\n");
	if (ts->is_suspended) {
		dev_err(dev, "Already in suspend state\n");
		return 0;
	}

	ts->is_suspended = true;
	gsl_halt_flag = 1;

#ifdef TPD_PROC_DEBUG
	if (gsl_proc_flag == 1)
		return 1;
#endif

#ifdef TPD_ESD_PROTECT
	cancel_delayed_work_sync(&gsl_esd_check_work);
	if (2 == gsl_esd_flag)
		return 2;
#endif

	disable_irq_nosync(ts->irq);
	cancel_work_sync(&ts->work);

	reset_chip(ts->client);
	gslX680_shutdown_high();
	gsl_power_on(ts, true);
	return 0;
}


static int gsl_ts_resume(struct device *dev)
{
	struct gsl_ts *ts = dev_get_drvdata(dev);

	GSL_INFO(KERN_DEBUG "I'am in gsl_ts_resume() start\n");

	if (!ts->is_suspended) {
		dev_err(dev, "Already in awake state\n");
		return 0;
	}

	gsl_power_on(ts, true);

#ifdef TPD_ESD_PROTECT
	if (2 == gsl_esd_flag) {
		gsl_halt_flag = 0;
		enable_irq(this_client->irq);
		return 0;
	}
#endif
	gslX680_shutdown_high();
	msleep(20);
	reset_chip(ts->client);
	startup_chip(ts->client);
	check_mem_data(ts->client);

#ifdef TPD_PROC_DEBUG
	if (gsl_proc_flag == 1)
		return 0;
#endif

	enable_irq(ts->irq);
	ts->is_suspended = false;

#ifdef TPD_ESD_PROTECT
	queue_delayed_work(gsl_esd_check_workqueue, &gsl_esd_check_work,
			   TPD_ESD_CHECK_CIRCLE);
	gsl_esd_flag = 0;
#endif

	gsl_halt_flag = 0;

#ifdef TPD_PHONE_MODE
	if (2 == gsl_phone_flag)
		gsl1688_tp_call_open();
	else if (3 == gsl_phone_flag)
		gsl1688_tp_call_release();
#endif

	return 0;
}


#if defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct gsl_ts *ts = container_of(self, struct gsl_ts, fb_notif);

	if (evdata && evdata->data && event == FB_EVENT_BLANK &&
	    ts && ts->client) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK)
			gsl_ts_resume(&ts->client->dev);
		else if (*blank == FB_BLANK_POWERDOWN)
			gsl_ts_suspend(&ts->client->dev);
	}

	return 0;
}
#elif defined CONFIG_HAS_EARLYSUSPEND
static void gsl_ts_early_suspend(struct early_suspend *h)
{
	struct gsl_ts *ts = container_of(h, struct gsl_ts, early_suspend);
	GSL_INFO(KERN_DEBUG "[GSL1680] Enter %s\n", __func__);
	gsl_ts_suspend(&ts->client->dev);
}

static void gsl_ts_late_resume(struct early_suspend *h)
{
	struct gsl_ts *ts = container_of(h, struct gsl_ts, early_suspend);
	GSL_INFO(KERN_DEBUG "[GSL1680] Enter %s\n", __func__);
	gsl_ts_resume(&ts->client->dev);
}
#endif

static const struct dev_pm_ops gsl2680_ts_pm_ops = {
#if (!defined(CONFIG_FB) && !defined(CONFIG_HAS_EARLYSUSPEND))
	.suspend = gsl_ts_suspend,
	.resume = gsl_ts_resume,
#endif
};


#ifdef TPD_PHONE_MODE
static void gsl1688_tp_call_open(void)
{
	u8 buf[4] = { 0 };

	gpio_set_value(RST_PORT, 1);
	usleep_range(10000, 10000); /* msleep(10); */
	if (1 == gsl_phone_count)
		return;

	if (1 == gsl_halt_flag) {
		gsl_phone_flag = 2;
		return;
	}

	gsl_phone_count = 1;

	gsl_phone_flag = 1;

	GSL_INFO(KERN_INFO "%s is start\n", __func__);

	reset_chip(this_client);
	gsl_config_data_id[213] = 0;
	/* page 0x3 */
	buf[0] = 0x3;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = gsl_phone_data[0] | 0x1;
	buf[1] = gsl_phone_data[1];
	buf[2] = gsl_phone_data[2];
	buf[3] = gsl_phone_data[3];

	/* GSL_INFO("buf[0]=0x%02x\n",buf[0]); */
	gsl_write_interfacexw(this_client, 0x0, buf, 4);
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x24, buf, 4);
	buf[0] = 0x20;
	buf[1] = 0x3;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x54, buf, 4);

	buf[0] = 0x20;
	buf[1] = 0x3;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x5c, buf, 4);

	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x74, buf, 4);
	/* page 0x4 */
	buf[0] = 0x4;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = 0x20;
	buf[1] = 0x3;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x3c, buf, 4);
	/* page 0x6 */
	buf[0] = 0x6;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = 0x28;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x78, buf, 4);

	/* page 0x7 */
	buf[0] = 0x7;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0x40, buf, 4);
	startup_chip(this_client);
	msleep(20);
	check_mem_data(this_client);
	gsl_phone_flag = 0;
}


static void gsl1688_tp_call_release(void)
{
	u8 buf[4] = { 0 };
	if (0 == gsl_phone_count)
		return;

	if (1 == gsl_halt_flag) {
		gsl_phone_flag = 3;
		return;
	}

	gsl_phone_count = 0;
	gsl_phone_flag = 1;
	GSL_INFO(KERN_INFO "%s is start\n", __func__);
	reset_chip(this_client);
	gsl_config_data_id[213] = gsl_phone_id_data;
	/* page 0x3 */
	buf[0] = 0x3;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = gsl_phone_data[0];
	buf[1] = gsl_phone_data[1];
	buf[2] = gsl_phone_data[2];
	buf[3] = gsl_phone_data[3];
	gsl_write_interfacexw(this_client, 0, buf, 4);
	buf[0] = gsl_phone_data[4];
	buf[1] = gsl_phone_data[5];
	buf[2] = gsl_phone_data[6];
	buf[3] = gsl_phone_data[7];
	gsl_write_interfacexw(this_client, 0x24, buf, 4);
	buf[0] = gsl_phone_data[16];
	buf[1] = gsl_phone_data[17];
	buf[2] = gsl_phone_data[18];
	buf[3] = gsl_phone_data[19];
	gsl_write_interfacexw(this_client, 0x54, buf, 4);

	buf[0] = gsl_phone_data[20];
	buf[1] = gsl_phone_data[21];
	buf[2] = gsl_phone_data[22];
	buf[3] = gsl_phone_data[23];
	gsl_write_interfacexw(this_client, 0x5c, buf, 4);

	buf[0] = gsl_phone_data[8];
	buf[1] = gsl_phone_data[9];
	buf[2] = gsl_phone_data[10];
	buf[3] = gsl_phone_data[11];
	gsl_write_interfacexw(this_client, 0x74, buf, 4);

	/* page 0x4 */
	buf[0] = 0x4;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = gsl_phone_data[24];
	buf[1] = gsl_phone_data[25];
	buf[2] = gsl_phone_data[26];
	buf[3] = gsl_phone_data[27];
	gsl_write_interfacexw(this_client, 0x3c, buf, 4);
	/* page 0x6 */
	buf[0] = 0x6;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = gsl_phone_data[28];
	buf[1] = gsl_phone_data[29];
	buf[2] = gsl_phone_data[30];
	buf[3] = gsl_phone_data[31];
	gsl_write_interfacexw(this_client, 0x78, buf, 4);
	/* page 0x7 */
	buf[0] = 0x7;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(this_client, 0xf0, buf, 4);
	buf[0] = gsl_phone_data[12];
	buf[1] = gsl_phone_data[13];
	buf[2] = gsl_phone_data[14];
	buf[3] = gsl_phone_data[15];
	gsl_write_interfacexw(this_client, 0x40, buf, 4);
	startup_chip(this_client);
	msleep(20);
	check_mem_data(this_client);
	gsl_phone_flag = 0;
}


static void gsl_gain_original_value(struct i2c_client *client)
{
	int tmp;
	u8 buf[4] = { 0 };
	buf[0] = 0x3;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(client, 0xf0, buf, 4);

	tmp = gsl_ts_read(client, 0x0, &gsl_phone_data[0], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x0, &gsl_phone_data[0], 4);

	tmp = gsl_ts_read(client, 0x24, &gsl_phone_data[4], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x24, &gsl_phone_data[4], 4);

	tmp = gsl_ts_read(client, 0x54, &gsl_phone_data[16], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x54, &gsl_phone_data[16], 4);

	tmp = gsl_ts_read(client, 0x5c, &gsl_phone_data[20], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x5c, &gsl_phone_data[20], 4);

	tmp = gsl_ts_read(client, 0x74, &gsl_phone_data[8], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x74, &gsl_phone_data[8], 4);

	buf[0] = 0x4;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(client, 0xf0, buf, 4);

	tmp = gsl_ts_read(client, 0x3c, &gsl_phone_data[24], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x3c, &gsl_phone_data[24], 4);

	buf[0] = 0x6;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(client, 0xf0, buf, 4);

	tmp = gsl_ts_read(client, 0x78, &gsl_phone_data[28], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x78, &gsl_phone_data[28], 4);

	buf[0] = 0x7;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = 0;
	gsl_write_interfacexw(client, 0xf0, buf, 4);
	tmp = gsl_ts_read(client, 0x40, &gsl_phone_data[12], 4);
	if (tmp != 4)
		gsl_ts_read(client, 0x40, &gsl_phone_data[12], 4);

	gsl_phone_id_data = gsl_config_data_id[213];
}

static int gsl1688_tp_call_file_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int gsl1688_tp_call_file_close(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t gsl1688_tp_call_file_write(struct file *file,
					  const char __user *user,
					  size_t count, loff_t *loff)
{
	unsigned char buf[10];

	if (copy_from_user(buf, user, (count > 10 ? 10 : count))) {
		GSL_INFO(KERN_ERR "gtp_file_write copy_from_user error!\n");
		return -EPERM;
	}
	switch (buf[0]) {
	case '0':
		gsl1688_tp_call_release();
		break;
	case '1':
		gsl1688_tp_call_open();
		break;
	default:
		break;
	}

	return count;
}


static long gsl1688_tp_call_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	return 0;
}

static const struct file_operations gsl1688_tp_call_fops = {
	.owner = THIS_MODULE,
	.open = gsl1688_tp_call_file_open,
	.release = gsl1688_tp_call_file_close,
	.write = gsl1688_tp_call_file_write,
	.unlocked_ioctl = gsl1688_tp_call_ioctl
};

struct miscdevice gsl1688_tp_call_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = GSLX680_TP_CALL_NAME,	/* match the hal's name */
	.fops = &gsl1688_tp_call_fops
};
#endif

static ssize_t virtual_keys_register(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	char *virtual_keys = __stringify(EV_KEY) ":" __stringify(KEY_MENU) \
	    ":36:1320:40:40" ":" __stringify(EV_KEY) \
	    ":" __stringify(KEY_HOMEPAGE) ":396:1320:40:40" \
	    ":" __stringify(EV_KEY) ":" \
	    __stringify(KEY_BACK) ":612:1320:40:40" "\n";

	return snprintf(buf, strnlen(virtual_keys, 100) + 1, "%s",
			virtual_keys);
}

static struct kobj_attribute virtual_keys_attr = {
	.attr = {
		 .name = "virtualkeys.gslX680",
		 .mode = S_IRUGO,
		 },
	.show = &virtual_keys_register,
};

static struct attribute *virtual_key_properties_attrs[] = {
	&virtual_keys_attr.attr,
	NULL,
};

static struct attribute_group virtual_key_properties_attr_group = {
	.attrs = virtual_key_properties_attrs,
};

struct kobject *virtual_key_properties_kobj;


static int gsl_ts_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct gsl_ts *ts;
	int rc, len;
	static int err;
	struct dentry *temp;
	u8 ic_ver_buf[2] = { 0 };

	/* pr_pos_info(); */

	GSL_INFO(KERN_INFO "GSLX680 Enter %s\n", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "I2C functionality not supported\n");
		return -ENODEV;
	}
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	GSL_INFO(KERN_DEBUG "==kzalloc success=\n");

	this_client = client;
	ts->client = client;
	i2c_set_clientdata(client, ts);
	ts->device_id = 0;
	ts->is_suspended = false;

	rc = gsl_parse_dt(&client->dev, ts);
	if (rc) {
		dev_err(&client->dev, "DT parsing failed\n");
		return rc;
	}

	rc = gsl_power_init(ts, true);
	if (rc) {
		dev_err(&client->dev, "power on failed");
		goto error_free_ts;
	}

	rc = gsl_power_on(ts, true);
	if (rc) {
		dev_err(&client->dev, "power on failed");
		goto error_pwr_deinit;
	}

	ts->enable = true;

	if (ts->fw_name_ptr) {
		len = strlen(ts->fw_name_ptr);
		if (len > GSL_FW_NAME_MAX_LEN - 1) {
			dev_err(&client->dev, "Invalid firmware name\n");
			goto error_fw_name;
		}
		strlcpy(ts->fw_name, ts->fw_name_ptr, len + 1);
	}

	ts->int_pending = false;
	if (gpio_is_valid(ts->irq_gpio)) {
		rc = gpio_request(ts->irq_gpio, "GSL_IRQ");
		if (rc) {
			dev_err(&client->dev,
				"rc = %d : could not req gpio irq\n", rc);
			goto error_fw_name;
		}
		rc = gpio_direction_input(ts->irq_gpio);
		if (rc) {
			dev_err(&client->dev,
				"set_direction for irq gpio failed\n");
			goto error_free_irq_gpio;
		}
	}

	if (gpio_is_valid(ts->reset_gpio)) {
		rc = gpio_request(ts->reset_gpio, "GSL_RST");
		if (rc) {
			dev_err(&client->dev,
				"rc = %d : could not req gpio reset\n", rc);
			goto error_free_irq_gpio;
		}
		rc = gpio_direction_output(ts->reset_gpio, 1);
		if (rc) {
			dev_err(&client->dev,
				"set_direction for reset gpio failed\n");
			goto error_free_reset_gpio;
		}
	}
	msleep(20);

	mutex_init(&ts->sus_lock);
	rc = gsl_ts_init_ts(client, ts);
	if (rc < 0) {
		dev_err(&client->dev, "GSLX680 init failed\n");
		goto error_mutex_destroy;
	}

	init_chip(ts->client);
	check_mem_data(ts->client);

	rc = request_threaded_irq(client->irq, NULL, gsl_ts_irq,
				  IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					client->name, ts);
	if (rc < 0) {
		GSL_INFO(KERN_ERR "gsl_probe: request irq failed\n");
		goto error_irq_free;
	}
#ifdef GSL_TIMER
	GSL_INFO(KERN_DEBUG "gsl_ts_probe () : add gsl_timer\n");

	init_timer(&ts->gsl_timer);
	ts->gsl_timer.expires = jiffies + 3 * HZ;
	ts->gsl_timer.function = &gsl_timer_handle;
	ts->gsl_timer.data = (unsigned long)ts;
	add_timer(&ts->gsl_timer);
#endif

	rc = gsl_ts_read(client, 0xfe, ic_ver_buf, sizeof(ic_ver_buf));
	if (0 == rc) {
		dev_err(&client->dev, "Read IC version failed\n");
		goto error_irq_free;
	}

	ts->ic_ver = (ic_ver_buf[1] << 8) | ic_ver_buf[0];

	if (ts->ic_ver != GSL_IC_VERSION) {
		dev_err(&client->dev, "Unsupported controller\n");
		goto error_irq_free;
	}

	/* create debug attribute */
	rc = device_create_file(&client->dev, &dev_attr_ts_info);
	if (rc) {
		dev_err(&client->dev, "sys file creation failed\n");
		goto error_irq_free;
	}

	rc = device_create_file(&client->dev, &dev_attr_mt_protocol_type);
	if (rc) {
		dev_err(&client->dev, "sys file creation failed\n");
		goto error_irq_free;
	}

	rc = device_create_file(&client->dev, &dev_attr_enable);
	if (rc) {
		dev_err(&client->dev, "sys file creation failed\n");
		goto error_irq_free;
	}

	rc = device_create_file(&client->dev, &dev_attr_fw_name);
	if (rc) {
		dev_err(&client->dev, "sys file creation failed\n");
		goto error_irq_free;
	}

	rc = device_create_file(&client->dev, &dev_attr_update_fw);
	if (rc) {
		dev_err(&client->dev, "sys file creation failed\n");
		goto error_free_fw_name_sys;
	}

	rc = device_create_file(&client->dev, &dev_attr_force_update_fw);
	if (rc) {
		dev_err(&client->dev, "sys file creation failed\n");
		goto error_free_update_fw_sys;
	}

	ts->dir = debugfs_create_dir(GSL_DEBUG_DIR_NAME, NULL);
	if (ts->dir == NULL || IS_ERR(ts->dir)) {
		pr_err("debugfs_create_dir failed(%ld)\n", PTR_ERR(ts->dir));
		rc = PTR_ERR(ts->dir);
		goto error_free_force_update_fw_sys;
	}

	temp = debugfs_create_file("addr", S_IRUSR | S_IWUSR, ts->dir, ts,
				   &debug_addr_fops);
	if (temp == NULL || IS_ERR(temp)) {
		pr_err("debugfs_create_file failed: rc=%ld\n", PTR_ERR(temp));
		rc = PTR_ERR(temp);
		goto error_free_debug_dir;
	}

	temp = debugfs_create_file("data", S_IRUSR | S_IWUSR, ts->dir, ts,
				   &debug_data_fops);
	if (temp == NULL || IS_ERR(temp)) {
		pr_err("debugfs_create_file failed: rc=%ld\n", PTR_ERR(temp));
		rc = PTR_ERR(temp);
		goto error_free_debug_dir;
	}

	temp = debugfs_create_file("suspend", S_IRUSR | S_IWUSR, ts->dir,
				   ts, &debug_suspend_fops);
	if (temp == NULL || IS_ERR(temp)) {
		pr_err("debugfs_create_file failed: rc=%ld\n", PTR_ERR(temp));
		rc = PTR_ERR(temp);
		goto error_free_debug_dir;
	}

	temp = debugfs_create_file("dump_info", S_IRUSR | S_IWUSR, ts->dir,
				   ts, &debug_dump_info_fops);
	if (temp == NULL || IS_ERR(temp)) {
		pr_err("debugfs_create_file failed: rc=%ld\n", PTR_ERR(temp));
		rc = PTR_ERR(temp);
		goto error_free_debug_dir;
	}

	ts->ts_info = devm_kzalloc(&client->dev, GSL_INFO_MAX_LEN, GFP_KERNEL);
	if (!ts->ts_info) {
		dev_err(&client->dev, "Not enough memory\n");
		goto error_free_debug_dir;
	}

	GSL_STORE_TS_INFO(ts->ts_info, ts->input->name, ts->ic_ver,
			  MAX_FINGERS, ts->fw_vkey_support ? "yes" : "no",
			  ts->fw_name);

#if defined(CONFIG_FB)
	ts->fb_notif.notifier_call = fb_notifier_callback;

	rc = fb_register_client(&ts->fb_notif);

	if (rc)
		dev_err(&client->dev, "Unable to register fb_notifier: %d\n",
			rc);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = gsl_ts_early_suspend;
	ts->early_suspend.resume = gsl_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif
#ifdef GSL_ALG_ID
	gsl_DataInit(gsl_config_data_id);
#endif

#ifdef TPD_PROC_DEBUG
#if 0
	gsl_config_proc = proc_create(GSL_CONFIG_PROC_FILE, 0666, NULL,&gsl_seq_fops);
	if (gsl_config_proc == NULL) {
		print_info("create_proc_entry %s failed\n",
			   GSL_CONFIG_PROC_FILE);
	} else {
		gsl_config_proc->read_proc = gsl_config_read_proc;
		gsl_config_proc->write_proc = gsl_config_write_proc;
	}
#else
	proc_create(GSL_CONFIG_PROC_FILE, 0666, NULL,&gsl_seq_fops);
#endif	
	gsl_proc_flag = 0;
#endif
#ifdef TPD_ESD_PROTECT
	INIT_DELAYED_WORK(&gsl_esd_check_work, gsl_esd_check_func);
	gsl_esd_check_workqueue = create_workqueue("gsl_esd_check");
	queue_delayed_work(gsl_esd_check_workqueue, &gsl_esd_check_work,
			   TPD_ESD_CHECK_CIRCLE);
#endif

#ifdef TPD_PHONE_MODE
	gsl_gain_original_value(this_client);
	err = misc_register(&gsl1688_tp_call_misc);
	if (err < 0)
		pr_err("%s: could not register misc device\n", __func__);
#endif

	virtual_key_properties_kobj =
	    kobject_create_and_add("board_properties", NULL);
	if (virtual_key_properties_kobj)
		err =
		    sysfs_create_group(virtual_key_properties_kobj,
				       &virtual_key_properties_attr_group);
	if (!virtual_key_properties_kobj || err)
		pr_err("failed to create gsl board_properties\n");

	GSL_INFO(KERN_INFO "[GSLX680] End %s\n", __func__);

#ifdef GSL_ANTIWATER
	u8 addr_buf[4] = { 0 };
	u8 verify_buf[4] = { 0 };
	addr_buf[0] = 0x0a;
	gsl_write_interface(client, 0xf0, addr_buf, sizeof(addr_buf));
	gsl_ts_read(client, 0x0c, verify_buf, sizeof(verify_buf));

	if (verify_buf[3] == 0xa5 && verify_buf[2] == 0xa5
	    && verify_buf[1] == 0x5a && verify_buf[0] == 0x5a) {
		waterproof(ts->client);
	} else {
		print_info("=== The value of {0x0a,0x0c} = %x%x%x%x ===",
			   verify_buf[3], verify_buf[2], verify_buf[1],
			   verify_buf[0])
	}
#endif

	return 0;

error_free_debug_dir:
	debugfs_remove_recursive(ts->dir);
error_free_force_update_fw_sys:
	device_remove_file(&client->dev, &dev_attr_force_update_fw);
error_free_update_fw_sys:
	device_remove_file(&client->dev, &dev_attr_update_fw);
error_free_fw_name_sys:
	device_remove_file(&client->dev, &dev_attr_fw_name);
error_irq_free:
	free_irq(client->irq, ts);
error_mutex_destroy:
	mutex_destroy(&ts->sus_lock);
	input_free_device(ts->input);
error_free_reset_gpio:
	if (gpio_is_valid(ts->reset_gpio))
		gpio_free(ts->reset_gpio);
error_free_irq_gpio:
	if (gpio_is_valid(ts->irq_gpio))
		gpio_free(ts->irq_gpio);
error_fw_name:
	gsl_power_on(ts, false);
error_pwr_deinit:
	gsl_power_init(ts, false);
error_free_ts:
	kfree(ts);
	return rc;
}

static int gsl_ts_remove(struct i2c_client *client)
{
	struct gsl_ts *ts = i2c_get_clientdata(client);
	GSL_INFO(KERN_INFO "==gsl_ts_remove=\n");

#if defined(CONFIG_FB)
	if (fb_unregister_client(&ts->fb_notif))
		dev_err(&client->dev,
			"Error occurred while unregistering fb_notifier.\n");
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&ts->early_suspend);
#endif

	device_init_wakeup(&client->dev, 0);
	cancel_work_sync(&ts->work);
	free_irq(ts->irq, ts);
	destroy_workqueue(ts->wq);
	input_unregister_device(ts->input);
	mutex_destroy(&ts->sus_lock);

	gsl_power_on(ts, false);
	gsl_power_init(ts, false);

	kfree(ts->touch_data);
	kfree(ts);

	return 0;
}

static const struct i2c_device_id gsl_ts_id[] = {
	{GSLX680_I2C_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, gsl_ts_id);
#ifdef CONFIG_OF
static struct of_device_id gsl2688_match_table[] = {
	{.compatible = "gsl,2688",},
	{},
};
#else
#define gsl2688_match_table NULL
#endif

static struct i2c_driver gsl_ts_driver = {
	.driver = {
		   .name = GSLX680_I2C_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = gsl2688_match_table,
#ifdef CONFIG_PM
		   .pm = &gsl2680_ts_pm_ops,
#endif
		   },
	.probe = gsl_ts_probe,
	.remove = gsl_ts_remove,
	.id_table = gsl_ts_id,
};

static int __init gsl_ts_init(void)
{
	int ret;
	GSL_INFO(KERN_INFO "gsl init\n");
	ret = i2c_add_driver(&gsl_ts_driver);
	if (ret)
		GSL_INFO(KERN_INFO "i2c add driver failed\n");

	return ret;
}

static void __exit gsl_ts_exit(void)
{
	GSL_INFO(KERN_INFO "==gsl_ts_exit==\n");
	i2c_del_driver(&gsl_ts_driver);
	return;
}

module_init(gsl_ts_init);
module_exit(gsl_ts_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Sileadinc touchscreen controller driver");