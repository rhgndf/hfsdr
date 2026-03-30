#include "si5351_hw.h"

#include "hw/i2c_hw.h"

#include "debug.h"

#include <stdint.h>

/* Etherkit-style internal scaling: frequencies in 0.01 Hz units (×100). */
#define SI5351_FREQ_MULT 100ULL

/* Si5351 / Etherkit limits (Hz at output, before R-div scaling in algorithm). */
#define SI5351_CLKOUT_MAX_HZ   225000000ULL
#define SI5351_MULTISYNTH_DIVBY4_HZ 150000000ULL
#define RFRAC_DENOM            1000000ULL
#define SI5351_MULTISYNTH_A_MIN 6U
#define SI5351_MULTISYNTH_A_MAX 1800U

/* Si5351 register addresses (AN619 / datasheet). */
#define SI5351_REG_DEVICE_STATUS   0U
#define SI5351_REG_OUTPUT_ENABLE   3U
#define SI5351_REG_CLK0_CTRL       16U
#define SI5351_REG_CLK1_CTRL       17U
#define SI5351_REG_PLL_A_PARAMS    26U
#define SI5351_REG_MS0_PARAMS      42U
#define SI5351_REG_MS1_PARAMS      50U
#define SI5351_REG_PLL_RESET       177U
#define SI5351_REG_CRYSTAL_LOAD    183U

#define SI5351_CLK_INTEGER_MODE    (1U << 6)
#define SI5351_OUTPUT_CLK_DIV_SHIFT 4U
#define SI5351_OUTPUT_CLK_DIV_MASK  (7U << 4)
#define SI5351_OUTPUT_CLK_DIVBY4   (3U << 2)

/* R divider encoding for CLK0 ctrl bits [6:4]. */
#define SI5351_R_DIV_1    0U
#define SI5351_R_DIV_2    1U
#define SI5351_R_DIV_4    2U
#define SI5351_R_DIV_8    3U
#define SI5351_R_DIV_16   4U
#define SI5351_R_DIV_32   5U
#define SI5351_R_DIV_64   6U
#define SI5351_R_DIV_128  7U

/*
 * Fixed PLL A VCO = 846 MHz from 24 MHz XO:
 *   846e6 = 24e6 × (35 + 1/4)
 */
#define PLL_A_A 35U
#define PLL_A_B 1U
#define PLL_A_C 4U

#define PLL_VCO_HZ 846000000ULL

struct si5351_ms
{
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
};

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

/* Etherkit select_r_div: increase *freq_scaled until MS sits in a valid band; returns R divider code. */
static uint8_t si5351_select_r_div(uint64_t *freq_scaled)
{
    uint64_t f = *freq_scaled;
    const uint64_t lo = SI5351_MIN_OUTPUT_HZ * SI5351_FREQ_MULT;

    if((f >= lo) && (f < lo * 2ULL))
    {
        *freq_scaled = f * 128ULL;
        return SI5351_R_DIV_128;
    }
    if((f >= lo * 2ULL) && (f < lo * 4ULL))
    {
        *freq_scaled = f * 64ULL;
        return SI5351_R_DIV_64;
    }
    if((f >= lo * 4ULL) && (f < lo * 8ULL))
    {
        *freq_scaled = f * 32ULL;
        return SI5351_R_DIV_32;
    }
    if((f >= lo * 8ULL) && (f < lo * 16ULL))
    {
        *freq_scaled = f * 16ULL;
        return SI5351_R_DIV_16;
    }
    if((f >= lo * 16ULL) && (f < lo * 32ULL))
    {
        *freq_scaled = f * 8ULL;
        return SI5351_R_DIV_8;
    }
    if((f >= lo * 32ULL) && (f < lo * 64ULL))
    {
        *freq_scaled = f * 4ULL;
        return SI5351_R_DIV_4;
    }
    if((f >= lo * 64ULL) && (f < lo * 128ULL))
    {
        *freq_scaled = f * 2ULL;
        return SI5351_R_DIV_2;
    }

    return SI5351_R_DIV_1;
}

static ErrorStatus si5351_multisynth_calc(uint64_t freq_scaled, uint64_t pll_freq_scaled, struct si5351_ms *out,
                                          uint8_t *int_mode, uint8_t *div_by_4)
{
    uint64_t freq = freq_scaled;
    uint64_t pll_freq = pll_freq_scaled;
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t t;

    if(freq > SI5351_CLKOUT_MAX_HZ * SI5351_FREQ_MULT)
    {
        freq = SI5351_CLKOUT_MAX_HZ * SI5351_FREQ_MULT;
    }
    if(freq < 500000ULL * SI5351_FREQ_MULT)
    {
        freq = 500000ULL * SI5351_FREQ_MULT;
    }

    *div_by_4 = 0U;
    if(freq >= SI5351_MULTISYNTH_DIVBY4_HZ * SI5351_FREQ_MULT)
    {
        *div_by_4 = 1U;
        out->p1 = 0U;
        out->p2 = 0U;
        out->p3 = 1U;
        *int_mode = 1U;
        return READY;
    }

    a = (uint32_t)(pll_freq / freq);
    if(a < SI5351_MULTISYNTH_A_MIN || a > SI5351_MULTISYNTH_A_MAX)
    {
        return NoREADY;
    }

    b = (uint32_t)(((pll_freq % freq) * RFRAC_DENOM) / freq);
    c = b ? (uint32_t)RFRAC_DENOM : 1U;

    t = (128U * b) / c;
    out->p1 = 128U * a + t - 512U;
    out->p2 = 128U * b - c * t;
    out->p3 = c;
    *int_mode = (b == 0U) ? 1U : 0U;

    return READY;
}

static ErrorStatus si5351_wait_sys_init(void)
{
    uint8_t st;
    uint32_t timeout = 100000U;

    do
    {
        if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_DEVICE_STATUS, &st) != READY)
        {
            return NoREADY;
        }
        if((st & 0x80U) == 0U)
        {
            return READY;
        }
        if(timeout-- == 0U)
        {
            return NoREADY;
        }
    } while(1);
}

static ErrorStatus si5351_hw_apply_clk0(uint8_t r_div, uint8_t div_by_4, uint8_t int_mode)
{
    uint8_t reg;

    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_CTRL, &reg) != READY)
    {
        return NoREADY;
    }

    /* Exit power-down; clear R-div / div-by-4 / int MS; then rebuild (Etherkit-style). */
    reg &= (uint8_t) ~0x80U;
    reg &= (uint8_t) ~(SI5351_OUTPUT_CLK_DIV_MASK | SI5351_OUTPUT_CLK_DIVBY4 | SI5351_CLK_INTEGER_MODE);

    /* 8 mA + MS0 ← PLL A routing (bits 0–3 = 0x0F). */
    reg |= 0x0FU;

    if(div_by_4 != 0U)
    {
        reg |= (uint8_t)SI5351_OUTPUT_CLK_DIVBY4;
    }

    reg |= (uint8_t)((r_div << SI5351_OUTPUT_CLK_DIV_SHIFT) & SI5351_OUTPUT_CLK_DIV_MASK);

    if(int_mode != 0U)
    {
        reg |= (uint8_t)SI5351_CLK_INTEGER_MODE;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_CTRL, reg) != READY)
    {
        return NoREADY;
    }

    return READY;
}

static ErrorStatus si5351_hw_apply_clk1(uint8_t r_div, uint8_t div_by_4, uint8_t int_mode)
{
    uint8_t reg;

    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_CTRL, &reg) != READY)
    {
        return NoREADY;
    }

    reg &= (uint8_t) ~0x80U;
    reg &= (uint8_t) ~(SI5351_OUTPUT_CLK_DIV_MASK | SI5351_OUTPUT_CLK_DIVBY4 | SI5351_CLK_INTEGER_MODE);

    /* Same drive/routing style as CLK0; CLK1 register drives MS1 from PLL A. */
    reg |= 0x0FU;

    if(div_by_4 != 0U)
    {
        reg |= (uint8_t)SI5351_OUTPUT_CLK_DIVBY4;
    }

    reg |= (uint8_t)((r_div << SI5351_OUTPUT_CLK_DIV_SHIFT) & SI5351_OUTPUT_CLK_DIV_MASK);

    if(int_mode != 0U)
    {
        reg |= (uint8_t)SI5351_CLK_INTEGER_MODE;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_CTRL, reg) != READY)
    {
        return NoREADY;
    }

    return READY;
}

static ErrorStatus si5351_enable_clk0_output(void)
{
    uint8_t oe;

    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, &oe) != READY)
    {
        return NoREADY;
    }
    oe &= (uint8_t) ~(1U << 0);
    return i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, oe);
}

static ErrorStatus si5351_enable_clk1_output(void)
{
    uint8_t oe;

    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, &oe) != READY)
    {
        return NoREADY;
    }
    oe &= (uint8_t) ~(1U << 1);
    return i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, oe);
}

ErrorStatus si5351_hw_clk0_set_freq_hz(uint64_t hz)
{
    uint8_t pll_buf[8];
    uint8_t ms0_buf[8];
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
    uint64_t freq_scaled;
    uint64_t pll_scaled;
    uint8_t r_div;
    struct si5351_ms ms;
    uint8_t int_mode;
    uint8_t div_by_4;

    if(hz == 0ULL)
    {
        return NoREADY;
    }

    if(SI5351_XTAL_FREQ_HZ != 24000000UL)
    {
        return NoREADY;
    }

    /* Practical minimum for CLK0..5 path (Etherkit / datasheet regime). */
    if(hz < SI5351_MIN_OUTPUT_HZ)
    {
        return NoREADY;
    }

    if(hz > SI5351_CLKOUT_MAX_HZ)
    {
        return NoREADY;
    }

    if(si5351_wait_sys_init() != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CRYSTAL_LOAD, 0xD2U) != READY)
    {
        return NoREADY;
    }

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

    freq_scaled = hz * SI5351_FREQ_MULT;
    r_div = si5351_select_r_div(&freq_scaled);
    pll_scaled = PLL_VCO_HZ * SI5351_FREQ_MULT;

    if(si5351_multisynth_calc(freq_scaled, pll_scaled, &ms, &int_mode, &div_by_4) != READY)
    {
        return NoREADY;
    }

    si5351_pack_ms(ms.p1, ms.p2, ms.p3, ms0_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS0_PARAMS, ms0_buf, sizeof(ms0_buf)) != READY)
    {
        return NoREADY;
    }

    if(si5351_hw_apply_clk0(r_div, div_by_4, int_mode) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_RESET, 0x20U) != READY)
    {
        return NoREADY;
    }
    Delay_Ms(2U);

    if(si5351_enable_clk0_output() != READY)
    {
        return NoREADY;
    }

    return READY;
}

ErrorStatus si5351_hw_clk1_set_freq_hz(uint64_t hz)
{
    uint8_t pll_buf[8];
    uint8_t ms1_buf[8];
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
    uint64_t freq_scaled;
    uint64_t pll_scaled;
    uint8_t r_div;
    struct si5351_ms ms;
    uint8_t int_mode;
    uint8_t div_by_4;

    if(hz == 0ULL)
    {
        return NoREADY;
    }

    if(SI5351_XTAL_FREQ_HZ != 24000000UL)
    {
        return NoREADY;
    }

    if(hz < SI5351_MIN_OUTPUT_HZ)
    {
        return NoREADY;
    }

    if(hz > SI5351_CLKOUT_MAX_HZ)
    {
        return NoREADY;
    }

    if(si5351_wait_sys_init() != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CRYSTAL_LOAD, 0xD2U) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_CTRL, 0x80U) != READY)
    {
        return NoREADY;
    }

    si5351_calc_ms_p(PLL_A_A, PLL_A_B, PLL_A_C, &p1, &p2, &p3);
    si5351_pack_ms(p1, p2, p3, pll_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_A_PARAMS, pll_buf, sizeof(pll_buf)) != READY)
    {
        return NoREADY;
    }

    freq_scaled = hz * SI5351_FREQ_MULT;
    r_div = si5351_select_r_div(&freq_scaled);
    pll_scaled = PLL_VCO_HZ * SI5351_FREQ_MULT;

    if(si5351_multisynth_calc(freq_scaled, pll_scaled, &ms, &int_mode, &div_by_4) != READY)
    {
        return NoREADY;
    }

    si5351_pack_ms(ms.p1, ms.p2, ms.p3, ms1_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS1_PARAMS, ms1_buf, sizeof(ms1_buf)) != READY)
    {
        return NoREADY;
    }

    if(si5351_hw_apply_clk1(r_div, div_by_4, int_mode) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_RESET, 0x20U) != READY)
    {
        return NoREADY;
    }
    Delay_Ms(2U);

    if(si5351_enable_clk1_output() != READY)
    {
        return NoREADY;
    }

    return READY;
}

ErrorStatus si5351_hw_fm_lo_both_hz(uint64_t hz_hz)
{
    if(si5351_hw_clk0_set_freq_hz(hz_hz) != READY)
    {
        return NoREADY;
    }
    if(si5351_hw_clk1_set_freq_hz(hz_hz) != READY)
    {
        return NoREADY;
    }
    return READY;
}

ErrorStatus si5351_hw_clk0_set_94mhz(void)
{
    return si5351_hw_clk0_set_freq_hz(94000000ULL);
}
