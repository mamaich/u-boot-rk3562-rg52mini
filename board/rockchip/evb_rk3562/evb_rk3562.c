/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * (C) Copyright 2022 Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <dwc3-uboot.h>
#include <fdt_support.h>
#include <i2c.h>
#include <usb.h>
#include <asm/gpio.h>
#include <dm/ofnode.h>
#include <dm/uclass.h>
#include <power/fuel_gauge.h>

DECLARE_GLOBAL_DATA_PTR;

#if defined(CONFIG_USB_DWC3_GADGET) && !defined(CONFIG_DM_USB_GADGET)
static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_HIGH,
	.base = 0xfe500000,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.dis_u2_susphy_quirk = 1,
	.usb2_phyif_utmi_width = 16,
};

int usb_gadget_handle_interrupts(void)
{
	dwc3_uboot_handle_interrupt(0);
	return 0;
}

int board_usb_init(int index, enum usb_init_type init)
{
	return dwc3_uboot_init(&dwc3_device_data);
}
#endif

/*
 * Light a charge LED as early as possible, so that a user can tell the
 * board has started: the display needs about 15 seconds more. Red means
 * a charger is attached, green means running from the battery. This is
 * what the vendor bootloader does.
 */
static void board_power_on_indicate(void)
{
	struct gpio_desc red, green;
	struct udevice *fg;
	bool charging = false;
	ofnode node;

	node = ofnode_path("/i2c@ff200000/pmic@20/battery");
	if (!ofnode_valid(node)) {
		debug("%s: no battery node\n", __func__);
		return;
	}

	if (!uclass_get_device(UCLASS_FG, 0, &fg))
		charging = fuel_gauge_get_chrg_online(fg);

	if (!gpio_request_by_name_nodev(node, "charge_green_gpio", 0, &green,
					GPIOD_IS_OUT))
		dm_gpio_set_value(&green, charging ? 0 : 1);

	if (!gpio_request_by_name_nodev(node, "charge_red_gpio", 0, &red,
					GPIOD_IS_OUT))
		dm_gpio_set_value(&red, charging ? 1 : 0);
}

int rk_board_init(void)
{
	board_power_on_indicate();

	return 0;
}

/*
 * Board revision A carries no HUSB311 Type-C controller, revision B does.
 * The kernel tree describes one and marks the dwc3 controller with
 * "usb-role-switch": dwc3_drd_init() then registers a role switch and
 * returns, never looking at the extcon, so on revision A nothing ever
 * sets the role and USB host stays dead.
 *
 * Ask the chip itself. When it does not answer, hand the kernel the tree
 * the vendor used for that revision: no Type-C controller, and the role
 * taken from the USB2 PHY extcon.
 */
#define HUSB311_I2C_PATH	"/i2c@ffa10000"
#define HUSB311_PATH		HUSB311_I2C_PATH "/husb311@4e"
#define HUSB311_I2C_ADDR	0x4e
#define USB2_PHY_PATH		"/usb2-phy@ff740000"
#define DWC3_PATH		"/usbdrd/usb@fe500000"

/* 1 - answers, 0 - silent, negative - could not ask */
static int husb311_present(void)
{
	struct udevice *bus, *chip;
	ofnode node;
	int ret;

	node = ofnode_path(HUSB311_I2C_PATH);
	if (!ofnode_valid(node))
		return -ENODEV;

	ret = uclass_get_device_by_ofnode(UCLASS_I2C, node, &bus);
	if (ret)
		return ret;

	return dm_i2c_probe(bus, HUSB311_I2C_ADDR, 0, &chip) ? 0 : 1;
}

static int fdt_node_by_path_or_alias(void *blob, const char *path,
				     const char *alias)
{
	int off = fdt_path_offset(blob, path);

	return off >= 0 ? off : fdt_path_offset(blob, alias);
}

static void fdt_fixup_no_type_c(void *blob)
{
	u32 phandle;
	int off;

	/* room for the extcon property and a phandle, if one is missing */
	fdt_increase_size(blob, 512);

	off = fdt_path_offset(blob, HUSB311_PATH);
	if (off >= 0)
		fdt_setprop_string(blob, off, "status", "disabled");

	off = fdt_node_by_path_or_alias(blob, USB2_PHY_PATH, "u2phy");
	if (off < 0)
		return;

	phandle = fdt_get_phandle(blob, off);
	if (!phandle) {
		phandle = fdt_alloc_phandle(blob);
		if (fdt_set_phandle(blob, off, phandle))
			return;
	}

	/* offsets move on every change, so look the node up again */
	off = fdt_node_by_path_or_alias(blob, DWC3_PATH, "usbdrd_dwc3");
	if (off < 0)
		return;
	fdt_delprop(blob, off, "usb-role-switch");

	off = fdt_node_by_path_or_alias(blob, DWC3_PATH, "usbdrd_dwc3");
	if (off < 0)
		return;
	fdt_setprop_u32(blob, off, "extcon", phandle);
}

int rk_board_fdt_fixup(void *blob)
{
	int ret = husb311_present();

	if (ret < 0) {
		printf("USB: cannot reach the Type-C controller (%d), tree kept as is\n",
		       ret);
		return 0;
	}

	if (ret)
		return 0;

	printf("USB: no Type-C controller, taking the OTG role from the PHY\n");
	fdt_fixup_no_type_c(blob);

	return 0;
}
