#include "tlv320adc6120.h"

#include "hw/i2c.h"

#include "debug.h"

/* Page 0: Table 8-55 (SBASA92A). */
#define TLV320_REG_PAGE_CFG        0x00U
#define TLV320_REG_SW_RESET        0x01U
#define TLV320_REG_SLEEP_CFG       0x02U
#define TLV320_REG_ASI_CFG0        0x07U
#define TLV320_REG_ASI_CH1         0x0BU
#define TLV320_REG_ASI_CH2         0x0CU
#define TLV320_REG_MST_CFG0        0x13U
#define TLV320_REG_MST_CFG1        0x14U
#define TLV320_REG_GPIO_CFG0       0x21U
#define TLV320_REG_CM_TOL_CFG      0x3AU
#define TLV320_REG_BIAS_CFG        0x3BU
#define TLV320_REG_CH1_CFG0        0x3CU
#define TLV320_REG_CH1_CFG1        0x3DU
#define TLV320_REG_CH1_CFG3        0x3FU
#define TLV320_REG_CH1_CFG4        0x40U
#define TLV320_REG_CH2_CFG0        0x41U
#define TLV320_REG_CH2_CFG1        0x42U
#define TLV320_REG_CH2_CFG3        0x44U
#define TLV320_REG_CH2_CFG4        0x45U
#define TLV320_REG_IN_CH_EN        0x73U
#define TLV320_REG_ASI_OUT_CH_EN   0x74U
#define TLV320_REG_PWR_CFG         0x75U

/*
 * SLEEP_CFG (0x02):
 * - bit7: AREG_SELECT = 0/1 for external/internal analog regulator
 * - bit0: SLEEP_ENZ = 1, device awake
 *
 * Choose the regulator source to match the AVDD hookup, but always keep the
 * device out of sleep during normal streaming.
 */
#define TLV320_SLEEP_WAKE_EXT_AREG 0x01U   /* 1.8 V AVDD, external AREG */
#define TLV320_SLEEP_WAKE_INT_AREG 0x81U   /* 3.3 V AVDD, internal AREG regulator */

#define TLV320_SLEEP_CFG_VAL       ((TLV320ADC6120_USE_INTERNAL_AREG) != 0 ? TLV320_SLEEP_WAKE_INT_AREG : TLV320_SLEEP_WAKE_EXT_AREG)

/*
 * ASI_CFG0 (0x07):
 * - bits7:6: ASI_FORMAT = 01b, I2S mode
 * - bits5:4: ASI_WLEN = 11b, 32-bit word length
 * - bit3: FSYNC_POL = 0
 * - bit2: BCLK_POL = 0
 * - bit1: TX_EDGE = 0
 * - bit0: TX_FILL = 0
 *
 * Keep the serial port in Philips I2S mode with 32-bit samples.
 */
#define TLV320_ASI_CFG0_I2S_32BIT   0x70U

/*
 * ASI_CH1 / ASI_CH2:
 * - bits7:6: ASI_CHx_SRC = 00b, output comes from ADC channel 1/2 path
 * - bits5:0: ASI_CHx_SLOT = slot index
 *   TLV320_ASI_CH1_LEFT_SLOT0  = 0x00: ASI_CH1_SLOT = 0, channel 1 -> left slot 0
 *   TLV320_ASI_CH2_RIGHT_SLOT0 = 0x20: ASI_CH2_SLOT = 32, channel 2 -> right slot 0
 *
 * In stereo I2S, CH1 must appear in the first slot and CH2 in the second slot.
 * CH2 reset-defaults to slot 1 in the TDM numbering, so program it explicitly.
 */
#define TLV320_ASI_CH1_LEFT_SLOT0   0x00U
#define TLV320_ASI_CH2_RIGHT_SLOT0  0x20U

/*
 * GPIO_CFG0 (0x21):
 * - bits7:5: GPIO1_CFG = 101b, MCLK input
 * - bits4:0: GPIO1_DRV = 0x02, TI-documented drive/buffer setting
 *
 * GPIO1 is repurposed from GPIO to the external 24 MHz master-clock input.
 */
#define TLV320_GPIO_CFG0_MCLK_INPUT 0xA2U

/*
 * CM_TOL_CFG (0x3A):
 * - bits7:6: CH1_INP_CM_TOL_CFG = 10b
 * - bits5:4: CH2_INP_CM_TOL_CFG = 10b
 *
 * TI documents this as the high-CMRR mode that supports 0-AVDD common-mode
 * tolerance when the analog input impedance is 10 kOhm or 20 kOhm.
 */
#define TLV320_CM_TOL_HIGH_CMRR_BOTH 0xA0U

/*
 * BIAS_CFG (0x3B):
 * - bits7:5: MBIAS_VAL = 000b, MICBIAS = VREF
 * - bits4:3: ADC_FSCALE = 00b, 2.75 V reference
 * - bits2:0: defaults unchanged
 *
 * Keep the ADC full-scale/reference setting at the default 2.75 V.
 */
#define TLV320_BIAS_CFG_VREF_2V75  0x00U

/*
 * CHx_CFG0 (0x3C / 0x41):
 * - bit7: INTYP = 1, line input
 * - bits6:5: INSRC = 00b, analog differential input
 * - bit4: DC = 0, AC-coupled input path
 * - bits3:2: IMP = 01b, 10-kOhm input impedance
 * - bit1: DREEN = 0, disabled
 * - bit0: reserved/default
 *
 * Differential AC-coupled mode lets the TLV320 establish the input common-mode
 * internally on the codec side of the coupling capacitors.
 */
#define TLV320_CH_CFG0_LINE_DIFF_AC_10K 0x84U

/*
 * CHx_CFG1 (0x3D / 0x42):
 * - bits7:1: CHx_GAIN = 0..84, 0.0 dB to 42.0 dB in 0.5-dB steps
 * - bit0: CHx_GAIN_SIGN_BIT = 0 for positive gain, 1 for negative gain
 */
#define TLV320_CH_CFG1_GAIN_0DB        0x00U

/*
 * CHx_CFG3 (0x3F / 0x44):
 * - bits7:4: CHx_GCAL = 0..15, -0.8 dB to +0.7 dB in 0.1-dB steps
 * - bits3:0: reserved, write reset value
 *
 * CHx_CFG4 (0x40 / 0x45):
 * - bits7:0: CHx_PCAL = phase delay in modulator-clock cycles
 */
#define TLV320_CH_CFG3_GAIN_CAL_0DB    0x80U
#define TLV320_CH_CFG3_GAIN_CAL_SHIFT  4U

/*
 * MST_CFG0 (0x13):
 * - bit7: MST_SLV_CFG = 1, controller mode (codec drives BCLK/FSYNC)
 * - bit6: AUTO_CLK_CFG = 0, auto clock enabled
 * - bit5: AUTO_PLL_CFG = 0, PLL enabled in auto mode
 * - bit4: BCLK_FSYNC_GATE = 0, do not gate BCLK/FSYNC
 * - bit3: FS_BCLK_RATIO = 0, 48-kHz family
 * - bits2:0: MCLK_FREQ_SEL = 110b, 24.000 MHz MCLK input selection
 */
#define TLV320_MST_CFG0_CTLR_24MHZ  0x86U

/*
 * MST_CFG1 (0x14):
 * - bits7:4: FS_RATE = 0110b, 192 kHz in the 48-kHz family
 * - bits3:0: BCLK_FSYNC_RATIO = 0100b, BCLK/FSYNC ratio = 64
 *
 * CH32's SPI/I2S block uses 32-bit channel frames for I2S_DataFormat_24b, so
 * the codec must also drive 64 BCLKs per stereo frame.
 */
#define TLV320_MST_CFG1_192K_BCLK64 0x64U

/*
 * IN_CH_EN (0x73):
 * - bits7:6: IN_CH_EN = 11b, analog CH1 + CH2 enabled
 * - remaining bits left at reset
 *
 * The reset value already enables both analog input channels, so no write is
 * needed here.
 */

/*
 * ASI_OUT_CH_EN (0x74):
 * - bit7: ASI_OUT_CH1_EN = 1, CH1 routed to ASI output
 * - bit6: ASI_OUT_CH2_EN = 1, CH2 routed to ASI output
 * - bits5:0: remaining ASI output slots disabled
 *
 * Enable only the two stereo channels on SDOUT.
 */
#define TLV320_ASI_OUT_CH12_EN      0xC0U

/*
 * PWR_CFG (0x75):
 * - bit6: ADC_PDZ = 1, ADC powered
 * - bit5: PLL_PDZ = 1, PLL powered
 * - bit7: MICBIAS_PDZ = 0, MICBIAS off
 * - remaining bits: defaults unchanged
 *
 * Bring up the ADC datapath and PLL without enabling microphone bias.
 */
#define TLV320_PWR_ADC_PLL_ON       0x60U

static ErrorStatus tlv320adc6120_hw_write_reg(uint8_t reg, uint8_t value)
{
    return i2c_hw_write_register(TLV320ADC6120_I2C_ADDR_7BIT, reg, value);
}

static uint8_t tlv320adc6120_hw_gain_cal_db_x10_to_reg(int8_t gain_cal_db_x10)
{
    if(gain_cal_db_x10 < TLV320ADC6120_CH_GAIN_CAL_MIN_DB_X10)
    {
        gain_cal_db_x10 = TLV320ADC6120_CH_GAIN_CAL_MIN_DB_X10;
    }
    if(gain_cal_db_x10 > TLV320ADC6120_CH_GAIN_CAL_MAX_DB_X10)
    {
        gain_cal_db_x10 = TLV320ADC6120_CH_GAIN_CAL_MAX_DB_X10;
    }

    return (uint8_t)((uint8_t)(gain_cal_db_x10 - TLV320ADC6120_CH_GAIN_CAL_MIN_DB_X10) << TLV320_CH_CFG3_GAIN_CAL_SHIFT);
}

ErrorStatus tlv320adc6120_hw_set_ch_gain_raw(uint8_t gain_raw)
{
    if(tlv320adc6120_hw_write_reg(TLV320_REG_PAGE_CFG, 0x00U) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH1_CFG1, gain_raw) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH2_CFG1, gain_raw) != READY)
    {
        return NoREADY;
    }

    return READY;
}

ErrorStatus tlv320adc6120_hw_set_ch_gain_db_x2(int8_t gain_db_x2)
{
    uint8_t magnitude_db_x2;
    uint8_t sign_bit;

    if(gain_db_x2 < TLV320ADC6120_CH_GAIN_MIN_DB_X2)
    {
        gain_db_x2 = TLV320ADC6120_CH_GAIN_MIN_DB_X2;
    }
    if(gain_db_x2 > (int8_t)TLV320ADC6120_CH_GAIN_MAX_DB_X2)
    {
        gain_db_x2 = (int8_t)TLV320ADC6120_CH_GAIN_MAX_DB_X2;
    }

    sign_bit = (gain_db_x2 < 0) ? 1U : 0U;
    magnitude_db_x2 = (uint8_t)((gain_db_x2 < 0) ? -gain_db_x2 : gain_db_x2);

    return tlv320adc6120_hw_set_ch_gain_raw((uint8_t)((magnitude_db_x2 << 1U) | sign_bit));
}

ErrorStatus tlv320adc6120_hw_set_ch_calibration(int8_t ch1_gain_cal_db_x10,
                                                uint8_t ch1_phase_cal_cycles,
                                                int8_t ch2_gain_cal_db_x10,
                                                uint8_t ch2_phase_cal_cycles)
{
    if(tlv320adc6120_hw_write_reg(TLV320_REG_PAGE_CFG, 0x00U) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH1_CFG3,
                                  tlv320adc6120_hw_gain_cal_db_x10_to_reg(ch1_gain_cal_db_x10)) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH1_CFG4, ch1_phase_cal_cycles) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH2_CFG3,
                                  tlv320adc6120_hw_gain_cal_db_x10_to_reg(ch2_gain_cal_db_x10)) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH2_CFG4, ch2_phase_cal_cycles) != READY)
    {
        return NoREADY;
    }

    return READY;
}

ErrorStatus tlv320adc6120_hw_init(void)
{
    /* Page 0 (default after POR; set explicitly after any page use). */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_PAGE_CFG, 0x00U) != READY)
    {
        return NoREADY;
    }

    /* Exit sleep; wait for wake per datasheet (>= 1 ms). */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_SLEEP_CFG, TLV320_SLEEP_CFG_VAL) != READY)
    {
        return NoREADY;
    }
    Delay_Ms(1U);

    /* Software reset to known defaults, then re-wake. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_SW_RESET, 0x01U) != READY)
    {
        return NoREADY;
    }
    Delay_Ms(2U);

    if(tlv320adc6120_hw_write_reg(TLV320_REG_PAGE_CFG, 0x00U) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_SLEEP_CFG, TLV320_SLEEP_CFG_VAL) != READY)
    {
        return NoREADY;
    }
    /*
     * Keep 10 ms after wake before CHx_CFG writes; datasheet requires this
     * for channel configuration registers after exiting sleep mode.
     */
    Delay_Ms(10U);

    /* ASI: I2S, 32-bit word length — matches i2s.c I2S_DataFormat_32b + Philips. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_ASI_CFG0, TLV320_ASI_CFG0_I2S_32BIT) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_ASI_CH1, TLV320_ASI_CH1_LEFT_SLOT0) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_ASI_CH2, TLV320_ASI_CH2_RIGHT_SLOT0) != READY)
    {
        return NoREADY;
    }

    /* External 24 MHz MCLK arrives on GPIO1. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_GPIO_CFG0, TLV320_GPIO_CFG0_MCLK_INPUT) != READY)
    {
        return NoREADY;
    }

    /*
     * Configure both analog channels explicitly instead of relying on reset
     * defaults: differential, AC-coupled, 10-kOhm line inputs. This avoids
     * requiring an external DC bias on the source.
     */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CM_TOL_CFG, TLV320_CM_TOL_HIGH_CMRR_BOTH) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_BIAS_CFG, TLV320_BIAS_CFG_VREF_2V75) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH1_CFG0, TLV320_CH_CFG0_LINE_DIFF_AC_10K) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_CH2_CFG0, TLV320_CH_CFG0_LINE_DIFF_AC_10K) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_set_ch_gain_raw(TLV320_CH_CFG1_GAIN_0DB) != READY)
    {
        return NoREADY;
    }

    /* Codec drives BCLK/FSYNC at 192 kHz stereo from the external 24 MHz MCLK. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_MST_CFG0, TLV320_MST_CFG0_CTLR_24MHZ) != READY)
    {
        return NoREADY;
    }
    if(tlv320adc6120_hw_write_reg(TLV320_REG_MST_CFG1, TLV320_MST_CFG1_192K_BCLK64) != READY)
    {
        return NoREADY;
    }

    /* Analog inputs CH1+CH2 enabled (same as reset 0xC0). */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_IN_CH_EN, 0xC0U) != READY)
    {
        return NoREADY;
    }

    /* Drive CH1+CH2 on TDM/I2S slots; I2S stereo uses first two slots. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_ASI_OUT_CH_EN, TLV320_ASI_OUT_CH12_EN) != READY)
    {
        return NoREADY;
    }

    /* Power ADC + PLL; controller-mode clocks are derived from the external 24 MHz MCLK. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_PWR_CFG, TLV320_PWR_ADC_PLL_ON) != READY)
    {
        return NoREADY;
    }

    return READY;
}
