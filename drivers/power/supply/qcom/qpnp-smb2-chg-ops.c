/* Jianchao.Shi@BSP.CHG.Basic, 2016/12/26, sjc Add for charging*/
static int smbchg_usb_suspend_disable(struct oppo_chg_chip *chip);
static int smbchg_usb_suspend_enable(struct oppo_chg_chip *chip);
static int smbchg_charging_enble(struct oppo_chg_chip *chip);
static bool oppo_chg_is_usb_present(void);
static int qpnp_get_prop_charger_voltage_now(void);

static int oppo_chg_hw_init(struct oppo_chg_chip *chip)
{
	int boot_mode = get_boot_mode();

	if (boot_mode != MSM_BOOT_MODE__RF && boot_mode != MSM_BOOT_MODE__WLAN) {
		smbchg_usb_suspend_disable(chip);
	} else {
		smbchg_usb_suspend_enable(chip);
	}
	smbchg_charging_enble(chip);

	return 0;
}

static int smbchg_set_fastchg_current_raw(struct oppo_chg_chip *chip, int current_ma)
{
	int rc = 0;

	rc = vote(chip->pmic_spmi.smb2_chip->chg.fcc_votable, DEFAULT_VOTER,
			true, current_ma * 1000);
	if (rc < 0)
		chg_err("Couldn't vote fcc_votable[%d], rc=%d\n", current_ma, rc);
	else
		chg_debug("vote fcc_votable[%d], rc = %d\n", current_ma, rc);
	return rc;
}

static void smbchg_set_aicl_point(struct oppo_chg_chip *chip, int vol)
{
	//DO Nothing
}

static void smbchg_aicl_enable(struct oppo_chg_chip *chip, bool enable)
{
	int rc = 0;

	rc = smblib_masked_write(&chip->pmic_spmi.smb2_chip->chg, USBIN_AICL_OPTIONS_CFG_REG,
			USBIN_AICL_EN_BIT, enable ? USBIN_AICL_EN_BIT : 0);
	if (rc < 0)
		chg_err("Couldn't write USBIN_AICL_OPTIONS_CFG_REG rc=%d\n", rc);
}

static void smbchg_rerun_aicl(struct oppo_chg_chip *chip)
{
	smbchg_aicl_enable(chip, false);
	/* Add a delay so that AICL successfully clears */
	msleep(50);
	smbchg_aicl_enable(chip, true);
}

static bool  oppo_chg_is_normal_mode(void)
{
	int boot_mode = get_boot_mode();

	if (boot_mode == MSM_BOOT_MODE__RF || boot_mode == MSM_BOOT_MODE__WLAN)
		return false;
	return true;
}

static bool oppo_chg_is_suspend_status(void)
{
	int rc = 0;
	u8 stat;
	struct smb_charger *chg = NULL;

	if (!g_oppo_chg_chip)
		return false;

	chg = &g_oppo_chg_chip->pmic_spmi.smb2_chip->chg;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		printk(KERN_ERR "oppo_chg_is_suspend_status: Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return false;
	}

	return (bool)(stat & USBIN_SUSPEND_STS_BIT);
}

static void oppo_chg_clear_suspend(void)
{
	int rc;
	struct smb_charger *chg = NULL;

	if (!g_oppo_chg_chip)
		return;

	chg = &g_oppo_chg_chip->pmic_spmi.smb2_chip->chg;

	rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT, 1);
	if (rc < 0) {
		printk(KERN_ERR "oppo_chg_monitor_work: Couldn't set USBIN_SUSPEND_BIT rc=%d\n", rc);
	}
	msleep(50);
	rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT, 0);
	if (rc < 0) {
		printk(KERN_ERR "oppo_chg_monitor_work: Couldn't clear USBIN_SUSPEND_BIT rc=%d\n", rc);
	}
}

static void oppo_chg_check_clear_suspend(void)
{
	use_present_status = true;
	oppo_chg_clear_suspend();
	use_present_status = false;
}

static int usb_icl[] = {
	300, 500, 900, 1200, 1500, 1750, 2000, 3000,
};

#define USBIN_25MA	25000
static int oppo_chg_set_input_current(struct oppo_chg_chip *chip, int current_ma)
{
	int rc = 0, i = 0;
	int chg_vol = 0;
	int aicl_point = 0;
	int aicl_point_temp = 0;

	if (chip->pmic_spmi.smb2_chip->chg.pre_current_ma == current_ma)
		return rc;
	else
		chip->pmic_spmi.smb2_chip->chg.pre_current_ma = current_ma;

	chg_debug( "usb input max current limit=%d setting %02x\n", current_ma, i);

	aicl_point_temp = aicl_point = 4500;

	smbchg_aicl_enable(chip, false);

	if (current_ma < 500) {
		i = 0;
		goto aicl_end;
	}

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	}

	i = 1; /* 500 */
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(90);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		chg_debug( "use 500 here\n");
		goto aicl_boost_back;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		chg_debug( "use 500 here\n");
		goto aicl_boost_back;
	}
	if (chg_vol < aicl_point_temp) {
		chg_debug( "use 500 here\n");
		goto aicl_end;
	} else if (current_ma < 900)
		goto aicl_end;

	i = 2; /* 900 */
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(90);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 1;
		goto aicl_boost_back;
	}
	if (oppo_chg_is_suspend_status() && oppo_chg_is_usb_present() && oppo_chg_is_normal_mode()) {
		i = i - 1;
		goto aicl_suspend;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 1;
		goto aicl_boost_back;
	}
	if (chg_vol < aicl_point_temp) {
		i = i - 1;
		goto aicl_pre_step;
	} else if (current_ma < 1200)
		goto aicl_end;

	i = 3; /* 1200 */
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(90);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 1;
		goto aicl_boost_back;
	}
	if (oppo_chg_is_suspend_status() && oppo_chg_is_usb_present() && oppo_chg_is_normal_mode()) {
		i = i - 1;
		goto aicl_suspend;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 1;
		goto aicl_boost_back;
	}
	if (chg_vol < aicl_point_temp) {
		i = i - 1;
		goto aicl_pre_step;
	}

	i = 4; /* 1500 */
	aicl_point_temp = aicl_point + 50;
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(120);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 2;
		goto aicl_boost_back;
	}
	if (oppo_chg_is_suspend_status() && oppo_chg_is_usb_present() && oppo_chg_is_normal_mode()) {
		i = i - 2;
		goto aicl_suspend;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 2;
		goto aicl_boost_back;
	}
	if (chg_vol < aicl_point_temp) {
		i = i - 2; //We DO NOT use 1.2A here
		goto aicl_pre_step;
	} else if (current_ma < 1500) {
		i = i - 1; //We use 1.2A here
		goto aicl_end;
	} else if (current_ma < 2000)
		goto aicl_end;

	i = 5; /* 1750 */
	aicl_point_temp = aicl_point + 50;
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(120);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 2;
		goto aicl_boost_back;
	}
	if (oppo_chg_is_suspend_status() && oppo_chg_is_usb_present() && oppo_chg_is_normal_mode()) {
		i = i - 2;
		goto aicl_suspend;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 2;
		goto aicl_boost_back;
	}
	if (chg_vol < aicl_point_temp) {
		i = i - 2; //1.2
		goto aicl_pre_step;
	}

	i = 6; /* 2000 */
	aicl_point_temp = aicl_point;
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(90);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 2;
		goto aicl_boost_back;
	}
	if (oppo_chg_is_suspend_status() && oppo_chg_is_usb_present() && oppo_chg_is_normal_mode()) {
		i = i - 2;
		goto aicl_suspend;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 2;
		goto aicl_boost_back;
	}
	if (chg_vol < aicl_point_temp) {
		i =  i - 2;//1.5
		goto aicl_pre_step;
	} else if (current_ma < 3000)
		goto aicl_end;

	i = 7; /* 3000 */
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	msleep(90);

	if (get_client_vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER) == 0
			&& get_effective_result(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable) < USBIN_25MA) {
		i = i - 1;
		goto aicl_boost_back;
	}
	if (oppo_chg_is_suspend_status() && oppo_chg_is_usb_present() && oppo_chg_is_normal_mode()) {
		i = i - 1;
		goto aicl_suspend;
	}

	chg_vol = qpnp_get_prop_charger_voltage_now();
	if (chg_vol < aicl_point_temp) {
		i = i - 1;
		goto aicl_pre_step;
	} else if (current_ma >= 3000)
		goto aicl_end;

aicl_pre_step:
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	chg_debug( "usb input max current limit aicl chg_vol=%d j[%d]=%d sw_aicl_point:%d aicl_pre_step\n", chg_vol, i, usb_icl[i], aicl_point_temp);
	smbchg_rerun_aicl(chip);
	return rc;
aicl_end:
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	chg_debug( "usb input max current limit aicl chg_vol=%d j[%d]=%d sw_aicl_point:%d aicl_end\n", chg_vol, i, usb_icl[i], aicl_point_temp);
	smbchg_rerun_aicl(chip);
	return rc;
aicl_boost_back:
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	chg_debug( "usb input max current limit aicl chg_vol=%d j[%d]=%d sw_aicl_point:%d aicl_boost_back\n", chg_vol, i, usb_icl[i], aicl_point_temp);
	if (chip->pmic_spmi.smb2_chip->chg.wa_flags & BOOST_BACK_WA)
		vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	smbchg_rerun_aicl(chip);
	return rc;
aicl_suspend:
	rc = vote(chip->pmic_spmi.smb2_chip->chg.usb_icl_votable, USB_PSY_VOTER, true, usb_icl[i] * 1000);
	chg_debug( "usb input max current limit aicl chg_vol=%d j[%d]=%d sw_aicl_point:%d aicl_suspend\n", chg_vol, i, usb_icl[i], aicl_point_temp);
	oppo_chg_check_clear_suspend();
	smbchg_rerun_aicl(chip);
	return rc;
}

static int smbchg_float_voltage_set(struct oppo_chg_chip *chip, int vfloat_mv)
{
	int rc = 0;

	chg_debug(" float_Voltage: %d mV\n", vfloat_mv);
	rc = vote(chip->pmic_spmi.smb2_chip->chg.fv_votable, BATT_PROFILE_VOTER,
			true, vfloat_mv * 1000);
	if (rc < 0)
		chg_err("Couldn't vote fv_votable[%d], rc=%d\n", vfloat_mv, rc);

	return rc;
}

static int smbchg_term_current_set(struct oppo_chg_chip *chip, int term_current)
{
	int rc = 0;
	u8 val_raw = 0;

	if (term_current < 0 || term_current > 750)
		term_current = 150;

	val_raw = term_current / 50;
	rc = smblib_masked_write(&chip->pmic_spmi.smb2_chip->chg, TCCC_CHARGE_CURRENT_TERMINATION_CFG_REG,
			TCCC_CHARGE_CURRENT_TERMINATION_SETTING_MASK, val_raw);
	if (rc < 0)
		chg_err("Couldn't write TCCC_CHARGE_CURRENT_TERMINATION_CFG_REG rc=%d\n", rc);

	return rc;
}

static int smbchg_charging_enble(struct oppo_chg_chip *chip)
{
	int rc = 0;

	rc = vote(chip->pmic_spmi.smb2_chip->chg.chg_disable_votable, DEFAULT_VOTER,
			false, 0);
	if (rc < 0)
		chg_err("Couldn't enable charging, rc=%d\n", rc);

	return rc;
}

static int smbchg_charging_disble(struct oppo_chg_chip *chip)
{
	int rc = 0;

	rc = vote(chip->pmic_spmi.smb2_chip->chg.chg_disable_votable, DEFAULT_VOTER,
			true, 0);
	if (rc < 0)
		chg_err("Couldn't disable charging, rc=%d\n", rc);

	chip->pmic_spmi.smb2_chip->chg.pre_current_ma = -1;

	return rc;
}

static int smbchg_get_charge_enable(struct oppo_chg_chip *chip)
{
	int rc = 0;
	u8 temp = 0;

	rc = smblib_read(&chip->pmic_spmi.smb2_chip->chg, CHARGING_ENABLE_CMD_REG, &temp);
	if (rc < 0) {
		chg_err("Couldn't read CHARGING_ENABLE_CMD_REG rc=%d\n", rc);
		return 0;
	}
	rc = temp & CHARGING_ENABLE_CMD_BIT;

	return rc;
}

static int smbchg_usb_suspend_enable(struct oppo_chg_chip *chip)
{
	int rc = 0;

	rc = smblib_set_usb_suspend(&chip->pmic_spmi.smb2_chip->chg, true);
	if (rc < 0)
		chg_err("Couldn't write enable to USBIN_SUSPEND_BIT rc=%d\n", rc);

	chip->pmic_spmi.smb2_chip->chg.pre_current_ma = -1;

	return rc;
}

static int smbchg_usb_suspend_disable(struct oppo_chg_chip *chip)
{
	int rc = 0;
	int boot_mode = get_boot_mode();

	if (boot_mode == MSM_BOOT_MODE__RF || boot_mode == MSM_BOOT_MODE__WLAN) {
		chg_err("RF/WLAN, suspending...\n");
		rc = smblib_set_usb_suspend(&chip->pmic_spmi.smb2_chip->chg, true);
		if (rc < 0)
			chg_err("Couldn't write enable to USBIN_SUSPEND_BIT rc=%d\n", rc);
		return rc;
	}

	rc = smblib_set_usb_suspend(&chip->pmic_spmi.smb2_chip->chg, false);
	if (rc < 0)
		chg_err("Couldn't write disable to USBIN_SUSPEND_BIT rc=%d\n", rc);

	return rc;
}

static int smbchg_set_rechg_vol(struct oppo_chg_chip *chip, int rechg_vol)
{
	return 0;
}

static int smbchg_reset_charger(struct oppo_chg_chip *chip)
{
	return 0;
}

static int smbchg_read_full(struct oppo_chg_chip *chip)
{
	int rc = 0;
	u8 stat = 0;

	if (!oppo_chg_is_usb_present())
		return 0;

	rc = smblib_read(&chip->pmic_spmi.smb2_chip->chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		chg_err("Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n", rc);
		return 0;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (stat == TERMINATE_CHARGE || stat == INHIBIT_CHARGE)
		return 1;
	return 0;
}

static int oppo_set_chging_term_disable(struct oppo_chg_chip *chip)
{
	return 0;
}

static bool qcom_check_charger_resume(struct oppo_chg_chip *chip)
{
	return true;
}

static int smbchg_get_chargerid_volt(struct oppo_chg_chip *chip)
{
	int rc = 0;
	int chargerid_volt = 0;
	int result = 0;

	if (!chip->pmic_spmi.pm660_vadc_dev) {
		chg_err("pm660_vadc_dev NULL\n");
		return 0;
	}

	// shygosh: Port to msm-4.19
	rc = iio_read_channel_processed(chip->pmic_spmi.pm660_vadc_dev, &result);
	if (rc) {
		chg_err("unable to read pm660_vadc_dev ADC7_VDC_16 rc = %d\n", rc);
		return 0;
	}

	chargerid_volt = result / 1000;
	chg_err("chargerid_volt: %d\n", chargerid_volt);

	return chargerid_volt;
}

static int smbchg_chargerid_switch_gpio_init(struct oppo_chg_chip *chip)
{
	chip->normalchg_gpio.pinctrl = devm_pinctrl_get(chip->dev);
	if (IS_ERR_OR_NULL(chip->normalchg_gpio.pinctrl)) {
		chg_err("get normalchg_gpio.pinctrl fail\n");
		return -EINVAL;
	}

	chip->normalchg_gpio.chargerid_switch_active = 
			pinctrl_lookup_state(chip->normalchg_gpio.pinctrl, "chargerid_switch_active");
	if (IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_active)) {
		chg_err("get chargerid_switch_active fail\n");
		return -EINVAL;
	}

	chip->normalchg_gpio.chargerid_switch_sleep = 
			pinctrl_lookup_state(chip->normalchg_gpio.pinctrl, "chargerid_switch_sleep");
	if (IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_sleep)) {
		chg_err("get chargerid_switch_sleep fail\n");
		return -EINVAL;
	}

	chip->normalchg_gpio.chargerid_switch_default = 
			pinctrl_lookup_state(chip->normalchg_gpio.pinctrl, "chargerid_switch_default");
	if (IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_default)) {
		chg_err("get chargerid_switch_default fail\n");
		return -EINVAL;
	}

	if (chip->normalchg_gpio.chargerid_switch_gpio > 0) {
		gpio_direction_output(chip->normalchg_gpio.chargerid_switch_gpio, 0);
	}
	pinctrl_select_state(chip->normalchg_gpio.pinctrl, chip->normalchg_gpio.chargerid_switch_default);

	return 0;
}

void smbchg_set_chargerid_switch_val(struct oppo_chg_chip *chip, int value)
{
	if (chip->normalchg_gpio.chargerid_switch_gpio <= 0) {
		chg_err("chargerid_switch_gpio not exist, return\n");
		return;
	}

	if (IS_ERR_OR_NULL(chip->normalchg_gpio.pinctrl)
		|| IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_active)
		|| IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_sleep)
		|| IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_default)) {
		chg_err("pinctrl null, return\n");
		return;
	}

	if (value) {
		gpio_direction_output(chip->normalchg_gpio.chargerid_switch_gpio, 1);
		pinctrl_select_state(chip->normalchg_gpio.pinctrl,
				chip->normalchg_gpio.chargerid_switch_default);
	} else {
		gpio_direction_output(chip->normalchg_gpio.chargerid_switch_gpio, 0);
		pinctrl_select_state(chip->normalchg_gpio.pinctrl,
				chip->normalchg_gpio.chargerid_switch_default);
	}
	chg_err("set value:%d, gpio_val:%d\n", 
		value, gpio_get_value(chip->normalchg_gpio.chargerid_switch_gpio));
}

static int smbchg_get_chargerid_switch_val(struct oppo_chg_chip *chip)
{
	if (chip->normalchg_gpio.chargerid_switch_gpio <= 0) {
		chg_err("chargerid_switch_gpio not exist, return\n");
		return -1;
	}

	if (IS_ERR_OR_NULL(chip->normalchg_gpio.pinctrl)
		|| IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_active)
		|| IS_ERR_OR_NULL(chip->normalchg_gpio.chargerid_switch_sleep)) {
		chg_err("pinctrl null, return\n");
		return -1;
	}

	return gpio_get_value(chip->normalchg_gpio.chargerid_switch_gpio);
}

static bool smbchg_need_to_check_ibatt(struct oppo_chg_chip *chip)
{
	return false;
}

static int smbchg_get_chg_current_step(struct oppo_chg_chip *chip)
{
	return 25;
}

static int opchg_get_charger_type(void)
{
	u8 apsd_stat;
	int rc;
	struct smb_charger *chg = NULL;

	if (!g_oppo_chg_chip)
		return POWER_SUPPLY_TYPE_UNKNOWN;

	chg = &g_oppo_chg_chip->pmic_spmi.smb2_chip->chg;

	/* reset for fastchg to normal */
	if (g_oppo_chg_chip->charger_type == POWER_SUPPLY_TYPE_UNKNOWN)
		chg->pre_current_ma = -1;

	rc = smblib_read(chg, APSD_STATUS_REG, &apsd_stat);
	if (rc < 0) {
		chg_err("Couldn't read APSD_STATUS rc=%d\n", rc);
		return POWER_SUPPLY_TYPE_UNKNOWN;
	}
	chg_debug("APSD_STATUS = 0x%02x, Chg_Type = %d\n", apsd_stat, chg->real_charger_type);

	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT))
		return POWER_SUPPLY_TYPE_UNKNOWN;

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB
			|| chg->real_charger_type == POWER_SUPPLY_TYPE_USB_CDP
			|| chg->real_charger_type == POWER_SUPPLY_TYPE_USB_DCP) {
		oppo_chg_soc_update();
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_CDP)
		return POWER_SUPPLY_TYPE_USB;
	
	return chg->real_charger_type;
}

static int qpnp_get_prop_charger_voltage_now(void)
{
	int val = 0;
	struct smb_charger *chg = NULL;

	if (!g_oppo_chg_chip)
		return 0;

	chg = &g_oppo_chg_chip->pmic_spmi.smb2_chip->chg;
	if (!chg->iio.usbin_v_chan || PTR_ERR(chg->iio.usbin_v_chan) == -EPROBE_DEFER)
		chg->iio.usbin_v_chan = iio_channel_get(chg->dev, "usbin_v");

	if (IS_ERR(chg->iio.usbin_v_chan))
		return PTR_ERR(chg->iio.usbin_v_chan);

	iio_read_channel_processed(chg->iio.usbin_v_chan, &val);

	if (val < 2000 * 1000)
		chg->pre_current_ma = -1;

	return val / 1000;
}

static bool oppo_chg_is_usb_present(void)
{
	int rc = 0;
	u8 stat = 0;
	bool vbus_rising = false;

	if (!g_oppo_chg_chip)
		return false;

	rc = smblib_read(&g_oppo_chg_chip->pmic_spmi.smb2_chip->chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		chg_err("Couldn't read USB_INT_RT_STS, rc=%d\n", rc);
		return false;
	}
	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	if (vbus_rising == false)
		g_oppo_chg_chip->pmic_spmi.smb2_chip->chg.pre_current_ma = -1;

	return vbus_rising;
}

static int qpnp_get_battery_voltage(void)
{
	return 3800;//Not use anymore
}

/* Ji.Xu PSW.BSP.CHG  2018-07-23  Save battery capacity to persist partition */
static int oppo_chg_get_shutdown_soc(void)
{
	int rc, shutdown_soc;
	union power_supply_propval ret = {0, };
	struct smb_charger *chg = NULL;

	chg = &g_oppo_chg_chip->pmic_spmi.smb2_chip->chg;

	if (!g_oppo_chg_chip || !chg || !chg->bms_psy) {
		chg_err("chip not ready\n");
		return 0;
	}

	rc = chg->bms_psy->desc->get_property(chg->bms_psy, POWER_SUPPLY_PROP_RESTORE_SOC, &ret);
	if (rc) {
			chg_err("bms psy doesn't support soc restore rc = %d\n", rc);
			goto restore_soc_err;
	}

	shutdown_soc = ret.intval;
	if (shutdown_soc >= 0 && shutdown_soc <= 100) {
			chg_debug("get restored soc = %d\n", shutdown_soc);
			return shutdown_soc;
	} else {
			chg_err("get restored soc = %d\n", shutdown_soc);
			goto restore_soc_err;
	}

restore_soc_err:
	rc = chg->bms_psy->desc->get_property(chg->bms_psy, POWER_SUPPLY_PROP_CAPACITY, &ret);
	if (rc) {
			chg_err("get soc error, return default 50, rc = %d\n",rc);
			return 50;
	}
	chg_debug("use QG soc = %d\n", ret.intval);

	return ret.intval;
}

static int oppo_chg_backup_soc(int backup_soc)
{
	return 0;
}

static int smbchg_get_aicl_level_ma(struct oppo_chg_chip *chip)
{
	return 0;
}

static int smbchg_force_tlim_en(struct oppo_chg_chip *chip, bool enable)
{
	return 0;
}

static int smbchg_system_temp_level_set(struct oppo_chg_chip *chip, int lvl_sel)
{
	return 0;
}

static int smbchg_dp_dm(struct oppo_chg_chip *chip, int val)
{
	return 0;
}

static int smbchg_calc_max_flash_current(struct oppo_chg_chip *chip)
{
	return 0;
}

static int oppo_chg_get_fv(struct oppo_chg_chip *chip)
{
	int flv = chip->limits.temp_normal_vfloat_mv_normalchg;
	int batt_temp = chip->temperature;

	if (batt_temp > chip->limits.hot_bat_decidegc) {//53C
		//default
	} else if (batt_temp >= chip->limits.warm_bat_decidegc) {//45C
		flv = chip->limits.temp_warm_vfloat_mv;
	} else if (batt_temp >= chip->limits.normal_bat_decidegc) {//16C
		flv = chip->limits.temp_normal_vfloat_mv_normalchg;
	} else if (batt_temp >= chip->limits.little_cool_bat_decidegc) {//12C
		flv = chip->limits.temp_little_cool_vfloat_mv;
	} else if (batt_temp >= chip->limits.cool_bat_decidegc) {//5C
		flv = chip->limits.temp_cool_vfloat_mv;
	} else if (batt_temp >= chip->limits.little_cold_bat_decidegc) {//0C
		flv = chip->limits.temp_little_cold_vfloat_mv;
	} else if (batt_temp >= chip->limits.cold_bat_decidegc) {//-3C
		flv = chip->limits.temp_cold_vfloat_mv;
	} else {
		//default
	}

	return flv;
}

static int oppo_chg_get_charging_current(struct oppo_chg_chip *chip)
{
	int charging_current = 0;
	int batt_temp = chip->temperature;

	if (batt_temp > chip->limits.hot_bat_decidegc) {//53C
		charging_current = 0;
	} else if (batt_temp >= chip->limits.warm_bat_decidegc) {//45C
		charging_current = chip->limits.temp_warm_fastchg_current_ma;
	} else if (batt_temp >= chip->limits.normal_bat_decidegc) {//16C
		charging_current = chip->limits.temp_normal_fastchg_current_ma;
	} else if (batt_temp >= chip->limits.little_cool_bat_decidegc) {//12C
		charging_current = chip->limits.temp_little_cool_fastchg_current_ma;
	} else if (batt_temp >= chip->limits.cool_bat_decidegc) {//5C
		if (chip->batt_volt > 4180)
			charging_current = chip->limits.temp_cool_fastchg_current_ma_low;
		else
			charging_current = chip->limits.temp_cool_fastchg_current_ma_high;
	} else if (batt_temp >= chip->limits.little_cold_bat_decidegc) {//0C
		charging_current = chip->limits.temp_little_cold_fastchg_current_ma;
	} else if (batt_temp >= chip->limits.cold_bat_decidegc) {//-3C
		charging_current = chip->limits.temp_cold_fastchg_current_ma;
	} else {
		charging_current = 0;
	}

	return charging_current;
}

static int get_current_time(unsigned long *now_tm_sec)
{
	struct rtc_time tm;
	struct rtc_device *rtc;
	int rc;

	rtc = rtc_class_open(CONFIG_RTC_HCTOSYS_DEVICE);
	if (rtc == NULL) {
		pr_err("%s: unable to open rtc device (%s)\n",
			__FILE__, CONFIG_RTC_HCTOSYS_DEVICE);
		return -EINVAL;
	}

	rc = rtc_read_time(rtc, &tm);
	if (rc) {
		pr_err("Error reading rtc device (%s) : %d\n",
			CONFIG_RTC_HCTOSYS_DEVICE, rc);
		goto close_time;
	}

	rc = rtc_valid_tm(&tm);
	if (rc) {
		pr_err("Invalid RTC time (%s): %d\n",
			CONFIG_RTC_HCTOSYS_DEVICE, rc);
		goto close_time;
	}
	rtc_tm_to_time(&tm, now_tm_sec);

close_time:
	rtc_class_close(rtc);
	return rc;
}

static unsigned long suspend_tm_sec = 0;
static int smb2_pm_resume(struct device *dev)
{
	int rc = 0;
	unsigned long resume_tm_sec = 0;
	unsigned long sleep_time = 0;

	if (!g_oppo_chg_chip)
		return 0;

	rc = get_current_time(&resume_tm_sec);
	if (rc || suspend_tm_sec == -1) {
		chg_err("RTC read failed\n");
		sleep_time = 0;
	} else {
		sleep_time = resume_tm_sec - suspend_tm_sec;
	}

	if (sleep_time < 0) {
		sleep_time = 0;
	}

	oppo_chg_soc_update_when_resume(sleep_time);

	return 0;
}

static int smb2_pm_suspend(struct device *dev)
{
	if (!g_oppo_chg_chip)
		return 0;

	if (get_current_time(&suspend_tm_sec)) {
		chg_err("RTC read failed\n");
		suspend_tm_sec = -1;
	}

	return 0;
}

static const struct dev_pm_ops smb2_pm_ops = {
	.resume		= smb2_pm_resume,
	.suspend	= smb2_pm_suspend,
};

struct oppo_chg_operations  smb2_chg_ops = {
	.hardware_init = oppo_chg_hw_init,
	.charging_current_write_fast = smbchg_set_fastchg_current_raw,
	.set_aicl_point = smbchg_set_aicl_point,
	.input_current_write = oppo_chg_set_input_current,
	.float_voltage_write = smbchg_float_voltage_set,
	.term_current_set = smbchg_term_current_set,
	.charging_enable = smbchg_charging_enble,
	.charging_disable = smbchg_charging_disble,
	.get_charging_enable = smbchg_get_charge_enable,
	.charger_suspend = smbchg_usb_suspend_enable,
	.charger_unsuspend = smbchg_usb_suspend_disable,
	.set_rechg_vol = smbchg_set_rechg_vol,
	.reset_charger = smbchg_reset_charger,
	.read_full = smbchg_read_full,
	.set_charging_term_disable = oppo_set_chging_term_disable,
	.check_charger_resume = qcom_check_charger_resume,
	.get_chargerid_volt = smbchg_get_chargerid_volt,
	.set_chargerid_switch_val = smbchg_set_chargerid_switch_val,
	.get_chargerid_switch_val = smbchg_get_chargerid_switch_val,
	.need_to_check_ibatt = smbchg_need_to_check_ibatt,
	.get_chg_current_step = smbchg_get_chg_current_step,
	.get_charger_type = opchg_get_charger_type,
	.get_charger_volt = qpnp_get_prop_charger_voltage_now,
	.check_chrdet_status = oppo_chg_is_usb_present,
	.get_instant_vbatt = qpnp_get_battery_voltage,
	.get_rtc_soc = oppo_chg_get_shutdown_soc,
	.set_rtc_soc = oppo_chg_backup_soc,
	.get_aicl_ma = smbchg_get_aicl_level_ma,
	.rerun_aicl = smbchg_rerun_aicl,
	.tlim_en = smbchg_force_tlim_en,
	.set_system_temp_level = smbchg_system_temp_level_set,
	.set_dp_dm = smbchg_dp_dm,
	.calc_flash_current = smbchg_calc_max_flash_current,
};
