#include "feature/blinky/blinky.h"

#include "debug.h"
#include "hw/pinout.h"
#include "hw/si5351.h"
#include "hw/usb.h"

typedef enum
{
    LED_MODE_MORSE_HELLO,
    LED_MODE_SLOW_BLINK,
    LED_MODE_ALTERNATING,
    LED_MODE_FAST_BLINK,
    LED_MODE_HEARTBEAT,
    LED_MODE_BREATHING,
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
    uint8_t led_duty_percent[2];
    uint8_t sw_pwm_phase;
    uint64_t sw_pwm_last_tick;
} led_control_state_t;

static led_control_state_t g_led_ctrl = {0};

#define LED_ID_PC0  0U
#define LED_ID_PC1  1U
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

static void led_set_duty(uint8_t led_id, uint8_t duty_percent)
{
    if(led_id >= 2U)
    {
        return;
    }

    if(duty_percent > 100U)
    {
        duty_percent = 100U;
    }

    g_led_ctrl.led_duty_percent[led_id] = duty_percent;
}

static void led_apply_duty_outputs(uint64_t now_tick)
{
    uint64_t sw_step_ticks = ticks_from_us(250U); /* 20 steps * 250us = 5ms (200 Hz). */
    uint8_t duty_pc0_steps;
    uint8_t duty_pc1_steps;
    uint8_t phase;

    if((now_tick - g_led_ctrl.sw_pwm_last_tick) >= sw_step_ticks)
    {
        uint64_t elapsed_steps = (now_tick - g_led_ctrl.sw_pwm_last_tick) / sw_step_ticks;
        g_led_ctrl.sw_pwm_last_tick += elapsed_steps * sw_step_ticks;
        g_led_ctrl.sw_pwm_phase = (uint8_t)((g_led_ctrl.sw_pwm_phase + (uint8_t)elapsed_steps) % LED_SW_PWM_STEPS);
    }

    duty_pc0_steps = (uint8_t)(((uint16_t)g_led_ctrl.led_duty_percent[LED_ID_PC0] * LED_SW_PWM_STEPS + 99U) / 100U);
    duty_pc1_steps = (uint8_t)(((uint16_t)g_led_ctrl.led_duty_percent[LED_ID_PC1] * LED_SW_PWM_STEPS + 99U) / 100U);
    phase = g_led_ctrl.sw_pwm_phase;

    GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, (phase < duty_pc0_steps) ? Bit_SET : Bit_RESET);
    GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, (phase < duty_pc1_steps) ? Bit_SET : Bit_RESET);
}

static uint8_t led_is_link_activity_enabled(uint64_t now_tick)
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

static void led_poll_button(uint64_t now_tick)
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

static void led_update_activity_gates(uint64_t now_tick)
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

static void led_render_selected_mode(led_mode_t mode, uint64_t now_tick)
{
    uint32_t t_ms = ticks_to_ms(now_tick);
    uint8_t duty_pc0 = 0U;
    uint8_t duty_pc1 = 0U;

    switch(mode)
    {
        case LED_MODE_SLOW_BLINK:
        {
            uint32_t phase = t_ms % 1000U;
            duty_pc0 = (phase < 500U) ? 100U : 0U;
            duty_pc1 = (phase < 80U) ? 100U : 0U;
            break;
        }

        case LED_MODE_FAST_BLINK:
        {
            uint32_t period = 200U;
            uint32_t phase1 = t_ms % period;
            uint32_t phase2 = (t_ms + 50U) % period; /* 90-degree phase offset. */
            duty_pc0 = (phase1 < 100U) ? 100U : 0U;
            duty_pc1 = (phase2 < 100U) ? 100U : 0U;
            break;
        }

        case LED_MODE_HEARTBEAT:
        {
            uint32_t phase = t_ms % 1000U;
            duty_pc0 = ((phase < 100U) || ((phase >= 200U) && (phase < 300U))) ? 100U : 0U;
            duty_pc1 = (duty_pc0 > 0U) ? 0U : 100U;
            break;
        }

        case LED_MODE_BREATHING:
        {
            uint32_t breath_phase = t_ms % 2000U;
            uint32_t ramp = (breath_phase < 1000U) ? breath_phase : (2000U - breath_phase);
            uint32_t level = (ramp * 100U) / 1000U;
            uint32_t inv_level = 100U - level;
            duty_pc0 = (uint8_t)level;
            duty_pc1 = (uint8_t)(inv_level / 2U);
            break;
        }

        case LED_MODE_ALTERNATING:
        default:
        {
            uint32_t phase = t_ms % 500U;
            duty_pc0 = (phase < 250U) ? 100U : 0U;
            duty_pc1 = (duty_pc0 > 0U) ? 0U : 100U;
            break;
        }

        case LED_MODE_MORSE_HELLO:
        {
            typedef struct
            {
                uint8_t units;
                uint8_t on;
            } morse_segment_t;

            /* HELLO = ".... . .-.. .-.. ---" */
            static const morse_segment_t hello_segments[] = {
                {1U, 1U}, {1U, 0U}, {1U, 1U}, {1U, 0U}, {1U, 1U}, {1U, 0U}, {1U, 1U}, {3U, 0U}, /* H */
                {1U, 1U}, {3U, 0U},                                                                         /* E */
                {1U, 1U}, {1U, 0U}, {3U, 1U}, {1U, 0U}, {1U, 1U}, {1U, 0U}, {1U, 1U}, {3U, 0U},           /* L */
                {1U, 1U}, {1U, 0U}, {3U, 1U}, {1U, 0U}, {1U, 1U}, {1U, 0U}, {1U, 1U}, {3U, 0U},           /* L */
                {3U, 1U}, {1U, 0U}, {3U, 1U}, {1U, 0U}, {3U, 1U}, {7U, 0U}                                  /* O + word gap */
            };
            uint32_t const unit_ms = 120U;
            uint32_t phase_ms;
            uint32_t cursor_ms = 0U;
            uint32_t i;

            for(i = 0U; i < (uint32_t)(sizeof(hello_segments) / sizeof(hello_segments[0])); ++i)
            {
                cursor_ms += (uint32_t)hello_segments[i].units * unit_ms;
            }
            if(cursor_ms == 0U)
            {
                cursor_ms = unit_ms;
            }

            phase_ms = t_ms % cursor_ms;
            cursor_ms = 0U;
            for(i = 0U; i < (uint32_t)(sizeof(hello_segments) / sizeof(hello_segments[0])); ++i)
            {
                uint32_t seg_ms = (uint32_t)hello_segments[i].units * unit_ms;
                if(phase_ms < (cursor_ms + seg_ms))
                {
                    if(hello_segments[i].on != 0U)
                    {
                        duty_pc0 = 100U;
                        /* LED2 only emphasizes dashes to keep the pattern distinct. */
                        duty_pc1 = (hello_segments[i].units >= 3U) ? 100U : 20U;
                    }
                    else
                    {
                        duty_pc0 = 0U;
                        duty_pc1 = 0U;
                    }
                    break;
                }
                cursor_ms += seg_ms;
            }
            break;
        }
    }

    led_set_duty(LED_ID_PC0, duty_pc0);
    led_set_duty(LED_ID_PC1, duty_pc1);
}

static void led_render_link_activity(uint64_t now_tick)
{
    uint32_t t_ms = ticks_to_ms(now_tick);
    uint8_t duty_pc0 = (now_tick < g_led_ctrl.activity_led1_until_tick) ? 100U : 0U;
    uint8_t duty_pc1 = ((t_ms % 1000U) < 180U) ? 35U : 0U;
    led_set_duty(LED_ID_PC0, duty_pc0);
    led_set_duty(LED_ID_PC1, duty_pc1);
}

void blinky_init(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, ENABLE);

    gpio_init.GPIO_Pin = ENC_BTN_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(ENC_BTN_GPIO_PORT, &gpio_init);

    gpio_init.GPIO_Pin = LED1_GPIO_PIN | LED2_GPIO_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED1_GPIO_PORT, &gpio_init);
    GPIO_WriteBit(LED1_GPIO_PORT, LED1_GPIO_PIN, Bit_RESET);
    GPIO_WriteBit(LED2_GPIO_PORT, LED2_GPIO_PIN, Bit_RESET);

    g_led_ctrl.selected_mode = LED_MODE_MORSE_HELLO;
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
    led_set_duty(LED_ID_PC0, 0U);
    led_set_duty(LED_ID_PC1, 0U);
    led_apply_duty_outputs(SysTick->CNT);
}

void blinky_task(void)
{
    uint64_t now_tick = SysTick->CNT;

    led_poll_button(now_tick);
    led_update_activity_gates(now_tick);

    if(led_is_link_activity_enabled(now_tick) != 0U)
    {
        led_render_link_activity(now_tick);
    }
    else
    {
        led_render_selected_mode(g_led_ctrl.selected_mode, now_tick);
    }

    led_apply_duty_outputs(now_tick);
}
