#include "encoder.h"

#include "hw/pinout.h"

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

static volatile uint16_t s_encoder_last_counter = ENCODER_COUNTER_MIDPOINT;
static volatile int16_t s_encoder_pending_raw_delta = 0;
static volatile int32_t s_encoder_raw_position = 0;
static int16_t s_encoder_detent_remainder = 0;

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

    s_encoder_pending_raw_delta += encoder_sync_delta();
}

void encoder_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    TIM_TimeBaseInitTypeDef tim = {0};
    TIM_ICInitTypeDef ic = {0};
    NVIC_InitTypeDef nvic = {0};

    RCC_APB2PeriphClockCmd(ENCODER_GPIO_PERIPH | ENCODER_TIMER_PERIPH, ENABLE);

    gpio.GPIO_Pin = ENC_A_GPIO_PIN | ENC_B_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(ENC_A_GPIO_PORT, &gpio);

    TIM_DeInit(ENCODER_TIMER);

    TIM_TimeBaseStructInit(&tim);
    tim.TIM_Prescaler = 0U;
    tim.TIM_Period = 0xFFFFU;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(ENCODER_TIMER, &tim);

    /* Filter both phases to suppress switch bounce from a mechanical encoder. */
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
    s_encoder_pending_raw_delta = 0;
    s_encoder_raw_position = 0;
    s_encoder_detent_remainder = 0;
}

int16_t encoder_take_delta(void)
{
    int16_t raw_delta = s_encoder_pending_raw_delta;
    int16_t detent_delta;

    s_encoder_pending_raw_delta = 0;
    raw_delta += s_encoder_detent_remainder;

    detent_delta = raw_delta / ENCODER_COUNTS_PER_DETENT;
    s_encoder_detent_remainder = raw_delta - (detent_delta * ENCODER_COUNTS_PER_DETENT);

    return detent_delta;
}

int32_t encoder_get_position(void)
{
    return s_encoder_raw_position / ENCODER_COUNTS_PER_DETENT;
}
