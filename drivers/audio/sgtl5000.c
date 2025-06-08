/*
 * SGTL5000 Codec Driver for Zephyr
 * Author: Nick McCarty nick@inphaseembedded.com
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/audio/codec.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/clock_control.h>

#define LOG_LEVEL CONFIG_AUDIO_CODEC_LOG_LEVEL
LOG_MODULE_REGISTER(nxp_sgtl5000);

#include "sgtl5000.h"

#define DT_DRV_COMPAT nxp_sgtl5000

struct sgtl5000_driver_config {
	struct i2c_dt_spec i2c;
	int clock_source;
	const struct device *mclk_dev;
	clock_control_subsys_t mclk_name;
	uint32_t pll_freq;
	uint32_t sys_fs_freq;
};

#define DEV_CFG(dev) ((const struct sgtl5000_driver_config *const)(dev)->config)

static void sgtl5000_write_reg(const struct device *dev, uint16_t reg, uint16_t val)
{
	const struct sgtl5000_driver_config *cfg = DEV_CFG(dev);
	uint8_t data[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};

	if (i2c_write(cfg->i2c.bus, data, 4, cfg->i2c.addr) != 0) {
		LOG_ERR("I2C write failed: reg 0x%04X", reg);
	} else {
		LOG_DBG("Write reg 0x%04X <- 0x%04X", reg, val);
	}
}

static void sgtl5000_read_reg(const struct device *dev, uint16_t reg, uint16_t *val)
{
	const struct sgtl5000_driver_config *cfg = DEV_CFG(dev);
	uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
	uint8_t val_buf[2] = {0};

	if (i2c_write_read(cfg->i2c.bus, cfg->i2c.addr, reg_buf, 2, val_buf, 2) == 0) {
		*val = ((uint16_t)val_buf[0] << 8) | val_buf[1];
		LOG_DBG("Read reg 0x%04X = 0x%04X", reg, *val);
	} else {
		LOG_ERR("I2C read failed: reg 0x%04X", reg);
	}
}

static void sgtl5000_update_reg(const struct device *dev, uint16_t reg, uint16_t mask, uint16_t val)
{
	uint16_t tmp;
	sgtl5000_read_reg(dev, reg, &tmp);
	tmp = (tmp & ~mask) | (val & mask);
	sgtl5000_write_reg(dev, reg, tmp);
}

static void sgtl5000_reset(const struct device *dev)
{
	sgtl5000_write_reg(dev, SGTL5000_CHIP_SOFT_RESET, 0x0000);
	k_msleep(1);
}

static int codec_configure_clocks(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct i2s_config *i2s;
	const struct sgtl5000_driver_config *const dev_cfg = dev->config;
	uint32_t rate;
	int madc, nadc, aosr, mdac, ndac, dosr, bclk_div, mclk_div;
	int p, r, j, d;
	int i, ret;

	i2s = &cfg->dai_cfg.i2s;

	if (dev_cfg->clock_source == 0) {
		ret = clock_control_get_rate(dev_cfg->mclk_dev, dev_cfg->mclk_name,
					     &cfg->mclk_freq);
		if (ret < 0) {
			LOG_ERR("MCLK clock source freq acquire fail: %d", ret);
		}
	}
	// CLK_TOP_CTRL
	if (dev_cfg->clock_source > 0) {
		// SGTL is I2S Master
		// Datasheet Pg. 14: Using the PLL - Asynchronous SYS_MCLK input
		if (cfg->mclk_freq > 17000000) {
			sgtl5000_write_reg(dev, SGTL5000_CLK_TOP_CTRL, 
				SGTL5000_CLK_TOP_CTRL_SET(
					SGTL5000_CLK_TOP_CTRL_ENABLE_INT_OSC_DISABLE,
					SGTL5000_CLK_TOP_CTRL_INPUT_FREQ_DIV2_SYS_MCLK_DIV2));
		} else {
			sgtl5000_write_reg(dev, SGTL5000_CLK_TOP_CTRL,
				SGTL5000_CLK_TOP_CTRL_SET(
					SGTL5000_CLK_TOP_CTRL_ENABLE_INT_OSC_DISABLE,
					SGTL5000_CLK_TOP_CTRL_INPUT_FREQ_DIV2_PASS_THROUGH));
		}

		uint32_t int_divisor = (dev_cfg->pll_freq / cfg->mclk_freq) & 0x1f;
		uint32_t frac_divisor = (uint32_t)((((float)cfg->mclk_freq / cfg->mclk_freq) - int_divisor) * 2048.0f) & 0x7ff;

		// PLL_CTRL
		sgtl5000_write_reg(dev, SGTL5000_PLL_CTRL, SGTL5000_PLL_CTRL_SET(
			int_divisor,
			frac_divisor));
		// ANA_POWER
		sgtl5000_write_reg(
			dev, SGTL5000_ANA_POWER,
			SGTL5000_ANA_POWER_SET(SGTL5000_ANA_POWER_DAC_MONO_STEREO,
				SGTL5000_ANA_POWER_LINREG_SIMPLE_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_STARTUP_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_VDDC_CHRGPMP_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_PLL_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_LINREG_D_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_VCOAMP_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_VAG_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_ADC_MONO_STEREO,
				SGTL5000_ANA_POWER_REFTOP_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_HEADPHONE_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_DAC_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_CAPLESS_HEADPHONE_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_ADC_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_LINEOUT_POWERUP_POWER_UP));
	} else {
		// SGTL is I2S Slave
		sgtl5000_write_reg(dev, SGTL5000_ANA_POWER, SGTL5000_ANA_POWER_SET(SGTL5000_ANA_POWER_DAC_MONO_STEREO,
				SGTL5000_ANA_POWER_LINREG_SIMPLE_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_STARTUP_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_VDDC_CHRGPMP_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_PLL_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_LINREG_D_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_VCOAMP_POWERUP_POWER_DOWN,
				SGTL5000_ANA_POWER_VAG_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_ADC_MONO_STEREO,
				SGTL5000_ANA_POWER_REFTOP_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_HEADPHONE_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_DAC_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_CAPLESS_HEADPHONE_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_ADC_POWERUP_POWER_UP,
				SGTL5000_ANA_POWER_LINEOUT_POWERUP_POWER_UP));
	}
	uint8_t sys_fs_reg_val = SGTL5000_CLK_CTRL_SYS_FS_DEFAULT;
	switch (dev_cfg->sys_fs_freq) {
		case 32000:
			sys_fs_reg_val = SGTL5000_CLK_CTRL_SYS_FS_32_KHZ;
			break;
		case 44100:
			sys_fs_reg_val = SGTL5000_CLK_CTRL_SYS_FS_44_1_KHZ;
			break;
		case 48000:
			sys_fs_reg_val = SGTL5000_CLK_CTRL_SYS_FS_48_KHZ;
			break;
		case 96000:
			sys_fs_reg_val = SGTL5000_CLK_CTRL_SYS_FS_96_KHZ;
			break;
		default:
			LOG_ERR("Invalid SYS_FS frequency: %u", dev_cfg->sys_fs_freq);
			return -EINVAL;
	}
	// Check if MCLK_FREQ is a multiple of 256, 384, or 512. If it is not a multiple of 256, 384, or 512, use PLL (0x3)
	uint8_t mclk_freq_reg_val = SGTL5000_CLK_CTRL_MCLK_FREQ_DEFAULT;
	if (cfg->mclk_freq % 256 == 0) {
		mclk_freq_reg_val = SGTL5000_CLK_CTRL_MCLK_FREQ_256_FS;
	} else if (cfg->mclk_freq % 384 == 0) {
		mclk_freq_reg_val = SGTL5000_CLK_CTRL_MCLK_FREQ_384_FS;
	} else if (cfg->mclk_freq % 512 == 0) {
		mclk_freq_reg_val = SGTL5000_CLK_CTRL_MCLK_FREQ_512_FS;
	} else {
		mclk_freq_reg_val = SGTL5000_CLK_CTRL_MCLK_FREQ_PLL;
	}
	
	// CLK_CTRL
	if (dev_cfg->clock_source > 0) {
		// SGTL is I2S Master
		sgtl5000_write_reg(dev, SGTL5000_CLK_CTRL, SGTL5000_CLK_CTRL_SET(
			SGTL5000_CLK_CTRL_RATE_MODE_SYS_FS,
			sys_fs_reg_val,
			mclk_freq_reg_val));
		sgtl5000_write_reg(dev, SGTL5000_I2S_CTRL, SGTL5000_I2S_CTRL_SET(
			SGTL5000_I2S_CTRL_SCLKFREQ_64_FS,
			SGTL5000_I2S_CTRL_MS_MASTER,
			SGTL5000_I2S_CTRL_SCLK_INV_RISING,
			SGTL5000_I2S_CTRL_DLEN_16_BITS,
			SGTL5000_I2S_CTRL_I2S_MODE_I2S_MODE_OR_L_JF,
			SGTL5000_I2S_CTRL_LRALIGN_DATAWORD_START_1I2S_DELAY,
			SGTL5000_I2S_CTRL_LRPOL_0_LEFT_1_RIGHT));		
	} else {
		// SGTL is I2S Slave
		sgtl5000_write_reg(dev, SGTL5000_CLK_CTRL, SGTL5000_CLK_CTRL_SET(
			SGTL5000_CLK_CTRL_RATE_MODE_SYS_FS,
			SGTL5000_CLK_CTRL_SYS_FS_44_1_KHZ,
			SGTL5000_CLK_CTRL_MCLK_FREQ_256_FS));
		sgtl5000_write_reg(dev, SGTL5000_I2S_CTRL, SGTL5000_I2S_CTRL_SET(
			SGTL5000_I2S_CTRL_SCLKFREQ_64_FS,
			SGTL5000_I2S_CTRL_MS_SLAVE,
			SGTL5000_I2S_CTRL_SCLK_INV_FALLING,
			SGTL5000_I2S_CTRL_DLEN_16_BITS,
			SGTL5000_I2S_CTRL_I2S_MODE_I2S_MODE_OR_L_JF,
			SGTL5000_I2S_CTRL_LRALIGN_DATAWORD_START_1I2S_DELAY,
			SGTL5000_I2S_CTRL_LRPOL_0_LEFT_1_RIGHT));
	}

	LOG_DBG("MCLK %u Hz Sampling Rate: %u Hz", cfg->mclk_freq, i2s->frame_clk_freq);
	return 0;
}

static int sgtl5000_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	LOG_INF("Configuring SGTL5000...");
	const struct sgtl5000_driver_config *const dev_cfg = DEV_CFG(dev);

	if (cfg->dai_type >= AUDIO_DAI_TYPE_INVALID) {
		LOG_ERR("dai_type not supported");
		return -EINVAL;
	}
	if (cfg->dai_type == AUDIO_ROUTE_BYPASS) {
		return 0;
	}

	sgtl5000_reset(dev);

	/* Test Read ID Reg */
	uint16_t reg_val;
	sgtl5000_read_reg(dev, SGTL5000_ID, &reg_val);
	if (((reg_val & SGTL5000_PARTID_MASK) >> SGTL5000_ID_PARTID_SHIFT) !=
	    SGTL5000_ID_PARTID_DEFAULT) {
		LOG_ERR("Read ID Fail\n");
		return -EINVAL;
	}

	/* Enable regulators */
	// Disable internal VDDD
	sgtl5000_write_reg(
		dev, SGTL5000_ANA_POWER,
		SGTL5000_ANA_POWER_SET(SGTL5000_ANA_POWER_DAC_MONO_STEREO,
				       SGTL5000_ANA_POWER_LINREG_SIMPLE_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_STARTUP_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_VDDC_CHRGPMP_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_PLL_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_LINREG_D_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_VCOAMP_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_VAG_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_ADC_MONO_STEREO,
				       SGTL5000_ANA_POWER_REFTOP_POWERUP_POWER_UP,
				       SGTL5000_ANA_POWER_HEADPHONE_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_DAC_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_CAPLESS_HEADPHONE_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_ADC_POWERUP_POWER_DOWN,
				       SGTL5000_ANA_POWER_LINEOUT_POWERUP_POWER_DOWN));

	sgtl5000_write_reg(dev, SGTL5000_LINREG_CTRL,
			   SGTL5000_LINREG_CTRL_SET(SGTL5000_LINREG_CTRL_VDDC_MAN_ASSN_VDDA,
						    SGTL5000_LINREG_CTRL_VDDC_ASSN_OVRD_AUTO,
						    0xC)); // 50mV * 12 = 600mV = 0.6V | 12 = 0xC

	sgtl5000_write_reg(dev, SGTL5000_REF_CTRL,
			   SGTL5000_REF_CTRL_SET(SGTL5000_REF_CTRL_VAG_VAL_1_575V,
						 SGTL5000_REF_CTRL_BIAS_CTRL_MINUS_12_5_PERCENT,
						 SGTL5000_REF_CTRL_SMALL_POP_NORMAL));
	sgtl5000_write_reg(dev, SGTL5000_LINE_OUT_CTRL,
			   SGTL5000_LINE_OUT_CTRL_SET(SGTL5000_LINE_OUT_CTRL_OUT_CURRENT_0_54_MILIA,
						      SGTL5000_LINE_OUT_CTRL_LO_VAGCNTRL_1_65V));

	sgtl5000_write_reg(dev, SGTL5000_SHORT_CTRL,
			   SGTL5000_SHORT_CTRL_SET(SGTL5000_SHORT_CTRL_LVLADJR_100_MILIA,
						   SGTL5000_SHORT_CTRL_LVLADJL_100_MILIA,
						   SGTL5000_SHORT_CTRL_LVLADJC_200_MILIA,
						   SGTL5000_SHORT_CTRL_MODE_LR_DISABLE_SHORTDET,
						   SGTL5000_SHORT_CTRL_MODE_CM_DISABLE_SHORTDET));

	sgtl5000_write_reg(dev, SGTL5000_ANA_CTRL,
			   SGTL5000_ANA_CTRL_SET(
				   SGTL5000_ANA_CTRL_MUTE_LO_MUTE, SGTL5000_ANA_CTRL_SELECT_HP_DAC,
				   SGTL5000_ANA_CTRL_EN_ZCD_HP_ENABLED,
				   SGTL5000_ANA_CTRL_MUTE_HP_MUTE, SGTL5000_ANA_CTRL_SELECT_ADC_MIC,
				   SGTL5000_ANA_CTRL_EN_ZCD_ADC_ENABLED,
				   SGTL5000_ANA_CTRL_MUTE_ADC_MUTE));

	codec_configure_clocks(dev, cfg);


	// Power Up Digital
	sgtl5000_write_reg(dev, SGTL5000_DIG_POWER, SGTL5000_DIG_POWER_SET(SGTL5000_DIG_POWER_ADC_POWERUP_ENABLE,
				SGTL5000_DIG_POWER_DAC_POWERUP_ENABLE,
				SGTL5000_DIG_POWER_DAP_POWERUP_ENABLE,
				SGTL5000_DIG_POWER_I2S_OUT_POWERUP_ENABLE,
				SGTL5000_DIG_POWER_I2S_IN_POWERUP_ENABLE));
	k_msleep(400);
	// Set Line Out Volume
	sgtl5000_write_reg(dev, SGTL5000_LINE_OUT_VOL, SGTL5000_LINE_OUT_VOL_SET(0x1D, 0x1D)); // default approx 1.3 volts peak-to-peak


	sgtl5000_write_reg(dev, SGTL5000_SSS_CTRL, SGTL5000_SSS_CTRL_SET(
		SGTL5000_SSS_CTRL_DAP_MIX_LRSWAP_NO_LR_SWAP,
		SGTL5000_SSS_CTRL_DAP_LRSWAP_NO_LR_SWAP,
		SGTL5000_SSS_CTRL_DAC_LRSWAP_NO_LR_SWAP,
		SGTL5000_SSS_CTRL_DAP_MIX_SELECT_ADC,
		SGTL5000_SSS_CTRL_DAP_SELECT_ADC,
		SGTL5000_SSS_CTRL_DAC_SELECT_I2S_IN));

	sgtl5000_write_reg(dev, SGTL5000_ADCDAC_CTRL, SGTL5000_ADCDAC_CTRL_SET(SGTL5000_ADCDAC_CTRL_VOL_RAMP_EN_ENABLE,
		SGTL5000_ADCDAC_CTRL_VOL_EXPO_RAMP_LINEAR_4_OCTAVE,
		SGTL5000_ADCDAC_CTRL_DAC_MUTE_RIGHT_UNMUTE,
		SGTL5000_ADCDAC_CTRL_DAC_MUTE_LEFT_UNMUTE,
		SGTL5000_ADCDAC_CTRL_ADC_HPF_FREEZE_NORMAL,
		SGTL5000_ADCDAC_CTRL_ADC_HPF_BYPASS_NORMAL));
	sgtl5000_write_reg(dev, SGTL5000_DAC_VOL, SGTL5000_DAC_VOL_SET(SGTL5000_DAC_VOL_DAC_VOL_RIGHT_0DB,
		SGTL5000_DAC_VOL_DAC_VOL_LEFT_0DB)); // digital gain, 0dB
	sgtl5000_write_reg(dev, SGTL5000_ANA_HP_CTRL, SGTL5000_ANA_HP_CTRL_SET(SGTL5000_ANA_HP_CTRL_HP_VOL_RIGHT_NEG_51_5_DB,
		SGTL5000_ANA_HP_CTRL_HP_VOL_LEFT_NEG_51_5_DB)); // 0x7F lowest level
	// Unmute DAC and ADC, Select LineIn, Enable Zero Cross Detectors
	sgtl5000_write_reg(dev, SGTL5000_ANA_CTRL, SGTL5000_ANA_CTRL_SET(
		SGTL5000_ANA_CTRL_MUTE_LO_UNMUTE,
		SGTL5000_ANA_CTRL_SELECT_HP_DAC,
		SGTL5000_ANA_CTRL_EN_ZCD_HP_ENABLED,
		SGTL5000_ANA_CTRL_MUTE_HP_MUTE,
		SGTL5000_ANA_CTRL_SELECT_ADC_LINEIN,
		SGTL5000_ANA_CTRL_EN_ZCD_ADC_ENABLED,
		SGTL5000_ANA_CTRL_MUTE_ADC_UNMUTE));

	
		return 0;
}

static void sgtl5000_start_output(const struct device *dev)
{
}

static void sgtl5000_stop_output(const struct device *dev)
{
}

static int sgtl5000_set_property(const struct device *dev, audio_property_t property,
				 audio_channel_t channel, audio_property_value_t val)
{
	return -ENOTSUP;
}

static int sgtl5000_apply_properties(const struct device *dev)
{
	return 0;
}

static const struct audio_codec_api sgtl5000_driver_api = {.configure = sgtl5000_configure,
							   .start_output = sgtl5000_start_output,
							   .stop_output = sgtl5000_stop_output,
							   .set_property = sgtl5000_set_property,
							   .apply_properties =
								   sgtl5000_apply_properties};

#define SGTL5000_INIT(inst)                                                                        \
	static const struct sgtl5000_driver_config sgtl5000_config_##inst = {                      \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                 \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, NULL, &sgtl5000_config_##inst, POST_KERNEL,        \
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY, &sgtl5000_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SGTL5000_INIT)
