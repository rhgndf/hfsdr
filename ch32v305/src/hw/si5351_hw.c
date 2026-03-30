#include "si5351_hw.h"

#include "hw/i2c_hw.h"

#include "debug.h"

/* Si5351 register addresses (see Silicon Labs AN619 / datasheet). */
#define SI5351_REG_DEVICE_STATUS   0U
#define SI5351_REG_OUTPUT_ENABLE   3U
#define SI5351_REG_CLK0_CTRL       16U
#define SI5351_REG_PLL_A_PARAMS    26U
#define SI5351_REG_MS0_PARAMS      42U
#define SI5351_REG_PLL_RESET       177U
#define SI5351_REG_CRYSTAL_LOAD    183U

/*
 * 94 MHz on CLK0 with 24 MHz XO:
 *   VCO = 846 MHz = 24 MHz × (35 + 1/4)
 *   Fout = 846 MHz / 9 = 94 MHz (integer MS0 divide)
 */
#define PLL_A_A 35U
#define PLL_A_B 1U
#define PLL_A_C 4U

#define MS0_DIV_A 9U
#define MS0_DIV_B 0U
#define MS0_DIV_C 1U

static void si5351_pack_ms(uint32_t p1, uint32_t p2, uint32_t p3, uint8_t buf[8])
{
    buf[0] = (uint8_t)((p3 >> 8) & 0xFFU);
    buf[1] = (uint8_t)(p3 & 0xFFU);
    buf[2] = (uint8_t)((p1 >> 16) & 0x03U);
    buf[3] = (uint8_t)((p1 >> 8) & 0xFFU);
    buf[4] = (uint8_t)(p1 & 0xFFU);
    buf[5] = (uint8_t)(((p3 >> 12) & 0xF0U) | ((p2 >> 16) & 0x0FU));
    buf[6] = (uint8_t)((p2 >> 8) & 0xFFU);
    buf[7] = (uint8_t)(p2 & 0xFFU);
}

static void si5351_calc_ms_p(uint32_t a, uint32_t b, uint32_t c, uint32_t *p1, uint32_t *p2, uint32_t *p3)
{
    uint32_t t;

    t = (128U * b) / c;
    *p1 = 128U * a + t - 512U;
    *p2 = 128U * b - c * t;
    *p3 = c;
}

ErrorStatus si5351_hw_clk0_set_94mhz(void)
{
    uint8_t pll_buf[8];
    uint8_t ms0_buf[8];
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
    uint8_t st;
    uint8_t oe;
    uint32_t timeout;

    if(SI5351_XTAL_FREQ_HZ != 24000000UL)
    {
        /* Precomputed for 24 MHz XO only; extend si5351_hw.c for other references. */
        return NoREADY;
    }

    timeout = 100000U;
    do
    {
        if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_DEVICE_STATUS, &st) != READY)
        {
            return NoREADY;
        }
        if((st & 0x80U) == 0U)
        {
            break;
        }
        if(timeout-- == 0U)
        {
            return NoREADY;
        }
    } while(1);

    /* ~10 pF internal load on XO (same class as Etherkit default). */
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CRYSTAL_LOAD, 0xD2U) != READY)
    {
        return NoREADY;
    }

    /* Power down CLK0 while programming multisynth (datasheet flow). */
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_CTRL, 0x80U) != READY)
    {
        return NoREADY;
    }

    si5351_calc_ms_p(PLL_A_A, PLL_A_B, PLL_A_C, &p1, &p2, &p3);
    si5351_pack_ms(p1, p2, p3, pll_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_A_PARAMS, pll_buf, sizeof(pll_buf)) != READY)
    {
        return NoREADY;
    }

    si5351_calc_ms_p(MS0_DIV_A, MS0_DIV_B, MS0_DIV_C, &p1, &p2, &p3);
    si5351_pack_ms(p1, p2, p3, ms0_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS0_PARAMS, ms0_buf, sizeof(ms0_buf)) != READY)
    {
        return NoREADY;
    }

    /*
     * CLK0: MS0 fed from PLL A, integer-mode MS (bit 6), 8 mA drive (bits 0–1 = 3).
     * Same pattern as common Arduino Si5351 libraries (0x0F | 0x40).
     */
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_CTRL, 0x4FU) != READY)
    {
        return NoREADY;
    }

    /* Reset PLL A so it locks to new parameters. */
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_RESET, 0x20U) != READY)
    {
        return NoREADY;
    }
    Delay_Ms(2U);

    /* Output enable: bit set = clock off; clear bit 0 to enable CLK0 (read/modify for any POR value). */
    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, &oe) != READY)
    {
        return NoREADY;
    }
    oe &= (uint8_t) ~(1U << 0);
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, oe) != READY)
    {
        return NoREADY;
    }

    return READY;
}
