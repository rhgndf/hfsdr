#include "si5351_hw.h"

#include "hw/i2c_hw.h"

#include "debug.h"

#include <stdint.h>

/* Etherkit-style internal scaling: frequencies in 0.01 Hz units (×100). */
#define SI5351_FREQ_MULT 100ULL

/* Si5351 / Etherkit limits (Hz at output, before R-div scaling in algorithm). */
#define SI5351_CLKOUT_MAX_HZ   225000000ULL
#define SI5351_PLL_VCO_MIN_HZ  400000000ULL
#define SI5351_PLL_TARGET_HZ   900000000ULL
#define SI5351_PLL_VCO_MAX_HZ  1000000000ULL
#define SI5351_PLL_DENOM_MAX   1048575U
#define SI5351_MULTISYNTH_A_MIN 6U
#define SI5351_MULTISYNTH_A_MAX 1800U
#define SI5351_PLL_A_MIN       15U
#define SI5351_PLL_A_MAX       90U

/* Si5351 register addresses (AN619 / datasheet). */
#define SI5351_REG_DEVICE_STATUS   0U
#define SI5351_REG_OUTPUT_ENABLE   3U
#define SI5351_REG_CLK0_CTRL       16U
#define SI5351_REG_CLK1_CTRL       17U
#define SI5351_REG_PLL_A_PARAMS    26U
#define SI5351_REG_MS0_PARAMS      42U
#define SI5351_REG_MS1_PARAMS      50U
#define SI5351_REG_CLK0_PHASE      165U
#define SI5351_REG_CLK1_PHASE      166U
#define SI5351_REG_PLL_RESET       177U
#define SI5351_REG_CRYSTAL_LOAD    183U
#define SI5351_DEVICE_STATUS_LOL_A 0x20U

#define SI5351_CLK_INTEGER_MODE    (1U << 6)
#define SI5351_MS_R_DIV_SHIFT      4U
#define SI5351_MS_R_DIV_MASK       (7U << 4)
#define SI5351_MS_DIVBY4_MASK      (3U << 2)

/* R divider encoding for CLK0 ctrl bits [6:4]. */
#define SI5351_R_DIV_1    0U
#define SI5351_R_DIV_2    1U
#define SI5351_R_DIV_4    2U
#define SI5351_R_DIV_8    3U
#define SI5351_R_DIV_16   4U
#define SI5351_R_DIV_32   5U
#define SI5351_R_DIV_64   6U
#define SI5351_R_DIV_128  7U

struct si5351_ms
{
    uint32_t p1;
    uint32_t p2;
    uint32_t p3;
};

struct si5351_pll_config
{
    uint32_t mult;
    uint32_t num;
    uint32_t denom;
    struct si5351_ms ms;
};

struct si5351_output_config
{
    uint32_t div;
    uint8_t r_div;
    uint8_t r_div_factor;
    uint8_t div_by_4;
    uint8_t allow_integer_mode;
    uint8_t phase_offset;
    struct si5351_ms ms;
};

uint64_t si5351_actual_frequency;

static void si5351_pack_ms(struct si5351_ms ms, uint8_t buf[8])
{
    buf[0] = (uint8_t)((ms.p3 >> 8) & 0xFFU);
    buf[1] = (uint8_t)(ms.p3 & 0xFFU);
    buf[2] = (uint8_t)((ms.p1 >> 16) & 0x03U);
    buf[3] = (uint8_t)((ms.p1 >> 8) & 0xFFU);
    buf[4] = (uint8_t)(ms.p1 & 0xFFU);
    buf[5] = (uint8_t)(((ms.p3 >> 12) & 0xF0U) | ((ms.p2 >> 16) & 0x0FU));
    buf[6] = (uint8_t)((ms.p2 >> 8) & 0xFFU);
    buf[7] = (uint8_t)(ms.p2 & 0xFFU);
}

static void si5351_pack_output_ms(const struct si5351_output_config *out_conf, uint8_t buf[8])
{
    si5351_pack_ms(out_conf->ms, buf);
    buf[2] &= 0x03U;
    buf[2] |= (uint8_t)((out_conf->r_div << SI5351_MS_R_DIV_SHIFT) & SI5351_MS_R_DIV_MASK);
    if(out_conf->div_by_4 != 0U)
    {
        buf[2] |= (uint8_t)SI5351_MS_DIVBY4_MASK;
    }
}

static ErrorStatus si5351_validate_phase_offset(uint32_t phase_offset)
{
    if(phase_offset > 127U)
    {
        return NoREADY;
    }

    return READY;
}

static struct si5351_ms si5351_calc_pll_ms(const struct si5351_pll_config *pll_conf)
{
    struct si5351_ms ms;
    uint32_t t;

    t = (128U * pll_conf->num) / pll_conf->denom;
    ms.p1 = 128U * pll_conf->mult + t - 512U;
    ms.p2 = 128U * pll_conf->num - pll_conf->denom * t;
    ms.p3 = pll_conf->denom;
    return ms;
}

static void si5351_approximate_fraction(uint64_t num, uint64_t den, uint64_t max_den,
                                        uint64_t *out_num, uint64_t *out_den)
{
    uint64_t p0 = 0U, q0 = 1U;
    uint64_t p1 = 1U, q1 = 0U;
    uint64_t n = num, d = den;

    while(d != 0U)
    {
        uint64_t a = n / d;
        uint64_t q2 = q0 + a * q1;

        if(q2 > max_den)
        {
            break;
        }

        {
            uint64_t p2 = p0 + a * p1;

            p0 = p1;
            q0 = q1;
            p1 = p2;
            q1 = q2;
        }

        {
            uint64_t rem = n - a * d;

            n = d;
            d = rem;
        }
    }

    if(q1 == 0U)
    {
        *out_num = 0U;
        *out_den = 1U;
        return;
    }

    {
        uint64_t k = (max_den - q0) / q1;
        uint64_t bound1_num = p0 + k * p1;
        uint64_t bound1_den = q0 + k * q1;

        if((2U * d * bound1_den) <= den)
        {
            *out_num = p1;
            *out_den = q1;
        }
        else
        {
            *out_num = bound1_num;
            *out_den = bound1_den;
        }
    }
}

static uint8_t si5351_select_r_div(uint64_t *freq_scaled)
{
    (void)freq_scaled;
    return SI5351_R_DIV_1;
}

static uint8_t si5351_r_div_factor(uint8_t r_div)
{
    switch(r_div)
    {
    case SI5351_R_DIV_2:
        return 2U;
    case SI5351_R_DIV_4:
        return 4U;
    case SI5351_R_DIV_8:
        return 8U;
    case SI5351_R_DIV_16:
        return 16U;
    case SI5351_R_DIV_32:
        return 32U;
    case SI5351_R_DIV_64:
        return 64U;
    case SI5351_R_DIV_128:
        return 128U;
    default:
        return 1U;
    }
}

static ErrorStatus si5351_calculate_clk0_config(uint64_t freq_scaled, struct si5351_pll_config *pll_conf,
                                                struct si5351_output_config *out_conf)
{
    const uint64_t xtal_scaled = (uint64_t)SI5351_XTAL_FREQ_HZ * SI5351_FREQ_MULT;
    uint64_t output_clock_scaled = freq_scaled;
    uint64_t pll_num, base_num, base_den;

    out_conf->r_div = si5351_select_r_div(&output_clock_scaled);
    out_conf->r_div_factor = si5351_r_div_factor(out_conf->r_div);
    out_conf->allow_integer_mode = 1U;
    out_conf->phase_offset = 0U;


    uint64_t calculated_div = (SI5351_PLL_TARGET_HZ * SI5351_FREQ_MULT / 2) / freq_scaled * 2;
    if (calculated_div < 4) {
        calculated_div = 4;
    } else if (calculated_div > 126) {
        calculated_div = 126;
    }
    out_conf->div = calculated_div;
    out_conf->div_by_4 = (out_conf->div == 4U) ? 1U : 0U;
    out_conf->ms = out_conf->div_by_4 ? (struct si5351_ms){0U, 0U, 1U}
                                      : (struct si5351_ms){out_conf->div, 0U, 1U};

    uint64_t vco_freq = output_clock_scaled * calculated_div;
    si5351_approximate_fraction(vco_freq, xtal_scaled, SI5351_PLL_DENOM_MAX, &base_num, &base_den);
    
    if(base_den == 0U)
    {
        return NoREADY;
    }

    pll_conf->mult = (uint32_t)(base_num / base_den);
    pll_conf->num = (uint32_t)(base_num % base_den);
    pll_conf->denom = base_den;
    pll_conf->ms = si5351_calc_pll_ms(pll_conf);
    si5351_actual_frequency = xtal_scaled * (pll_conf->mult * base_den + pll_conf->num) / (base_den * calculated_div);

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

static ErrorStatus si5351_hw_apply_clk(uint8_t ctrl_reg, const struct si5351_output_config *out_conf)
{
    uint8_t reg;

    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, ctrl_reg, &reg) != READY)
    {
        return NoREADY;
    }

    /* Exit power-down; clear int MS; then rebuild drive/source settings. */
    reg &= (uint8_t) ~0x80U;
    reg &= (uint8_t) ~SI5351_CLK_INTEGER_MODE;

    /* 8 mA + MS0 ← PLL A routing (bits 0–3 = 0x0F). */
    reg |= 0x0FU;

    if(out_conf->allow_integer_mode != 0U)
    {
        reg |= (uint8_t)SI5351_CLK_INTEGER_MODE;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, ctrl_reg, reg) != READY)
    {
        return NoREADY;
    }

    return READY;
}

static ErrorStatus si5351_enable_clk_output(uint8_t clk_index)
{
    uint8_t oe;

    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, &oe) != READY)
    {
        return NoREADY;
    }
    oe &= (uint8_t) ~(1U << clk_index);
    return i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_OUTPUT_ENABLE, oe);
}

static ErrorStatus si5351_hw_clk0_quadrature_set_freq_hz(uint64_t hz)
{
    uint8_t pll_buf[8];
    uint8_t ms0_buf[8];
    uint8_t ms1_buf[8];
    uint64_t freq_scaled;
    struct si5351_pll_config pll_conf;
    struct si5351_output_config clk0_conf;
    struct si5351_output_config clk1_conf;
    uint32_t phase_offset;

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

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_CTRL, 0x80U) != READY)
    {
        return NoREADY;
    }
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_CTRL, 0x80U) != READY)
    {
        return NoREADY;
    }

    freq_scaled = hz * SI5351_FREQ_MULT;
    if(si5351_calculate_clk0_config(freq_scaled, &pll_conf, &clk0_conf) != READY)
    {
        return NoREADY;
    }

    phase_offset = (uint32_t)clk0_conf.div * (uint32_t)clk0_conf.r_div_factor;

    clk1_conf = clk0_conf;
    clk0_conf.allow_integer_mode = 0U;
    clk1_conf.allow_integer_mode = 0U;
    clk1_conf.phase_offset = (uint8_t)phase_offset;

    si5351_pack_ms(pll_conf.ms, pll_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_A_PARAMS, pll_buf, sizeof(pll_buf)) != READY)
    {
        return NoREADY;
    }

    si5351_pack_output_ms(&clk0_conf, ms0_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS0_PARAMS, ms0_buf, sizeof(ms0_buf)) != READY)
    {
        return NoREADY;
    }

    si5351_pack_output_ms(&clk1_conf, ms1_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS1_PARAMS, ms1_buf, sizeof(ms1_buf)) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_PHASE, clk0_conf.phase_offset) != READY)
    {
        return NoREADY;
    }
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_PHASE, clk1_conf.phase_offset) != READY)
    {
        return NoREADY;
    }

    if(si5351_hw_apply_clk(SI5351_REG_CLK0_CTRL, &clk0_conf) != READY)
    {
        return NoREADY;
    }
    if(si5351_hw_apply_clk(SI5351_REG_CLK1_CTRL, &clk1_conf) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_RESET, 0x20U) != READY)
    {
        return NoREADY;
    }
    Delay_Ms(2U);

    if(si5351_enable_clk_output(0U) != READY)
    {
        return NoREADY;
    }
    if(si5351_enable_clk_output(1U) != READY)
    {
        return NoREADY;
    }

    return READY;
}

ErrorStatus si5351_hw_clk0_set_freq_hz(uint64_t hz)
{
    return si5351_hw_clk0_quadrature_set_freq_hz(hz);
}

uint64_t si5351_hw_clk0_get_freq_hz()
{
    return si5351_actual_frequency / SI5351_FREQ_MULT;
}

ErrorStatus si5351_hw_get_plla_lock(uint8_t *locked)
{
    uint8_t device_status;

    if(locked == 0)
    {
        return NoREADY;
    }
    if(i2c_hw_read_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_DEVICE_STATUS, &device_status) != READY)
    {
        return NoREADY;
    }

    *locked = ((device_status & SI5351_DEVICE_STATUS_LOL_A) == 0U) ? 1U : 0U;
    return READY;
}
