/**********************************************************************************
* Copyright (c)  2008-2015  Guangdong OPPO Mobile Comm Corp., Ltd
* CONFIG_OPPO_VENDOR_EDIT
* Description: Charger IC management module for charger system framework.
*                          Manage all charger IC and define abstarct function flow.
* Version   : 1.0
* Date          : 2015-06-22
* Author        : fanhui@PhoneSW.BSP
*                         : Fanhong.Kong@ProDrv.CHG
* ------------------------------ Revision History: --------------------------------
* <version>           <date>                <author>                            <desc>
* Revision 1.0        2015-06-22        fanhui@PhoneSW.BSP             Created for new architecture
* Revision 1.0        2015-06-22        Fanhong.Kong@ProDrv.CHG        Created for new architecture
* Revision 1.1        2016-03-07        wenbin.liu@SW.Bsp.Driver       edit for log optimize
***********************************************************************************/
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/kthread.h>

#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/of.h>

#include <linux/bitops.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <linux/spmi.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <linux/leds.h>
#include <linux/rtc.h>
#include <linux/batterydata-lib.h>
#include <linux/of_batterydata.h>
#include <linux/ktime.h>
#include <linux/kernel.h>

#include "oppo_charger.h"
#include "oppo_gauge.h"

struct oppo_chg_chip *g_oppo_chg_chip = NULL;
static int enable_charger_log;
static int charger_abnormal_log;
static bool vbatt_higherthan_4180mv;
static bool vbatt_lowerthan_3300mv;

#define OPPO_CHG_UPDATE_INTERVAL_SEC 5
#define OPPO_CHG_UPDATE_INTERVAL                                                                   \
	round_jiffies_relative(msecs_to_jiffies(OPPO_CHG_UPDATE_INTERVAL_SEC * 1000))
#define OPPO_CHG_UPDATE_INIT_DELAY round_jiffies_relative(msecs_to_jiffies(10 * 1000))

static void oppo_chg_variables_init(struct oppo_chg_chip *chip);
static void oppo_chg_update_work(struct work_struct *work);
static void oppo_chg_protection_check(struct oppo_chg_chip *chip);
static void oppo_chg_get_battery_data(struct oppo_chg_chip *chip);
static void oppo_chg_check_tbatt_status(struct oppo_chg_chip *chip);
static void oppo_chg_set_input_current_limit(struct oppo_chg_chip *chip);
static void oppo_chg_battery_update_status(struct oppo_chg_chip *chip);
#ifdef CONFIG_HAS_EARLYSUSPEND
static void chg_early_suspend(struct early_suspend *h);
static void chg_late_resume(struct early_suspend *h);
#elif defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *nb, unsigned long event, void *data);
#endif

/****************************************/

static ssize_t chg_log_write(struct file *filp, const char __user *buff, size_t len, loff_t *data)
{
	char write_data[32] = { 0 };

	if (copy_from_user(&write_data, buff, len)) {
		return -EFAULT;
	}

	if (write_data[0] == '1') {
		enable_charger_log = 1;
	} else if ((write_data[0] >= '2') && (write_data[0] <= '9')) {
		enable_charger_log = 2;
	} else {
		enable_charger_log = 0;
	}

	return len;
}
static ssize_t chg_log_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char page[256] = { 0 };
	char read_data[32] = { 0 };
	int len = 0;

	if (enable_charger_log == 1) {
		read_data[0] = '1';
	} else if (enable_charger_log == 2) {
		read_data[0] = '2';
	} else {
		read_data[0] = '0';
	}
	len = sprintf(page, "%s", read_data);
	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}
	if (copy_to_user(buff, page, (len < count ? len : count))) {
		return -EFAULT;
	}
	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static const struct file_operations chg_log_proc_fops = {
	.write = chg_log_write,
	.read = chg_log_read,
};

static int init_proc_chg_log(void)
{
	struct proc_dir_entry *p = NULL;

	p = proc_create("charger_log", 0664, NULL, &chg_log_proc_fops);
	return 0;
}

static ssize_t chg_cycle_write(struct file *file, const char __user *buff, size_t count,
			       loff_t *ppos)
{
	char proc_chg_cycle_data[16];

	if (copy_from_user(&proc_chg_cycle_data, buff, count)) {
		return -EFAULT;
	}

	if (strncmp(proc_chg_cycle_data, "en808", 5) == 0) {
		g_oppo_chg_chip->chg_ops->charger_unsuspend(g_oppo_chg_chip);
		g_oppo_chg_chip->chg_ops->charging_enable(g_oppo_chg_chip);
	} else if (strncmp(proc_chg_cycle_data, "dis808", 6) == 0) {
		g_oppo_chg_chip->chg_ops->charging_disable(g_oppo_chg_chip);
		g_oppo_chg_chip->chg_ops->charger_suspend(g_oppo_chg_chip);
	} else {
		return -EFAULT;
	}

	return count;
}

static const struct file_operations chg_cycle_proc_fops = {
	.write = chg_cycle_write,
	.llseek = noop_llseek,
};

static void init_proc_chg_cycle(void)
{
	proc_create("charger_cycle", S_IWUSR | S_IWGRP | S_IWOTH, NULL, &chg_cycle_proc_fops);
}
static ssize_t critical_log_read(struct file *filp, char __user *buff, size_t count, loff_t *off)
{
	char page[256] = { 0 };
	char read_data[32] = { 0 };
	int len = 0;
	/*        itoa(charger_abnormal_log, read_data, 10);*/
	/*        sprintf(read_data,"%s",charger_abnormal_log);*/
	if (charger_abnormal_log >= 10) {
		charger_abnormal_log = 10;
	}
	read_data[0] = '0' + charger_abnormal_log % 10;
	len = sprintf(page, "%s", read_data);
	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}
	if (copy_to_user(buff, page, (len < count ? len : count))) {
		return -EFAULT;
	}
	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static ssize_t critical_log_write(struct file *filp, const char __user *buff, size_t len,
				  loff_t *data)
{
	char write_data[32] = { 0 };
	int critical_log = 0;
	if (copy_from_user(&write_data, buff, len)) {
		pr_err("bat_log_write error.\n");
		return -EFAULT;
	}
	/*        critical_log = atoi(write_data);*/
	/*        sprintf(critical_log,"%d",(void *)write_data);*/
	write_data[len] = '\0';
	if (write_data[len - 1] == '\n') {
		write_data[len - 1] = '\0';
	}
	critical_log = (int)simple_strtoul(write_data, NULL, 10);
	/*        pr_err("%s:data=%s,critical_log=%d\n",__func__,write_data,critical_log);*/
	if (critical_log > 256) {
		critical_log = 256;
	}
	charger_abnormal_log = critical_log;

	return len;
}

static const struct file_operations chg_critical_log_proc_fops = {
	.write = critical_log_write,
	.read = critical_log_read,
};
static void init_proc_critical_log(void)
{
	struct proc_dir_entry *p = NULL;

	p = proc_create("charger_critical_log", 0664, NULL, &chg_critical_log_proc_fops);
	if (!p) {
		pr_err("proc_create chg_critical_log_proc_fops fail!\n");
	}
}

/*ye.zhang@BSP.Sensor.Function, 2017-03-30, add interface for charging special feature in different projects*/
static int charging_limit_time_show(struct seq_file *seq_filp, void *v)
{
	seq_printf(seq_filp, "%d\n", g_oppo_chg_chip->limits.max_chg_time_sec);
	return 0;
}
static int charging_limit_time_open(struct inode *inode, struct file *file)
{
	int ret;
	ret = single_open(file, charging_limit_time_show, NULL);
	return ret;
}

static ssize_t charging_limit_time_write(struct file *filp, const char __user *buff, size_t len,
					 loff_t *data)
{
	int limit_time;
	char temp[16];
	if (copy_from_user(temp, buff, len)) {
		pr_err("charging_limit_time_write error.\n");
		return -EFAULT;
	}

	sscanf(temp, "%d", &limit_time);

	if (g_oppo_chg_chip) {
		g_oppo_chg_chip->limits.max_chg_time_sec = limit_time;
		printk(KERN_EMERG "charging_feature:max_chg_time_sec = %d\n",
		       g_oppo_chg_chip->limits.max_chg_time_sec);
	}

	return len;
}

static const struct file_operations charging_limit_time_fops = {
	.open = charging_limit_time_open,
	.write = charging_limit_time_write,
	.read = seq_read,
};
static int charging_limit_current_show(struct seq_file *seq_filp, void *v)
{
	seq_printf(seq_filp, "%d\n", g_oppo_chg_chip->limits.input_current_led_ma);
	return 0;
}
static int charging_limit_current_open(struct inode *inode, struct file *file)
{
	int ret;
	ret = single_open(file, charging_limit_current_show, NULL);
	return ret;
}

static ssize_t charging_limit_current_write(struct file *filp, const char __user *buff, size_t len,
					    loff_t *data)
{
	int limit_current;
	char temp[16];
	if (copy_from_user(temp, buff, len)) {
		pr_err("charging_limit_time_write error.\n");
		return -EFAULT;
	}
	sscanf(temp, "%d", &limit_current);

	if (g_oppo_chg_chip && (limit_current <= OPCHG_INPUT_CURRENT_LIMIT_LED_MA)) {
		g_oppo_chg_chip->limits.input_current_led_ma = limit_current;
		printk(KERN_EMERG "charging_feature:input_current_led_ma = %d\n",
		       g_oppo_chg_chip->limits.input_current_led_ma);
	}
	return len;
}

static const struct file_operations charging_limit_current_fops = {
	.open = charging_limit_current_open,
	.write = charging_limit_current_write,
	.read = seq_read,
};

static void init_proc_charging_feature(void)
{
	struct proc_dir_entry *p_time = NULL;
	struct proc_dir_entry *p_current = NULL;

	p_time = proc_create("charging_limit_time", 0664, NULL, &charging_limit_time_fops);
	if (!p_time) {
		pr_err("proc_create charging_feature_fops fail!\n");
	}
	p_current = proc_create("charging_limit_current", 0664, NULL, &charging_limit_current_fops);
	if (!p_current) {
		pr_err("proc_create charging_feature_fops fail!\n");
	}
}

int oppo_chg_init(struct oppo_chg_chip *chip)
{
	int rc = 0;
	struct power_supply *usb_psy;
	struct power_supply *batt_psy;
	struct power_supply *dc_psy;

	if (!chip->chg_ops) {
		dev_err(chip->dev, "charger operations cannot be NULL\n");
		return -1;
	}
	oppo_chg_variables_init(chip);
	oppo_chg_get_battery_data(chip);
	usb_psy = power_supply_get_by_name("usb");
	if (!usb_psy) {
		dev_err(chip->dev, "USB psy not found; deferring probe\n");
		/*return -EPROBE_DEFER;*/
		goto usb_psy_reg_failed;
	}

	chip->usb_psy = usb_psy;

	dc_psy = power_supply_get_by_name("dc");
	if (!dc_psy) {
		dev_err(chip->dev, "dc psy not found; deferring probe\n");
		goto dc_psy_reg_failed;
	}
	chip->dc_psy = dc_psy;

	batt_psy = power_supply_get_by_name("battery");
	if (!batt_psy) {
		dev_err(chip->dev, "battery psy not found; deferring probe\n");
		goto batt_psy_reg_failed;
	}
	chip->batt_psy = batt_psy;

	chip->pmic_spmi.psy_registered = true;
	wake_lock_init(&chip->suspend_lock, "battery suspend wakelock");

	INIT_DELAYED_WORK(&chip->update_work, oppo_chg_update_work);

#ifdef CONFIG_HAS_EARLYSUSPEND
	chip->chg_early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	chip->chg_early_suspend.suspend = chg_early_suspend;
	chip->chg_early_suspend.resume = chg_late_resume;
	register_early_suspend(&chip->chg_early_suspend);
#elif defined(CONFIG_FB)
	chip->chg_fb_notify.notifier_call = fb_notifier_callback;
	rc = fb_register_client(&chip->chg_fb_notify);
	if (rc) {
		pr_err("Unable to register chg_fb_notify: %d\n", rc);
	}
#endif

	init_proc_chg_log();
	init_proc_chg_cycle();
	init_proc_critical_log();
	/*ye.zhang@BSP.Sensor.Function, 2017-03-30, add interface for charging special feature in different projects*/
	init_proc_charging_feature();
	/*ye.zhang add end*/
	schedule_delayed_work(&chip->update_work, OPPO_CHG_UPDATE_INIT_DELAY);

	return 0;
batt_psy_reg_failed:
dc_psy_reg_failed:
usb_psy_reg_failed:

	return rc;
}

/*--------------------------------------------------------*/
int oppo_chg_parse_dt(struct oppo_chg_chip *chip)
{
	int rc;
	struct device_node *node = chip->dev->of_node;
	int batt_cold_degree_negative, batt_removed_degree_negative;

	if (!node) {
		dev_err(chip->dev, "device tree info. missing\n");
		return -EINVAL;
	}

	chip->usb_switch_gpio = of_get_named_gpio(node, "qcom,usb_switch_gpio", 0);
	if (chip->usb_switch_gpio <= 0) {
		dev_err(chip->dev, "Couldn't read chargerid_switch-gpio:%d\n",
			chip->usb_switch_gpio);
	} else {
		if (gpio_is_valid(chip->usb_switch_gpio)) {
			rc = gpio_request(chip->usb_switch_gpio, "chargerid-switch-gpio");
		}
	}

	/*hardware init*/
	rc = of_property_read_u32(node, "qcom,input_current_usb_ma",
				  &chip->limits.input_current_usb_ma);
	if (rc) {
		chip->limits.input_current_usb_ma = OPCHG_INPUT_CURRENT_LIMIT_USB_MA;
	}

#ifdef CONFIG_OPPO_SPECIAL_BUILD
	chip->limits.input_current_usb_ma = 1000;
#endif

	rc = of_property_read_u32(node, "qcom,input_current_led_ma",
				  &chip->limits.input_current_led_ma);
	if (rc) {
		chip->limits.input_current_led_ma = OPCHG_INPUT_CURRENT_LIMIT_LED_MA;
	}

	rc = of_property_read_u32(node, "qcom,input_current_led_ma_forcmcc",
				  &chip->limits.input_current_led_ma_forcmcc);
	if (rc) {
		chip->limits.input_current_led_ma_forcmcc = 500;
	}

	chip->limits.iterm_disabled = of_property_read_bool(node, "qcom,iterm-disabled");

	rc = of_property_read_u32(node, "qcom,iterm-ma", &chip->limits.iterm_ma);
	if (rc < 0) {
		chip->limits.iterm_ma = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,recharge-mv", &chip->limits.recharge_mv);
	if (rc < 0) {
		chip->limits.recharge_mv = -EINVAL;
	}

	/* over temp input_current limit*/
	/* 35C */
	rc = of_property_read_u32(node, "qcom,input_current_led_ma_overtemp",
				  &chip->limits.input_current_led_ma_overtemp);
	if (rc) {
		chip->limits.input_current_led_ma_overtemp = -EINVAL;
	}
	rc = of_property_read_u32(node, "qcom,overtemp_bat_decidegc",
				  &chip->limits.overtemp_bat_decidegc);
	if (rc < 0) {
		chip->limits.overtemp_bat_decidegc = 350;
	}

	/*-19C*/
	rc = of_property_read_u32(node, "qcom,removed_bat_decidegc", &batt_removed_degree_negative);
	if (rc < 0) {
		chip->limits.removed_bat_decidegc = -19;
	} else {
		chip->limits.removed_bat_decidegc = -batt_removed_degree_negative;
	}

	/*-3~0 C*/
	rc = of_property_read_u32(node, "qcom,cold_bat_decidegc", &batt_cold_degree_negative);
	if (rc < 0) {
		chip->limits.cold_bat_decidegc = -EINVAL;
	} else {
		chip->limits.cold_bat_decidegc = -batt_cold_degree_negative;
	}

	rc = of_property_read_u32(node, "qcom,temp_cold_vfloat_mv",
				  &chip->limits.temp_cold_vfloat_mv);

	rc = of_property_read_u32(node, "qcom,temp_cold_fastchg_current_ma",
				  &chip->limits.temp_cold_fastchg_current_ma);

	/*0~5 C*/
	rc = of_property_read_u32(node, "qcom,little_cold_bat_decidegc",
				  &chip->limits.little_cold_bat_decidegc);
	if (rc < 0) {
		chip->limits.little_cold_bat_decidegc = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_little_cold_vfloat_mv",
				  &chip->limits.temp_little_cold_vfloat_mv);
	if (rc < 0) {
		chip->limits.temp_little_cold_vfloat_mv = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_little_cold_fastchg_current_ma",
				  &chip->limits.temp_little_cold_fastchg_current_ma);
	if (rc < 0) {
		chip->limits.temp_little_cold_fastchg_current_ma = -EINVAL;
	}

	/*5~12 C*/
	rc = of_property_read_u32(node, "qcom,cool_bat_decidegc", &chip->limits.cool_bat_decidegc);
	if (rc < 0) {
		chip->limits.cool_bat_decidegc = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_cool_vfloat_mv",
				  &chip->limits.temp_cool_vfloat_mv);
	if (rc < 0) {
		chip->limits.temp_cool_vfloat_mv = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_cool_fastchg_current_ma_high",
				  &chip->limits.temp_cool_fastchg_current_ma_high);
	if (rc < 0) {
		chip->limits.temp_cool_fastchg_current_ma_high = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_cool_fastchg_current_ma_low",
				  &chip->limits.temp_cool_fastchg_current_ma_low);
	if (rc < 0) {
		chip->limits.temp_cool_fastchg_current_ma_low = -EINVAL;
	}

	/*12~16 C*/
	rc = of_property_read_u32(node, "qcom,little_cool_bat_decidegc",
				  &chip->limits.little_cool_bat_decidegc);
	if (rc < 0) {
		chip->limits.little_cool_bat_decidegc = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_little_cool_vfloat_mv",
				  &chip->limits.temp_little_cool_vfloat_mv);
	if (rc < 0) {
		chip->limits.temp_little_cool_vfloat_mv = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_little_cool_fastchg_current_ma",
				  &chip->limits.temp_little_cool_fastchg_current_ma);
	if (rc < 0) {
		chip->limits.temp_little_cool_fastchg_current_ma = -EINVAL;
	}

	/*16~45 C*/
	rc = of_property_read_u32(node, "qcom,normal_bat_decidegc",
				  &chip->limits.normal_bat_decidegc);

	rc = of_property_read_u32(node, "qcom,temp_normal_fastchg_current_ma",
				  &chip->limits.temp_normal_fastchg_current_ma);
	if (rc) {
		chip->limits.temp_normal_fastchg_current_ma = OPCHG_FAST_CHG_MAX_MA;
	}

	rc = of_property_read_u32(node, "qcom,temp_normal_vfloat_mv_normalchg",
				  &chip->limits.temp_normal_vfloat_mv_normalchg);
	if (rc < 0) {
		chip->limits.temp_normal_vfloat_mv_normalchg = 4320;
	}

	/*45~55 C*/
	rc = of_property_read_u32(node, "qcom,warm_bat_decidegc", &chip->limits.warm_bat_decidegc);
	if (rc < 0) {
		chip->limits.warm_bat_decidegc = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_warm_vfloat_mv",
				  &chip->limits.temp_warm_vfloat_mv);
	if (rc < 0) {
		chip->limits.temp_warm_vfloat_mv = -EINVAL;
	}

	rc = of_property_read_u32(node, "qcom,temp_warm_fastchg_current_ma",
				  &chip->limits.temp_warm_fastchg_current_ma);
	if (rc < 0) {
		chip->limits.temp_warm_fastchg_current_ma = -EINVAL;
	}

	/*>55 C*/
	rc = of_property_read_u32(node, "qcom,hot_bat_decidegc", &chip->limits.hot_bat_decidegc);
	if (rc < 0) {
		chip->limits.hot_bat_decidegc = -EINVAL;
	}

	/*non standard battery*/
	rc = of_property_read_u32(node, "qcom,non_standard_vfloat_mv",
				  &chip->limits.non_standard_vfloat_mv);
	if (rc < 0) {
		chip->limits.non_standard_vfloat_mv = -EINVAL;
	}
	rc = of_property_read_u32(node, "qcom,non_standard_fastchg_current_ma",
				  &chip->limits.non_standard_fastchg_current_ma);
	if (rc < 0) {
		chip->limits.non_standard_fastchg_current_ma = -EINVAL;
	}

	/*vfloat_sw_limit*/
	rc = of_property_read_u32(node, "qcom,non_standard_vfloat_sw_limit",
				  &chip->limits.non_standard_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.non_standard_vfloat_sw_limit = 3960;
	}

	rc = of_property_read_u32(node, "qcom,cold_vfloat_sw_limit",
				  &chip->limits.cold_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.cold_vfloat_sw_limit = 3960;
	}

	rc = of_property_read_u32(node, "qcom,little_cold_vfloat_sw_limit",
				  &chip->limits.little_cold_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.little_cold_vfloat_sw_limit = 4330;
	}

	rc = of_property_read_u32(node, "qcom,cool_vfloat_sw_limit",
				  &chip->limits.cool_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.cool_vfloat_sw_limit = 4330;
	}

	rc = of_property_read_u32(node, "qcom,little_cool_vfloat_sw_limit",
				  &chip->limits.little_cool_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.little_cool_vfloat_sw_limit = 4330;
	}

	rc = of_property_read_u32(node, "qcom,normal_vfloat_sw_limit",
				  &chip->limits.normal_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.normal_vfloat_sw_limit = 4330;
	}

	rc = of_property_read_u32(node, "qcom,warm_vfloat_sw_limit",
				  &chip->limits.warm_vfloat_sw_limit);
	if (rc < 0) {
		chip->limits.warm_vfloat_sw_limit = 4060;
	}

	/*vfloat_over_sw_limit*/
	chip->limits.sw_vfloat_over_protect_enable =
		of_property_read_bool(node, "qcom,sw_vfloat_over_protect_enable");

	rc = of_property_read_u32(node, "qcom,non_standard_vfloat_over_sw_limit",
				  &chip->limits.non_standard_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.non_standard_vfloat_over_sw_limit = 3980;
	}

	rc = of_property_read_u32(node, "qcom,cold_vfloat_over_sw_limit",
				  &chip->limits.cold_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.cold_vfloat_over_sw_limit = 3980;
	}

	rc = of_property_read_u32(node, "qcom,little_cold_vfloat_over_sw_limit",
				  &chip->limits.little_cold_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.little_cold_vfloat_over_sw_limit = 4390;
	}

	rc = of_property_read_u32(node, "qcom,cool_vfloat_over_sw_limit",
				  &chip->limits.cool_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.cool_vfloat_over_sw_limit = 4390;
	}

	rc = of_property_read_u32(node, "qcom,little_cool_vfloat_over_sw_limit",
				  &chip->limits.little_cool_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.little_cool_vfloat_over_sw_limit = 4390;
	}

	rc = of_property_read_u32(node, "qcom,normal_vfloat_over_sw_limit",
				  &chip->limits.normal_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.normal_vfloat_over_sw_limit = 4390;
	}

	rc = of_property_read_u32(node, "qcom,warm_vfloat_over_sw_limit",
				  &chip->limits.warm_vfloat_over_sw_limit);
	if (rc < 0) {
		chip->limits.warm_vfloat_over_sw_limit = 4080;
	}

	rc = of_property_read_u32(node, "qcom,max_chg_time_sec", &chip->limits.max_chg_time_sec);
	if (rc < 0) {
		chip->limits.max_chg_time_sec = 36000;
	}
	rc = of_property_read_u32(node, "qcom,charger_hv_thr", &chip->limits.charger_hv_thr);
	if (rc < 0) {
		chip->limits.charger_hv_thr = 5800;
	}
	rc = of_property_read_u32(node, "qcom,charger_lv_thr", &chip->limits.charger_lv_thr);
	if (rc < 0) {
		chip->limits.charger_lv_thr = 3400;
	}
	rc = of_property_read_u32(node, "qcom,vbatt_full_thr", &chip->limits.vbatt_full_thr);
	if (rc < 0) {
		chip->limits.vbatt_full_thr = 4400;
	}
	rc = of_property_read_u32(node, "qcom,vbatt_hv_thr", &chip->limits.vbatt_hv_thr);
	if (rc < 0) {
		chip->limits.vbatt_hv_thr = 4500;
	}
	rc = of_property_read_u32(node, "qcom,vfloat_step_mv", &chip->limits.vfloat_step_mv);
	if (rc < 0) {
		chip->limits.vfloat_step_mv = 16;
	}
	rc = of_property_read_u32(node, "qcom,batt_capacity_mah", &chip->batt_capacity_mah);
	if (rc < 0) {
		chip->batt_capacity_mah = 2000;
	}

	chip->suspend_after_full = of_property_read_bool(node, "qcom,suspend_after_full");
	chip->check_batt_full_by_sw = of_property_read_bool(node, "qcom,check_batt_full_by_sw");
	chip->external_gauge = of_property_read_bool(node, "qcom,external_gauge");
	chip->fg_bcl_poll = of_property_read_bool(node, "qcom,fg_bcl_poll_enable");
	chip->chg_ctrl_by_lcd = of_property_read_bool(node, "qcom,chg_ctrl_by_lcd");

	return 0;
}

static void oppo_chg_set_charging_current(struct oppo_chg_chip *chip)
{
	int charging_current;

	switch (chip->tbatt_status) {
	case BATTERY_STATUS__INVALID:
	case BATTERY_STATUS__REMOVED:
	case BATTERY_STATUS__LOW_TEMP:
	case BATTERY_STATUS__HIGH_TEMP:
		return;
	case BATTERY_STATUS__COLD_TEMP:
		charging_current = chip->limits.temp_cold_fastchg_current_ma;
		break;
	case BATTERY_STATUS__LITTLE_COLD_TEMP:
		charging_current = chip->limits.temp_little_cold_fastchg_current_ma;
		break;
	case BATTERY_STATUS__COOL_TEMP:
		if (vbatt_higherthan_4180mv) {
			charging_current = chip->limits.temp_cool_fastchg_current_ma_low;
		} else {
			charging_current = chip->limits.temp_cool_fastchg_current_ma_high;
		}
		break;
	case BATTERY_STATUS__LITTLE_COOL_TEMP:
		charging_current = chip->limits.temp_little_cool_fastchg_current_ma;
		break;
	case BATTERY_STATUS__NORMAL:
		charging_current = chip->limits.temp_normal_fastchg_current_ma;
		break;
	case BATTERY_STATUS__WARM_TEMP:
		charging_current = chip->limits.temp_warm_fastchg_current_ma;
		break;
	default:
		break;
	}

	if ((!chip->authenticate) &&
	    (charging_current > chip->limits.non_standard_fastchg_current_ma)) {
		charging_current = chip->limits.non_standard_fastchg_current_ma;
	}

	if (charging_current == 0) {
		return;
	}
	chip->chg_ops->charging_current_write_fast(chip, charging_current);
}

static void oppo_chg_set_input_current_limit(struct oppo_chg_chip *chip)
{
	int current_limit = 0;

	switch (chip->charger_type) {
	case POWER_SUPPLY_TYPE_USB:
		current_limit = chip->limits.input_current_usb_ma;
		break;
	default:
		return;
	}

	if ((chip->chg_ctrl_by_lcd) && (chip->led_on) &&
	    (current_limit > chip->limits.input_current_led_ma)) {
		current_limit = chip->limits.input_current_led_ma;
	}

	if ((chip->led_on) && (chip->limits.input_current_led_ma_overtemp != -EINVAL) &&
	    (chip->overtemp_status) &&
	    (current_limit > chip->limits.input_current_led_ma_overtemp)) {
		current_limit = chip->limits.input_current_led_ma_overtemp;
	}
	chip->chg_ops->input_current_write(chip, current_limit);
}
static int oppo_chg_get_float_voltage(struct oppo_chg_chip *chip)
{
	int flv = chip->limits.temp_normal_vfloat_mv_normalchg;
	switch (chip->tbatt_status) {
	case BATTERY_STATUS__INVALID:
	case BATTERY_STATUS__REMOVED:
	case BATTERY_STATUS__LOW_TEMP:
	case BATTERY_STATUS__HIGH_TEMP:
		return flv;
	case BATTERY_STATUS__COLD_TEMP:
		flv = chip->limits.temp_cold_vfloat_mv;
		break;
	case BATTERY_STATUS__LITTLE_COLD_TEMP:
		flv = chip->limits.temp_little_cold_vfloat_mv;
		break;
	case BATTERY_STATUS__COOL_TEMP:
		flv = chip->limits.temp_cool_vfloat_mv;
		break;
	case BATTERY_STATUS__LITTLE_COOL_TEMP:
		flv = chip->limits.temp_little_cool_vfloat_mv;
		break;
	case BATTERY_STATUS__NORMAL:
		flv = chip->limits.temp_normal_vfloat_mv_normalchg;
		break;
	case BATTERY_STATUS__WARM_TEMP:
		flv = chip->limits.temp_warm_vfloat_mv;
		break;
	default:
		break;
	}

	return flv;
}

static void oppo_chg_set_float_voltage(struct oppo_chg_chip *chip)
{
	int flv = oppo_chg_get_float_voltage(chip);

	if ((!chip->authenticate) && (flv > chip->limits.non_standard_vfloat_mv)) {
		flv = chip->limits.non_standard_vfloat_mv;
	}

	chip->chg_ops->float_voltage_write(chip, flv);
	chip->limits.vfloat_sw_set = flv;
}

#define VFLOAT_OVER_NUM 2
static void oppo_chg_vfloat_over_check(struct oppo_chg_chip *chip)
{
	if (chip->charging_state == CHARGING_STATUS_FULL) {
		return;
	}
	if (chip->check_batt_full_by_sw && (chip->limits.sw_vfloat_over_protect_enable == false)) {
		return;
	}
	if (chip->limits.sw_vfloat_over_protect_enable) {
		if ((chip->batt_volt >= chip->limits.cold_vfloat_over_sw_limit &&
		     chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) ||
		    (chip->batt_volt >= chip->limits.little_cold_vfloat_over_sw_limit &&
		     chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) ||
		    (chip->batt_volt >= chip->limits.cool_vfloat_over_sw_limit &&
		     chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) ||
		    (chip->batt_volt >= chip->limits.little_cool_vfloat_over_sw_limit &&
		     chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) ||
		    (chip->batt_volt >= chip->limits.normal_vfloat_over_sw_limit &&
		     chip->tbatt_status == BATTERY_STATUS__NORMAL) ||
		    (chip->batt_volt >= chip->limits.warm_vfloat_over_sw_limit &&
		     chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) ||
		    (!chip->authenticate &&
		     (chip->batt_volt >= chip->limits.non_standard_vfloat_over_sw_limit))) {
			chip->limits.vfloat_over_counts++;
			if (chip->limits.vfloat_over_counts > VFLOAT_OVER_NUM) {
				chip->limits.vfloat_over_counts = 0;
				chip->limits.vfloat_sw_set -= chip->limits.vfloat_step_mv;
				chip->chg_ops->float_voltage_write(chip,
								   chip->limits.vfloat_sw_set);
			}
		} else {
			chip->limits.vfloat_over_counts = 0;
		}

		return;
	}
	if ((chip->batt_volt >= chip->limits.cold_vfloat_sw_limit &&
	     chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) ||
	    (chip->batt_volt >= chip->limits.little_cold_vfloat_sw_limit &&
	     chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) ||
	    (chip->batt_volt >= chip->limits.cool_vfloat_sw_limit &&
	     chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) ||
	    (chip->batt_volt >= chip->limits.little_cool_vfloat_sw_limit &&
	     chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) ||
	    (chip->batt_volt >= chip->limits.normal_vfloat_sw_limit &&
	     chip->tbatt_status == BATTERY_STATUS__NORMAL) ||
	    (chip->batt_volt >= chip->limits.warm_vfloat_sw_limit &&
	     chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) ||
	    (!chip->authenticate &&
	     (chip->batt_volt >= chip->limits.non_standard_vfloat_sw_limit))) {
		chip->limits.vfloat_over_counts++;
		if (chip->limits.vfloat_over_counts > VFLOAT_OVER_NUM) {
			chip->limits.vfloat_over_counts = 0;
			chip->limits.vfloat_sw_set -= chip->limits.vfloat_step_mv;
			chip->chg_ops->float_voltage_write(chip, chip->limits.vfloat_sw_set);
		}
	} else {
		chip->limits.vfloat_over_counts = 0;
	}
}

static void oppo_chg_check_vbatt_higher_than_4180mv(struct oppo_chg_chip *chip)
{
	static bool vol_high_pre = false;
	static int lower_count = 0, higher_count = 0;

	if (chip->tbatt_status != BATTERY_STATUS__COOL_TEMP) {
		vbatt_higherthan_4180mv = false;
		vol_high_pre = false;
		lower_count = 0;
		higher_count = 0;
		return;
	}

	if (chip->batt_volt > 4180) {
		higher_count++;
		if (higher_count > 2) {
			lower_count = 0;
			higher_count = 3;
			vbatt_higherthan_4180mv = true;
		}
	} else if (vbatt_higherthan_4180mv) {
		if (chip->batt_volt < 4000) {
			lower_count++;
			if (lower_count > 2) {
				lower_count = 3;
				higher_count = 0;
				vbatt_higherthan_4180mv = false;
			}
		}
	}

	/*chip->tbatt_status,chip->batt_volt,vol_high_pre,vbatt_higherthan_4180mv);*/
	if (vol_high_pre != vbatt_higherthan_4180mv) {
		vol_high_pre = vbatt_higherthan_4180mv;
		oppo_chg_set_charging_current(chip);
	}
}

void oppo_chg_turn_on_charging(struct oppo_chg_chip *chip)
{
	if (!chip->authenticate) {
		return;
	}
	chip->chg_ops->hardware_init(chip);
	if (chip->check_batt_full_by_sw) {
		chip->chg_ops->set_charging_term_disable(chip);
	}
	oppo_chg_check_tbatt_status(chip);
	oppo_chg_set_float_voltage(chip);
	oppo_chg_set_charging_current(chip);
	oppo_chg_set_input_current_limit(chip);
	chip->chg_ops->term_current_set(chip, chip->limits.iterm_ma);
}

void oppo_chg_turn_off_charging(struct oppo_chg_chip *chip)
{
	switch (chip->tbatt_status) {
	case BATTERY_STATUS__INVALID:
	case BATTERY_STATUS__REMOVED:
	case BATTERY_STATUS__LOW_TEMP:
		break;
	case BATTERY_STATUS__HIGH_TEMP:
		break;
	case BATTERY_STATUS__COLD_TEMP:
		break;
	case BATTERY_STATUS__LITTLE_COLD_TEMP:
	case BATTERY_STATUS__COOL_TEMP:
		chip->chg_ops->charging_current_write_fast(
			chip, chip->limits.temp_cold_fastchg_current_ma);
		msleep(50);
		break;
	case BATTERY_STATUS__LITTLE_COOL_TEMP:
	case BATTERY_STATUS__NORMAL:
		chip->chg_ops->charging_current_write_fast(
			chip, chip->limits.temp_cool_fastchg_current_ma_high);
		msleep(50);
		chip->chg_ops->charging_current_write_fast(
			chip, chip->limits.temp_cold_fastchg_current_ma);
		msleep(50);
		break;
	case BATTERY_STATUS__WARM_TEMP:
		chip->chg_ops->charging_current_write_fast(
			chip, chip->limits.temp_cold_fastchg_current_ma);
		msleep(50);
		break;
	default:
		break;
	}
	chip->chg_ops->charging_disable(chip);
}

static int oppo_chg_check_suspend_or_disable(struct oppo_chg_chip *chip)
{
	if (chip->suspend_after_full) {
		if ((chip->tbatt_status == BATTERY_STATUS__HIGH_TEMP ||
		     chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) &&
		    (chip->batt_volt < 4250)) {
			return CHG_DISABLE;
		} else {
			return CHG_SUSPEND;
		}
	} else {
		return CHG_DISABLE;
	}
}

static void oppo_chg_voter_charging_start(struct oppo_chg_chip *chip, OPPO_CHG_STOP_VOTER voter)
{
	chip->chging_on = true;
	chip->stop_voter &= ~(int)voter;
	oppo_chg_turn_on_charging(chip);

	switch (voter) {
	case CHG_STOP_VOTER__FULL:
		chip->charging_state = CHARGING_STATUS_CCCV;
		chip->chg_ops->charger_unsuspend(chip);
		chip->chg_ops->charging_enable(chip);
		break;
	case CHG_STOP_VOTER__VCHG_ABNORMAL:
		chip->charging_state = CHARGING_STATUS_CCCV;
		chip->chg_ops->charger_unsuspend(chip);
		break;
	case CHG_STOP_VOTER__BATTTEMP_ABNORMAL:
	case CHG_STOP_VOTER__VBAT_TOO_HIGH:
	case CHG_STOP_VOTER__MAX_CHGING_TIME:
		chip->charging_state = CHARGING_STATUS_CCCV;
		break;
	default:
		break;
	}
}

static void oppo_chg_voter_charging_stop(struct oppo_chg_chip *chip, OPPO_CHG_STOP_VOTER voter)
{
	chip->chging_on = false;
	chip->stop_voter |= (int)voter;

	switch (voter) {
	case CHG_STOP_VOTER__FULL:
		chip->charging_state = CHARGING_STATUS_FULL;
		if (oppo_chg_check_suspend_or_disable(chip) == CHG_SUSPEND) {
			chip->chg_ops->charger_suspend(chip);
		} else {
			oppo_chg_turn_off_charging(chip);
		}
		break;
	case CHG_STOP_VOTER__VCHG_ABNORMAL:
		chip->charging_state = CHARGING_STATUS_FAIL;
		chip->total_time = 0;
		chip->chg_ops->charger_suspend(chip);
		oppo_chg_turn_off_charging(chip);
		break;
	case CHG_STOP_VOTER__BATTTEMP_ABNORMAL:
	case CHG_STOP_VOTER__VBAT_TOO_HIGH:
		chip->charging_state = CHARGING_STATUS_FAIL;
		chip->total_time = 0;
		oppo_chg_turn_off_charging(chip);
		break;
	case CHG_STOP_VOTER__MAX_CHGING_TIME:
		chip->charging_state = CHARGING_STATUS_FAIL;
		oppo_chg_turn_off_charging(chip);
		break;
	default:
		break;
	}
}

#define HYSTERISIS_DECIDEGC 20
#define TBATT_PRE_SHAKE_INVALID 999
static void battery_temp_anti_shake_handle(struct oppo_chg_chip *chip)
{
	int tbatt_cur_shake = chip->temperature, low_shake = 0, high_shake = 0;

	if (tbatt_cur_shake > chip->tbatt_pre_shake) { /*get warm*/
		low_shake = -HYSTERISIS_DECIDEGC;
		high_shake = 0;
	} else { /*get cool*/
		low_shake = 0;
		high_shake = HYSTERISIS_DECIDEGC;
	}

	if (chip->tbatt_status == BATTERY_STATUS__HIGH_TEMP) { /*>53C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound; /*-3C*/
		chip->limits.little_cold_bat_decidegc =
			chip->anti_shake_bound.little_cold_bound; /*0C*/
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound; /*5C*/
		chip->limits.little_cool_bat_decidegc =
			chip->anti_shake_bound.little_cool_bound; /*12C*/
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound; /*16C*/
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound; /*45C*/
		chip->limits.hot_bat_decidegc =
			chip->anti_shake_bound.hot_bound + low_shake; /*53C*/
	} else if (chip->tbatt_status == BATTERY_STATUS__LOW_TEMP) { /*<-3C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound + high_shake;
		chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
		chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	} else if (chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) { /*-3C~0C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound + low_shake;
		chip->limits.little_cold_bat_decidegc =
			chip->anti_shake_bound.little_cold_bound + high_shake;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
		chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) { /*0C-5C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
		chip->limits.little_cold_bat_decidegc =
			chip->anti_shake_bound.little_cold_bound + low_shake;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound + high_shake;
		chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	} else if (chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) { /*5C~12C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
		chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound + low_shake;
		chip->limits.little_cool_bat_decidegc =
			chip->anti_shake_bound.little_cool_bound + high_shake;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) { /*12C~16C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
		chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
		chip->limits.little_cool_bat_decidegc =
			chip->anti_shake_bound.little_cool_bound + low_shake;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound + high_shake;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	} else if (chip->tbatt_status == BATTERY_STATUS__NORMAL) { /*16C~45C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
		chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
		chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	} else if (chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) { /*45C~53C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
		chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
		chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound + low_shake;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound + high_shake;
	} else { /*BATTERY_STATUS__REMOVED                                                                <-19C*/
		chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
		chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
		chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
		chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
		chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
		chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
		chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	}
	chip->tbatt_pre_shake = tbatt_cur_shake;
}

#define TEMP_CNT 1
static bool oppo_chg_check_tbatt_is_good(struct oppo_chg_chip *chip)
{
	static bool ret = true;
	static int temp_counts = 0;
	int batt_temp = chip->temperature;
	OPPO_CHG_TBATT_STATUS tbatt_status = chip->tbatt_status;

	if (batt_temp > chip->limits.hot_bat_decidegc ||
	    batt_temp < chip->limits.cold_bat_decidegc) {
		temp_counts++;
		if (temp_counts >= TEMP_CNT) {
			temp_counts = 0;
			ret = false;
			if (batt_temp <= chip->limits.removed_bat_decidegc) {
				tbatt_status = BATTERY_STATUS__REMOVED;
			} else if (batt_temp > chip->limits.hot_bat_decidegc) {
				tbatt_status = BATTERY_STATUS__HIGH_TEMP;
			} else {
				tbatt_status = BATTERY_STATUS__LOW_TEMP;
			}
		}
	} else {
		temp_counts = 0;
		ret = true;
		if (batt_temp >= chip->limits.warm_bat_decidegc) { /*45C*/
			tbatt_status = BATTERY_STATUS__WARM_TEMP;
		} else if (batt_temp >= chip->limits.normal_bat_decidegc) { /*16C*/
			tbatt_status = BATTERY_STATUS__NORMAL;
		} else if (batt_temp >= chip->limits.little_cool_bat_decidegc) { /*12C*/
			tbatt_status = BATTERY_STATUS__LITTLE_COOL_TEMP;
		} else if (batt_temp >= chip->limits.cool_bat_decidegc) { /*5C*/
			tbatt_status = BATTERY_STATUS__COOL_TEMP;
		} else if (batt_temp >= chip->limits.little_cold_bat_decidegc) { /*0C*/
			tbatt_status = BATTERY_STATUS__LITTLE_COLD_TEMP;
		} else if (batt_temp >= chip->limits.cold_bat_decidegc) { /*-3C*/
			tbatt_status = BATTERY_STATUS__COLD_TEMP;
		} else {
			tbatt_status = BATTERY_STATUS__COLD_TEMP;
		}
	}

	if (tbatt_status == BATTERY_STATUS__REMOVED) {
		chip->batt_exist = false;
	} else {
		chip->batt_exist = true;
	}

	if (chip->tbatt_pre_shake == TBATT_PRE_SHAKE_INVALID) {
		chip->tbatt_pre_shake = batt_temp;
	}

	if (tbatt_status != chip->tbatt_status) {
		chip->tbatt_status = tbatt_status;
		oppo_chg_set_float_voltage(chip);
		oppo_chg_set_charging_current(chip);
		battery_temp_anti_shake_handle(chip);
	}
	return ret;
}

static void oppo_chg_check_tbatt_status(struct oppo_chg_chip *chip)
{
	int batt_temp = chip->temperature;
	OPPO_CHG_TBATT_STATUS tbatt_status = chip->tbatt_status;

	if (batt_temp > chip->limits.hot_bat_decidegc) { /*53C*/
		tbatt_status = BATTERY_STATUS__HIGH_TEMP;
	} else if (batt_temp >= chip->limits.warm_bat_decidegc) { /*45C*/
		tbatt_status = BATTERY_STATUS__WARM_TEMP;
	} else if (batt_temp >= chip->limits.normal_bat_decidegc) { /*16C*/
		tbatt_status = BATTERY_STATUS__NORMAL;
	} else if (batt_temp >= chip->limits.little_cool_bat_decidegc) { /*12C*/
		tbatt_status = BATTERY_STATUS__LITTLE_COOL_TEMP;
	} else if (batt_temp >= chip->limits.cool_bat_decidegc) { /*5C*/
		tbatt_status = BATTERY_STATUS__COOL_TEMP;
	} else if (batt_temp >= chip->limits.little_cold_bat_decidegc) { /*0C*/
		tbatt_status = BATTERY_STATUS__LITTLE_COLD_TEMP;
	} else if (batt_temp >= chip->limits.cold_bat_decidegc) { /*-3C*/
		tbatt_status = BATTERY_STATUS__COLD_TEMP;
	} else if (batt_temp > chip->limits.removed_bat_decidegc) { /*-20C*/
		tbatt_status = BATTERY_STATUS__LOW_TEMP;
	} else {
		tbatt_status = BATTERY_STATUS__REMOVED;
	}
	if (tbatt_status == BATTERY_STATUS__REMOVED) {
		chip->batt_exist = false;
	} else {
		chip->batt_exist = true;
	}
	chip->tbatt_status = tbatt_status;

	if (batt_temp >= chip->limits.overtemp_bat_decidegc) {
		chip->overtemp_status = true;
		chip->limits.overtemp_bat_decidegc =
			chip->anti_shake_bound.overtemp_bound - HYSTERISIS_DECIDEGC;
	}
}

#define VCHG_CNT 1
static bool oppo_chg_check_vchg_is_good(struct oppo_chg_chip *chip)
{
	static bool ret = true;
	static int vchg_counts = 0;
	int chg_volt = chip->charger_volt;
	OPPO_CHG_VCHG_STATUS vchg_status = chip->vchg_status;

	if (chg_volt > chip->limits.charger_hv_thr) {
		vchg_counts++;
		if (vchg_counts >= VCHG_CNT) {
			vchg_counts = 0;
			ret = false;
			vchg_status = CHARGER_STATUS__VOL_HIGH;
		}
	} else {
		vchg_counts = 0;
		ret = true;
		vchg_status = CHARGER_STATUS__GOOD;
	}

	if (vchg_status != chip->vchg_status) {
		chip->vchg_status = vchg_status;
	}

	return ret;
}

#define VBAT_CNT 1

static bool oppo_chg_check_vbatt_is_good(struct oppo_chg_chip *chip)
{
	static bool ret = true;
	static int vbat_counts = 0;
	int batt_volt = chip->batt_volt;

	if (batt_volt >= chip->limits.vbatt_hv_thr) {
		vbat_counts++;
		if (vbat_counts >= VBAT_CNT) {
			vbat_counts = 0;
			ret = false;
			chip->vbatt_over = true;
		}
	} else {
		vbat_counts = 0;
		ret = true;
		chip->vbatt_over = false;
	}

	return ret;
}

static bool oppo_chg_check_time_is_good(struct oppo_chg_chip *chip)
{
	if (chip->limits.max_chg_time_sec < 0) {
		chip->chging_over_time = false;
		return true;
	}
	if (chip->total_time >= chip->limits.max_chg_time_sec) {
		chip->total_time = chip->limits.max_chg_time_sec;
		chip->chging_over_time = true;
		return false;
	} else {
		chip->chging_over_time = false;
		return true;
	}
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void chg_early_suspend(struct early_suspend *h)
{
	if (g_oppo_chg_chip) {
		g_oppo_chg_chip->led_on = false;
		g_oppo_chg_chip->led_status_change = true;
	}
}

static void chg_late_resume(struct early_suspend *h)
{
	if (g_oppo_chg_chip) {
		g_oppo_chg_chip->led_on = true;
		g_oppo_chg_chip->led_status_change = true;
	}
}

void oppo_chg_set_led_status(bool val)
{
	/*Do nothing*/
}
EXPORT_SYMBOL(oppo_chg_set_led_status);
#elif defined(CONFIG_FB)
static int fb_notifier_callback(struct notifier_block *nb, unsigned long event, void *data)
{
	int blank;
	struct fb_event *evdata = data;

	if (!g_oppo_chg_chip) {
		return 0;
	}
	if (evdata && evdata->data) {
		if (event == FB_EVENT_BLANK) {
			blank = *(int *)evdata->data;
			if (blank == FB_BLANK_UNBLANK) {
				g_oppo_chg_chip->led_on = true;
				g_oppo_chg_chip->led_status_change = true;
			} else if (blank == FB_BLANK_POWERDOWN) {
				g_oppo_chg_chip->led_on = false;
				g_oppo_chg_chip->led_status_change = true;
			}
		}
	}
	return 0;
}

void oppo_chg_set_led_status(bool val)
{
	/*Do nothing*/
}
EXPORT_SYMBOL(oppo_chg_set_led_status);
#else
void oppo_chg_set_led_status(bool val)
{
	if (!g_oppo_chg_chip) {
		return;
	} else {
		g_oppo_chg_chip->led_on = val;
		g_oppo_chg_chip->led_status_change = true;
	}
}
EXPORT_SYMBOL(oppo_chg_set_led_status);
#endif

static void oppo_chg_check_led_on_ichging(struct oppo_chg_chip *chip)
{
	if (chip->led_status_change) {
		chip->led_status_change = false;
		oppo_chg_set_input_current_limit(chip);
	}
}

static void oppo_chg_check_temp_ichging(struct oppo_chg_chip *chip)
{
	int batt_temp = chip->temperature;

	if (batt_temp >= chip->limits.overtemp_bat_decidegc) {
		if (!chip->overtemp_status) {
			chip->overtemp_status = true;
			chip->limits.overtemp_bat_decidegc =
				chip->anti_shake_bound.overtemp_bound - HYSTERISIS_DECIDEGC;
			oppo_chg_set_input_current_limit(chip);
		}
	} else {
		if (chip->overtemp_status) {
			chip->overtemp_status = false;
			chip->limits.overtemp_bat_decidegc = chip->anti_shake_bound.overtemp_bound;
			oppo_chg_set_input_current_limit(chip);
		}
	}
}

static void oppo_chg_battery_authenticate_check(struct oppo_chg_chip *chip)
{
	static bool charger_exist_pre = false;

	if (charger_exist_pre ^ chip->charger_exist) {
		charger_exist_pre = chip->charger_exist;
		if (chip->charger_exist && !chip->authenticate) {
			chip->authenticate = oppo_gauge_get_batt_authenticate();
		}
	}
}

void oppo_chg_variables_reset(struct oppo_chg_chip *chip, bool in)
{
	if (in) {
		chip->charger_exist = true;
		chip->chging_on = true;
	} else {
		chip->charger_exist = false;
		chip->chging_on = false;
		chip->charger_type = POWER_SUPPLY_TYPE_UNKNOWN;
	}

	/*chip->charger_volt = 5000;*/
	chip->vchg_status = CHARGER_STATUS__GOOD;

	chip->batt_full = false;
	chip->tbatt_status = BATTERY_STATUS__NORMAL;
	chip->tbatt_pre_shake = TBATT_PRE_SHAKE_INVALID;
	chip->vbatt_over = 0;

	chip->total_time = 0;
	chip->chging_over_time = 0;
	chip->in_rechging = 0;
	/*chip->batt_volt = 0;*/
	/*chip->temperature = 0;*/

	chip->stop_voter = 0x00;
	chip->charging_state = CHARGING_STATUS_CCCV;
	chip->notify_code = 0;
	chip->notify_flag = 0;
	chip->limits.cold_bat_decidegc = chip->anti_shake_bound.cold_bound;
	chip->limits.little_cold_bat_decidegc = chip->anti_shake_bound.little_cold_bound;
	chip->limits.cool_bat_decidegc = chip->anti_shake_bound.cool_bound;
	chip->limits.little_cool_bat_decidegc = chip->anti_shake_bound.little_cool_bound;
	chip->limits.normal_bat_decidegc = chip->anti_shake_bound.normal_bound;
	chip->limits.warm_bat_decidegc = chip->anti_shake_bound.warm_bound;
	chip->limits.hot_bat_decidegc = chip->anti_shake_bound.hot_bound;
	chip->limits.vfloat_over_counts = 0;

	chip->limits.overtemp_bat_decidegc = chip->anti_shake_bound.overtemp_bound;
	chip->overtemp_status = false;
	chip->pmic_spmi.aicl_suspend = false;

	oppo_chg_battery_authenticate_check(chip);
}

static void oppo_chg_variables_init(struct oppo_chg_chip *chip)
{
	chip->charger_exist = false;
	chip->chging_on = false;
	chip->charger_type = POWER_SUPPLY_TYPE_UNKNOWN;
	chip->charger_volt = 0;
	chip->vchg_status = CHARGER_STATUS__GOOD;
	chip->prop_status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	chip->batt_exist = true;
	chip->batt_full = false;
	chip->tbatt_status = BATTERY_STATUS__NORMAL;
	chip->vbatt_over = 0;
	chip->total_time = 0;
	chip->chging_over_time = 0;
	chip->in_rechging = 0;
	chip->batt_volt = 3800;
	chip->icharging = 0;
	chip->temperature = 250;
	chip->soc = 0;
	chip->ui_soc = 50;
	chip->notify_code = 0;
	chip->notify_flag = 0;
	chip->request_power_off = 0;
	chip->tbatt_pre_shake = TBATT_PRE_SHAKE_INVALID;
	chip->led_on = true;
	chip->stop_voter = 0x00;
	chip->charging_state = CHARGING_STATUS_CCCV;
	chip->anti_shake_bound.cold_bound = chip->limits.cold_bat_decidegc;
	chip->anti_shake_bound.little_cold_bound = chip->limits.little_cold_bat_decidegc;
	chip->anti_shake_bound.cool_bound = chip->limits.cool_bat_decidegc;
	chip->anti_shake_bound.little_cool_bound = chip->limits.little_cool_bat_decidegc;
	chip->anti_shake_bound.normal_bound = chip->limits.normal_bat_decidegc;
	chip->anti_shake_bound.warm_bound = chip->limits.warm_bat_decidegc;
	chip->anti_shake_bound.hot_bound = chip->limits.hot_bat_decidegc;
	chip->anti_shake_bound.overtemp_bound = chip->limits.overtemp_bat_decidegc;
	chip->overtemp_status = false;
	chip->limits.vfloat_over_counts = 0;
}

static void oppo_chg_fail_action(struct oppo_chg_chip *chip)
{
	chip->charging_state = CHARGING_STATUS_FAIL;
	chip->chging_on = false;

	chip->batt_full = false;
	chip->in_rechging = 0;
}

#define D_RECHGING_CNT 5
static void oppo_chg_check_rechg_status(struct oppo_chg_chip *chip)
{
	int recharging_vol;
	int nbat_vol = chip->batt_volt;
	static int rechging_cnt = 0;

	if (chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) {
		recharging_vol = oppo_chg_get_float_voltage(chip) - 300;
	} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) {
		recharging_vol = oppo_chg_get_float_voltage(chip) - 200;
	} else {
		recharging_vol = oppo_chg_get_float_voltage(chip);
		if (recharging_vol > chip->limits.temp_normal_vfloat_mv_normalchg) {
			recharging_vol = chip->limits.temp_normal_vfloat_mv_normalchg;
		}
		recharging_vol = recharging_vol - chip->limits.recharge_mv;
	}

	if (!chip->authenticate) {
		recharging_vol = chip->limits.non_standard_vfloat_sw_limit - 400;
	}
	if (nbat_vol <= recharging_vol) {
		rechging_cnt++;
	} else {
		rechging_cnt = 0;
	}

	if (rechging_cnt > D_RECHGING_CNT) {
		rechging_cnt = 0;
		oppo_chg_voter_charging_start(chip, CHG_STOP_VOTER__FULL); /*now rechging!*/
		chip->in_rechging = true;
	}
}

static void oppo_chg_full_action(struct oppo_chg_chip *chip)
{
	oppo_chg_voter_charging_stop(chip, CHG_STOP_VOTER__FULL);
	/*chip->charging_state = CHARGING_STATUS_FULL;*/
	chip->batt_full = true;
	chip->total_time = 0;
	chip->limits.vfloat_over_counts = 0;
	oppo_chg_check_rechg_status(chip);
}

void oppo_charger_detect_check(struct oppo_chg_chip *chip)
{
	static bool charger_resumed = true;

	if (chip->chg_ops->check_chrdet_status()) {
		wake_lock(&chip->suspend_lock);
		if (chip->charger_type == POWER_SUPPLY_TYPE_UNKNOWN) {
			oppo_chg_variables_reset(chip, true);
			chip->charger_type = chip->chg_ops->get_charger_type();
			charger_resumed = chip->chg_ops->check_charger_resume(chip);
			oppo_chg_turn_on_charging(chip);
		} else {
			if (charger_resumed == false) {
				charger_resumed = chip->chg_ops->check_charger_resume(chip);
				oppo_chg_turn_on_charging(chip);
			}
		}
	} else {
		oppo_chg_variables_reset(chip, false);
		oppo_gauge_set_batt_full(false);
		if (chip->chg_ops->get_charging_enable(chip) == true) {
			oppo_chg_turn_off_charging(chip);
		}
		wake_unlock(&chip->suspend_lock);
	}
}

#define RETRY_COUNTS 12
static void oppo_chg_get_battery_data(struct oppo_chg_chip *chip)
{
	static int ui_soc_cp_flag = 0;
	static int soc_load = 0;
	int remain_100_thresh = 97;
	static int retry_counts = 0;

	chip->batt_volt = oppo_gauge_get_batt_mvolts() / 1000;
	chip->icharging = oppo_gauge_get_batt_current();
	chip->temperature = oppo_gauge_get_batt_temperature();
	chip->soc = oppo_gauge_get_batt_soc();
	chip->batt_fcc = oppo_gauge_get_batt_fcc();
	chip->batt_cc = oppo_gauge_get_batt_cc();
	chip->batt_soh = oppo_gauge_get_batt_soh();
	chip->batt_rm = oppo_gauge_get_remaining_capacity();
	chip->charger_volt = chip->chg_ops->get_charger_volt();

	if (ui_soc_cp_flag == 0) {
		if ((chip->soc < 0 || chip->soc > 100) && retry_counts < RETRY_COUNTS) {
			retry_counts++;
			chip->soc = 50;
			goto next;
		}

		ui_soc_cp_flag = 1;
		if (chip->chg_ops->get_rtc_soc() > 100) {
			soc_load = chip->soc;
		}
		else {
			soc_load = chip->chg_ops->get_rtc_soc();
		}
		chip->soc_load = soc_load;
		if ((chip->soc < 0 || chip->soc > 100) && soc_load > 0 && soc_load <= 100) {
			chip->soc = soc_load;
		}
		if ((soc_load != 0) && ((abs(soc_load - chip->soc)) <= 20)) {
			if (chip->suspend_after_full && chip->external_gauge) {
				remain_100_thresh = 95;
			} else if (chip->suspend_after_full && !chip->external_gauge) {
				remain_100_thresh = 94;
			} else if (!chip->suspend_after_full && chip->external_gauge) {
				remain_100_thresh = 97;
			} else if (!chip->suspend_after_full && !chip->external_gauge) {
				remain_100_thresh = 95;
			} else {
				remain_100_thresh = 97;
			}
			if (chip->soc < soc_load) {
				if (soc_load == 100 && chip->soc > remain_100_thresh) {
					chip->ui_soc = soc_load;
				} else {
					chip->ui_soc = soc_load - 1;
				}
			} else {
				chip->ui_soc = soc_load;
			}
		} else {
			chip->ui_soc = chip->soc;
			if (!chip->external_gauge && soc_load == 0 && chip->soc < 5) {
				chip->ui_soc = 0;
			}
		}
	}
next:
	return;
}

/*need to extend it*/
static void oppo_chg_set_aicl_point(struct oppo_chg_chip *chip)
{
	chip->chg_ops->set_aicl_point(chip, chip->batt_volt);
}

#define AICL_DELAY_15MIN 180
static void oppo_chg_check_aicl_input_limit(struct oppo_chg_chip *chip)
{
	static int aicl_delay_count = 0;

	if (chip->charging_state == CHARGING_STATUS_FAIL || chip->batt_full == true ||
	    ((chip->tbatt_status != BATTERY_STATUS__NORMAL) &&
	     (chip->tbatt_status != BATTERY_STATUS__LITTLE_COOL_TEMP)) ||
	    ((chip->ui_soc > 85) && (chip->pmic_spmi.aicl_suspend == false))) {
		aicl_delay_count = 0;
		return;
	}

	if (aicl_delay_count > AICL_DELAY_15MIN) {
		aicl_delay_count = 0;
		oppo_chg_set_input_current_limit(chip);
	} else if (chip->pmic_spmi.aicl_suspend == true && chip->charger_volt > 4450 &&
		   chip->charger_volt < 5800) {
		aicl_delay_count = 0;
		chip->chg_ops->rerun_aicl(chip);
		oppo_chg_set_input_current_limit(chip);

	} else {
		aicl_delay_count++;
	}
	if (chip->charger_type == POWER_SUPPLY_TYPE_USB) {
		chip->pmic_spmi.usb_hc_count++;
		if (chip->pmic_spmi.usb_hc_count >= 3) {
			chip->pmic_spmi.usb_hc_mode = true;
			chip->pmic_spmi.usb_hc_count = 3;
		}
	}
	if (chip->pmic_spmi.usb_hc_mode && !chip->pmic_spmi.hc_mode_flag) {
		oppo_chg_set_input_current_limit(chip);
		chip->pmic_spmi.hc_mode_flag = true;
	}
}

static void oppo_chg_aicl_check(struct oppo_chg_chip *chip)
{
	oppo_chg_set_aicl_point(chip);
	oppo_chg_check_aicl_input_limit(chip);
}

static void oppo_chg_protection_check(struct oppo_chg_chip *chip)
{
	if (false == oppo_chg_check_tbatt_is_good(chip)) {
		oppo_chg_voter_charging_stop(chip, CHG_STOP_VOTER__BATTTEMP_ABNORMAL);
	} else {
		if ((chip->stop_voter & CHG_STOP_VOTER__BATTTEMP_ABNORMAL) ==
		    CHG_STOP_VOTER__BATTTEMP_ABNORMAL) {
			oppo_chg_voter_charging_start(chip, CHG_STOP_VOTER__BATTTEMP_ABNORMAL);
		}
	}

	if (false == oppo_chg_check_vchg_is_good(chip)) {
		oppo_chg_voter_charging_stop(chip, CHG_STOP_VOTER__VCHG_ABNORMAL);
	} else {
		if ((chip->stop_voter & CHG_STOP_VOTER__VCHG_ABNORMAL) ==
		    CHG_STOP_VOTER__VCHG_ABNORMAL) {
			oppo_chg_voter_charging_start(chip, CHG_STOP_VOTER__VCHG_ABNORMAL);
		}
	}

#ifdef FEATURE_VBAT_PROTECT
	if (false == oppo_chg_check_vbatt_is_good(chip)) {
		oppo_chg_voter_charging_stop(chip, CHG_STOP_VOTER__VBAT_TOO_HIGH);
	}
#endif

	if (!oppo_chg_check_time_is_good(chip)) {
		oppo_chg_voter_charging_stop(chip, CHG_STOP_VOTER__MAX_CHGING_TIME);
	}

	oppo_chg_check_vbatt_higher_than_4180mv(chip);

	oppo_chg_vfloat_over_check(chip);

	if (chip->chg_ctrl_by_lcd) {
		oppo_chg_check_led_on_ichging(chip);
	}

	if (chip->limits.input_current_led_ma_overtemp != -EINVAL) {
		oppo_chg_check_temp_ichging(chip);
	}
}

static void battery_notify_tbat_check(struct oppo_chg_chip *chip)
{
	static int count = 0;
	if (BATTERY_STATUS__HIGH_TEMP == chip->tbatt_status) {
		if (chip->charger_exist) {
			chip->notify_code |= 1 << NOTIFY_BAT_OVER_TEMP;
		}
	}

	if (BATTERY_STATUS__LOW_TEMP == chip->tbatt_status) {
		if (chip->charger_exist) {
			chip->notify_code |= 1 << NOTIFY_BAT_LOW_TEMP;
		}
	}

	if (BATTERY_STATUS__REMOVED == chip->tbatt_status) {
		count++;

		if (count > 10) {
			count = 11;
			chip->notify_code |= 1 << NOTIFY_BAT_NOT_CONNECT;
		}
	} else {
		count = 0;
	}
}

static void battery_notify_authenticate_check(struct oppo_chg_chip *chip)
{
	if (!chip->authenticate) {
		chip->notify_code |= 1 << NOTIFY_BAT_NOT_CONNECT;
	}
}

static void battery_notify_vcharger_check(struct oppo_chg_chip *chip)
{
	if (CHARGER_STATUS__VOL_HIGH == chip->vchg_status) {
		chip->notify_code |= 1 << NOTIFY_CHARGER_OVER_VOL;
	}

	if (CHARGER_STATUS__VOL_LOW == chip->vchg_status) {
		chip->notify_code |= 1 << NOTIFY_CHARGER_LOW_VOL;
	}
}

static void battery_notify_vbat_check(struct oppo_chg_chip *chip)
{
	if (true == chip->vbatt_over) {
		chip->notify_code |= 1 << NOTIFY_BAT_OVER_VOL;

	} else {
		if ((chip->batt_full) && (chip->charger_exist)) {
			if (chip->tbatt_status == BATTERY_STATUS__WARM_TEMP &&
			    chip->ui_soc != 100) {
				chip->notify_code |= 1 << NOTIFY_BAT_FULL_PRE_HIGH_TEMP;
			} else if ((chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) &&
				   (chip->ui_soc != 100)) {
				chip->notify_code |= 1 << NOTIFY_BAT_FULL_PRE_LOW_TEMP;
			} else if (!chip->authenticate) {
				/*chip->notify_code |=  1 << NOTIFY_BAT_FULL_THIRD_BATTERY;*/
			} else {
				if (chip->ui_soc == 100) {
					chip->notify_code |= 1 << NOTIFY_BAT_FULL;
				}
			}
		}
	}
}

static void battery_notify_max_charging_time_check(struct oppo_chg_chip *chip)
{
	if (true == chip->chging_over_time) {
		chip->notify_code |= 1 << NOTIFY_CHGING_OVERTIME;
	}
}

static void battery_notify_flag_check(struct oppo_chg_chip *chip)
{
	if (chip->notify_code & (1 << NOTIFY_CHGING_OVERTIME)) {
		chip->notify_flag = NOTIFY_CHGING_OVERTIME;
	} else if (chip->notify_code & (1 << NOTIFY_CHARGER_OVER_VOL)) {
		chip->notify_flag = NOTIFY_CHARGER_OVER_VOL;
	} else if (chip->notify_code & (1 << NOTIFY_CHARGER_LOW_VOL)) {
		chip->notify_flag = NOTIFY_CHARGER_LOW_VOL;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_OVER_TEMP)) {
		chip->notify_flag = NOTIFY_BAT_OVER_TEMP;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_LOW_TEMP)) {
		chip->notify_flag = NOTIFY_BAT_LOW_TEMP;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_NOT_CONNECT)) {
		chip->notify_flag = NOTIFY_BAT_NOT_CONNECT;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_OVER_VOL)) {
		chip->notify_flag = NOTIFY_BAT_OVER_VOL;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_FULL_PRE_HIGH_TEMP)) {
		chip->notify_flag = NOTIFY_BAT_FULL_PRE_HIGH_TEMP;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_FULL_PRE_LOW_TEMP)) {
		chip->notify_flag = NOTIFY_BAT_FULL_PRE_LOW_TEMP;
	} else if (chip->notify_code & (1 << NOTIFY_BAT_FULL)) {
		chip->notify_flag = NOTIFY_BAT_FULL;
	} else {
		chip->notify_flag = 0;
	}
}

static void oppo_chg_battery_notify_check(struct oppo_chg_chip *chip)
{
	chip->notify_code = 0x0000;

	battery_notify_tbat_check(chip);

	battery_notify_authenticate_check(chip);

	battery_notify_vcharger_check(chip);

	battery_notify_vbat_check(chip);

	battery_notify_max_charging_time_check(chip);

	battery_notify_flag_check(chip);
}

int oppo_chg_get_prop_batt_health(struct oppo_chg_chip *chip)
{
	int bat_health = POWER_SUPPLY_HEALTH_GOOD;
	bool vbatt_over = chip->vbatt_over;
	OPPO_CHG_TBATT_STATUS tbatt_status = chip->tbatt_status;

	if (vbatt_over == true) {
		bat_health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	} else if (tbatt_status == BATTERY_STATUS__REMOVED) {
		bat_health = POWER_SUPPLY_HEALTH_DEAD;
	} else if (tbatt_status == BATTERY_STATUS__HIGH_TEMP) {
		bat_health = POWER_SUPPLY_HEALTH_OVERHEAT;
	} else if (tbatt_status == BATTERY_STATUS__LOW_TEMP) {
		bat_health = POWER_SUPPLY_HEALTH_COLD;
	} else {
		bat_health = POWER_SUPPLY_HEALTH_GOOD;
	}
	return bat_health;
}

static bool oppo_chg_soc_reduce_slow_when_1(struct oppo_chg_chip *chip)
{
	static int reduce_count = 0;
	int reduce_count_limit = 0;

	if (chip->batt_exist == false) {
		return false;
	}

	if (chip->charger_exist) {
		reduce_count_limit = 12;
	} else {
		reduce_count_limit = 4;
	}
	if (chip->batt_volt < 3410) {
		reduce_count++;
	} else {
		reduce_count = 0;
	}

	if (reduce_count > reduce_count_limit) {
		reduce_count = reduce_count_limit + 1;
		return true;
	} else {
		return false;
	}
}

#define SOC_SYNC_UP_RATE_10S 2
#define SOC_SYNC_UP_RATE_60S 12
#define SOC_SYNC_DOWN_RATE_300S 60
#define SOC_SYNC_DOWN_RATE_150S 30
#define SOC_SYNC_DOWN_RATE_90S 18
#define SOC_SYNC_DOWN_RATE_60S 12
#define SOC_SYNC_DOWN_RATE_40S 8
#define SOC_SYNC_DOWN_RATE_30S 6
#define SOC_SYNC_DOWN_RATE_15S 3
#define TEN_MINUTES 600

static void oppo_chg_update_ui_soc(struct oppo_chg_chip *chip)
{
	static int soc_down_count = 0, soc_up_count = 0, ui_soc_pre = 50;
	int soc_down_limit = 0, soc_up_limit = 0;
	unsigned long sleep_tm = 0, soc_reduce_margin = 0;
	bool vbatt_too_low = false;
	vbatt_lowerthan_3300mv = false;

	if (chip->ui_soc == 100) {
		soc_down_limit = SOC_SYNC_DOWN_RATE_300S;
	} else if (chip->ui_soc >= 95) {
		soc_down_limit = SOC_SYNC_DOWN_RATE_150S;
	} else if (chip->ui_soc >= 60) {
		soc_down_limit = SOC_SYNC_DOWN_RATE_60S;
	} else if (chip->charger_exist && chip->ui_soc == 1) {
		soc_down_limit = SOC_SYNC_DOWN_RATE_90S;
	} else {
		soc_down_limit = SOC_SYNC_DOWN_RATE_40S;
	}

	if (chip->batt_exist && chip->batt_volt < 3300 && chip->batt_volt > 2500) {
		soc_down_limit = SOC_SYNC_DOWN_RATE_15S;
		vbatt_too_low = true;
		vbatt_lowerthan_3300mv = true;
	}
	if (chip->batt_full) {
		soc_up_limit = SOC_SYNC_UP_RATE_60S;
	} else {
		soc_up_limit = SOC_SYNC_UP_RATE_10S;
	}
	if (chip->charger_exist && chip->batt_exist && chip->batt_full &&
	    chip->charger_type == POWER_SUPPLY_TYPE_USB) {
		chip->sleep_tm_sec = 0;
		if ((chip->tbatt_status == BATTERY_STATUS__NORMAL) ||
		    (chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) ||
		    (chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) ||
		    (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP)) {
			soc_down_count = 0;
			soc_up_count++;
			if (soc_up_count >= soc_up_limit) {
				soc_up_count = 0;
				chip->ui_soc++;
			}
			if (chip->ui_soc >= 100) {
				chip->ui_soc = 100;
				chip->prop_status = POWER_SUPPLY_STATUS_FULL;
			} else {
				chip->prop_status = POWER_SUPPLY_STATUS_CHARGING;
			}
		} else {
			chip->prop_status = POWER_SUPPLY_STATUS_FULL;
		}
	} else if (chip->charger_exist && chip->batt_exist &&
		   (CHARGING_STATUS_FAIL != chip->charging_state) &&
		   (chip->charger_type == POWER_SUPPLY_TYPE_USB)) {
		chip->sleep_tm_sec = 0;
		chip->prop_status = POWER_SUPPLY_STATUS_CHARGING;
		if (chip->soc == chip->ui_soc) {
			soc_down_count = 0;
			soc_up_count = 0;
		} else if (chip->soc > chip->ui_soc) {
			soc_down_count = 0;
			soc_up_count++;
			if (soc_up_count >= soc_up_limit) {
				soc_up_count = 0;
				chip->ui_soc++;
			}
		} else if (chip->soc < chip->ui_soc) {
			soc_up_count = 0;
			soc_down_count++;
			if (soc_down_count >= soc_down_limit) {
				soc_down_count = 0;
				chip->ui_soc--;
			}
		}
	} else {
		chip->prop_status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		soc_up_count = 0;
		if (chip->soc <= chip->ui_soc || vbatt_too_low) {
			if (soc_down_count > soc_down_limit) {
				soc_down_count = soc_down_limit + 1;
			} else {
				soc_down_count++;
			}
			sleep_tm = chip->sleep_tm_sec;
			if (chip->sleep_tm_sec > 0) {
				soc_reduce_margin = chip->sleep_tm_sec / TEN_MINUTES;
				if (soc_reduce_margin == 0) {
					if ((chip->ui_soc - chip->soc) > 2) {
						chip->ui_soc--;
						soc_down_count = 0;
						chip->sleep_tm_sec = 0;
					}
				} else if (soc_reduce_margin < (chip->ui_soc - chip->soc)) {
					chip->ui_soc -= soc_reduce_margin;
					soc_down_count = 0;
					chip->sleep_tm_sec = 0;
				} else if (soc_reduce_margin >= (chip->ui_soc - chip->soc)) {
					chip->ui_soc = chip->soc;
					soc_down_count = 0;
					chip->sleep_tm_sec = 0;
				}
			}
			if (soc_down_count >= soc_down_limit &&
			    (chip->soc < chip->ui_soc || vbatt_too_low)) {
				chip->sleep_tm_sec = 0;
				soc_down_count = 0;
				chip->ui_soc--;
			}
		}
	}

	if (chip->ui_soc < 2) {
		if (oppo_chg_soc_reduce_slow_when_1(chip) == true) {
			chip->ui_soc = 0;
		} else {
			chip->ui_soc = 1;
		}
	}
	if (chip->ui_soc != ui_soc_pre) {
		ui_soc_pre = chip->ui_soc;
		chip->chg_ops->set_rtc_soc(chip->ui_soc);
		if (chip->chg_ops->get_rtc_soc() != chip->ui_soc) {
			chip->chg_ops->set_rtc_soc(chip->ui_soc);
		}
	}
}
static void fg_update(struct oppo_chg_chip *chip)
{
	static int ui_soc_pre_fg = 50;
	static struct power_supply *bms_psy = NULL;
	if (!bms_psy) {
		bms_psy = power_supply_get_by_name("bms");
	}
	if (bms_psy) {
		if (chip->ui_soc != ui_soc_pre_fg) {
			power_supply_changed(bms_psy);
		}
		if (chip->ui_soc != ui_soc_pre_fg) {
			ui_soc_pre_fg = chip->ui_soc;
		}
	}
}
static void battery_update(struct oppo_chg_chip *chip)
{
	oppo_chg_update_ui_soc(chip);

	if (chip->fg_bcl_poll) {
		fg_update(chip);
	}
	power_supply_changed(chip->batt_psy);
}

static void oppo_chg_battery_update_status(struct oppo_chg_chip *chip)
{
	battery_update(chip);
}

#define FULL_COUNTS_SW 5
#define FULL_COUNTS_HW 3
static bool oppo_chg_check_vbatt_is_full_by_sw(struct oppo_chg_chip *chip)
{
	static bool ret_sw = false;
	static bool ret_hw = false;
	static int vbat_counts_sw = 0;
	static int vbat_counts_hw = 0;
	int vbatt_full_vol_sw = 0;
	int vbatt_full_vol_hw = 0;

	if (!chip->check_batt_full_by_sw) {
		return false;
	}
	if (!chip->charger_exist) {
		vbat_counts_sw = 0;
		vbat_counts_hw = 0;
		ret_sw = false;
		ret_hw = false;
		return false;
	}

	vbatt_full_vol_hw = oppo_chg_get_float_voltage(chip);
	if (chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) {
		vbatt_full_vol_sw = chip->limits.cold_vfloat_sw_limit;
		/*vbatt_full_vol_hw = chip->limits.temp_cold_vfloat_mv;*/
	} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) {
		vbatt_full_vol_sw = chip->limits.little_cold_vfloat_sw_limit;
		/*vbatt_full_vol_hw = chip->limits.temp_little_cold_vfloat_mv;*/
	} else if (chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) {
		vbatt_full_vol_sw = chip->limits.cool_vfloat_sw_limit;
		/*vbatt_full_vol_hw = chip->limits.temp_cool_vfloat_mv;*/
	} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) {
		vbatt_full_vol_sw = chip->limits.little_cool_vfloat_sw_limit;
		/*vbatt_full_vol_hw = chip->limits.temp_little_cool_vfloat_mv;*/
	} else if (chip->tbatt_status == BATTERY_STATUS__NORMAL) {
		vbatt_full_vol_sw = chip->limits.normal_vfloat_sw_limit;
		/*vbatt_full_vol_hw = chip->limits.temp_normal_vfloat_mv_normalchg;*/
	} else if (chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) {
		vbatt_full_vol_sw = chip->limits.warm_vfloat_sw_limit;
		/*vbatt_full_vol_hw = chip->limits.temp_warm_vfloat_mv;*/
	} else {
		vbat_counts_sw = 0;
		vbat_counts_hw = 0;
		ret_sw = 0;
		ret_hw = 0;
		return false;
	}
	if (!chip->authenticate) {
		vbatt_full_vol_sw = chip->limits.non_standard_vfloat_sw_limit;
		vbatt_full_vol_hw = chip->limits.non_standard_vfloat_mv;
	}
	/* use SW Vfloat to check */
	if (chip->batt_volt > vbatt_full_vol_sw) {
		if (chip->icharging < 0 && (chip->icharging * -1) <= chip->limits.iterm_ma) {
			vbat_counts_sw++;
			if (vbat_counts_sw > FULL_COUNTS_SW) {
				vbat_counts_sw = 0;
				ret_sw = true;
			}
		} else if (chip->icharging >= 0) {
			vbat_counts_sw++;
			if (vbat_counts_sw > FULL_COUNTS_SW * 2) {
				vbat_counts_sw = 0;
				ret_sw = true;
			}
		} else {
			vbat_counts_sw = 0;
			ret_sw = false;
		}
	} else {
		vbat_counts_sw = 0;
		ret_sw = false;
	}

	/* use HW Vfloat to check */
	if (chip->batt_volt >= vbatt_full_vol_hw + 18) {
		vbat_counts_hw++;
		if (vbat_counts_hw >= FULL_COUNTS_HW) {
			vbat_counts_hw = 0;
			ret_hw = true;
		}
	} else {
		vbat_counts_hw = 0;
		ret_hw = false;
	}

	if (ret_sw == true || ret_hw == true) {
		ret_sw = ret_hw = false;
		return true;
	} else {
		return false;
	}
}

#define FULL_DELAY_COUNTS 4
static void oppo_chg_check_status_full(struct oppo_chg_chip *chip)
{
	int is_batt_full = 0;
	static int fastchg_present_wait_count = 0;

	is_batt_full = chip->chg_ops->read_full(chip);
	fastchg_present_wait_count = 0;

	if ((is_batt_full == 1) || (chip->charging_state == CHARGING_STATUS_FULL) ||
	    oppo_chg_check_vbatt_is_full_by_sw(chip)) {
		oppo_chg_full_action(chip);
		if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP ||
		    chip->tbatt_status == BATTERY_STATUS__COOL_TEMP ||
		    chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP ||
		    chip->tbatt_status == BATTERY_STATUS__NORMAL) {
			oppo_gauge_set_batt_full(true);
		}
	} else if (chip->charging_state == CHARGING_STATUS_FAIL) {
		oppo_chg_fail_action(chip);
	} else {
		chip->charging_state = CHARGING_STATUS_CCCV;
	}
}

static void oppo_chg_print_log(struct oppo_chg_chip *chip)
{
}

#define CHARGER_ABNORMAL_DETECT_TIME 24
static void oppo_chg_critical_log(struct oppo_chg_chip *chip)
{
	static int chg_abnormal_count = 0;

	if (chip->charger_exist) {
		if (chip->stop_voter == 0 && chip->charger_type == POWER_SUPPLY_TYPE_USB &&
		    chip->soc <= 75 && chip->icharging >= -20) {
			chg_abnormal_count++;
			if (chg_abnormal_count >= CHARGER_ABNORMAL_DETECT_TIME) {
				chg_abnormal_count = CHARGER_ABNORMAL_DETECT_TIME;
				charger_abnormal_log = CRITICAL_LOG_UNABLE_CHARGING;
			}

		} else {
			chg_abnormal_count = 0;
		}
		if ((chip->stop_voter & CHG_STOP_VOTER__BATTTEMP_ABNORMAL) ==
		    CHG_STOP_VOTER__BATTTEMP_ABNORMAL) {
			charger_abnormal_log = CRITICAL_LOG_BATTTEMP_ABNORMAL;
		} else if ((chip->stop_voter & CHG_STOP_VOTER__VCHG_ABNORMAL) ==
			   CHG_STOP_VOTER__VCHG_ABNORMAL) {
			charger_abnormal_log = CRITICAL_LOG_VCHG_ABNORMAL;
		} else if ((chip->stop_voter & CHG_STOP_VOTER__VBAT_TOO_HIGH) ==
			   CHG_STOP_VOTER__VBAT_TOO_HIGH) {
			charger_abnormal_log = CRITICAL_LOG_VBAT_TOO_HIGH;
		} else if ((chip->stop_voter & CHG_STOP_VOTER__MAX_CHGING_TIME) ==
			   CHG_STOP_VOTER__MAX_CHGING_TIME) {
			charger_abnormal_log = CRITICAL_LOG_CHARGING_OVER_TIME;
		} else {
			/*do nothing*/
		}
	} else {
		charger_abnormal_log = 0;
	}
}

static void oppo_chg_other_thing(struct oppo_chg_chip *chip)
{
	if (chip->charger_exist) {
		chip->total_time += OPPO_CHG_UPDATE_INTERVAL_SEC;
	}
	oppo_chg_print_log(chip);
	oppo_chg_critical_log(chip);
}

#define IBATT_COUNT 10
static void oppo_chg_ibatt_check_and_set(struct oppo_chg_chip *chip)
{
	static int average_current = 0;
	static int ibatt_count = 0;
	static int current_adapt = 0;
	static int pre_tbatt_status = BATTERY_STATUS__INVALID;
	static int fail_count = 0;
	bool set_current_flag = false;
	int recharge_volt = 0;
	int current_limit = 0;
	int current_init = 0;
	int threshold = 0;
	int current_step = 0;

	if (chip->chg_ops->need_to_check_ibatt &&
	    chip->chg_ops->need_to_check_ibatt(chip) == false) {
		return;
	}

	if (!chip->charger_exist) {
		current_adapt = 0;
		ibatt_count = 0;
		average_current = 0;
		fail_count = 0;
		return;
	}

	if (current_adapt == 0) {
		if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) {
			current_adapt = chip->limits.temp_little_cold_fastchg_current_ma;
			pre_tbatt_status = BATTERY_STATUS__LITTLE_COLD_TEMP;
		} else if (chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) {
			current_adapt = chip->limits.temp_warm_fastchg_current_ma;
			pre_tbatt_status = BATTERY_STATUS__WARM_TEMP;
		} else if (chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) {
			current_adapt = chip->limits.temp_cold_fastchg_current_ma;
			pre_tbatt_status = BATTERY_STATUS__COLD_TEMP;
		} else if (chip->tbatt_status == BATTERY_STATUS__NORMAL) {
			current_adapt = chip->limits.temp_normal_fastchg_current_ma;
			pre_tbatt_status = BATTERY_STATUS__NORMAL;
		} else if (chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) {
			if (chip->batt_volt > 4180) {
				current_adapt = chip->limits.temp_cool_fastchg_current_ma_low;
			} else {
				current_adapt = chip->limits.temp_cool_fastchg_current_ma_high;
			}
			pre_tbatt_status = BATTERY_STATUS__COOL_TEMP;
		} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) {
			current_adapt = chip->limits.temp_little_cool_fastchg_current_ma;
			pre_tbatt_status = BATTERY_STATUS__LITTLE_COOL_TEMP;
		}
	}

	if (chip->tbatt_status != pre_tbatt_status) {
		current_adapt = 0;
		ibatt_count = 0;
		average_current = 0;
		fail_count = 0;
		return;
	}

	if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COLD_TEMP) {
		recharge_volt = chip->limits.temp_little_cold_vfloat_mv - chip->limits.recharge_mv;
		current_init = chip->limits.temp_little_cold_fastchg_current_ma;
		current_limit = chip->batt_capacity_mah * 15 / 100;
		threshold = 50;
	} else if (chip->tbatt_status == BATTERY_STATUS__WARM_TEMP) {
		recharge_volt = chip->limits.temp_warm_vfloat_mv - chip->limits.recharge_mv;
		current_init = chip->limits.temp_warm_fastchg_current_ma;
		current_limit = chip->batt_capacity_mah * 25 / 100;
		threshold = 50;
	} else if (chip->tbatt_status == BATTERY_STATUS__COLD_TEMP) {
		recharge_volt = chip->limits.temp_cold_vfloat_mv - chip->limits.recharge_mv;
		current_init = chip->limits.temp_cold_fastchg_current_ma;
		current_limit = 350;
		threshold = 50;
	} else if (chip->tbatt_status == BATTERY_STATUS__NORMAL) {
		recharge_volt =
			chip->limits.temp_normal_vfloat_mv_normalchg - chip->limits.recharge_mv;
		current_init = chip->limits.temp_normal_fastchg_current_ma;
		current_limit = chip->batt_capacity_mah * 65 / 100;
		threshold = 70;
	} else if (chip->tbatt_status == BATTERY_STATUS__COOL_TEMP) {
		recharge_volt = chip->limits.temp_cool_vfloat_mv - chip->limits.recharge_mv;
		if (vbatt_higherthan_4180mv) {
			current_init = chip->limits.temp_cool_fastchg_current_ma_low;
			current_limit = chip->batt_capacity_mah * 15 / 100;
		} else {
			current_init = chip->limits.temp_cool_fastchg_current_ma_high;
			current_limit = chip->batt_capacity_mah * 25 / 100;
		}
		threshold = 50;
	} else if (chip->tbatt_status == BATTERY_STATUS__LITTLE_COOL_TEMP) {
		recharge_volt = chip->limits.temp_little_cool_vfloat_mv - chip->limits.recharge_mv;
		current_init = chip->limits.temp_little_cool_fastchg_current_ma;
		current_limit = chip->batt_capacity_mah * 45 / 100;
		threshold = 70;
	}

	if (chip->batt_volt > recharge_volt || chip->led_on) {
		ibatt_count = 0;
		average_current = 0;
		fail_count = 0;
		return;
	}

	current_step = chip->chg_ops->get_chg_current_step(chip);

	if (chip->icharging < 0) {
		ibatt_count++;
		average_current = average_current + chip->icharging;
	}

	/*charge current larger than limit*/
	if ((-1 * chip->icharging) > current_limit) {
		if (current_adapt > current_init) {
			current_adapt = current_init;
		} else {
			current_adapt -= 2 * current_step;
		}
		set_current_flag = true;
		fail_count++;
	} else if (ibatt_count == IBATT_COUNT) {
		average_current = -1 * average_current / ibatt_count;
		threshold += fail_count * current_step;
		if (average_current < current_limit - threshold) {
			current_adapt += current_step;
			set_current_flag = true;
		} else {
			ibatt_count = 0;
			average_current = 0;
		}
	}

	if (set_current_flag == true) {
		if (current_adapt > (current_limit + 100)) {
			current_adapt = current_limit + 100;
		} else if (current_adapt < 103) { /*(512*20%)*/
			current_adapt = 103;
		}

		chip->chg_ops->charging_current_write_fast(chip, current_adapt);
		ibatt_count = 0;
		average_current = 0;
	}
}

static void oppo_chg_update_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct oppo_chg_chip *chip = container_of(dwork, struct oppo_chg_chip, update_work);

	oppo_charger_detect_check(chip);

	oppo_chg_get_battery_data(chip);

	if (chip->charger_exist) {
		oppo_chg_aicl_check(chip);
		oppo_chg_protection_check(chip);
		oppo_chg_battery_notify_check(chip);
		oppo_chg_check_status_full(chip);
	}
	oppo_chg_ibatt_check_and_set(chip);

	oppo_chg_battery_update_status(chip);

	oppo_chg_other_thing(chip);

	/* run again after interval */
	schedule_delayed_work(&chip->update_work, OPPO_CHG_UPDATE_INTERVAL);
}

bool oppo_chg_wake_update_work(void)
{
	int shedule_work = 0;

	if (!g_oppo_chg_chip) {
		return true;
	}
	shedule_work = mod_delayed_work(system_wq, &g_oppo_chg_chip->update_work, 0);

	return true;
}

void oppo_chg_soc_update_when_resume(unsigned long sleep_tm_sec)
{
	if (!g_oppo_chg_chip) {
		return;
	}
	g_oppo_chg_chip->sleep_tm_sec = sleep_tm_sec;
	g_oppo_chg_chip->soc = oppo_gauge_get_batt_soc();
	oppo_chg_update_ui_soc(g_oppo_chg_chip);
}

void oppo_chg_soc_update(void)
{
	if (!g_oppo_chg_chip) {
		return;
	}
	oppo_chg_update_ui_soc(g_oppo_chg_chip);
}
