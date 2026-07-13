#include <dm.h>
#include <net.h>
#include <asm/io.h>
#include <env.h>

#define AXIADO_OUI_B2 0xF9 /* Magic number for AX3005 MAC Addre3ss*/
#define R_MAC_0 0x00c /* Offset of MAC Address Register 0 */
#define R_MAC_1 0x010 /* Offset of MAC Address Register 1 */
#define MAC_CONFIG_BYTE_CNT 0x400 /* Configure Size of each MAC */

struct axiado_net_priv {
	void __iomem *base;
};

static int axiado_net_start(struct udevice *dev)
{
	/* enable TX/RX */
	return 0;
}
static int axiado_net_send(struct udevice *dev, void *p, int len)
{ /* TX */
	return 0;
}
static int axiado_net_recv(struct udevice *dev, int flags, uchar **pp)
{ /* RX */
	return 0;
}
static void axiado_net_stop(struct udevice *dev)
{ /* disable */
}

/* Called by eth-uclass to push MAC into HW MAC Registers */
static int axiado_net_write_hwaddr(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	unsigned char *mac = pdata->enetaddr; /* 6 bytes */
	uint32_t mac0, mac1;
	char *macid;
	unsigned long mac_index;
	uintptr_t mac_base;

	if (AXIADO_OUI_B2 != mac[2]) {
		printf("MAC is not provisioned with AXIADO AX3005 OUI\n");
		return -EINVAL;
	}

	macid = env_get("macid");
	if (!macid) {
		printf("MAC id is invalid\n");
		return -EINVAL;
	}

	mac_index = simple_strtoul(macid, NULL, 10);
	/* MAC Index range is 1 ~ 4 */
	if (mac_index > 4 || mac_index < 1) {
		printf("MAC id is not set\n");
		return -ENOENT;
	}

	/* program pdata->enetaddr (6 bytes) into the MAC registers */
	mac0 = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
	       ((uint32_t)mac[1] << 8) | ((uint32_t)mac[0] << 0);
	mac1 = ((uint32_t)mac[5] << 8) | ((uint32_t)mac[4] << 0);

	mac_base = pdata->iobase + mac_index * MAC_CONFIG_BYTE_CNT;
	/* program the MAC regs of port #1 */
	writel(mac0, mac_base + R_MAC_0);
	writel(mac1, mac_base + R_MAC_1);

	return 0;
}

static int axiado_net_probe(struct udevice *dev)
{
	struct eth_pdata *pdata = dev_get_plat(dev);
	struct axiado_net_priv *priv = dev_get_priv(dev);

	priv->base = dev_remap_addr(dev);
	if (!priv->base)
		return -EINVAL;
	pdata->iobase = (uintptr_t)priv->base;
	return 0;
}

static const struct eth_ops axiado_net_ops = {
	.start = axiado_net_start,
	.send = axiado_net_send,
	.recv = axiado_net_recv,
	.stop = axiado_net_stop,
	.write_hwaddr = axiado_net_write_hwaddr,
};

static const struct udevice_id axiado_net_ids[] = {
	{ .compatible = "axiado,ax3005-mac" },
	{}
};

U_BOOT_DRIVER(axiado_mac) = {
	.name = "axiado_mac",
	.id = UCLASS_ETH,
	.of_match = axiado_net_ids,
	.probe = axiado_net_probe,
	.ops = &axiado_net_ops,
	.priv_auto = sizeof(struct axiado_net_priv),
	.plat_auto = sizeof(struct eth_pdata),
};
