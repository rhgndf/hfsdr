#include "encoder.h"

#include "hw/pinout.h"

#include <stdatomic.h>
#include <stdbool.h>

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

/* Encoder switch: IPD, high when pressed (tied to VCC). */
#define ENC_BTN_PRESSED(st) ((st) != 0U)

static volatile uint16_t s_encoder_last_counter = ENCODER_COUNTER_MIDPOINT;
static atomic_int_fast32_t s_encoder_pending_raw_delta = 0;
static volatile int32_t s_encoder_raw_position = 0;
static int16_t s_encoder_detent_remainder = 0;
static uint8_t s_button_raw_state = 0U;
static uint8_t s_button_stable_state = 0U;
static uint64_t s_button_last_change_tick = 0U;
static bool s_button_pressed = false;

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

static void encoder_poll_button(void)
{
    uint64_t now_tick = SysTick->CNT;
    uint8_t raw_state = (uint8_t)GPIO_ReadInputDataBit(ENC_BTN_GPIO_PORT, ENC_BTN_GPIO_PIN);

    if(raw_state != s_button_raw_state)
    {
        s_button_raw_state = raw_state;
        s_button_last_change_tick = now_tick;
    }

    if((now_tick - s_button_last_change_tick) < encoder_ticks_from_ms(ENCODER_BUTTON_DEBOUNCE_MS))
    {
        return;
    }

    if(s_button_stable_state == s_button_raw_state)
    {
        return;
    }

    {
        uint8_t const prev_stable = s_button_stable_state;

        s_button_stable_state = s_button_raw_state;
        if(!ENC_BTN_PRESSED(prev_stable) && ENC_BTN_PRESSED(s_button_stable_state))
        {
            s_button_pressed = true;
        }
    }
}

__attribute__((interrupt)) void TIM10_CC_IRQHandler(void)
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
    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_ICInitTypeDef ic = {0};
    NVIC_InitTypeDef nvic = {0};

    RCC_APB2PeriphClockCmd(ENCODER_GPIO_PERIPH | ENCODER_TIMER_PERIPH | RCC_APB2Periph_GPIOC, ENABLE);

    gpio.GPIO_Pin = ENC_A_GPIO_PIN | ENC_B_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(ENC_A_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = ENC_BTN_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(ENC_BTN_GPIO_PORT, &gpio);

    TIM_DeInit(ENCODER_TIMER);

    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Prescaler = 0U;
    tim.TIM_Period = 0xFFFFU;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(ENCODER_TIMER, &tim);

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

    nvic.NVIC_IRQChannel = TIM10_CC_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(ENCODER_TIMER, ENABLE);

    s_encoder_last_counter = ENCODER_COUNTER_MIDPOINT;
    atomic_store_explicit(&s_encoder_pending_raw_delta, 0, memory_order_relaxed);
    s_encoder_raw_position = 0;
    s_encoder_detent_remainder = 0;
    s_button_raw_state = (uint8_t)GPIO_ReadInputDataBit(ENC_BTN_GPIO_PORT, ENC_BTN_GPIO_PIN);
    s_button_stable_state = s_button_raw_state;
    s_button_last_change_tick = SysTick->CNT;
    s_button_pressed = false;
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

bool encoder_take_button_press(void)
{
    encoder_poll_button();
    bool pressed = s_button_pressed;

    s_button_pressed = false;
    return pressed;
}

int32_t encoder_get_position(void)
{
    return s_encoder_raw_position / ENCODER_COUNTS_PER_DETENT;
}
