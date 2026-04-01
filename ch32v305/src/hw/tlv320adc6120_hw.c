#include "tlv320adc6120_hw.h"

#include "hw/i2c_hw.h"

#include "debug.h"

/* Page 0: Table 8-55 (SBASA92A). */
#define TLV320_REG_PAGE_CFG        0x00U
#define TLV320_REG_SW_RESET        0x01U
#define TLV320_REG_SLEEP_CFG       0x02U
#define TLV320_REG_ASI_CFG0        0x07U
#define TLV320_REG_MST_CFG0        0x13U
#define TLV320_REG_MST_CFG1        0x14U
#define TLV320_REG_GPIO_CFG0       0x21U
#define TLV320_REG_CM_TOL_CFG      0x3AU
#define TLV320_REG_BIAS_CFG        0x3BU
#define TLV320_REG_CH1_CFG0        0x3CU
#define TLV320_REG_CH2_CFG0        0x41U
#define TLV320_REG_IN_CH_EN        0x73U
#define TLV320_REG_ASI_OUT_CH_EN   0x74U
#define TLV320_REG_PWR_CFG         0x75U

/* SLEEP_CFG (0x02): SLEEP_ENZ=1 exits sleep; AREG_SELECT per AVDD hookup. */
#define TLV320_SLEEP_WAKE_EXT_AREG 0x01U   /* 1.8 V AVDD, external AREG */
#define TLV320_SLEEP_WAKE_INT_AREG 0x81U   /* 3.3 V AVDD, internal AREG regulator */

#define TLV320_SLEEP_CFG_VAL       ((TLV320ADC6120_USE_INTERNAL_AREG) != 0 ? TLV320_SLEEP_WAKE_INT_AREG : TLV320_SLEEP_WAKE_EXT_AREG)

/*
 * ASI_CFG0 (0x07): I2S (ASI_FORMAT=01), 24-bit word (ASI_WLEN=10).
 * Remaining bits default: FSYNC/BCLK polarity default, TX_EDGE/TX_FILL = 0.
 */
#define TLV320_ASI_CFG0_I2S_24BIT   0x60U

/*
 * GPIO_CFG0 (0x21): GPIO1 as MCLK input.
 * The drive mode bits are left at TI's documented 0b010 setting.
 */
#define TLV320_GPIO_CFG0_MCLK_INPUT 0xA2U

/*
 * CM_TOL_CFG (0x3A):
 * - CH1_INP_CM_TOL_CFG = 10b
 * - CH2_INP_CM_TOL_CFG = 10b
 *
 * TI documents this as the high-CMRR mode that supports 0-AVDD common-mode
 * tolerance when the analog input impedance is 10 kOhm or 20 kOhm.
 */
#define TLV320_CM_TOL_HIGH_CMRR_BOTH 0xA0U

/*
 * BIAS_CFG (0x3B):
 * - MBIAS_VAL = 000b: MICBIAS = VREF
 * - ADC_FSCALE = 00b: VREF = 2.75 V
 *
 * Keep the ADC reference at its default 2.75 V.
 */
#define TLV320_BIAS_CFG_VREF_2V75  0x00U

/*
 * CHx_CFG0 (0x3C / 0x41):
 * - INTYP = 1b: line input
 * - INSRC = 00b: analog differential input
 * - DC = 0b: AC-coupled input
 * - IMP = 01b: 10-kOhm input impedance
 * - DREEN = 0b: disabled
 *
 * Differential AC-coupled mode lets the TLV320 establish the input common-mode
 * internally on the codec side of the coupling capacitors.
 */
#define TLV320_CH_CFG0_LINE_DIFF_AC_10K 0x84U

/*
 * MST_CFG0 (0x13):
 * - bit7 = 1: controller mode (codec drives BCLK/FSYNC)
 * - bit6 = 0: auto clock enabled
 * - bit5 = 0: PLL enabled in auto mode
 * - bit4 = 0: do not gate BCLK/FSYNC
 * - bit3 = 0: 48-kHz family
 * - bits2:0 = 110: 24.000 MHz MCLK input selection
 */
#define TLV320_MST_CFG0_CTLR_24MHZ  0x86U

/*
 * MST_CFG1 (0x14):
 * - bits7:4 = 0110: FS_RATE = 192 kHz in the 48-kHz family
 * - bits3:0 = 0100: BCLK/FSYNC ratio = 64
 *
 * CH32's SPI/I2S block uses 32-bit channel frames for I2S_DataFormat_24b, so
 * the codec must also drive 64 BCLKs per stereo frame.
 */
#define TLV320_MST_CFG1_192K_BCLK64 0x64U

/* IN_CH_EN reset 0xC0: analog CH1+CH2 on; no change required. */

/* ASI_OUT_CH_EN: enable slots for CH1 and CH2 on SDOUT. */
#define TLV320_ASI_OUT_CH12_EN      0xC0U

/* PWR_CFG: power ADC + PLL (MICBIAS off). Add 0x80 if mic bias is required. */
#define TLV320_PWR_ADC_PLL_ON       0x60U

static ErrorStatus tlv320adc6120_hw_write_reg(uint8_t reg, uint8_t value)
{
    return i2c_hw_write_register(TLV320ADC6120_I2C_ADDR_7BIT, reg, value);
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
    Delay_Ms(10U);

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
    Delay_Ms(10U);

    /* ASI: I2S, 24-bit — matches i2s_hw.c I2S_DataFormat_24b + Philips. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_ASI_CFG0, TLV320_ASI_CFG0_I2S_24BIT) != READY)
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
