/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

/*
 *@Note
 GPIO routine:
 PA0 push-pull output.

*/

#include "debug.h"
#include <stddef.h>
#include "hw/pinout.h"
#include "hw/dac_hw.h"
#include "hw/spi_manual.h"
#include "hw/st7789/st7789.h"
/* #include "test/spi_gpio_pins.h" */
/* #include "test/display_spi_test.h" */
#include "test/dac_hw_sine_test.h"
#include "hw/i2c_hw.h"
#include "hw/tlv320adc6120_hw.h"
#include "hw/si5351_hw.h"
#include "hw/i2s_hw.h"
#include "hw/usb_hw.h"
#include "hw/watchdog.h"
#include "tusb.h"

/*********************************************************************
 * @fn      GPIO_Toggle_INIT
 *
 * @brief   Initializes GPIOA.0
 *
 * @return  none
 */
void GPIO_Toggle_INIT(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC, ENABLE);
    GPIO_InitStructure.GPIO_Pin = LED_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = ENC_BTN_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(ENC_BTN_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = LED1_GPIO_PIN | LED2_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED1_GPIO_PORT, &GPIO_InitStructure);
    GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, Bit_RESET);
    GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_RESET);
}

[[maybe_unused]] static void TP_Reset_Pin_On(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* TP/LCD reset net is active-low: drive high to release ("on"). */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    gpio_init.GPIO_Pin = TP_RST_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TP_RST_GPIO_PORT, &gpio_init);
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_SET);
}

[[maybe_unused]] static void TP_Reset_Pin_Off(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* TP/LCD reset net is active-low: drive high to release ("on"). */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);
    gpio_init.GPIO_Pin = TP_RST_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TP_RST_GPIO_PORT, &gpio_init);
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_RESET);
}


typedef enum
{
    LED_MODE_SLOW_BLINK = 0,
    LED_MODE_FAST_BLINK,
    LED_MODE_HEARTBEAT,
    LED_MODE_BREATHING,
    LED_MODE_ALTERNATING,
    LED_MODE_COUNT
} led_mode_t;

typedef struct
{
    led_mode_t selected_mode;
    uint8_t usb_data_seen;
    uint8_t freq_changed_seen;
    uint32_t last_vendor_total_words;
    uint64_t initial_frequency_hz;
    uint64_t activity_window_until_tick;
    uint64_t activity_led1_until_tick;
    uint8_t button_raw_state;
    uint8_t button_stable_state;
    uint64_t button_last_change_tick;
    uint8_t led_duty_percent[3];
    uint8_t sw_pwm_phase;
    uint64_t sw_pwm_last_tick;
} led_control_state_t;

static led_control_state_t g_led_ctrl = {0};
static uint16_t g_led_pwm_tim2_arr = 999U;

#define LED_ID_PA3  0U
#define LED_ID_PC0  1U
#define LED_ID_PC1  2U

#define LED_SW_PWM_STEPS 20U

static uint64_t ticks_from_ms(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)SystemCoreClock * (uint64_t)ms) / 1000ULL;
    if(ticks == 0U)
    {
        ticks = 1U;
    }
    return ticks;
}

static uint32_t ticks_to_ms(uint64_t ticks)
{
    uint64_t div = (uint64_t)SystemCoreClock / 1000ULL;
    if(div == 0U)
    {
        div = 1U;
    }
    return (uint32_t)(ticks / div);
}

static uint64_t ticks_from_us(uint32_t us)
{
    uint64_t ticks = ((uint64_t)SystemCoreClock * (uint64_t)us) / 1000000ULL;
    if(ticks == 0U)
    {
        ticks = 1U;
    }
    return ticks;
}

/* Reserve TIM2 for LED PWM output on PA3.
 * TIM6/TIM7/TIM8 are intentionally not used here because DAC/I2S depend on them.
 */
static void LED_PWM_TIM2_Init(void)
{
    GPIO_InitTypeDef gpio_init = {0};
    TIM_TimeBaseInitTypeDef tim_base = {0};
    TIM_OCInitTypeDef tim_oc = {0};
    uint32_t tim_clk_hz = (uint32_t)SystemCoreClock;
    uint16_t prescaler = 0U;
    uint32_t target_pwm_hz = 2000U;
    uint32_t period;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);

    gpio_init.GPIO_Pin = LED_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &gpio_init);

    period = tim_clk_hz / target_pwm_hz;
    if(period > 65535U)
    {
        prescaler = (uint16_t)((period / 65535U) + 1U);
        period = tim_clk_hz / ((uint32_t)(prescaler + 1U) * target_pwm_hz);
    }
    if(period == 0U)
    {
        period = 1U;
    }
    if(period > 65535U)
    {
        period = 65535U;
    }
    g_led_pwm_tim2_arr = (uint16_t)(period - 1U);

    tim_base.TIM_Period = g_led_pwm_tim2_arr;
    tim_base.TIM_Prescaler = prescaler;
    tim_base.TIM_ClockDivision = TIM_CKD_DIV1;
    tim_base.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &tim_base);

    tim_oc.TIM_OCMode = TIM_OCMode_PWM1;
    tim_oc.TIM_OutputState = TIM_OutputState_Enable;
    tim_oc.TIM_Pulse = 0U;
    tim_oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC4Init(TIM2, &tim_oc);
    TIM_OC4PreloadConfig(TIM2, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM2, ENABLE);
    TIM_Cmd(TIM2, ENABLE);
}

static void LED_Set_Duty(uint8_t led_id, uint8_t duty_percent)
{
    if(led_id >= 3U)
    {
        return;
    }

    if(duty_percent > 100U)
    {
        duty_percent = 100U;
    }

    g_led_ctrl.led_duty_percent[led_id] = duty_percent;
}

static void LED_Apply_Duty_Outputs(uint64_t now_tick)
{
    uint64_t sw_step_ticks = ticks_from_us(250U); /* 20 steps * 250us = 5ms (200 Hz). */
    uint8_t duty_pc0_steps;
    uint8_t duty_pc1_steps;
    uint8_t phase;
    uint16_t pulse;

    if((now_tick - g_led_ctrl.sw_pwm_last_tick) >= sw_step_ticks)
    {
        uint64_t elapsed_steps = (now_tick - g_led_ctrl.sw_pwm_last_tick) / sw_step_ticks;
        g_led_ctrl.sw_pwm_last_tick += elapsed_steps * sw_step_ticks;
        g_led_ctrl.sw_pwm_phase = (uint8_t)((g_led_ctrl.sw_pwm_phase + (uint8_t)elapsed_steps) % LED_SW_PWM_STEPS);
    }

    pulse = (uint16_t)(((uint32_t)g_led_ctrl.led_duty_percent[LED_ID_PA3] * (uint32_t)(g_led_pwm_tim2_arr + 1U)) / 100U);
    if(pulse > g_led_pwm_tim2_arr)
    {
        pulse = g_led_pwm_tim2_arr;
    }
    TIM_SetCompare4(TIM2, pulse);

    duty_pc0_steps = (uint8_t)(((uint16_t)g_led_ctrl.led_duty_percent[LED_ID_PC0] * LED_SW_PWM_STEPS + 99U) / 100U);
    duty_pc1_steps = (uint8_t)(((uint16_t)g_led_ctrl.led_duty_percent[LED_ID_PC1] * LED_SW_PWM_STEPS + 99U) / 100U);
    phase = g_led_ctrl.sw_pwm_phase;

    GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, (phase < duty_pc0_steps) ? Bit_SET : Bit_RESET);
    GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, (phase < duty_pc1_steps) ? Bit_SET : Bit_RESET);
}

static uint8_t LED_Is_Link_Activity_Enabled(uint64_t now_tick)
{
    if(g_led_ctrl.freq_changed_seen == 0U)
    {
        return 0U;
    }

    if(g_led_ctrl.usb_data_seen == 0U)
    {
        return 0U;
    }

    return (uint8_t)(now_tick < g_led_ctrl.activity_window_until_tick);
}

static void LED_Mode_Init(void)
{
    g_led_ctrl.selected_mode = LED_MODE_SLOW_BLINK;
    g_led_ctrl.usb_data_seen = 0U;
    g_led_ctrl.freq_changed_seen = 0U;
    g_led_ctrl.last_vendor_total_words = usb_hw_vendor_total_words();
    g_led_ctrl.initial_frequency_hz = si5351_hw_clk0_get_freq_hz();
    g_led_ctrl.activity_window_until_tick = 0U;
    g_led_ctrl.activity_led1_until_tick = 0U;
    g_led_ctrl.button_raw_state = (uint8_t)GPIO_ReadInputDataBit(ENC_BTN_GPIO_PORT, ENC_BTN_GPIO_PIN);
    g_led_ctrl.button_stable_state = g_led_ctrl.button_raw_state;
    g_led_ctrl.button_last_change_tick = SysTick->CNT;
    g_led_ctrl.sw_pwm_phase = 0U;
    g_led_ctrl.sw_pwm_last_tick = SysTick->CNT;
    LED_Set_Duty(LED_ID_PA3, 0U);
    LED_Set_Duty(LED_ID_PC0, 0U);
    LED_Set_Duty(LED_ID_PC1, 0U);
    LED_PWM_TIM2_Init();
    LED_Apply_Duty_Outputs(SysTick->CNT);
}

static void LED_Poll_Button(uint64_t now_tick)
{
    uint8_t raw_state = (uint8_t)GPIO_ReadInputDataBit(ENC_BTN_GPIO_PORT, ENC_BTN_GPIO_PIN);
    uint64_t debounce_ticks = ticks_from_ms(30U);

    if(raw_state != g_led_ctrl.button_raw_state)
    {
        g_led_ctrl.button_raw_state = raw_state;
        g_led_ctrl.button_last_change_tick = now_tick;
    }

    if((now_tick - g_led_ctrl.button_last_change_tick) < debounce_ticks)
    {
        return;
    }

    if(g_led_ctrl.button_stable_state == g_led_ctrl.button_raw_state)
    {
        return;
    }

    g_led_ctrl.button_stable_state = g_led_ctrl.button_raw_state;
    if(g_led_ctrl.button_stable_state != 0U)
    {
        g_led_ctrl.selected_mode = (led_mode_t)(((uint32_t)g_led_ctrl.selected_mode + 1U) % (uint32_t)LED_MODE_COUNT);
        printf("LED mode -> %u\r\n", (unsigned int)g_led_ctrl.selected_mode);
    }
}

static void LED_Update_Activity_Gates(uint64_t now_tick)
{
    uint32_t vendor_total_words_now = usb_hw_vendor_total_words();
    uint32_t vendor_delta_words = vendor_total_words_now - g_led_ctrl.last_vendor_total_words;
    uint64_t freq_now_hz = si5351_hw_clk0_get_freq_hz();

    if(vendor_delta_words > 0U)
    {
        g_led_ctrl.usb_data_seen = 1U;
        g_led_ctrl.activity_window_until_tick = now_tick + ticks_from_ms(500U);
        g_led_ctrl.activity_led1_until_tick = now_tick + ticks_from_ms(60U + (vendor_delta_words > 240U ? 240U : vendor_delta_words));
    }
    g_led_ctrl.last_vendor_total_words = vendor_total_words_now;

    if(freq_now_hz != g_led_ctrl.initial_frequency_hz)
    {
        g_led_ctrl.freq_changed_seen = 1U;
    }
}

static void LED_Render_Selected_Mode(led_mode_t mode, uint64_t now_tick)
{
    uint32_t t_ms = ticks_to_ms(now_tick);
    uint8_t duty_pa3 = 0U;
    uint8_t duty_pc0 = 0U;
    uint8_t duty_pc1 = 0U;

    switch(mode)
    {
        case LED_MODE_SLOW_BLINK:
        {
            uint32_t phase = t_ms % 1000U;
            duty_pa3 = (phase < 500U) ? 100U : 0U;
            duty_pc0 = duty_pa3;
            duty_pc1 = (phase < 80U) ? 100U : 0U;
            break;
        }

        case LED_MODE_FAST_BLINK:
        {
            uint32_t period = 200U;
            uint32_t phase1 = t_ms % period;
            uint32_t phase2 = (t_ms + 50U) % period; /* 90-degree phase offset. */
            duty_pa3 = (phase1 < 100U) ? 100U : 0U;
            duty_pc0 = duty_pa3;
            duty_pc1 = (phase2 < 100U) ? 100U : 0U;
            break;
        }

        case LED_MODE_HEARTBEAT:
        {
            uint32_t phase = t_ms % 1000U;
            duty_pa3 = ((phase < 100U) || ((phase >= 200U) && (phase < 300U))) ? 100U : 0U;
            duty_pc0 = duty_pa3;
            duty_pc1 = (duty_pa3 > 0U) ? 0U : 100U;
            break;
        }

        case LED_MODE_BREATHING:
        {
            uint32_t breath_phase = t_ms % 2000U;
            uint32_t ramp = (breath_phase < 1000U) ? breath_phase : (2000U - breath_phase);
            uint32_t level = (ramp * 100U) / 1000U;
            uint32_t inv_level = 100U - level;
            duty_pa3 = (uint8_t)level;
            duty_pc0 = duty_pa3;
            duty_pc1 = (uint8_t)(inv_level / 2U);
            break;
        }

        case LED_MODE_ALTERNATING:
        default:
        {
            uint32_t phase = t_ms % 500U;
            duty_pa3 = (phase < 250U) ? 100U : 0U;
            duty_pc0 = duty_pa3;
            duty_pc1 = (duty_pa3 > 0U) ? 0U : 100U;
            break;
        }
    }

    LED_Set_Duty(LED_ID_PA3, duty_pa3);
    LED_Set_Duty(LED_ID_PC0, duty_pc0);
    LED_Set_Duty(LED_ID_PC1, duty_pc1);
}

static void LED_Render_Link_Activity(uint64_t now_tick)
{
    uint32_t t_ms = ticks_to_ms(now_tick);
    uint8_t duty_pa3 = (now_tick < g_led_ctrl.activity_led1_until_tick) ? 100U : 0U;
    uint8_t duty_pc0 = duty_pa3;
    uint8_t duty_pc1 = ((t_ms % 1000U) < 180U) ? 35U : 0U;
    LED_Set_Duty(LED_ID_PA3, duty_pa3);
    LED_Set_Duty(LED_ID_PC0, duty_pc0);
    LED_Set_Duty(LED_ID_PC1, duty_pc1);
}

static void LED_Mode_Task(void)
{
    uint64_t now_tick = SysTick->CNT;

    LED_Poll_Button(now_tick);
    LED_Update_Activity_Gates(now_tick);

    if(LED_Is_Link_Activity_Enabled(now_tick) != 0U)
    {
        LED_Render_Link_Activity(now_tick);
    }
    else
    {
        LED_Render_Selected_Mode(g_led_ctrl.selected_mode, now_tick);
    }

    LED_Apply_Duty_Outputs(now_tick);
}

[[maybe_unused]] static void SysTick_Report_USB_EverySecond(void)
{
    static uint64_t last_report_tick = 0;
    static uint8_t initialized = 0;
    uint64_t now_tick = SysTick->CNT;
    uint64_t report_period_ticks = (uint64_t)SystemCoreClock / 2;

    if(report_period_ticks == 0U)
    {
        report_period_ticks = 1U;
    }

    if(initialized == 0U)
    {
        last_report_tick = now_tick;
        initialized = 1U;
        return;
    }

    if((now_tick - last_report_tick) < report_period_ticks)
    {
        return;
    }

    printf("%d systick_now=0x%lx last=0x%lx %d\r\n",
           25,
           (uint32_t)now_tick,
           (uint32_t)last_report_tick,
           25);

    last_report_tick = now_tick;
}

[[maybe_unused]] static void Scan_I2CBus_EverySecond(void)
{
    static uint64_t last_scan_tick = 0;
    static uint8_t initialized = 0;
    uint64_t now_tick = SysTick->CNT;
    uint64_t scan_period_ticks = (uint64_t)SystemCoreClock;
    uint8_t addr = 0;
    uint8_t device_count = 0;

    if(scan_period_ticks == 0U)
    {
        scan_period_ticks = 1U;
    }

    if(initialized == 0U)
    {
        last_scan_tick = now_tick;
        initialized = 1U;
        return;
    }

    if((now_tick - last_scan_tick) < scan_period_ticks)
    {
        return;
    }

    printf("I2C scan:");

    for(addr = 0x08U; addr <= 0x77U; ++addr)
    {
        if(i2c_hw_scan_bus_at(addr) == READY)
        {
            ++device_count;
            printf(" 0x%02X", addr);
        }
    }

    if(device_count == 0U)
    {
        printf(" no devices\r\n");
    }
    else
    {
        printf(" (%u)\r\n", device_count);
    }

    last_scan_tick = now_tick;
}

/*********************************************************************
 * TLV320ADC6120 I2S capture (CH1/CH2)
 *
 * In the current 24-bit I2S mode, the CH32 peripheral uses 32-bit channel
 * frames, so each stereo frame arrives as four 16-bit DMA words.
 * SPI2 RX DMA runs in circular mode; DMA HT/TC interrupts count those words.
 * Report the incoming data rate once per second.
 *********************************************************************/
static void TLV320_I2S_Poll(void)
{
    static uint64_t last_report_tick = 0U;
    static uint32_t last_word_count = 0U;
    static uint32_t last_vendor_total_word_count = 0U;
    static uint32_t last_vendor_dropped_word_count = 0U;
    static uint8_t initialized = 0U;
    uint64_t now_tick;
    uint64_t elapsed_ticks;
    uint32_t words_now;
    uint32_t words_per_sec;
    uint32_t frames_per_sec;
    uint32_t bytes_per_sec;
    uint32_t vendor_words_now;
    uint32_t vendor_dropped_words_now;
    uint32_t vendor_words_per_sec;
    uint32_t vendor_dropped_words_per_sec;

    now_tick = SysTick->CNT;
    words_now = i2s_hw_rx_word_count();
    vendor_words_now = usb_hw_vendor_total_words();
    vendor_dropped_words_now = usb_hw_vendor_dropped_words();

    if(initialized == 0U)
    {
        last_report_tick = now_tick;
        last_word_count = words_now;
        last_vendor_total_word_count = vendor_words_now;
        last_vendor_dropped_word_count = vendor_dropped_words_now;
        initialized = 1U;
    }
    else
    {
        elapsed_ticks = now_tick - last_report_tick;
        if(elapsed_ticks >= (uint64_t)SystemCoreClock)
        {
            words_per_sec = (uint32_t)((((uint64_t)(words_now - last_word_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);
            frames_per_sec = words_per_sec / 4U;
            bytes_per_sec = words_per_sec * (uint32_t)sizeof(uint16_t);
            vendor_words_per_sec = (uint32_t)((((uint64_t)(vendor_words_now - last_vendor_total_word_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);
            vendor_dropped_words_per_sec = (uint32_t)((((uint64_t)(vendor_dropped_words_now - last_vendor_dropped_word_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);

            printf("ADC I2S rate: %lu words/s, %lu frames/s, %lu B/s | vendor %lu words/s drop %lu words/s | LO %lu Hz\r\n",
                   (unsigned long)words_per_sec,
                   (unsigned long)frames_per_sec,
                   (unsigned long)bytes_per_sec,
                   (unsigned long)vendor_words_per_sec,
                   (unsigned long)vendor_dropped_words_per_sec,
                   (unsigned long)si5351_hw_clk0_get_freq_hz());

            last_word_count = words_now;
            last_vendor_total_word_count = vendor_words_now;
            last_vendor_dropped_word_count = vendor_dropped_words_now;
            last_report_tick = now_tick;

            if(i2s_needs_reset())
            {
                printf("bitslipped, resetting\n");
                /*i2s_hw_deinit();
                i2s_hw_init();
                i2s_hw_enable(ENABLE);
                initialized = 0U;*/
            }
        }
    }
}

static void DAC_Poll(void)
{
    static uint64_t last_report_tick = 0U;
    static uint32_t last_frame_count = 0U;
    static uint8_t initialized = 0U;
    uint64_t now_tick;
    uint64_t elapsed_ticks;
    uint32_t frames_now;
    uint32_t frames_per_sec;
    uint32_t words_per_sec;
    uint32_t bytes_per_sec;

    now_tick = SysTick->CNT;
    frames_now = dac_hw_tx_frame_count();

    if(initialized == 0U)
    {
        last_report_tick = now_tick;
        last_frame_count = frames_now;
        initialized = 1U;
        return;
    }

    elapsed_ticks = now_tick - last_report_tick;
    if(elapsed_ticks < (uint64_t)SystemCoreClock)
    {
        return;
    }

    frames_per_sec = (uint32_t)((((uint64_t)(frames_now - last_frame_count)) * (uint64_t)SystemCoreClock) / elapsed_ticks);
    words_per_sec = frames_per_sec * 2U;
    bytes_per_sec = frames_per_sec * (uint32_t)sizeof(uint32_t);

    printf("DAC rate: %lu words/s, %lu frames/s, %lu B/s\r\n",
           (unsigned long)words_per_sec,
           (unsigned long)frames_per_sec,
           (unsigned long)bytes_per_sec);

    last_frame_count = frames_now;
    last_report_tick = now_tick;
}

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();

    usb_hw_init();
    
    //SysTick_Config(SystemCoreClock / 1000);	
    printf("SystemClk:%ld\r\n", SystemCoreClock);
    printf( "ChipID:%08lx\r\n", DBGMCU_GetCHIPID() );

    printf("GPIO Toggle TEST\r\n");
    GPIO_Toggle_INIT();
    // TP_Reset_Pin_Off();


    /*printf("SPI lines: GPIO mode, all outputs high (SCK MOSI CS RS RST)\r\n");
    spi_gpio_pins_enable();
    spi_gpio_pins_all_on();*/
    
    // printf("SPI: sample 0xFF (one CS-framed byte, command/RS low)\r\n");
    // spi_manual_init();
    // spi_manual_cs_begin();
    // spi_manual_rs_cmd();
    // (void)spi_manual_transfer_u8(0xFFU);
    // spi_manual_cs_end();
    printf("ST7789 init + built-in screen test\r\n");
    ST7789_Init();
    //ST7789_Test();

    i2c_hw_init();

    if(tlv320adc6120_hw_init() == READY)
    {
        printf("TLV320ADC6120: I2C 0x4E, I2S controller 24-bit CH1+CH2, expects 24 MHz MCLK\r\n");
        GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_SET);
    }
    else
    {
        printf("TLV320ADC6120: I2C init failed (check wiring / AVDD AREG define / 24 MHz MCLK)\r\n");
    }

    if(usb_hw_set_clk_freq_hz(7067333ULL) == READY)
    {
        printf("Si5351: LO CLK0/CLK1 = 12000000 Hz, CLK1 = +90 deg\r\n");
    }
    else
    {
        printf("Si5351: LO program failed (I2C 0x60)\r\n");
    }

    i2s_hw_init();
    i2s_hw_enable(ENABLE);

    dac_hw_init();
    dac_hw_static_noise_start(192000U);
    printf("DAC: static noise PA4+PA5 @ 192 ksps (TIM7 TRGO + DMA2 Ch3 refill IRQ)\r\n");

    LED_Mode_Init();

    /* display_spi_test_run(); */
    //watchdog_init();

    while(1)
    {
        TLV320_I2S_Poll();
        DAC_Poll();
        tud_task();
        //Scan_I2CBus_EverySecond();
        //SysTick_Report_USB_EverySecond();
        LED_Mode_Task();
        //watchdog_kick();
    }
}
