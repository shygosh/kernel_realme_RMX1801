/**********************************************************************************
* Copyright (c)  2008-2015  Guangdong OPPO Mobile Comm Corp., Ltd
* CONFIG_OPPO_VENDOR_EDIT
* Description: Charger IC management module for charger system framework.
*                  Manage all charger IC and define abstarct function flow.
* Version   : 1.0
* Date      : 2015-06-22
* Author    : fanhui@PhoneSW.BSP
*           : Fanhong.Kong@ProDrv.CHG
* ------------------------------ Revision History: --------------------------------
* <version>         <date>              <author>                      <desc>
* Revision 1.0    2015-06-22      fanhui@PhoneSW.BSP          Created for new architecture
* Revision 1.0    2015-06-22      Fanhong.Kong@ProDrv.CHG     Created for new architecture
***********************************************************************************/

#ifndef _OPPO_CHARGER_H_
#define _OPPO_CHARGER_H_

#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/wakelock.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#elif defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

#include <linux/version.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/iio/consumer.h>
#include <linux/iio/iio.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/machine.h>
#include <soc/oppo/boot_mode.h>
#include <soc/oppo/device_info.h>

#define CHG_LOG_CRTI 1
#define CHG_LOG_FULL 2

#define OPCHG_INPUT_CURRENT_LIMIT_CHARGER_MA 2000
#define OPCHG_INPUT_CURRENT_LIMIT_USB_MA 500
#define OPCHG_INPUT_CURRENT_LIMIT_LED_MA 1200
#define OPCHG_INPUT_CURRENT_LIMIT_CAMERA_MA 1000

#define OPCHG_FAST_CHG_MAX_MA 2000

#define FEATURE_PRINT_CHGR_LOG
#define FEATURE_PRINT_BAT_LOG
#define FEATURE_PRINT_GAUGE_LOG
#define FEATURE_PRINT_STATUS_LOG
/*#define FEATURE_PRINT_OTHER_LOG*/
#define FEATURE_PRINT_VOTE_LOG
#define FEATURE_PRINT_ICHGING_LOG
#define FEATURE_VBAT_PROTECT

#define NOTIFY_CHARGER_OVER_VOL 1
#define NOTIFY_CHARGER_LOW_VOL 2
#define NOTIFY_BAT_OVER_TEMP 3
#define NOTIFY_BAT_LOW_TEMP 4
#define NOTIFY_BAT_NOT_CONNECT 5
#define NOTIFY_BAT_OVER_VOL 6
#define NOTIFY_BAT_FULL 7
#define NOTIFY_CHGING_CURRENT 8
#define NOTIFY_CHGING_OVERTIME 9
#define NOTIFY_BAT_FULL_PRE_HIGH_TEMP 10
#define NOTIFY_BAT_FULL_PRE_LOW_TEMP 11
#define NOTIFY_BAT_FULL_THIRD_BATTERY 14

#define OPPO_CHG_MONITOR_INTERVAL round_jiffies_relative(msecs_to_jiffies(5000))

#define chg_debug(fmt, ...)                                                    \
	printk(KERN_NOTICE "[OPPO_CHG][%s]" fmt, __func__, ##__VA_ARGS__)

#define chg_err(fmt, ...)                                                      \
	printk(KERN_ERR "[OPPO_CHG][%s]" fmt, __func__, ##__VA_ARGS__)

typedef enum {
	CHG_NONE = 0,
	CHG_DISABLE,
	CHG_SUSPEND,
} OPPO_CHG_DISABLE_STATUS;

typedef enum {
	CHG_STOP_VOTER_NONE = 0,
	CHG_STOP_VOTER__BATTTEMP_ABNORMAL = (1 << 0),
	CHG_STOP_VOTER__VCHG_ABNORMAL = (1 << 1),
	CHG_STOP_VOTER__VBAT_TOO_HIGH = (1 << 2),
	CHG_STOP_VOTER__MAX_CHGING_TIME = (1 << 3),
	CHG_STOP_VOTER__FULL = (1 << 4),
} OPPO_CHG_STOP_VOTER;

typedef enum {
	CHARGER_STATUS__GOOD,
	CHARGER_STATUS__VOL_HIGH,
	CHARGER_STATUS__VOL_LOW,
	CHARGER_STATUS__INVALID
} OPPO_CHG_VCHG_STATUS;

typedef enum {
	BATTERY_STATUS__NORMAL = 0, /*16C~45C*/
	BATTERY_STATUS__REMOVED, /*<-20C*/
	BATTERY_STATUS__LOW_TEMP, /*<-3C*/
	BATTERY_STATUS__HIGH_TEMP, /*>55C*/
	BATTERY_STATUS__COLD_TEMP, /*-3C~0C*/
	BATTERY_STATUS__LITTLE_COLD_TEMP, /*0C~5C*/
	BATTERY_STATUS__COOL_TEMP, /*5C~12C*/
	BATTERY_STATUS__LITTLE_COOL_TEMP, /*12C~16C*/
	BATTERY_STATUS__WARM_TEMP, /*45C~55C*/
	BATTERY_STATUS__INVALID
} OPPO_CHG_TBATT_STATUS;

typedef enum {
	CRITICAL_LOG_NORMAL = 0,
	CRITICAL_LOG_UNABLE_CHARGING,
	CRITICAL_LOG_BATTTEMP_ABNORMAL,
	CRITICAL_LOG_VCHG_ABNORMAL,
	CRITICAL_LOG_VBAT_TOO_HIGH,
	CRITICAL_LOG_CHARGING_OVER_TIME,
	CRITICAL_LOG_VOOC_WATCHDOG,
	CRITICAL_LOG_VOOC_BAD_CONNECTED,
	CRITICAL_LOG_VOOC_BTB
} OPPO_CHG_CRITICAL_LOG;

typedef enum {
	CHARGING_STATUS_CCCV = 0X01,
	CHARGING_STATUS_FULL = 0X02,
	CHARGING_STATUS_FAIL = 0X03,
} OPPO_CHG_CHARGING_STATUS;

struct tbatt_anti_shake {
	int cold_bound;
	int little_cold_bound;
	int cool_bound;
	int little_cool_bound;
	int normal_bound;
	int warm_bound;
	int hot_bound;
	int overtemp_bound;
};

struct oppo_chg_limits {
	int input_current_usb_ma;
	int input_current_led_ma;
	int input_current_led_ma_forcmcc;
	int input_current_led_ma_overtemp;

	int iterm_ma;
	bool iterm_disabled;
	int recharge_mv;

	int removed_bat_decidegc; /*-19C*/

	int cold_bat_decidegc; /*-3C*/
	int temp_cold_vfloat_mv;
	int temp_cold_fastchg_current_ma;

	int little_cold_bat_decidegc; /*0C*/
	int temp_little_cold_vfloat_mv;
	int temp_little_cold_fastchg_current_ma;

	int cool_bat_decidegc; /*5C*/
	int temp_cool_vfloat_mv;
	int temp_cool_fastchg_current_ma_high;
	int temp_cool_fastchg_current_ma_low;

	int little_cool_bat_decidegc; /*12C*/
	int temp_little_cool_vfloat_mv;
	int temp_little_cool_fastchg_current_ma;

	int normal_bat_decidegc; /*16C*/
	int temp_normal_fastchg_current_ma;
	int temp_normal_vfloat_mv_normalchg;

	int warm_bat_decidegc; /*45C*/
	int temp_warm_vfloat_mv;
	int temp_warm_fastchg_current_ma;

	int hot_bat_decidegc; /*53C*/
	int non_standard_vfloat_mv;
	int non_standard_fastchg_current_ma;
	int max_chg_time_sec;
	int charger_hv_thr;
	int charger_lv_thr;
	int vbatt_full_thr;
	int vbatt_hv_thr;

	int vfloat_step_mv;
	int vfloat_sw_set;
	int vfloat_over_counts;

	int non_standard_vfloat_sw_limit;
	int cold_vfloat_sw_limit;
	int little_cold_vfloat_sw_limit;
	int cool_vfloat_sw_limit;
	int little_cool_vfloat_sw_limit;
	int normal_vfloat_sw_limit;
	int warm_vfloat_sw_limit;

	int overtemp_bat_decidegc; /*35C*/

	bool sw_vfloat_over_protect_enable;
	int non_standard_vfloat_over_sw_limit;
	int cold_vfloat_over_sw_limit;
	int little_cold_vfloat_over_sw_limit;
	int cool_vfloat_over_sw_limit;
	int little_cool_vfloat_over_sw_limit;
	int normal_vfloat_over_sw_limit;
	int warm_vfloat_over_sw_limit;
};

struct qcom_pmic {
	struct smb2 *smb2_chip;
	struct iio_channel *pm660_vadc_dev;

	/* for complie*/
	int pulse_cnt;
	unsigned int therm_lvl_sel;
	bool psy_registered;
	int usb_online;

	/* copy from msm8976_pmic begin */
	int bat_charging_state;
	bool suspending;
	bool aicl_suspend;
	bool usb_hc_mode;
	int usb_hc_count;
	bool hc_mode_flag;
	/* copy form msm8976_pmic end */
};

struct oppo_chg_chip {
	struct i2c_client *client;
	struct device *dev;
	const struct oppo_chg_operations *chg_ops;

	struct power_supply *dc_psy;

	struct power_supply *usb_psy;
	struct qcom_pmic pmic_spmi;
	struct power_supply *batt_psy;
	/* struct battery_data battery_main */
	struct delayed_work update_work;
	struct wake_lock suspend_lock;
	atomic_t charger_suspended;

	struct oppo_chg_limits limits;
	struct tbatt_anti_shake anti_shake_bound;
	int usb_switch_gpio;

	bool charger_exist;
	int charger_type;
	int charger_volt;
	int charger_volt_pre;
	int chg_pretype;

	int temperature;
	int batt_volt;
	int icharging;
	int soc;
	int ui_soc;
	int soc_load;
	bool authenticate;
	int hw_aicl_point;
	int sw_aicl_point;
	int batt_fcc;
	int batt_cc;
	int batt_soh;
	int batt_rm;
	int batt_capacity_mah;
	int tbatt_pre_shake;

	bool batt_exist;
	bool batt_full;
	bool chging_on;
	bool in_rechging;
	int charging_state;
	int total_time;
	unsigned long sleep_tm_sec;

	bool vbatt_over;
	bool chging_over_time;
	int vchg_status;
	int tbatt_status;
	int prop_status;
	int stop_voter;
	int notify_code;
	int notify_flag;
	int request_power_off;

	bool led_on;
	bool led_status_change;
	bool ac_online;

	bool suspend_after_full;
	bool check_batt_full_by_sw;
	bool external_gauge;
	bool chg_ctrl_by_lcd;
	bool fg_bcl_poll;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend chg_early_suspend;
#elif defined(CONFIG_FB)
	struct notifier_block chg_fb_notify;
#endif
	bool overtemp_status;
};

struct oppo_chg_operations {
	int (*hardware_init)(struct oppo_chg_chip *chip);
	int (*charging_current_write_fast)(struct oppo_chg_chip *chip, int cur);
	void (*set_aicl_point)(struct oppo_chg_chip *chip, int vbatt);
	int (*input_current_write)(struct oppo_chg_chip *chip, int cur);
	int (*float_voltage_write)(struct oppo_chg_chip *chip, int cur);
	int (*term_current_set)(struct oppo_chg_chip *chip, int cur);
	int (*charging_enable)(struct oppo_chg_chip *chip);
	int (*charging_disable)(struct oppo_chg_chip *chip);
	int (*get_charging_enable)(struct oppo_chg_chip *chip);
	int (*charger_suspend)(struct oppo_chg_chip *chip);
	int (*charger_unsuspend)(struct oppo_chg_chip *chip);
	int (*set_rechg_vol)(struct oppo_chg_chip *chip, int vol);
	int (*reset_charger)(struct oppo_chg_chip *chip);
	int (*read_full)(struct oppo_chg_chip *chip);
	int (*set_charging_term_disable)(struct oppo_chg_chip *chip);
	bool (*check_charger_resume)(struct oppo_chg_chip *chip);

	int (*get_charger_type)(void);
	int (*get_charger_volt)(void);
	bool (*check_chrdet_status)(void);
	int (*get_instant_vbatt)(void);
	int (*get_rtc_soc)(void);
	int (*set_rtc_soc)(int val);
	int (*get_aicl_ma)(struct oppo_chg_chip *chip);
	void (*rerun_aicl)(struct oppo_chg_chip *chip);
	int (*tlim_en)(struct oppo_chg_chip *chip, bool);
	int (*set_system_temp_level)(struct oppo_chg_chip *chip, int);
	int (*set_dp_dm)(struct oppo_chg_chip *chip, int);
	int (*calc_flash_current)(struct oppo_chg_chip *chip);
	int (*get_chg_current_step)(struct oppo_chg_chip *chip);
	bool (*need_to_check_ibatt)(struct oppo_chg_chip *chip);
	int (*get_dyna_aicl_result)(struct oppo_chg_chip *chip);
	void (*check_is_iindpm_mode)(struct oppo_chg_chip *chip);
};

/*********************************************
 * oppo_chg_init - initialize oppo_chg_chip
 * @chip: pointer to the oppo_chg_cip
 * @clinet: i2c client of the chip
 *
 * Returns: 0 - success; -1/errno - failed
 **********************************************/
int oppo_chg_parse_dt(struct oppo_chg_chip *chip);
int oppo_chg_init(struct oppo_chg_chip *chip);
void oppo_charger_detect_check(struct oppo_chg_chip *chip);
int oppo_chg_get_prop_batt_health(struct oppo_chg_chip *chip);
bool oppo_chg_wake_update_work(void);
void oppo_chg_soc_update_when_resume(unsigned long sleep_tm_sec);
void oppo_chg_soc_update(void);
void oppo_chg_turn_on_charging(struct oppo_chg_chip *chip);
void oppo_chg_turn_off_charging(struct oppo_chg_chip *chip);
void oppo_chg_variables_reset(struct oppo_chg_chip *chip, bool in);

extern struct oppo_chg_chip *g_oppo_chg_chip;

#endif /*_OPPO_CHARGER_H_*/
