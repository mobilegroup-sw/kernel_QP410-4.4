/*
 * Ethernet driver for the WIZnet W5100/W5200/W5500 chip.
 *
 * Copyright (C) 2016 Akinobu Mita <akinobu.mita@gmail.com>
 *
 * Licensed under the GPL-2 or later.
 *
 * Datasheet:
 * http://www.wiznet.co.kr/wp-content/uploads/wiznethome/Chip/W5100/Document/W5100_Datasheet_v1.2.6.pdf
 * http://wiznethome.cafe24.com/wp-content/uploads/wiznethome/Chip/W5200/Documents/W5200_DS_V140E.pdf
 * http://wizwiki.net/wiki/lib/exe/fetch.php?media=products:w5500:w5500_ds_v106e_141230.pdf
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>
#include <linux/regulator/consumer.h>

#include "w5100.h"

#define W5100_SPI_WRITE_OPCODE 0xf0
#define W5100_SPI_READ_OPCODE 0x0f

static struct w5100_ops *ops;

static int w5100_spi_read(struct net_device *ndev, u32 addr)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[3] = { W5100_SPI_READ_OPCODE, addr >> 8, addr & 0xff };
	u8 data;
	int ret;

	ret = spi_write_then_read(spi, cmd, sizeof(cmd), &data, 1);

	return ret ? ret : data;
}

static int w5100_spi_write(struct net_device *ndev, u32 addr, u8 data)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[4] = { W5100_SPI_WRITE_OPCODE, addr >> 8, addr & 0xff, data};

	return spi_write_then_read(spi, cmd, sizeof(cmd), NULL, 0);
}

static int w5100_spi_read16(struct net_device *ndev, u32 addr)
{
	u16 data;
	int ret;

	ret = w5100_spi_read(ndev, addr);
	if (ret < 0)
		return ret;
	data = ret << 8;
	ret = w5100_spi_read(ndev, addr + 1);

	return ret < 0 ? ret : data | ret;
}

static int w5100_spi_write16(struct net_device *ndev, u32 addr, u16 data)
{
	int ret;

	ret = w5100_spi_write(ndev, addr, data >> 8);
	if (ret)
		return ret;

	return w5100_spi_write(ndev, addr + 1, data & 0xff);
}

static int w5100_spi_readbulk(struct net_device *ndev, u32 addr, u8 *buf,
			      int len)
{
	int i;

	for (i = 0; i < len; i++) {
		int ret = w5100_spi_read(ndev, addr + i);

		if (ret < 0)
			return ret;
		buf[i] = ret;
	}

	return 0;
}

static int w5100_spi_writebulk(struct net_device *ndev, u32 addr, const u8 *buf,
			       int len)
{
	int i;

	for (i = 0; i < len; i++) {
		int ret = w5100_spi_write(ndev, addr + i, buf[i]);

		if (ret)
			return ret;
	}

	return 0;
}

static struct w5100_ops w5100_spi_ops = {
	.may_sleep = true,
	.chip_id = W5100,
	.read = w5100_spi_read,
	.write = w5100_spi_write,
	.read16 = w5100_spi_read16,
	.write16 = w5100_spi_write16,
	.readbulk = w5100_spi_readbulk,
	.writebulk = w5100_spi_writebulk,
};

#define W5200_SPI_WRITE_OPCODE 0x80

struct w5200_spi_priv {
	/* Serialize access to cmd_buf */
	struct mutex cmd_lock;

	/* DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	u8 cmd_buf[4] ____cacheline_aligned;
};

static struct w5200_spi_priv *w5200_spi_priv(struct net_device *ndev)
{
	return w5100_ops_priv(ndev);
}

static int w5200_spi_init(struct net_device *ndev)
{
	struct w5200_spi_priv *spi_priv = w5200_spi_priv(ndev);

	mutex_init(&spi_priv->cmd_lock);

	return 0;
}

static int w5200_spi_read(struct net_device *ndev, u32 addr)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[4] = { addr >> 8, addr & 0xff, 0, 1 };
	u8 data;
	int ret;

	ret = spi_write_then_read(spi, cmd, sizeof(cmd), &data, 1);

	return ret ? ret : data;
}

static int w5200_spi_write(struct net_device *ndev, u32 addr, u8 data)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[5] = { addr >> 8, addr & 0xff, W5200_SPI_WRITE_OPCODE, 1, data };

	return spi_write_then_read(spi, cmd, sizeof(cmd), NULL, 0);
}

static int w5200_spi_read16(struct net_device *ndev, u32 addr)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[4] = { addr >> 8, addr & 0xff, 0, 2 };
	__be16 data;
	int ret;

	ret = spi_write_then_read(spi, cmd, sizeof(cmd), &data, sizeof(data));

	return ret ? ret : be16_to_cpu(data);
}

static int w5200_spi_write16(struct net_device *ndev, u32 addr, u16 data)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[6] = {
		addr >> 8, addr & 0xff,
		W5200_SPI_WRITE_OPCODE, 2,
		data >> 8, data & 0xff
	};

	return spi_write_then_read(spi, cmd, sizeof(cmd), NULL, 0);
}

static int w5200_spi_readbulk(struct net_device *ndev, u32 addr, u8 *buf,
			      int len)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	struct w5200_spi_priv *spi_priv = w5200_spi_priv(ndev);
	struct spi_transfer xfer[] = {
		{
			.tx_buf = spi_priv->cmd_buf,
			.len = sizeof(spi_priv->cmd_buf),
		},
		{
			.rx_buf = buf,
			.len = len,
		},
	};
	int ret;

	mutex_lock(&spi_priv->cmd_lock);

	spi_priv->cmd_buf[0] = addr >> 8;
	spi_priv->cmd_buf[1] = addr;
	spi_priv->cmd_buf[2] = len >> 8;
	spi_priv->cmd_buf[3] = len;
	ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));

	mutex_unlock(&spi_priv->cmd_lock);

	return ret;
}

static int w5200_spi_writebulk(struct net_device *ndev, u32 addr, const u8 *buf,
			       int len)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	struct w5200_spi_priv *spi_priv = w5200_spi_priv(ndev);
	struct spi_transfer xfer[] = {
		{
			.tx_buf = spi_priv->cmd_buf,
			.len = sizeof(spi_priv->cmd_buf),
		},
		{
			.tx_buf = buf,
			.len = len,
		},
	};
	int ret;

	mutex_lock(&spi_priv->cmd_lock);

	spi_priv->cmd_buf[0] = addr >> 8;
	spi_priv->cmd_buf[1] = addr;
	spi_priv->cmd_buf[2] = W5200_SPI_WRITE_OPCODE | (len >> 8);
	spi_priv->cmd_buf[3] = len;
	ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));

	mutex_unlock(&spi_priv->cmd_lock);

	return ret;
}

static struct w5100_ops w5200_ops = {
	.may_sleep = true,
	.chip_id = W5200,
	.read = w5200_spi_read,
	.write = w5200_spi_write,
	.read16 = w5200_spi_read16,
	.write16 = w5200_spi_write16,
	.readbulk = w5200_spi_readbulk,
	.writebulk = w5200_spi_writebulk,
	.init = w5200_spi_init,
};

#define W5500_SPI_BLOCK_SELECT(addr) (((addr) >> 16) & 0x1f)
#define W5500_SPI_READ_CONTROL(addr) (W5500_SPI_BLOCK_SELECT(addr) << 3)
#define W5500_SPI_WRITE_CONTROL(addr)	\
	((W5500_SPI_BLOCK_SELECT(addr) << 3) | BIT(2))

struct w5500_spi_priv {
	/* Serialize access to cmd_buf */
	struct mutex cmd_lock;

	/* DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	u8 cmd_buf[3] ____cacheline_aligned;
};

static struct w5500_spi_priv *w5500_spi_priv(struct net_device *ndev)
{
	return w5100_ops_priv(ndev);
}

static int w5500_spi_init(struct net_device *ndev)
{
	struct w5500_spi_priv *spi_priv = w5500_spi_priv(ndev);

	mutex_init(&spi_priv->cmd_lock);

	return 0;
}

static int w5500_spi_read(struct net_device *ndev, u32 addr)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[3] = {
		addr >> 8,
		addr,
		W5500_SPI_READ_CONTROL(addr)
	};
	u8 data;
	int ret;

	ret = spi_write_then_read(spi, cmd, sizeof(cmd), &data, 1);

	return ret ? ret : data;
}

static int w5500_spi_write(struct net_device *ndev, u32 addr, u8 data)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[4] = {
		addr >> 8,
		addr,
		W5500_SPI_WRITE_CONTROL(addr),
		data
	};

	return spi_write_then_read(spi, cmd, sizeof(cmd), NULL, 0);
}

static int w5500_spi_read16(struct net_device *ndev, u32 addr)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[3] = {
		addr >> 8,
		addr,
		W5500_SPI_READ_CONTROL(addr)
	};
	__be16 data;
	int ret;

	ret = spi_write_then_read(spi, cmd, sizeof(cmd), &data, sizeof(data));

	return ret ? ret : be16_to_cpu(data);
}

static int w5500_spi_write16(struct net_device *ndev, u32 addr, u16 data)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	u8 cmd[5] = {
		addr >> 8,
		addr,
		W5500_SPI_WRITE_CONTROL(addr),
		data >> 8,
		data
	};

	return spi_write_then_read(spi, cmd, sizeof(cmd), NULL, 0);
}

static int w5500_spi_readbulk(struct net_device *ndev, u32 addr, u8 *buf,
			      int len)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	struct w5500_spi_priv *spi_priv = w5500_spi_priv(ndev);
	struct spi_transfer xfer[] = {
		{
			.tx_buf = spi_priv->cmd_buf,
			.len = sizeof(spi_priv->cmd_buf),
		},
		{
			.rx_buf = buf,
			.len = len,
		},
	};
	int ret;

	mutex_lock(&spi_priv->cmd_lock);

	spi_priv->cmd_buf[0] = addr >> 8;
	spi_priv->cmd_buf[1] = addr;
	spi_priv->cmd_buf[2] = W5500_SPI_READ_CONTROL(addr);
	ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));

	mutex_unlock(&spi_priv->cmd_lock);

	return ret;
}

static int w5500_spi_writebulk(struct net_device *ndev, u32 addr, const u8 *buf,
			       int len)
{
	struct spi_device *spi = to_spi_device(ndev->dev.parent);
	struct w5500_spi_priv *spi_priv = w5500_spi_priv(ndev);
	struct spi_transfer xfer[] = {
		{
			.tx_buf = spi_priv->cmd_buf,
			.len = sizeof(spi_priv->cmd_buf),
		},
		{
			.tx_buf = buf,
			.len = len,
		},
	};
	int ret;

	mutex_lock(&spi_priv->cmd_lock);

	spi_priv->cmd_buf[0] = addr >> 8;
	spi_priv->cmd_buf[1] = addr;
	spi_priv->cmd_buf[2] = W5500_SPI_WRITE_CONTROL(addr);
	ret = spi_sync_transfer(spi, xfer, ARRAY_SIZE(xfer));

	mutex_unlock(&spi_priv->cmd_lock);

	return ret;
}
static int g_rst;
static int w5500_spi_reset(struct net_device *ndev)
{
	gpio_direction_output(g_rst, 1);
	mdelay(100);
	gpio_direction_output(g_rst, 0);
	mdelay(200);
	gpio_direction_output(g_rst, 1);
	mdelay(100);

	return 0;
}

static struct w5100_ops w5500_ops = {
	.may_sleep = true,
	.chip_id = W5500,
	.read = w5500_spi_read,
	.write = w5500_spi_write,
	.read16 = w5500_spi_read16,
	.write16 = w5500_spi_write16,
	.readbulk = w5500_spi_readbulk,
	.writebulk = w5500_spi_writebulk,
	.init = w5500_spi_init,
	.reset = w5500_spi_reset,
};


static int w5500_parse_dt(struct spi_device *spi, struct w5100_ops *ops)
{
	int ret;
	enum of_gpio_flags flags;
	struct device_node *dnode = spi->dev.of_node;

	if (!dnode) {
		pr_err("w5500 dt is null\n");
		return -1;
	}

	ops->vdd_en = devm_regulator_get(&spi->dev, "vdd-en");
	if (IS_ERR(ops->vdd_en)) {
		ret = PTR_ERR(ops->vdd_en);
		goto fail_vdd_en;
	}
	ret = regulator_enable(ops->vdd_en);
	if (ret) {
		pr_err("w5500 regulator_enable vdd_en fail\n");
		goto fail_vdd_en2;
	}

	ops->en_gpio = of_get_named_gpio_flags(dnode, "w5500,gpio-en",0, &flags);
	if (gpio_is_valid(ops->en_gpio)) {
		ret = gpio_request(ops->en_gpio, "wz-en");
		if (ret) {
			pr_err("w5500 en_gpio failed: %d\n", ret);
			return ret;
		}
		pr_err("w5500 gpio_direction_output en_gpio %d\n", ops->en_gpio);
		gpio_direction_output(ops->en_gpio, 1);
	}

	ops->level_gpio = of_get_named_gpio_flags(dnode, "w5500,gpio-level",0, &flags);
	if (gpio_is_valid(ops->level_gpio)) {
		ret = gpio_request(ops->level_gpio, "wz-level");
		if (ret) {
			pr_err("w5500 level_gpio failed: %d\n", ret);
			return ret;
		}
		pr_err("w5500 gpio_direction_output level_gpio %d\n", ops->level_gpio);
		gpio_direction_output(ops->level_gpio, 1);
	}

	usleep_range(10000, 11000);

	ops->rst_gpio = of_get_named_gpio_flags(dnode, "w5500,gpio-rest",0, &flags);
	if (gpio_is_valid(ops->rst_gpio)) {
		ret = gpio_request(ops->rst_gpio, "wz-rst");
		if (ret) {
			pr_err("w5500 en_gpio failed: %d\n", ret);
			return ret;
		}
		pr_err("w5500 gpio_direction_output rst_gpio %d\n", ops->rst_gpio);
		gpio_direction_output(ops->rst_gpio, 1);
	}
	else {
		pr_err("w5500 fail_rst_gpio\n");
		goto fail_rst_gpio;
	}

        ops->power_gpio = of_get_named_gpio_flags(dnode, "w5500,gpio-en-3p3v",0, &flags);
	if (gpio_is_valid(ops->power_gpio)) {
		ret = gpio_request(ops->power_gpio, "wz-3p3");
		if (ret) {
			pr_err("w5500 wz-3p3 failed: %d\n", ret);
			return ret;		
		}		
		pr_err("w5500 gpio_direction_output 3p3v_gpio\n");
		gpio_direction_output(ops->power_gpio, 1);	
	}
	else {
		pr_err("w5500 power_gpio\n");
		goto fail_rst_gpio;
	}


	ops->link_gpio = of_get_named_gpio_flags(dnode, "w5500,gpio-link",0, &flags);

	ops->link_pinctrl = devm_pinctrl_get(&(spi->dev));
	if (IS_ERR_OR_NULL(ops->link_pinctrl)) {
		ret = PTR_ERR(ops->link_pinctrl);
		printk("Target does not use pinctrl %d\n", ret);
		return ret;
	}

	ops->pinctrl_state_default
		= pinctrl_lookup_state(ops->link_pinctrl,
				"link_pin_init");
	if (IS_ERR_OR_NULL(ops->pinctrl_state_default)) {
		ret = PTR_ERR(ops->pinctrl_state_default);
		printk("default state err: %d\n", ret);
		return ret;
	}

	ret = pinctrl_select_state(ops->link_pinctrl,ops->pinctrl_state_default);
	if (ret){
		printk("set state err: %d\n", ret);
		return ret;
	}

	g_rst = ops->rst_gpio;

	return 0;
fail_rst_gpio:
	if (gpio_is_valid(ops->rst_gpio))
		gpio_free(ops->rst_gpio);
fail_vdd_en2:
	if (!IS_ERR(ops->vdd_en)) {
		regulator_put(ops->vdd_en);
	}
fail_vdd_en:
	return -1;
}

static int w5100_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *id = spi_get_device_id(spi);
	int priv_size;
	const void *mac = of_get_mac_address(spi->dev.of_node);

	switch (id->driver_data) {
	case W5100:
		ops = &w5100_spi_ops;
		priv_size = 0;
		break;
	case W5200:
		ops = &w5200_ops;
		priv_size = sizeof(struct w5200_spi_priv);
		break;
	case W5500:
		ops = &w5500_ops;
		priv_size = sizeof(struct w5500_spi_priv);
		break;
	default:
		return -EINVAL;
	}

	w5500_parse_dt(spi, ops);

	return w5100_probe(&spi->dev, ops, priv_size, mac, spi->irq, ops->link_gpio, ops->en_gpio, ops->level_gpio);
}

static int w5100_spi_remove(struct spi_device *spi)
{
	if (gpio_is_valid(ops->rst_gpio))
		gpio_free(ops->rst_gpio);
	if (gpio_is_valid(ops->en_gpio))
		gpio_free(ops->en_gpio);
	if (!IS_ERR(ops->vdd_en)) {
		regulator_disable(ops->vdd_en);
		regulator_put(ops->vdd_en);
	}
	return w5100_remove(&spi->dev);
}

static const struct spi_device_id w5100_spi_ids[] = {
	{ "w5100", W5100 },
	{ "w5200", W5200 },
	{ "w5500", W5500 },
	{}
};
MODULE_DEVICE_TABLE(spi, w5100_spi_ids);

static struct of_device_id w5x00_match_table[] = {
	{ .compatible = "wiznet,w5500", },
	{}
};

static struct spi_driver w5100_spi_driver = {
	.driver		= {
		.name	= "w5500",
		.pm	= &w5100_pm_ops,
		.of_match_table = w5x00_match_table,
	},
	.probe		= w5100_spi_probe,
	.remove		= w5100_spi_remove,
	.id_table	= w5100_spi_ids,
};
module_spi_driver(w5100_spi_driver);

MODULE_DESCRIPTION("WIZnet W5100/W5200/W5500 Ethernet driver for SPI mode");
MODULE_AUTHOR("Akinobu Mita <akinobu.mita@gmail.com>");
MODULE_LICENSE("GPL");


