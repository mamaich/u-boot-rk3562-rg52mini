// SPDX-License-Identifier: GPL-2.0+
/*
 * Boot menu for an RG52 Mini card that carries two systems.
 *
 * A dual card keeps GammaOS on partition "dArkOS_Fat" (the name its OTA
 * packages write to) and dArkOS on "darkos_boot" (its boot FAT, the only
 * one marked bootable, so the early distro dtb and the logo come from
 * there). Both boot through extlinux from their own FAT. A card that lacks
 * either partition is a single-system card and boots exactly as before.
 *
 * Items: GammaOS, dArkOS and, when GammaOS keeps the file /bootmenu_emmc on
 * its FAT, the internal eMMC. That file follows the eMMC switch of the
 * GammaOS Toolbox, which shows the same item in the Android power menu.
 *
 * Until GammaOS has finished its first-time setup the card boots straight
 * into it, without the menu: Android formats and grows userdata, and its
 * setup wizard fills the ROMs folder there, which dArkOS takes its ROMs
 * from (and wipes the RetroArch auto-saves in it). GammaOS tells it by
 * /bootmenu_ready on its FAT, kept in step with /data/setupcompleted. Up or
 * down held at power-on brings the menu up anyway.
 *
 * The menu is a set of ready-made pictures on the bootable FAT, one per
 * highlighted item, plus one for the default item with the auto-boot hint:
 *
 *	bootmenu_<n>.bmp	item n highlighted, three items
 *	bootmenu_<n>t.bmp	the same with "starts in 5 s", for the default
 *	bootmenu2_<n>[t].bmp	the same without the eMMC item
 *
 * There is no text console on the panel, and every decoded picture stays in
 * the 32 MB display pool for good, so the set is kept this small.
 *
 * Keys: d-pad up / volume up and d-pad down / volume down move, A, Start
 * or a short press of power boots. Without a key the default item boots
 * after BOOTMENU_TIMEOUT_MS. The default is the last choice, kept in the
 * Rockchip vendor storage of the card (sectors 7168+, outside any file
 * system), and GammaOS when there is none.
 */

#include <common.h>
#include <adc.h>
#include <boot_rkimg.h>
#include <fs.h>
#include <key.h>
#include <malloc.h>
#include <part.h>
#include <video_rockchip.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <asm/arch/boot_mode.h>
#include <asm/arch/vendor.h>
#include <dm/device.h>
#include <dm/ofnode.h>
#include <dm/pinctrl.h>
#include <dm/uclass.h>

#define BOOTMENU_TIMEOUT_MS	5000
#define BOOTMENU_POLL_MS	10
/* a vendor storage id of our own, far from the ones Rockchip assigns */
#define BOOTMENU_VENDOR_ID	0x5242
#define BOOTMENU_MAGIC		0x52473532	/* "RG52" */
/* on the GammaOS FAT: the setup is done, show the menu */
#define BOOTMENU_READY_FLAG	"/bootmenu_ready"
/* on the GammaOS FAT: show the eMMC item */
#define BOOTMENU_EMMC_FLAG	"/bootmenu_emmc"

/* the joystick node of the kernel tree, which U-Boot runs on */
#define JOYSTICK_PATH		"/play_joystick"
#define DPAD_DOWN_ADC		"saradc@ffaa0000"
#define DPAD_DOWN_CHANNEL	3
#define BMP_MAX_BYTES		(8 << 20)	/* MAX_IMAGE_BYTES of the display */

/* the order on the screen; the eMMC item, if shown, is always the last */
enum {
	SEL_GAMMAOS,
	SEL_DARKOS,
	SEL_EMMC,
	SEL_COUNT,
};

static const char * const sel_names[SEL_COUNT] = {
	"GammaOS", "dArkOS", "eMMC",
};

/* GPT names of the boot FAT of each system on the card */
static const char * const sel_parts[SEL_COUNT] = {
	"dArkOS_Fat", "darkos_boot", NULL,
};

struct bootmenu_saved {
	u32 magic;
	u32 sel;
};

enum {
	MENU_UP,
	MENU_DOWN,
	MENU_OK,
	MENU_COUNT,
};

/* buttons of the joystick wired to plain GPIOs, low when pressed */
static const struct {
	const char *name;
	int key;
} bootmenu_gpios[] = {
	{ "gpio124", MENU_UP },		/* GPIO1_D0, d-pad up */
	{ "gpio130", MENU_OK },		/* GPIO1_D6, A */
	{ "gpio131", MENU_OK },		/* GPIO1_D7, Start */
};

struct bootmenu {
	struct blk_desc *dev;
	int part[SEL_COUNT];
	int count;			/* items on the screen */
	int sel;
	struct gpio_desc gpio[ARRAY_SIZE(bootmenu_gpios)];
	bool gpio_ok[ARRAY_SIZE(bootmenu_gpios)];
	struct udevice *adc;
	unsigned int adc_pressed;	/* below this the d-pad down is pressed */
	bool down_ok;
};

static bool bootmenu_file_exists(struct blk_desc *dev, int part,
				 const char *name)
{
	if (fs_set_blk_dev_with_part(dev, part))
		return false;

	return fs_exists(name);
}

static int bootmenu_find_part(struct blk_desc *dev, const char *name)
{
	disk_partition_t info;
	int part;

	part = part_get_info_by_name_strict(dev, name, &info);
	if (part <= 0)
		return -ENOENT;

	if (!bootmenu_file_exists(dev, part, "/extlinux/extlinux.conf"))
		return -ENOENT;

	return part;
}

/* the last choice, or -ENOENT when there is none yet */
static int bootmenu_load_default(void)
{
	struct bootmenu_saved saved;

	if (vendor_storage_read(BOOTMENU_VENDOR_ID, &saved, sizeof(saved)) !=
	    sizeof(saved))
		return -ENOENT;
	if (saved.magic != BOOTMENU_MAGIC || saved.sel >= SEL_COUNT)
		return -ENOENT;

	return saved.sel;
}

static void bootmenu_save_default(int sel)
{
	struct bootmenu_saved saved = {
		.magic = BOOTMENU_MAGIC,
		.sel = sel,
	};

	/* written only when it changes: this is flash */
	if (bootmenu_load_default() == sel)
		return;
	if (vendor_storage_write(BOOTMENU_VENDOR_ID, &saved, sizeof(saved)) < 0)
		printf("bootmenu: could not remember the choice\n");
}

/*
 * Nobody probes the joystick node in U-Boot, so its pinctrl (pull-ups on
 * the button lines) is never applied: do it by hand. Only the first group,
 * the buttons; the second one drives the vibration motor.
 */
static void bootmenu_pull_up_buttons(void)
{
	struct udevice *config, *pctldev;
	const fdt32_t *cell;
	ofnode node;
	int len;

	node = ofnode_path(JOYSTICK_PATH);
	if (!ofnode_valid(node))
		return;

	cell = ofnode_get_property(node, "pinctrl-0", &len);
	if (!cell || len < sizeof(*cell))
		return;

	if (uclass_get_device_by_phandle_id(UCLASS_PINCONFIG,
					    fdt32_to_cpu(*cell), &config))
		return;

	for (pctldev = dev_get_parent(config); pctldev;
	     pctldev = dev_get_parent(pctldev)) {
		if (device_get_uclass_id(pctldev) == UCLASS_PINCTRL)
			break;
	}
	if (pctldev)
		pinctrl_get_ops(pctldev)->set_state(pctldev, config);
}

static void bootmenu_init_keys(struct bootmenu *m)
{
	unsigned int mask;
	int i;

	bootmenu_pull_up_buttons();
	udelay(100);

	for (i = 0; i < ARRAY_SIZE(bootmenu_gpios); i++) {
		struct gpio_desc *desc = &m->gpio[i];

		if (dm_gpio_lookup_name(bootmenu_gpios[i].name, desc))
			continue;
		if (dm_gpio_request(desc, "bootmenu"))
			continue;
		if (dm_gpio_set_dir_flags(desc, GPIOD_IS_IN)) {
			dm_gpio_free(desc->dev, desc);
			continue;
		}
		m->gpio_ok[i] = true;
	}

	if (!uclass_get_device_by_name(UCLASS_ADC, DPAD_DOWN_ADC, &m->adc) &&
	    !adc_data_mask(m->adc, &mask)) {
		/* the line sits near 1.8 V and drops below 80 mV when pressed */
		m->adc_pressed = mask / 4;
		m->down_ok = true;
	}

	/* a press of power that switched the device on is not a choice */
	key_read(KEY_POWER);
}

static void bootmenu_release_keys(struct bootmenu *m)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(bootmenu_gpios); i++)
		if (m->gpio_ok[i])
			dm_gpio_free(m->gpio[i].dev, &m->gpio[i]);
}

/* the state of each logical key right now; power reports a whole press */
static void bootmenu_read_keys(struct bootmenu *m, bool *held, bool *power)
{
	unsigned int val;
	int i;

	memset(held, 0, MENU_COUNT * sizeof(*held));

	for (i = 0; i < ARRAY_SIZE(bootmenu_gpios); i++)
		if (m->gpio_ok[i] && dm_gpio_get_value(&m->gpio[i]) == 0)
			held[bootmenu_gpios[i].key] = true;

	if (m->down_ok &&
	    !adc_channel_single_shot(DPAD_DOWN_ADC, DPAD_DOWN_CHANNEL, &val) &&
	    val < m->adc_pressed)
		held[MENU_DOWN] = true;

	if (key_read(KEY_VOLUMEUP) == KEY_PRESS_DOWN)
		held[MENU_UP] = true;
	if (key_read(KEY_VOLUMEDOWN) == KEY_PRESS_DOWN)
		held[MENU_DOWN] = true;

	*power = key_is_pressed(key_read(KEY_POWER));
}

static void bootmenu_draw(struct bootmenu *m, bool countdown)
{
	char name[24];

	snprintf(name, sizeof(name), "bootmenu%s_%d%s.bmp",
		 m->count == SEL_COUNT ? "" : "2", m->sel,
		 countdown ? "t" : "");
	rockchip_show_bmp(name);
}

/* the logo of the chosen system, from its own FAT, while its kernel loads */
static void bootmenu_show_logo(struct bootmenu *m, int sel)
{
	char name[32];
	loff_t len;
	void *buf;

	if (sel == SEL_DARKOS) {
		/* the bootable FAT, already decoded and cached */
		rockchip_show_bmp("logo.bmp");
		return;
	}

	buf = malloc(BMP_MAX_BYTES);
	if (!buf)
		return;
	memset(buf, 0, BMP_MAX_BYTES);
	if (!fs_set_blk_dev_with_part(m->dev, m->part[sel]) &&
	    !fs_read("/logo.bmp", (ulong)buf, 0, BMP_MAX_BYTES, &len) && len) {
		/*
		 * The cache goes by name, keep it apart from the bootable one;
		 * the display keeps 20 bytes of it.
		 */
		snprintf(name, sizeof(name), "logo_p%d.bmp", m->part[sel]);
		rockchip_show_bmp_by_address(name, (uintptr_t)buf);
	}
	free(buf);
}

static int bootmenu_choose(struct bootmenu *m)
{
	bool held[MENU_COUNT], prev[MENU_COUNT], power;
	bool countdown = true;
	ulong start;

	/* keys held since power-on count only once released and pressed again */
	bootmenu_read_keys(m, prev, &power);
	bootmenu_draw(m, countdown);
	printf("bootmenu: %s starts in %d s, up/down stops the timer\n",
	       sel_names[m->sel], BOOTMENU_TIMEOUT_MS / 1000);

	start = get_timer(0);
	for (;;) {
		bool ok, up, down;

		mdelay(BOOTMENU_POLL_MS);
		bootmenu_read_keys(m, held, &power);
		ok = held[MENU_OK] && !prev[MENU_OK];
		up = held[MENU_UP] && !prev[MENU_UP];
		down = held[MENU_DOWN] && !prev[MENU_DOWN];
		memcpy(prev, held, sizeof(prev));

		if (power || ok)
			return m->sel;

		if (up == down) {
			if (countdown && get_timer(start) >= BOOTMENU_TIMEOUT_MS)
				return m->sel;
			continue;
		}

		/* from here on the menu waits for a choice */
		countdown = false;
		m->sel = (m->sel + (up ? m->count - 1 : 1)) % m->count;
		printf("bootmenu: %s\n", sel_names[m->sel]);
		bootmenu_draw(m, false);
	}
}

static void bootmenu_boot(struct bootmenu *m, int sel, bool remember)
{
	char cmd[128];

	if (remember)
		bootmenu_save_default(sel);

	if (sel == SEL_EMMC) {
		/* the same path as "reboot emmc" from a running system */
		printf("bootmenu: rebooting into the eMMC\n");
		writel(BOOT_EMMC, CONFIG_ROCKCHIP_BOOT_MODE_REG);
		flushc();
		do_reset(NULL, 0, 0, NULL);
		return;
	}

	bootmenu_show_logo(m, sel);

	/* the stock distro scan still follows, should the chosen one fail */
	snprintf(cmd, sizeof(cmd),
		 "sysboot mmc %d:%d any ${scriptaddr} /extlinux/extlinux.conf; "
		 "run distro_bootcmd", m->dev->devnum, m->part[sel]);
	env_set("bootcmd", cmd);
	printf("bootmenu: %s from mmc %d:%d\n", sel_names[sel],
	       m->dev->devnum, m->part[sel]);
}

void rg52_bootmenu(void)
{
	struct bootmenu m = { 0 };
	int i, saved;

	if (rockchip_get_boot_mode() != BOOT_MODE_NORMAL)
		return;

	/*
	 * The card only. This check is what keeps the menu away from
	 * "reboot emmc": BOOT_EMMC reads as a normal boot above, but
	 * boot_devtype_init() has already switched the boot device to mmc 0 by
	 * now. A boot without a card lands on mmc 0 as well.
	 */
	m.dev = rockchip_get_bootdev();
	if (!m.dev || m.dev->if_type != IF_TYPE_MMC || m.dev->devnum != 1)
		return;

	for (i = 0; i < SEL_COUNT; i++) {
		if (!sel_parts[i])
			continue;
		m.part[i] = bootmenu_find_part(m.dev, sel_parts[i]);
		if (m.part[i] < 0)
			return;
	}

	bootmenu_init_keys(&m);

	if (!bootmenu_file_exists(m.dev, m.part[SEL_GAMMAOS],
				  BOOTMENU_READY_FLAG)) {
		bool held[MENU_COUNT], power;

		bootmenu_read_keys(&m, held, &power);
		if (!held[MENU_UP] && !held[MENU_DOWN]) {
			/* not a choice: the last one stays as it was */
			printf("bootmenu: GammaOS has not finished its setup yet\n");
			bootmenu_boot(&m, SEL_GAMMAOS, false);
			bootmenu_release_keys(&m);
			return;
		}
	}

	m.count = bootmenu_file_exists(m.dev, m.part[SEL_GAMMAOS],
				       BOOTMENU_EMMC_FLAG) ? SEL_COUNT : SEL_EMMC;
	saved = bootmenu_load_default();
	m.sel = saved >= 0 && saved < m.count ? saved : SEL_GAMMAOS;

	bootmenu_boot(&m, bootmenu_choose(&m), true);
	bootmenu_release_keys(&m);
}
