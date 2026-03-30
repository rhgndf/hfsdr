#include "tlv320adc6120_hw.h"

#include "hw/i2c_hw.h"

#include "debug.h"

/* Page 0: Table 8-55 (SBASA92A). */
#define TLV320_REG_PAGE_CFG        0x00U
#define TLV320_REG_SW_RESET        0x01U
#define TLV320_REG_SLEEP_CFG       0x02U
#define TLV320_REG_ASI_CFG0        0x07U
#define TLV320_REG_MST_CFG0        0x13U
#define TLV320_REG_IN_CH_EN        0x73U
#define TLV320_REG_ASI_OUT_CH_EN   0x74U
#define TLV320_REG_PWR_CFG         0x75U

/* SLEEP_CFG (0x02): SLEEP_ENZ=1 exits sleep; AREG_SELECT per AVDD hookup. */
#define TLV320_SLEEP_WAKE_EXT_AREG 0x01U   /* 1.8 V AVDD, external AREG */
#define TLV320_SLEEP_WAKE_INT_AREG 0x81U   /* 3.3 V AVDD, internal AREG regulator */

#define TLV320_SLEEP_CFG_VAL       ((TLV320ADC6120_USE_INTERNAL_AREG) != 0 ? TLV320_SLEEP_WAKE_INT_AREG : TLV320_SLEEP_WAKE_EXT_AREG)

/*
 * ASI_CFG0 (0x07): I2S (ASI_FORMAT=01), 16-bit word (ASI_WLEN=00).
 * Remaining bits default: FSYNC/BCLK polarity default, TX_EDGE/TX_FILL = 0.
 */
#define TLV320_ASI_CFG0_I2S_16BIT   0x40U

/* MST_CFG0 reset 0x02: slave, auto clock, PLL enabled in auto — OK for I2S slave. */

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

    /* ASI: I2S, 16-bit — matches i2s_hw.c I2S_DataFormat_16b + Philips. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_ASI_CFG0, TLV320_ASI_CFG0_I2S_16BIT) != READY)
    {
        return NoREADY;
    }

    /* Confirm slave + auto clock (POR 0x02). */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_MST_CFG0, 0x02U) != READY)
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

    /* Power ADC + PLL; FSYNC/BCLK from MCU lock the internal PLL in auto mode. */
    if(tlv320adc6120_hw_write_reg(TLV320_REG_PWR_CFG, TLV320_PWR_ADC_PLL_ON) != READY)
    {
        return NoREADY;
    }

    return READY;
}
