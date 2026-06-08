#include "si5351.h"

#include "hw/i2c.h"

#include "debug.h"

#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_tim.h"

#include <stdbool.h>
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
#define SI5351_PHASE_REG_MS_MAX 126U
#define SI5351_PLL_A_MIN       15U
#define SI5351_PLL_A_MAX       90U
#define SI5351_PHASE_REG_MIN_OUTPUT_HZ 3174604ULL
#define SI5351_TEMP_OFFSET_HZ  250ULL
#define SI5351_TIM6_MAX_TICKS  65536UL
#define SI5351_TIM6_PRESCALER_DIV_MAX 65536UL
#define SI5351_PLL_LOCK_POLL_HZ 1000UL
#define SI5351_PLL_LOCK_POLLS 100U

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

enum si5351_tim6_state
{
    SI5351_TIM6_IDLE = 0,
    SI5351_TIM6_WAIT_PLL_LOCK = 1,
    SI5351_TIM6_ARMED_FINALIZE_CLK1 = 2
};

static uint64_t si5351_actual_frequency;
static volatile enum si5351_tim6_state si5351_tim6_state;
static volatile uint32_t si5351_tim6_phase_ticks;
static volatile uint32_t si5351_tim6_lock_polls_remaining;
static volatile ErrorStatus si5351_tim6_last_status = READY;
static uint8_t si5351_tim6_temp_ms1[8];
static uint8_t si5351_tim6_temp_clk1_ctrl;
static uint8_t si5351_tim6_final_ms1[8];
static uint8_t si5351_tim6_final_clk1_ctrl;

static ErrorStatus si5351_enable_quadrature_outputs(void);

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
    uint32_t t = (128U * pll_conf->num) / pll_conf->denom;
    struct si5351_ms ms;
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

static uint8_t si5351_select_r_div(uint64_t freq_scaled)
{
    uint64_t min_vco_scaled = SI5351_PLL_VCO_MIN_HZ * SI5351_FREQ_MULT;
    for(uint8_t r_div = SI5351_R_DIV_1; r_div <= SI5351_R_DIV_128; ++r_div)
    {
        uint64_t r_factor = si5351_r_div_factor(r_div);
        uint64_t output_clock_scaled = freq_scaled * r_factor;
        if((output_clock_scaled * SI5351_MULTISYNTH_A_MAX) >= min_vco_scaled)
        {
            return r_div;
        }
    }

    return SI5351_R_DIV_128;
}

static uint8_t si5351_select_no_r_div(uint64_t freq_scaled)
{
    (void)freq_scaled;
    return SI5351_R_DIV_1;
}

static uint64_t si5351_pll_actual_vco_scaled(const struct si5351_pll_config *pll_conf)
{
    const uint64_t xtal_scaled = (uint64_t)SI5351_XTAL_FREQ_HZ * SI5351_FREQ_MULT;
    uint64_t pll_num = (uint64_t)pll_conf->mult * (uint64_t)pll_conf->denom + (uint64_t)pll_conf->num;
    return (xtal_scaled * pll_num) / (uint64_t)pll_conf->denom;
}

static uint8_t si5351_clk_ctrl_value(const struct si5351_output_config *out_conf)
{
    uint8_t reg = 0x0FU;
    if(out_conf->allow_integer_mode != 0U)
    {
        reg |= (uint8_t)SI5351_CLK_INTEGER_MODE;
    }
    return reg;
}

static struct si5351_ms si5351_calc_output_ms_fraction(uint32_t div, uint32_t num, uint32_t denom)
{
    uint32_t t = (128U * num) / denom;
    struct si5351_ms ms;
    ms.p1 = 128U * div + t - 512U;
    ms.p2 = 128U * num - denom * t;
    ms.p3 = denom;
    return ms;
}

static uint32_t si5351_select_temp_b1_denominator(uint64_t final_output_scaled, uint32_t div)
{
    /*
     * Temporary CLK1 uses the same integer divider as final CLK1, plus 1/c:
     *
     *   f_final = F / a
     *   f_temp  = F / (a + 1/c)
     *   delta   = f_final - f_temp = f_final / (a*c + 1)
     *
     * Pick c ~= (f_final - delta) / (delta * a) using scaled integer Hz.
     * Clamp c >= 2 so b/c stays below 1 and the temporary integer part
     * remains a. TIM6 is later timed from the actual chosen-c delta.
     */
    uint64_t target_delta_scaled = SI5351_TEMP_OFFSET_HZ * SI5351_FREQ_MULT;
    if((target_delta_scaled == 0U) || (div == 0U) || (final_output_scaled <= target_delta_scaled))
    {
        return 2U;
    }

    uint64_t num = final_output_scaled - target_delta_scaled;
    uint64_t den = target_delta_scaled * (uint64_t)div;
    uint64_t c = (num + (den / 2U)) / den;
    if(c < 2U)
    {
        return 2U;
    }
    if(c > SI5351_PLL_DENOM_MAX)
    {
        return SI5351_PLL_DENOM_MAX;
    }
    return (uint32_t)c;
}

static ErrorStatus si5351_calc_fixed_div_b1_output_ms(uint64_t vco_scaled,
                                                      uint32_t div,
                                                      uint32_t denom,
                                                      struct si5351_ms *ms,
                                                      uint64_t *actual_output_scaled)
{
    if((denom < 2U) || (ms == 0) || (actual_output_scaled == 0))
    {
        return NoREADY;
    }

    if((div < SI5351_MULTISYNTH_A_MIN) || (div > SI5351_MULTISYNTH_A_MAX))
    {
        return NoREADY;
    }

    if(denom > SI5351_PLL_DENOM_MAX)
    {
        return NoREADY;
    }

    *ms = si5351_calc_output_ms_fraction(div, 1U, denom);
    *actual_output_scaled = (vco_scaled * denom) / ((uint64_t)div * denom + 1U);
    return READY;
}

static ErrorStatus si5351_calculate_clk0_config(uint64_t freq_scaled, uint32_t max_output_div,
                                                bool allow_r_div, struct si5351_pll_config *pll_conf,
                                                struct si5351_output_config *out_conf)
{
    const uint64_t xtal_scaled = (uint64_t)SI5351_XTAL_FREQ_HZ * SI5351_FREQ_MULT;
    uint64_t output_clock_scaled = freq_scaled;
    uint64_t base_num, base_den;

    out_conf->r_div = allow_r_div ? si5351_select_r_div(freq_scaled) : si5351_select_no_r_div(freq_scaled);
    out_conf->r_div_factor = si5351_r_div_factor(out_conf->r_div);
    output_clock_scaled = freq_scaled * (uint64_t)out_conf->r_div_factor;
    out_conf->allow_integer_mode = 1U;
    out_conf->phase_offset = 0U;

    uint64_t calculated_div = (SI5351_PLL_TARGET_HZ * SI5351_FREQ_MULT / 2U) / output_clock_scaled * 2U;
    if(calculated_div < 4U)
    {
        calculated_div = 4U;
    }
    else if(calculated_div > max_output_div)
    {
        calculated_div = max_output_div;
    }
    out_conf->div = calculated_div;
    out_conf->div_by_4 = (out_conf->div == 4U) ? 1U : 0U;
    out_conf->ms = out_conf->div_by_4 ? (struct si5351_ms){0U, 0U, 1U}
                                      : (struct si5351_ms){128U * out_conf->div - 512, 0U, 1U};

    uint64_t vco_freq = output_clock_scaled * calculated_div;
    if((vco_freq < (SI5351_PLL_VCO_MIN_HZ * SI5351_FREQ_MULT)) ||
       (vco_freq > (SI5351_PLL_VCO_MAX_HZ * SI5351_FREQ_MULT)))
    {
        return NoREADY;
    }

    si5351_approximate_fraction(vco_freq, xtal_scaled, SI5351_PLL_DENOM_MAX, &base_num, &base_den);
    
    if(base_den == 0U)
    {
        return NoREADY;
    }

    pll_conf->mult = (uint32_t)(base_num / base_den);
    pll_conf->num = (uint32_t)(base_num % base_den);
    pll_conf->denom = base_den;
    pll_conf->ms = si5351_calc_pll_ms(pll_conf);
    uint64_t actual_vco_scaled = si5351_pll_actual_vco_scaled(pll_conf);
    si5351_actual_frequency = actual_vco_scaled / ((uint64_t)calculated_div * (uint64_t)out_conf->r_div_factor);
    printf("div: %lu r:%u pll: %lu %lu %lu\n",
           (unsigned long)out_conf->div,
           (unsigned int)out_conf->r_div_factor,
           (unsigned long)pll_conf->mult,
           (unsigned long)pll_conf->num,
           (unsigned long)pll_conf->denom);
    return READY;
}

static uint32_t si5351_tim6_counter_clock_hz(void)
{
    RCC_ClocksTypeDef clk = {0};

    RCC_GetClocksFreq(&clk);
    uint32_t pclk1 = clk.PCLK1_Frequency;
    uint32_t ppre1 = (RCC->CFGR0 & RCC_PPRE1) >> 8;
    if(ppre1 != 0U)
    {
        return pclk1 * 2U;
    }
    return pclk1;
}

static ErrorStatus si5351_tim6_arm_ticks(uint32_t raw_ticks)
{
    if(raw_ticks == 0U)
    {
        return NoREADY;
    }

    /*
     * raw_ticks is measured at the APB timer input clock. Pick the smallest
     * prescaler divisor that lets the one-shot period fit in the 16-bit ARR;
     * this keeps the delay single-arm while preserving maximum timer resolution.
     */
    uint32_t prescaler_div = ((raw_ticks - 1U) >> 16) + 1U;
    if(prescaler_div > SI5351_TIM6_PRESCALER_DIV_MAX)
    {
        return NoREADY;
    }

    uint32_t ticks = ((raw_ticks - 1U) / prescaler_div) + 1U;
    if(ticks > SI5351_TIM6_MAX_TICKS)
    {
        return NoREADY;
    }

    TIM_Cmd(TIM6, DISABLE);
    TIM_PrescalerConfig(TIM6, (uint16_t)(prescaler_div - 1U), TIM_PSCReloadMode_Immediate);
    TIM_SetCounter(TIM6, 0U);
    TIM_SetAutoreload(TIM6, (uint16_t)(ticks - 1U));
    TIM_ClearFlag(TIM6, TIM_FLAG_Update);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM6, ENABLE);
    return READY;
}

static void si5351_tim6_cancel(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
    TIM_Cmd(TIM6, DISABLE);
    TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);
    TIM_ClearFlag(TIM6, TIM_FLAG_Update);
    NVIC_ClearPendingIRQ(TIM6_IRQn);
    si5351_tim6_state = SI5351_TIM6_IDLE;
    si5351_tim6_phase_ticks = 0U;
    si5351_tim6_lock_polls_remaining = 0U;
}

static void si5351_tim6_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
    TIM_DeInit(TIM6);

    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Period = 0xFFFFU;
    tim.TIM_Prescaler = 0U;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM6, &tim);
    TIM_SelectOnePulseMode(TIM6, TIM_OPMode_Single);
    TIM_ClearFlag(TIM6, TIM_FLAG_Update);
    TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);

    NVIC_InitTypeDef nvic = {0};
    nvic.NVIC_IRQChannel = TIM6_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 0;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

static uint32_t si5351_tim6_phase_delay_ticks(uint64_t delta_scaled)
{
    uint32_t timclk = si5351_tim6_counter_clock_hz();
    if(delta_scaled == 0U)
    {
        return 0U;
    }

    uint64_t ticks = ((uint64_t)timclk * SI5351_FREQ_MULT) / (4ULL * delta_scaled);
    if(ticks > 0xFFFFFFFFULL)
    {
        return 0U;
    }

    uint32_t delay_ticks = (uint32_t)ticks;
    if(delay_ticks == 0U)
    {
        delay_ticks = 1U;
    }
    return delay_ticks;
}

static uint32_t si5351_tim6_pll_lock_poll_ticks(void)
{
    uint32_t timclk = si5351_tim6_counter_clock_hz();
    uint32_t poll_ticks = timclk / SI5351_PLL_LOCK_POLL_HZ;
    if(poll_ticks == 0U)
    {
        poll_ticks = 1U;
    }
    return poll_ticks;
}

static ErrorStatus si5351_write_clk1_ms_and_ctrl(const uint8_t ms1[8], uint8_t clk1_ctrl)
{
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS1_PARAMS, ms1, 8U) != READY)
    {
        return NoREADY;
    }
    return i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_CTRL, clk1_ctrl);
}

static ErrorStatus si5351_tim6_start_pll_lock_wait(const uint8_t temp_ms1[8], uint8_t temp_clk1_ctrl,
                                                   const uint8_t final_ms1[8], uint8_t final_clk1_ctrl,
                                                   uint32_t phase_ticks)
{
    if(phase_ticks == 0U)
    {
        return NoREADY;
    }

    for(uint32_t i = 0U; i < 8U; ++i)
    {
        si5351_tim6_temp_ms1[i] = temp_ms1[i];
        si5351_tim6_final_ms1[i] = final_ms1[i];
    }
    si5351_tim6_temp_clk1_ctrl = temp_clk1_ctrl;
    si5351_tim6_final_clk1_ctrl = final_clk1_ctrl;
    si5351_tim6_phase_ticks = phase_ticks;

    si5351_tim6_state = SI5351_TIM6_WAIT_PLL_LOCK;
    si5351_tim6_last_status = READY;
    si5351_tim6_lock_polls_remaining = SI5351_PLL_LOCK_POLLS;
    return si5351_tim6_arm_ticks(si5351_tim6_pll_lock_poll_ticks());
}

static void si5351_tim6_finalize_clk1(void)
{
    TIM_Cmd(TIM6, DISABLE);
    TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);
    si5351_tim6_state = SI5351_TIM6_IDLE;
    si5351_tim6_last_status = si5351_write_clk1_ms_and_ctrl(si5351_tim6_final_ms1, si5351_tim6_final_clk1_ctrl);
}

static void si5351_tim6_handle_pll_lock_wait(void)
{
    uint8_t locked;
    if(si5351_hw_get_plla_lock(&locked) != READY)
    {
        si5351_tim6_last_status = NoREADY;
        si5351_tim6_cancel();
        return;
    }

    if(locked == 0U)
    {
        if(si5351_tim6_lock_polls_remaining == 0U)
        {
            si5351_tim6_last_status = NoREADY;
            si5351_tim6_cancel();
            return;
        }

        --si5351_tim6_lock_polls_remaining;
        if(si5351_tim6_arm_ticks(si5351_tim6_pll_lock_poll_ticks()) != READY)
        {
            si5351_tim6_last_status = NoREADY;
            si5351_tim6_cancel();
        }
        return;
    }

    if(si5351_enable_quadrature_outputs() != READY)
    {
        si5351_tim6_last_status = NoREADY;
        si5351_tim6_cancel();
        return;
    }

    if(si5351_write_clk1_ms_and_ctrl(si5351_tim6_temp_ms1, si5351_tim6_temp_clk1_ctrl) != READY)
    {
        si5351_tim6_last_status = NoREADY;
        si5351_tim6_cancel();
        return;
    }

    si5351_tim6_state = SI5351_TIM6_ARMED_FINALIZE_CLK1;
    if(si5351_tim6_arm_ticks(si5351_tim6_phase_ticks) != READY)
    {
        si5351_tim6_last_status = NoREADY;
        si5351_tim6_cancel();
    }
}

static void si5351_tim6_handle_finalize_wait(void)
{
    si5351_tim6_finalize_clk1();
}

__attribute__((interrupt)) void TIM6_IRQHandler(void)
{
    if(TIM_GetITStatus(TIM6, TIM_IT_Update) == RESET)
    {
        return;
    }

    TIM_ClearITPendingBit(TIM6, TIM_IT_Update);

    switch(si5351_tim6_state)
    {
    case SI5351_TIM6_WAIT_PLL_LOCK:
        si5351_tim6_handle_pll_lock_wait();
        break;

    case SI5351_TIM6_ARMED_FINALIZE_CLK1:
        si5351_tim6_handle_finalize_wait();
        break;

    case SI5351_TIM6_IDLE:
    default:
        TIM_Cmd(TIM6, DISABLE);
        TIM_ITConfig(TIM6, TIM_IT_Update, DISABLE);
        break;
    }
}

static ErrorStatus si5351_wait_sys_init(void)
{
    uint32_t timeout = 100000U;

    do
    {
        uint8_t st;
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

    /* Exit power-down; MSx <- PLLA routing, 8 mA drive, integer mode as requested. */
    reg &= (uint8_t) ~(0x8FU | SI5351_CLK_INTEGER_MODE);
    reg |= si5351_clk_ctrl_value(out_conf);

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

static ErrorStatus si5351_write_common_quadrature_config(const struct si5351_pll_config *pll_conf,
                                                         const struct si5351_output_config *clk0_conf,
                                                         const struct si5351_output_config *clk1_conf)
{
    uint8_t pll_buf[8];
    uint8_t ms0_buf[8];
    uint8_t ms1_buf[8];

    si5351_pack_ms(pll_conf->ms, pll_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_A_PARAMS, pll_buf, sizeof(pll_buf)) != READY)
    {
        return NoREADY;
    }

    si5351_pack_output_ms(clk0_conf, ms0_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS0_PARAMS, ms0_buf, sizeof(ms0_buf)) != READY)
    {
        return NoREADY;
    }

    si5351_pack_output_ms(clk1_conf, ms1_buf);
    if(i2c_hw_write_register_burst(SI5351_I2C_ADDR_7BIT, SI5351_REG_MS1_PARAMS, ms1_buf, sizeof(ms1_buf)) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK0_PHASE, clk0_conf->phase_offset) != READY)
    {
        return NoREADY;
    }
    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_CLK1_PHASE, clk1_conf->phase_offset) != READY)
    {
        return NoREADY;
    }

    if(si5351_hw_apply_clk(SI5351_REG_CLK0_CTRL, clk0_conf) != READY)
    {
        return NoREADY;
    }
    if(si5351_hw_apply_clk(SI5351_REG_CLK1_CTRL, clk1_conf) != READY)
    {
        return NoREADY;
    }

    if(i2c_hw_write_register(SI5351_I2C_ADDR_7BIT, SI5351_REG_PLL_RESET, 0x20U) != READY)
    {
        return NoREADY;
    }

    return READY;
}

static ErrorStatus si5351_prepare_lagging_clk1_temp_config(const struct si5351_pll_config *pll_conf,
                                                           const struct si5351_output_config *final_conf,
                                                           struct si5351_output_config *temp_conf,
                                                           uint32_t *phase_ticks)
{
    if((pll_conf == 0) || (final_conf == 0) || (temp_conf == 0) || (phase_ticks == 0))
    {
        return NoREADY;
    }

    uint64_t actual_vco_scaled = si5351_pll_actual_vco_scaled(pll_conf);
    uint64_t final_output_clock_scaled = actual_vco_scaled / (uint64_t)final_conf->div;
    uint64_t final_output_scaled = final_output_clock_scaled / (uint64_t)final_conf->r_div_factor;
    *temp_conf = *final_conf;
    temp_conf->allow_integer_mode = 0U;
    temp_conf->div_by_4 = 0U;

    uint32_t temp_den = si5351_select_temp_b1_denominator(final_output_scaled, final_conf->div);
    uint64_t actual_temp_output_clock_scaled;
    if(si5351_calc_fixed_div_b1_output_ms(actual_vco_scaled,
                                          final_conf->div,
                                          temp_den,
                                          &temp_conf->ms,
                                          &actual_temp_output_clock_scaled) != READY)
    {
        return NoREADY;
    }

    uint64_t actual_temp_output_scaled = actual_temp_output_clock_scaled /
                                         (uint64_t)temp_conf->r_div_factor;
    if(final_output_scaled <= actual_temp_output_scaled)
    {
        return NoREADY;
    }

    uint64_t delta_scaled = final_output_scaled - actual_temp_output_scaled;
    uint32_t ticks = si5351_tim6_phase_delay_ticks(delta_scaled);
    if(ticks == 0U)
    {
        return NoREADY;
    }

    *phase_ticks = ticks;
    return READY;
}

static ErrorStatus si5351_enable_quadrature_outputs(void)
{
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

static ErrorStatus si5351_hw_clk0_phase_register_set_freq_hz(uint64_t hz)
{
    struct si5351_pll_config pll_conf;
    struct si5351_output_config clk0_conf;
    struct si5351_output_config clk1_conf;

    uint64_t freq_scaled = hz * SI5351_FREQ_MULT;
    if(si5351_calculate_clk0_config(freq_scaled, SI5351_PHASE_REG_MS_MAX, false,
                                    &pll_conf, &clk0_conf) != READY)
    {
        return NoREADY;
    }

    uint32_t phase_offset = (uint32_t)clk0_conf.div * (uint32_t)clk0_conf.r_div_factor;
    if(si5351_validate_phase_offset(phase_offset) != READY)
    {
        return NoREADY;
    }

    clk1_conf = clk0_conf;
    clk0_conf.allow_integer_mode = 0U;
    clk1_conf.allow_integer_mode = 0U;
    clk1_conf.phase_offset = (uint8_t)phase_offset;

    if(si5351_write_common_quadrature_config(&pll_conf, &clk0_conf, &clk1_conf) != READY)
    {
        return NoREADY;
    }

    return si5351_enable_quadrature_outputs();
}

static ErrorStatus si5351_hw_clk0_timed_set_freq_hz(uint64_t hz)
{
    struct si5351_pll_config pll_conf;
    struct si5351_output_config clk0_conf;
    struct si5351_output_config clk1_final_conf;
    struct si5351_output_config clk1_temp_conf;
    uint8_t clk1_final_buf[8];
    uint8_t clk1_temp_buf[8];
    uint32_t phase_ticks;

    uint64_t freq_scaled = hz * SI5351_FREQ_MULT;
    if(si5351_calculate_clk0_config(freq_scaled, SI5351_MULTISYNTH_A_MAX, true,
                                    &pll_conf, &clk0_conf) != READY)
    {
        return NoREADY;
    }

    clk1_final_conf = clk0_conf;
    clk0_conf.phase_offset = 0U;
    clk1_final_conf.phase_offset = 0U;
    clk0_conf.allow_integer_mode = 1U;
    clk1_final_conf.allow_integer_mode = 1U;

    if(si5351_prepare_lagging_clk1_temp_config(&pll_conf, &clk1_final_conf,
                                               &clk1_temp_conf, &phase_ticks) != READY)
    {
        return NoREADY;
    }

    si5351_pack_output_ms(&clk1_final_conf, clk1_final_buf);
    si5351_pack_output_ms(&clk1_temp_conf, clk1_temp_buf);

    if(si5351_write_common_quadrature_config(&pll_conf, &clk0_conf, &clk1_final_conf) != READY)
    {
        return NoREADY;
    }

    uint8_t clk1_temp_ctrl = si5351_clk_ctrl_value(&clk1_temp_conf);
    return si5351_tim6_start_pll_lock_wait(clk1_temp_buf, clk1_temp_ctrl,
                                           clk1_final_buf, si5351_clk_ctrl_value(&clk1_final_conf),
                                           phase_ticks);
}

static ErrorStatus si5351_hw_clk0_quadrature_set_freq_hz(uint64_t hz)
{
    if(hz == 0ULL)
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

    si5351_tim6_cancel();

    if(hz >= SI5351_PHASE_REG_MIN_OUTPUT_HZ)
    {
        return si5351_hw_clk0_phase_register_set_freq_hz(hz);
    }

    return si5351_hw_clk0_timed_set_freq_hz(hz);
}

ErrorStatus si5351_init()
{
    si5351_tim6_init();

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

    return READY;
}

ErrorStatus si5351_hw_clk0_set_freq_hz(uint64_t hz)
{
    // I2C will randomly fail, quick hack to make it working
    // We should actually reduce clock speed though
    for(int i = 0; i < 3; i++)
    {
        ErrorStatus ret = si5351_hw_clk0_quadrature_set_freq_hz(hz);
        if(ret == READY)
        {
            return READY;
        }
    }
    return NoREADY;
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
