/*
 * Driver for WCH PHY chip CH182.
 *
 * Copyright (C) 2025 Nanjing Qinheng Microelectronics Co., Ltd.
 * Web: http://wch.cn
 * Author: WCH <tech@wch.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Update Log:
 * V1.0 - initial version
 * V1.1 - add soft reset
 * V1.2 - add anti-interference monitoring function
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/atomic.h>
#include <linux/delay.h>

#define DRIVER_AUTHOR "WCH"
#define DRIVER_DESC   "WCH phy driver for CH182, etc."
#define VERSION_DESC  "V1.2 On 2025.12"

/* WCH PHY ID */
#define	PHY_ID_CH182D				0x737190D0

#define	PHY_ID_CH182F2				0x73719020
#define	PHY_ID_CH182F7				0x73719070
#define	PHY_ID_CH182F8				0x73719080

#define	PHY_ID_CH182H1				0x73719110
#define	PHY_ID_CH182H2				0x73719120
#define	PHY_ID_CH182H3				0x73719130
#define	PHY_ID_CH182H6				0x73719160
#define	PHY_ID_CH182H7				0x73719170
#define	PHY_ID_CH182H8				0x73719180

#define WCH_PHY_ID_MASK				0xfffffff0

/* Operation Mode Strap Override */
#define PHY_PAG_SEL					0x1F

/*  Extended register */
#define PHY_PAG0					0x00
#define CH182_PHY_STATUS0			0x10
#define CH182_POLARITY_STATE		BIT(12)
#define CH182_DESCR_LOCK_STATE		BIT(9)
#define CH182_LINK_STATE			BIT(0)
#define CH182_FLASE_CAR_CNT			0x14
#define CH182_PCSR_100M_CTL			0x16
#define CH182_FORCE_100_OK			BIT(5)
#define CH182_PWR_SAVE				0x18
#define CH182_PWR_ENPWR_SAVE		BIT(15)
#define CH182_PWR_RST_OK			BIT(1)
#define CH182_PHYCR					0x1C
#define CH182_REPEATER_MODE			BIT(13)
#define CH182_INTERRUPT_IND			0x1E

#define PHY_PAG1					0x01
#define CH182_RX_CNT_CTRL			0x17
#define CH182_RX_CNT_EN				BIT(12)
#define CH182_RX_CRCERR_CNT			0x18

#define PHY_PAG7					0x07
#define CH182_CUST_LED_SET			0x11
#define CH182_INTERRUPT_MASK		0x13
#define CH182_INT_LINKCHG			BIT(13)
#define CH182_CUSTOMIZED_LED		BIT(3)

#define PHY_PAG99					0x63

#define CH182_LINK_UP_INTVAL_MS 	200
#define CH182_LINK_DOWN_INTVAL_MS 	300

struct ch182_led_type {
	u32 led_mode_reg;
	bool has_traditional_led;
	bool has_custom_led;
};

/* led_tmode: traditional led mode. 
 * led0_mode/led1_mode: custom led mode.
 */
struct ch182_priv {
	struct phy_device *phydev;
	const struct ch182_led_type *type;
	int led_tmode, led0_mode, led1_mode;
	atomic_t page_count;
	bool has_valid_led_values;
	bool check_flag;
	struct workqueue_struct *ch182_check;
	struct delayed_work linkdw_work, linkup_work;
};

static const struct ch182_led_type ch182h8_led_type = {
	.led_mode_reg		= CH182_INTERRUPT_MASK,
	.has_traditional_led = true,
	.has_custom_led	= true,
};

static int ch182_setup_led_tmode(struct phy_device *phydev)
{
	struct ch182_priv *priv = phydev->priv;
	u32 reg;
	int val, ret, temp;

	reg = priv->type->led_mode_reg;
	val = priv->led_tmode;

	ret = phy_write(phydev, PHY_PAG_SEL, PHY_PAG7);
	if (ret < 0) {
		phydev_err(phydev, "failed to go PHY_PAG7\n");
		return ret;
	}

	temp = phy_read(phydev, reg);
	if (temp < 0) {
		ret = temp;
		goto out;
	}

	temp &= ~(0x3 << 4);
	temp |= (val & 0x3) << 4;

	ret = phy_write(phydev, reg, temp);

out:
	if (ret < 0)
		phydev_err(phydev, "failed to set led mode\n");

	return ret;
}

static int ch182_setup_led_cmode(struct phy_device *phydev)
{
	struct ch182_priv *priv = phydev->priv;
	u32 reg;
	int val_0, val_1, ret, temp;

	reg = CH182_CUST_LED_SET;
	val_0 = priv->led0_mode & 0x7;
	val_1 = (priv->led1_mode & 0x7) << 4;

	ret = phy_write(phydev, PHY_PAG_SEL, PHY_PAG7);
	if (ret < 0) {
		phydev_err(phydev, "failed to go PHY_PAG7\n");
		return ret;
	}

	temp = phy_read(phydev, reg);
	if (temp < 0) {
		ret = temp;
		goto out;
	}

	temp |= CH182_CUSTOMIZED_LED;

	phy_write(phydev, CH182_INTERRUPT_MASK, temp);

	temp = phy_read(phydev, reg);
	if (temp < 0) {
		ret = temp;
		goto out;
	}

	temp &= ~(0x77);
	temp |= (val_0 | val_1); 

	ret = phy_write(phydev, reg, temp);

out:
	if (ret < 0)
		phydev_err(phydev, "failed to set led mode\n");

	return ret;
}

static int ch182_init_rxcnt(struct phy_device *phydev)
{
	int val;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG1);

	val = phy_read(phydev, CH182_RX_CNT_CTRL);
	val |= CH182_RX_CNT_EN;
	phy_write(phydev, CH182_RX_CNT_CTRL, val);

	return 0;
}

static int ch182_setup_reptmod(struct phy_device *phydev)
{
	int val = 0, ret = 0;

	ret = phy_write(phydev, PHY_PAG_SEL, PHY_PAG0);
	if (ret < 0)
		phydev_err(phydev, "failed to go PHY_PAG0\n");

	val = phy_read(phydev, CH182_PHYCR);
	if (val < 0)
		phydev_err(phydev, "failed to read CH182_PHYCR\n");

	val |= CH182_REPEATER_MODE;

	ret = phy_write(phydev, CH182_PHYCR, val);
	if (ret < 0)
		phydev_err(phydev, "failed to write CH182_PHYCR\n");

	return ret;
}

static int ch182_config_init(struct phy_device *phydev)
{
	struct ch182_priv *priv = phydev->priv;
	const struct ch182_led_type *type;

	if (!priv)
		return 0;

	type = priv->type;

	if (priv->has_valid_led_values) {
		if (priv->led_tmode >= 0)
			ch182_setup_led_tmode(phydev);
		else
			ch182_setup_led_cmode(phydev);
	}

	ch182_init_rxcnt(phydev);
	ch182_setup_reptmod(phydev);

	return 0;
}

static void ch182_linkup_cfg(struct ch182_priv *priv)
{
	cancel_delayed_work_sync(&priv->linkdw_work);

	if (!delayed_work_pending(&priv->linkup_work) &&
		!work_busy(&priv->linkup_work.work))
		mod_delayed_work(priv->ch182_check, &priv->linkup_work, msecs_to_jiffies(CH182_LINK_UP_INTVAL_MS));

	atomic_set(&priv->page_count, 0);
}

static void ch182_linkdown_cfg(struct ch182_priv *priv)
{
	cancel_delayed_work_sync(&priv->linkup_work);

	if (!delayed_work_pending(&priv->linkdw_work) &&
		!work_busy(&priv->linkdw_work.work))
		mod_delayed_work(priv->ch182_check, &priv->linkdw_work, msecs_to_jiffies(CH182_LINK_DOWN_INTVAL_MS));
}

static void ch182_link_change(struct phy_device *phydev)
{
	struct ch182_priv *priv = phydev->priv;

	if (phydev->link)
		ch182_linkup_cfg(priv);
	else
		ch182_linkdown_cfg(priv);
}

static int ch182_soft_reset(struct phy_device *phydev)
{
	int ret = 0, val = 0;

	ret = phy_write(phydev, MII_BMCR, BMCR_RESET);
	if (ret < 0)
		return ret;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG0);
	val = phy_read(phydev, CH182_PWR_SAVE);
	if (val & CH182_PWR_RST_OK)
		phy_write(phydev, MII_BMCR, 0x3100);

	return 0;
}

static void ch182_enable_link_interpt(struct phy_device *phydev)
{
	int phy_stat;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG7);

	phy_stat = phy_read(phydev, CH182_INTERRUPT_MASK);
	phy_stat |= CH182_INT_LINKCHG;
	phy_write(phydev, CH182_INTERRUPT_MASK, phy_stat);
}

static void ch182_disable_link_interpt(struct phy_device *phydev)
{
	int phy_stat;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG7);

	phy_stat = phy_read(phydev, CH182_INTERRUPT_MASK);
	phy_stat &= ~CH182_INT_LINKCHG;
	phy_write(phydev, CH182_INTERRUPT_MASK, phy_stat);
}

static void ch182_set_phypn(struct phy_device *phydev)
{
	int val = 0;

	ch182_disable_link_interpt(phydev);

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG0);

	/* write PCSR bit[5] ->> 1 */
	val = phy_read(phydev, CH182_PCSR_100M_CTL);
	val |= CH182_FORCE_100_OK;
	phy_write(phydev, CH182_PCSR_100M_CTL, val);

	/* write PCSR bit[5] ->> 0 */
	val = phy_read(phydev, CH182_PCSR_100M_CTL);
	val &= ~(CH182_FORCE_100_OK);
	phy_write(phydev, CH182_PCSR_100M_CTL, val);

	phy_read(phydev, CH182_INTERRUPT_IND);

	ch182_enable_link_interpt(phydev);

	return;
}

static bool ch182_check_base_reg(struct phy_device *phydev)
{
	int val = 0;

	val = phy_read(phydev, MII_EXPANSION);

	if ((val & 0x0001) == 0)
		return true;
	else
		return false;
}

static void ch182_check_stat(struct phy_device *phydev)
{
	int val = 0, count = 0, i;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG99);

	for (i = 5; i > 0; i--) {
		val = phy_read(phydev, 0x19);

		if (((val & 0x03e0) >> 5) == 0x0f)
			count++;
		else
			count = 0;

		msleep(5);
	}

	if (count == 5) {
		ch182_soft_reset(phydev);
	}

	return;
}

static void ch182_check_link(struct phy_device *phydev)
{
	int val = 0, count = 0, i;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG0);

	for (i = 5; i > 0; i--) {
		val = phy_read(phydev, CH182_PHY_STATUS0);

		if ((val & CH182_DESCR_LOCK_STATE) == 0)
			count++;
		else
			count = 0;

		msleep(5);
	}

	if (count == 5) {
		ch182_soft_reset(phydev);
	}

	return;
}

static bool ch182_check_page99(struct phy_device *phydev)
{
	struct ch182_priv *priv = phydev->priv;
	int val = 0;

	phy_write(phydev, PHY_PAG_SEL, PHY_PAG99);
	val = phy_read(phydev, 0x19);
	if ((val & 0x0f) == 0x0c) {
		atomic_inc(&priv->page_count);
	}

	if (atomic_read(&priv->page_count) == 5) {
		atomic_set(&priv->page_count, 0);
    	ch182_soft_reset(phydev);
    }

	if ((val & 0x0f) == 0x02)
		return false;

	return true;
}

static void ch182_check_phypn(struct phy_device *phydev, bool flag)
{
	int phy_stat;
	unsigned long time;

	if (flag)
		time = 300000;
	else 
		time = 200000;

	if (ch182_check_page99(phydev))
		return;

    phy_write(phydev, PHY_PAG_SEL, PHY_PAG0);
    phy_stat = phy_read(phydev, CH182_PHY_STATUS0);
    if (phy_stat < 0) {
        printk("%s, read CH182_PHY_STATUS0 fail: %d\n", __func__, phy_stat);
        return;
    }

    if (phy_stat & CH182_POLARITY_STATE) {
		usleep_range(time, (time + 10));
		if (ch182_check_base_reg(phydev) || !flag)
			ch182_set_phypn(phydev);
    }

	return;
}

static void ch182_link_processing(struct phy_device *phydev)
{
	int phy_bcr;

	phy_bcr = phy_read(phydev, MII_BMSR);

    if (phydev->link == 0) {
        phy_bcr = phy_read(phydev, MII_BMCR);
        if (phy_bcr < 0)
            return;

        if (phy_bcr & BMCR_ANENABLE) {
            ch182_check_phypn(phydev, true);
        } else {
            if ((phy_bcr & BMCR_SPEED100) == 0)
                ch182_check_phypn(phydev, false);
        }
    }
}

static bool ch182_check_link_stat(struct phy_device *phydev)
{
	int val = 0;

	val = phy_read(phydev, MII_BMSR);

	if (val & BMSR_LSTATUS)
		return true;
	else
		return false;
}

static void ch182_phy_linkdw_work(struct work_struct *work)
{
    struct ch182_priv *priv = container_of(to_delayed_work(work),
                       struct ch182_priv, linkdw_work);
    struct phy_device *phydev = priv->phydev;

    if (!priv->check_flag) {
        return;
    }

	if (!ch182_check_link_stat(phydev)) {
		ch182_link_processing(phydev);
        mod_delayed_work(priv->ch182_check, &priv->linkdw_work, msecs_to_jiffies(CH182_LINK_DOWN_INTVAL_MS));
    } else {
		atomic_set(&priv->page_count, 0);
    }
}

static bool ch182_check_link_speed(struct phy_device *phydev)
{
	int bmcr = 0;

	bmcr = phy_read(phydev, MII_BMCR);

	if (bmcr & BMCR_SPEED100)
		return true;
	else
		return false;
}

static void ch182_phy_linkup_work(struct work_struct *work)
{
    struct ch182_priv *priv = container_of(to_delayed_work(work),
                       struct ch182_priv, linkup_work);
    struct phy_device *phydev = priv->phydev;

    if (!priv->check_flag) {
        return;
    }

	if (ch182_check_link_speed(phydev)) {
		ch182_check_stat(phydev);
		ch182_check_link(phydev);
	}

	if (ch182_check_link_stat(phydev)) {
        mod_delayed_work(priv->ch182_check, &priv->linkup_work, msecs_to_jiffies(CH182_LINK_UP_INTVAL_MS));
    }
}

static int ch182_probe(struct phy_device *phydev)
{
	const struct ch182_led_type *type = phydev->drv->driver_data;
	const struct device_node *np = phydev->mdio.dev.of_node;
	struct ch182_priv *priv;
	u32 led_tmode = 0, led0_mode = 0, led1_mode = 0;
	bool has_led0 = false, has_led1 = false, has_tmode = false;
	int ret;

	printk(KERN_INFO "ch182 phy probe, driver version: %s\n", VERSION_DESC);

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;
	priv->phydev = phydev;
	priv->type = type;
	priv->has_valid_led_values = false;
	priv->led_tmode = -1;
	priv->check_flag = true;
	atomic_set(&priv->page_count, 0);

	priv->ch182_check = alloc_workqueue("ch182_check_wq", WQ_MEM_RECLAIM, 1);
	if (!priv->ch182_check) {
		phydev_err(phydev, "failed to create ch182_check workqueue\n");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&priv->linkdw_work, ch182_phy_linkdw_work);
	INIT_DELAYED_WORK(&priv->linkup_work, ch182_phy_linkup_work);

	/* Try custom LED mode first */
	if (type->led_mode_reg && type->has_custom_led) {
		ret = of_property_read_u32(np, "wch,led0-mode", &led0_mode);
		if (!ret)
			has_led0 = true;

		ret = of_property_read_u32(np, "wch,led1-mode", &led1_mode);
		if (!ret)
			has_led1 = true;

		if (has_led0 && has_led1 && led0_mode <= 7 && led1_mode <= 7) {
			priv->led0_mode = led0_mode;
			priv->led1_mode = led1_mode;
			priv->has_valid_led_values = true;
			goto out;
		} else if (has_led0 || has_led1) {
			phydev_err(phydev, "invalid custom LED values, led0: 0x%02x, led1: 0x%02x\n",
				led0_mode, led1_mode);
		}
	}

	/* Try traditional LED mode if custom is not valid */
	if (type->led_mode_reg && type->has_traditional_led) {
		ret = of_property_read_u32(np, "wch,traditional-led", &led_tmode);
		if (!ret)
			has_tmode = true;

		if (has_tmode && led_tmode <= 3) {
			priv->led_tmode = led_tmode;
			priv->has_valid_led_values = true;
		} else if (has_tmode) {
			phydev_err(phydev, "invalid traditional LED mode: 0x%02x\n", led_tmode);
		}
	}

out:
	return 0;
}

static void ch182_remove(struct phy_device *phydev)
{
	struct ch182_priv *priv = phydev->priv;

	priv->check_flag = false;
	cancel_delayed_work_sync(&priv->linkdw_work);
	cancel_delayed_work_sync(&priv->linkup_work);

	if (priv->ch182_check) {
		destroy_workqueue(priv->ch182_check);
		priv->ch182_check = NULL;
	}
}

static struct phy_driver ch182_driver[] = {
{
	.phy_id 		= PHY_ID_CH182D,
	.name 			= "WCH CH182D Ethernet",
	.phy_id_mask 	= WCH_PHY_ID_MASK,
	.driver_data   	= &ch182h8_led_type,
	.probe	   		= ch182_probe,
	.remove         = ch182_remove,
	.config_init   	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features 		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset		= ch182_soft_reset,
	.suspend   		= genphy_suspend,
	.resume 	   	= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182F2,
	.name			= "WCH CH182F2 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182F7,
	.name			= "WCH CH182F7 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182F8,
	.name			= "WCH CH182F8 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182H1,
	.name			= "WCH CH182H1 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182H2,
	.name			= "WCH CH182H2 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182H3,
	.name			= "WCH CH182H3 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182H6,
	.name			= "WCH CH182H6 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182H7,
	.name			= "WCH CH182H7 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
}, {
	.phy_id 		= PHY_ID_CH182H8,
	.name			= "WCH CH182H8 Ethernet",
	.phy_id_mask	= WCH_PHY_ID_MASK,
	.driver_data	= &ch182h8_led_type,
	.probe			= ch182_probe,
	.remove         = ch182_remove,
	.config_init	= ch182_config_init,
	.link_change_notify = ch182_link_change,
	.features		= PHY_BASIC_FEATURES,
	.config_aneg	= &genphy_config_aneg,
	.read_status	= &genphy_read_status,
	.soft_reset 	= ch182_soft_reset,
	.suspend		= genphy_suspend,
	.resume 		= genphy_resume,
} };

module_phy_driver(ch182_driver);

static struct mdio_device_id __maybe_unused wch_tbl[] = {
	{ PHY_ID_CH182D, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182F2, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182F7, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182F8, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182H1, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182H2, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182H3, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182H6, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182H7, WCH_PHY_ID_MASK },
	{ PHY_ID_CH182H8, WCH_PHY_ID_MASK },
	{ }
};

MODULE_DEVICE_TABLE(mdio, wch_tbl);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(VERSION_DESC);
MODULE_LICENSE("GPL");

