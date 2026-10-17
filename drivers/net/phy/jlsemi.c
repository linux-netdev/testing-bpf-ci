// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for JLSemi Gigabit Ethernet PHYs (JL2121 / JL2101)
 *
 * Copyright (C) 2026 Pufan Jin <cyan2254650260@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/ethtool.h>
#include <linux/ethtool_netlink.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/pm_wakeirq.h>
#include <linux/property.h>

#define PHY_ID_JL2101			0x937c4032
#define PHY_ID_JL2121			0x937c4030
#define PHY_ID_JLSEMI			0x937c4000
#define JLSEMI_PAGE_SELECT		0x1f

/* Status page and register */
#define JLSEMI_STATUS_PAGE		0xa43
#define JLSEMI_PHYSR			0x1a
#define JLSEMI_PHYSR_DUPLEX		BIT(3)
#define JLSEMI_PHYSR_SPEED_MASK		GENMASK(5, 4)
#define JLSEMI_PHYSR_SPEED_1000		0x20
#define JLSEMI_PHYSR_SPEED_100		0x10
#define JLSEMI_PHYSR_SPEED_10		0x00
#define JLSEMI_PHYSR_MASTER		BIT(11)

/* RGMII delay page and registers */
#define JLSEMI_RGMII_PAGE		0xd08
#define JLSEMI_TXCR			0x11
#define JLSEMI_TX_DELAY			BIT(8)
#define JLSEMI_RXCR			0x15
#define JLSEMI_RX_DELAY			BIT(3)

/* Power and EEE configuration */
#define JLSEMI_PHYCR2			0x19
#define JLSEMI_PHYCR2_PHY_EEE_ENABLE	BIT(5)

/* Interrupt page and registers */
#define JLSEMI_INSR			0x1d
#define JLSEMI_INER_PAGE		0xa42
#define JLSEMI_INER			0x12
#define JLSEMI_INER_LINK_STATUS		BIT(4)
#define JLSEMI_INER_PME			BIT(7)

/* Wake-on-LAN settings */
#define JLSEMI_WOL_PAGE			0xd8a
#define JLSEMI_WOL_SETTINGS_EVENTS	16
#define JLSEMI_WOL_EVENT_MAGIC		BIT(12)
#define JLSEMI_WOL_RST_RMSQ		17
#define JLSEMI_WOL_RG_RSTB		BIT(15)
#define JLSEMI_WOL_RMSQ			0x1fff

/* Physical MAC address for WoL */
#define JLSEMI_PHYSICAL_ADDR_PAGE	0xd8c
#define JLSEMI_PHYSICAL_ADDR_WORD0	16
#define JLSEMI_PHYSICAL_ADDR_WORD1	17
#define JLSEMI_PHYSICAL_ADDR_WORD2	18

/* LED control */
#define JLSEMI_LED_COUNT		3
#define JLSEMI_LEDCR_PAGE		0xd04
#define JLSEMI_LEDCR			0x10
#define JLSEMI_LEDCR_MODE		BIT(15)
#define JLSEMI_LEDCR_ACT_TXRX		BIT(4)
#define JLSEMI_LEDCR_LINK_1000		BIT(3)
#define JLSEMI_LEDCR_LINK_100		BIT(1)
#define JLSEMI_LEDCR_LINK_10		BIT(0)
#define JLSEMI_LEDCR_MASK		GENMASK(4, 0)
#define JLSEMI_LEDCR_SHIFT		5

struct jlsemi_priv {
	u16 iner;
};

static int jlsemi_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, JLSEMI_PAGE_SELECT);
}

static int jlsemi_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, JLSEMI_PAGE_SELECT, page);
}

static int jlsemi_probe(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	struct jlsemi_priv *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	if (device_property_read_bool(dev, "wakeup-source") &&
	    phy_interrupt_is_valid(phydev)) {
		device_set_wakeup_capable(dev, true);
		devm_pm_set_wake_irq(dev, phydev->irq);
	}
	return 0;
}

static int jlsemi_config_rgmii_delay(struct phy_device *phydev)
{
	u16 val_txdly = 0, val_rxdly = 0;
	s32 rx_delay = -1, tx_delay = -1;
	int ret;

	if (!phy_interface_is_rgmii(phydev))
		return 0;

	if (phydev->interface != PHY_INTERFACE_MODE_RGMII_TXID)
		rx_delay = phy_get_internal_delay(phydev, NULL, 0, true);

	if (phydev->interface != PHY_INTERFACE_MODE_RGMII_RXID)
		tx_delay = phy_get_internal_delay(phydev, NULL, 0, false);

	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		val_txdly = 0;
		val_rxdly = 0;
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		val_txdly = JLSEMI_TX_DELAY;
		val_rxdly = JLSEMI_RX_DELAY;
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		val_txdly = 0;
		val_rxdly = JLSEMI_RX_DELAY;
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		val_txdly = JLSEMI_TX_DELAY;
		val_rxdly = 0;
		break;
	default:
		return 0;
	}

	if (rx_delay >= 0)
		val_rxdly = rx_delay > 0 ? JLSEMI_RX_DELAY : 0;
	if (tx_delay >= 0)
		val_txdly = tx_delay > 0 ? JLSEMI_TX_DELAY : 0;

	ret = phy_modify_paged_changed(phydev, JLSEMI_RGMII_PAGE,
				       JLSEMI_TXCR, JLSEMI_TX_DELAY, val_txdly);
	if (ret < 0)
		return ret;

	return phy_modify_paged_changed(phydev, JLSEMI_RGMII_PAGE,
					JLSEMI_RXCR, JLSEMI_RX_DELAY,
					val_rxdly);
}

static int jlsemi_disable_autonomous_eee(struct phy_device *phydev)
{
	return phy_modify(phydev, JLSEMI_PHYCR2,
				JLSEMI_PHYCR2_PHY_EEE_ENABLE, 0);
}

static int jlsemi_config_init(struct phy_device *phydev)
{
	int ret;

	ret = jlsemi_disable_autonomous_eee(phydev);
	if (ret < 0)
		return ret;

	return jlsemi_config_rgmii_delay(phydev);
}

static int jlsemi_ack_interrupt(struct phy_device *phydev)
{
	int err = phy_read(phydev, JLSEMI_INSR);

	return (err < 0) ? err : 0;
}

static int jlsemi_config_intr(struct phy_device *phydev)
{
	struct jlsemi_priv *priv = phydev->priv;
	int err;
	u16 val;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		err = jlsemi_ack_interrupt(phydev);
		if (err)
			return err;

		val = JLSEMI_INER_LINK_STATUS;
		err = phy_write_paged(phydev, JLSEMI_INER_PAGE, JLSEMI_INER, val);
		if (!err)
			priv->iner = val;
	} else {
		val = 0;
		err = phy_write_paged(phydev, JLSEMI_INER_PAGE, JLSEMI_INER, val);
		if (!err)
			priv->iner = val;

		err = jlsemi_ack_interrupt(phydev);
	}

	return err;
}

static irqreturn_t jlsemi_handle_interrupt(struct phy_device *phydev)
{
	int irq_status;

	irq_status = phy_read(phydev, JLSEMI_INSR);
	if (irq_status < 0) {
		phy_error(phydev);
		return IRQ_NONE;
	}

	if (irq_status & JLSEMI_INER_LINK_STATUS) {
		phy_trigger_machine(phydev);
		return IRQ_HANDLED;
	}

	if (irq_status & JLSEMI_INER_PME) {
		pm_wakeup_event(&phydev->mdio.dev, 0);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int jlsemi_read_status(struct phy_device *phydev)
{
	int err, val;

	err = genphy_read_status(phydev);
	if (err < 0 || !phydev->link)
		return err;

	val = phy_read_paged(phydev, JLSEMI_STATUS_PAGE, JLSEMI_PHYSR);
	if (val < 0)
		return val;

	if (val & JLSEMI_PHYSR_DUPLEX)
		phydev->duplex = DUPLEX_FULL;
	else
		phydev->duplex = DUPLEX_HALF;

	switch (val & JLSEMI_PHYSR_SPEED_MASK) {
	case JLSEMI_PHYSR_SPEED_1000:
		phydev->speed = SPEED_1000;
		break;
	case JLSEMI_PHYSR_SPEED_100:
		phydev->speed = SPEED_100;
		break;
	case JLSEMI_PHYSR_SPEED_10:
		phydev->speed = SPEED_10;
		break;
	default:
		phydev->speed = SPEED_UNKNOWN;
		break;
	}

	if (phydev->speed == SPEED_1000) {
		if (val & JLSEMI_PHYSR_MASTER)
			phydev->master_slave_state = MASTER_SLAVE_STATE_MASTER;
		else
			phydev->master_slave_state = MASTER_SLAVE_STATE_SLAVE;
	} else {
		phydev->master_slave_state = MASTER_SLAVE_STATE_UNSUPPORTED;
	}

	return 0;
}

static void jlsemi_get_wol(struct phy_device *phydev, struct ethtool_wolinfo *wol)
{
	int val;

	if (!device_can_wakeup(&phydev->mdio.dev)) {
		wol->supported = 0;
		wol->wolopts = 0;
		return;
	}

	wol->supported = WAKE_MAGIC;
	wol->wolopts = 0;

	val = phy_read_paged(phydev, JLSEMI_WOL_PAGE, JLSEMI_WOL_SETTINGS_EVENTS);
	if (val < 0)
		return;

	if (val & JLSEMI_WOL_EVENT_MAGIC)
		wol->wolopts |= WAKE_MAGIC;
}

static int jlsemi_set_wol(struct phy_device *phydev, struct ethtool_wolinfo *wol)
{
	const u8 *mac_addr;
	int oldpage, ret;

	if (!device_can_wakeup(&phydev->mdio.dev))
		return -EOPNOTSUPP;

	if (wol->wolopts & ~WAKE_MAGIC)
		return -EINVAL;

	if (wol->wolopts & WAKE_MAGIC) {
		if (!phydev->attached_dev)
			return -EINVAL;

		mac_addr = phydev->attached_dev->dev_addr;

		oldpage = phy_select_page(phydev, JLSEMI_PHYSICAL_ADDR_PAGE);
		if (oldpage < 0)
			return oldpage;

		__phy_write(phydev, JLSEMI_PHYSICAL_ADDR_WORD0,
			    mac_addr[1] << 8 | mac_addr[0]);
		__phy_write(phydev, JLSEMI_PHYSICAL_ADDR_WORD1,
			    mac_addr[3] << 8 | mac_addr[2]);
		__phy_write(phydev, JLSEMI_PHYSICAL_ADDR_WORD2,
			    mac_addr[5] << 8 | mac_addr[4]);

		ret = __phy_write(phydev, JLSEMI_PAGE_SELECT, JLSEMI_WOL_PAGE);
		if (ret < 0)
			goto restore;

		__phy_write(phydev, JLSEMI_WOL_SETTINGS_EVENTS,
			    JLSEMI_WOL_EVENT_MAGIC);
		__phy_write(phydev, JLSEMI_WOL_RST_RMSQ,
			    JLSEMI_WOL_RMSQ);
restore:
		phy_restore_page(phydev, oldpage, ret);
	} else {
		oldpage = phy_select_page(phydev, JLSEMI_WOL_PAGE);
		if (oldpage < 0)
			return oldpage;

		__phy_write(phydev, JLSEMI_WOL_SETTINGS_EVENTS, 0);
		__phy_clear_bits(phydev, JLSEMI_WOL_RST_RMSQ,
				 JLSEMI_WOL_RG_RSTB);

		phy_restore_page(phydev, oldpage, 0);
	}

	device_set_wakeup_enable(&phydev->mdio.dev, !!(wol->wolopts & WAKE_MAGIC));

	return 0;
}

static int jlsemi_suspend(struct phy_device *phydev)
{
	int ret;

	if (device_may_wakeup(&phydev->mdio.dev)) {
		ret = phy_write_paged(phydev, JLSEMI_INER_PAGE, JLSEMI_INER,
				      JLSEMI_INER_PME);
		if (ret < 0)
			return ret;

		jlsemi_ack_interrupt(phydev);
		return 0;
	}

	return genphy_suspend(phydev);
}

static int jlsemi_resume(struct phy_device *phydev)
{
	struct jlsemi_priv *priv = phydev->priv;
	int ret;

	ret = genphy_resume(phydev);
	if (ret < 0)
		return ret;

	return phy_write_paged(phydev, JLSEMI_INER_PAGE, JLSEMI_INER,
			       priv ? priv->iner : 0);
}

static int jlsemi_led_hw_is_supported(struct phy_device *phydev, u8 index,
				      unsigned long rules)
{
	const unsigned long mask = BIT(TRIGGER_NETDEV_LINK) |
					BIT(TRIGGER_NETDEV_LINK_10) |
					BIT(TRIGGER_NETDEV_LINK_100) |
					BIT(TRIGGER_NETDEV_LINK_1000) |
					BIT(TRIGGER_NETDEV_RX) |
					BIT(TRIGGER_NETDEV_TX);

	if (index >= JLSEMI_LED_COUNT)
		return -EINVAL;

	if (rules & ~mask)
		return -EOPNOTSUPP;

	if (!(rules & BIT(TRIGGER_NETDEV_RX)) ^ !(rules & BIT(TRIGGER_NETDEV_TX)))
		return -EOPNOTSUPP;

	return 0;
}

static int jlsemi_led_hw_control_get(struct phy_device *phydev, u8 index,
				     unsigned long *rules)
{
	int val;

	if (index >= JLSEMI_LED_COUNT)
		return -EINVAL;

	val = phy_read_paged(phydev, JLSEMI_LEDCR_PAGE, JLSEMI_LEDCR);
	if (val < 0)
		return val;

	val >>= JLSEMI_LEDCR_SHIFT * index;
	val &= JLSEMI_LEDCR_MASK;

	if (val & JLSEMI_LEDCR_LINK_10)
		__set_bit(TRIGGER_NETDEV_LINK_10, rules);

	if (val & JLSEMI_LEDCR_LINK_100)
		__set_bit(TRIGGER_NETDEV_LINK_100, rules);

	if (val & JLSEMI_LEDCR_LINK_1000)
		__set_bit(TRIGGER_NETDEV_LINK_1000, rules);

	if ((val & JLSEMI_LEDCR_LINK_10) &&
	    (val & JLSEMI_LEDCR_LINK_100) &&
	    (val & JLSEMI_LEDCR_LINK_1000)) {
		__set_bit(TRIGGER_NETDEV_LINK, rules);
	}

	if (val & JLSEMI_LEDCR_ACT_TXRX) {
		__set_bit(TRIGGER_NETDEV_RX, rules);
		__set_bit(TRIGGER_NETDEV_TX, rules);
	}

	return 0;
}

static int jlsemi_led_hw_control_set(struct phy_device *phydev, u8 index,
				     unsigned long rules)
{
	const u16 mask = JLSEMI_LEDCR_MASK << (JLSEMI_LEDCR_SHIFT * index);
	u16 reg = 0;

	if (index >= JLSEMI_LED_COUNT)
		return -EINVAL;

	if (test_bit(TRIGGER_NETDEV_LINK, &rules) ||
	    test_bit(TRIGGER_NETDEV_LINK_10, &rules)) {
		reg |= JLSEMI_LEDCR_LINK_10;
	}

	if (test_bit(TRIGGER_NETDEV_LINK, &rules) ||
	    test_bit(TRIGGER_NETDEV_LINK_100, &rules)) {
		reg |= JLSEMI_LEDCR_LINK_100;
	}

	if (test_bit(TRIGGER_NETDEV_LINK, &rules) ||
	    test_bit(TRIGGER_NETDEV_LINK_1000, &rules)) {
		reg |= JLSEMI_LEDCR_LINK_1000;
	}

	if (test_bit(TRIGGER_NETDEV_RX, &rules) ||
	    test_bit(TRIGGER_NETDEV_TX, &rules)) {
		reg |= JLSEMI_LEDCR_ACT_TXRX;
	}

	reg <<= JLSEMI_LEDCR_SHIFT * index;
	reg |= JLSEMI_LEDCR_MODE;

	return phy_modify_paged(phydev, JLSEMI_LEDCR_PAGE, JLSEMI_LEDCR,
				mask, reg);
}

static struct phy_driver jlsemi_drivers[] = {
	{
		PHY_ID_MATCH_EXACT(PHY_ID_JL2101),
		.name			= "JLSemi JL2101 Gigabit Ethernet",
		.flags			= PHY_ALWAYS_CALL_SUSPEND,
		.probe			= jlsemi_probe,
		.soft_reset		= genphy_soft_reset,
		.config_init		= jlsemi_config_init,
		.read_status		= jlsemi_read_status,
		.config_intr		= jlsemi_config_intr,
		.handle_interrupt	= jlsemi_handle_interrupt,
		.set_wol		= jlsemi_set_wol,
		.get_wol		= jlsemi_get_wol,
		.suspend		= jlsemi_suspend,
		.resume			= jlsemi_resume,
		.read_page		= jlsemi_read_page,
		.write_page		= jlsemi_write_page,
		.led_hw_is_supported	= jlsemi_led_hw_is_supported,
		.led_hw_control_get	= jlsemi_led_hw_control_get,
		.led_hw_control_set	= jlsemi_led_hw_control_set,
		.disable_autonomous_eee	= jlsemi_disable_autonomous_eee,
	},
	{
		PHY_ID_MATCH_MODEL(PHY_ID_JL2121),
		.name			= "JLSemi JL2121 Gigabit Ethernet",
		.flags			= PHY_ALWAYS_CALL_SUSPEND,
		.probe			= jlsemi_probe,
		.soft_reset		= genphy_soft_reset,
		.config_init		= jlsemi_config_init,
		.read_status		= jlsemi_read_status,
		.config_intr		= jlsemi_config_intr,
		.handle_interrupt	= jlsemi_handle_interrupt,
		.set_wol		= jlsemi_set_wol,
		.get_wol		= jlsemi_get_wol,
		.suspend		= jlsemi_suspend,
		.resume			= jlsemi_resume,
		.read_page		= jlsemi_read_page,
		.write_page		= jlsemi_write_page,
		.led_hw_is_supported	= jlsemi_led_hw_is_supported,
		.led_hw_control_get	= jlsemi_led_hw_control_get,
		.led_hw_control_set	= jlsemi_led_hw_control_set,
		.disable_autonomous_eee	= jlsemi_disable_autonomous_eee,
	},
	{
		PHY_ID_MATCH_VENDOR(PHY_ID_JLSEMI),
		.name			= "JLSemi Generic Gigabit Ethernet",
		.flags			= PHY_ALWAYS_CALL_SUSPEND,
		.probe			= jlsemi_probe,
		.soft_reset		= genphy_soft_reset,
		.config_init		= jlsemi_config_init,
		.read_status		= jlsemi_read_status,
		.config_intr		= jlsemi_config_intr,
		.handle_interrupt	= jlsemi_handle_interrupt,
		.suspend		= jlsemi_suspend,
		.resume			= jlsemi_resume,
		.read_page		= jlsemi_read_page,
		.write_page		= jlsemi_write_page,
		.disable_autonomous_eee	= jlsemi_disable_autonomous_eee,
	},
};

module_phy_driver(jlsemi_drivers);

static const struct mdio_device_id __maybe_unused jlsemi_tbl[] = {
	{ PHY_ID_MATCH_EXACT(PHY_ID_JL2101) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_JL2121) },
	{ PHY_ID_MATCH_VENDOR(PHY_ID_JLSEMI) },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(mdio, jlsemi_tbl);

MODULE_DESCRIPTION("JLSemi Ethernet PHY driver");
MODULE_AUTHOR("Pufan Jin <cyan2254650260@gmail.com>");
MODULE_LICENSE("GPL");
