#include "encoder.h"

#include "freertos/port_isr.h"
#include "hw/pinout.h"
#include "ui/ui.h"

#include <stdatomic.h>

#include "ch32v30x_exti.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_misc.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_tim.h"

#define ENCODER_TIMER             TIM10
#define ENCODER_TIMER_PERIPH      RCC_APB2Periph_TIM10
#define ENCODER_GPIO_PERIPH       RCC_APB2Periph_GPIOB
#define ENCODER_INPUT_FILTER      0x0FU
#define ENCODER_COUNTER_MIDPOINT  0x8000U
#define ENCODER_COUNTS_PER_DETENT 4
#define ENCODER_BUTTON_DEBOUNCE_MS 30U
#define ENCODER_BUTTON_LONG_PRESS_MS 1000U
#define ENCODER_BUTTON_EXTI_LINE  EXTI_Line15
#define ENCODER_BUTTON_EXTI_IRQn  EXTI15_10_IRQn

/* Encoder switch: IPU, low when pressed (tied to GND). */
#define ENC_BTN_PRESSED(st) ((st) == 0U)

typedef enum
{
    ENCODER_BUTTON_STATE_IDLE = 0,
    ENCODER_BUTTON_STATE_PRESSED,
} encoder_button_state_t;

static volatile uint16_t s_encoder_last_counter = ENCODER_COUNTER_MIDPOINT;
static atomic_int_fast32_t s_encoder_pending_raw_delta = 0;
static volatile int32_t s_encoder_raw_position = 0;
static int16_t s_encoder_detent_remainder = 0;
static volatile encoder_button_state_t s_button_state = ENCODER_BUTTON_STATE_IDLE;
static volatile uint64_t s_button_press_start_tick = 0U;
static volatile uint64_t s_button_debounce_ticks;
static volatile uint64_t s_button_long_press_ticks;

static uint64_t encoder_ticks_from_ms(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)SystemCoreClock * (uint64_t)ms) / 1000ULL;

    return (ticks == 0U) ? 1U : ticks;
}

static int16_t encoder_sync_delta(void)
{
    uint16_t now = TIM_GetCounter(ENCODER_TIMER);
    int16_t delta = (int16_t)(now - s_encoder_last_counter);

    if(delta != 0)
    {
        s_encoder_last_counter = now;
        s_encoder_raw_position += delta;
    }

    return delta;
}

PORT_ISR_BODY(EXTI15_10_IRQHandler)
{
    if(EXTI_GetITStatus(ENCODER_BUTTON_EXTI_LINE) == RESET)
    {
        return;
    }

    EXTI_ClearITPendingBit(ENCODER_BUTTON_EXTI_LINE);

    uint8_t raw_state = (uint8_t)GPIO_ReadInputDataBit(ENC_BTN_GPIO_PORT, ENC_BTN_GPIO_PIN);
    uint64_t now_tick = SysTick->CNT;

    if(ENC_BTN_PRESSED(raw_state))
    {
        if(s_button_state == ENCODER_BUTTON_STATE_IDLE)
        {
            s_button_press_start_tick = now_tick;
            s_button_state = ENCODER_BUTTON_STATE_PRESSED;
        }
        return;
    }

    if(s_button_state != ENCODER_BUTTON_STATE_PRESSED)
    {
        return;
    }

    uint64_t press_ticks = now_tick - s_button_press_start_tick;
    s_button_state = ENCODER_BUTTON_STATE_IDLE;

    if(press_ticks < s_button_debounce_ticks)
    {
        return;
    }

    if(press_ticks < s_button_long_press_ticks)
    {
        ui_handle_button_press();
    }
    else
    {
        ui_handle_button_long_press();
    }
}

PORT_ISR_BODY(TIM10_CC_IRQHandler)
{
    uint8_t have_cc1 = (uint8_t)(TIM_GetITStatus(ENCODER_TIMER, TIM_IT_CC1) != RESET);
    uint8_t have_cc2 = (uint8_t)(TIM_GetITStatus(ENCODER_TIMER, TIM_IT_CC2) != RESET);

    if((have_cc1 == 0U) && (have_cc2 == 0U))
    {
        return;
    }

    if(have_cc1 != 0U)
    {
        TIM_ClearITPendingBit(ENCODER_TIMER, TIM_IT_CC1);
    }

    if(have_cc2 != 0U)
    {
        TIM_ClearITPendingBit(ENCODER_TIMER, TIM_IT_CC2);
    }

    int_fast32_t delta = encoder_sync_delta();
    if(delta != 0)
    {
        atomic_fetch_add_explicit(&s_encoder_pending_raw_delta, delta, memory_order_relaxed);
    }
}

void encoder_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    RCC_APB2PeriphClockCmd(ENCODER_GPIO_PERIPH |
                          ENCODER_TIMER_PERIPH |
                          RCC_APB2Periph_GPIOC |
                          RCC_APB2Periph_AFIO,
                          ENABLE);

    gpio.GPIO_Pin = ENC_A_GPIO_PIN | ENC_B_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(ENC_A_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = ENC_BTN_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(ENC_BTN_GPIO_PORT, &gpio);

    TIM_DeInit(ENCODER_TIMER);

    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Prescaler = 0U;
    tim.TIM_Period = 0xFFFFU;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(ENCODER_TIMER, &tim);

    TIM_ICInitTypeDef ic = {0};
    TIM_ICStructInit(&ic);
    ic.TIM_ICPolarity = TIM_ICPolarity_Falling;
    ic.TIM_ICSelection = TIM_ICSelection_DirectTI;
    ic.TIM_ICPrescaler = TIM_ICPSC_DIV1;
    ic.TIM_ICFilter = ENCODER_INPUT_FILTER;

    ic.TIM_Channel = TIM_Channel_1;
    TIM_ICInit(ENCODER_TIMER, &ic);

    ic.TIM_Channel = TIM_Channel_2;
    TIM_ICInit(ENCODER_TIMER, &ic);

    TIM_EncoderInterfaceConfig(ENCODER_TIMER,
                               TIM_EncoderMode_TI12,
                               TIM_ICPolarity_Falling,
                               TIM_ICPolarity_Rising);

    TIM_SetAutoreload(ENCODER_TIMER, 0xFFFFU);
    TIM_SetCounter(ENCODER_TIMER, ENCODER_COUNTER_MIDPOINT);
    TIM_ITConfig(ENCODER_TIMER, TIM_IT_CC1 | TIM_IT_CC2, ENABLE);
    TIM_ClearFlag(ENCODER_TIMER, TIM_FLAG_Update);

    NVIC_InitTypeDef encoder_nvic = {0};
    encoder_nvic.NVIC_IRQChannel = TIM10_CC_IRQn;
    encoder_nvic.NVIC_IRQChannelPreemptionPriority = 2;
    encoder_nvic.NVIC_IRQChannelSubPriority = 0;
    encoder_nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&encoder_nvic);

    TIM_Cmd(ENCODER_TIMER, ENABLE);

    s_encoder_last_counter = ENCODER_COUNTER_MIDPOINT;
    atomic_store_explicit(&s_encoder_pending_raw_delta, 0, memory_order_relaxed);
    s_encoder_raw_position = 0;
    s_encoder_detent_remainder = 0;
    s_button_state = ENCODER_BUTTON_STATE_IDLE;
    s_button_press_start_tick = 0U;
    s_button_debounce_ticks = encoder_ticks_from_ms(ENCODER_BUTTON_DEBOUNCE_MS);
    s_button_long_press_ticks = encoder_ticks_from_ms(ENCODER_BUTTON_LONG_PRESS_MS);

    GPIO_EXTILineConfig(GPIO_PortSourceGPIOC, GPIO_PinSource15);

    EXTI_InitTypeDef button_exti = {
        .EXTI_Line = ENCODER_BUTTON_EXTI_LINE,
        .EXTI_Mode = EXTI_Mode_Interrupt,
        .EXTI_Trigger = EXTI_Trigger_Rising_Falling,
        .EXTI_LineCmd = ENABLE,
    };
    EXTI_Init(&button_exti);
    EXTI_ClearITPendingBit(ENCODER_BUTTON_EXTI_LINE);
    NVIC_ClearPendingIRQ(ENCODER_BUTTON_EXTI_IRQn);

    NVIC_InitTypeDef button_nvic = {0};
    button_nvic.NVIC_IRQChannel = ENCODER_BUTTON_EXTI_IRQn;
    button_nvic.NVIC_IRQChannelPreemptionPriority = 2;
    button_nvic.NVIC_IRQChannelSubPriority = 1;
    button_nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&button_nvic);
}

int16_t encoder_take_delta(void)
{
    int_fast32_t raw_delta = atomic_exchange_explicit(&s_encoder_pending_raw_delta, 0,
                                                      memory_order_relaxed);
    int_fast32_t combined = raw_delta + (int_fast32_t)s_encoder_detent_remainder;
    int_fast32_t detent_delta = combined / ENCODER_COUNTS_PER_DETENT;

    s_encoder_detent_remainder = (int16_t)(combined - (detent_delta * ENCODER_COUNTS_PER_DETENT));

    if(detent_delta > INT16_MAX)
    {
        detent_delta = INT16_MAX;
    }
    else if(detent_delta < INT16_MIN)
    {
        detent_delta = INT16_MIN;
    }

    return (int16_t)detent_delta;
}

int32_t encoder_get_position(void)
{
    return s_encoder_raw_position / ENCODER_COUNTS_PER_DETENT;
}
