/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * (C) Copyright 2022 Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <dwc3-uboot.h>
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
