#include "adc.h"

#include "hw/pinout.h"

#include "ch32v30x_adc.h"
#include "ch32v30x_dma.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_rcc.h"

#define BATT_ADC_CHANNEL ADC_Channel_14
#define VBUS_ADC_CHANNEL ADC_Channel_15
#define TEMP_ADC_CHANNEL ADC_Channel_TempSensor
#define VREF_ADC_CHANNEL ADC_Channel_Vrefint

#define ADC_DMA_CHANNEL  DMA1_Channel1
#define ADC_OVERSAMPLE_N 16U

#define VREFINT_NOMINAL_MV 1200U

static volatile uint16_t s_dma_buf[ADC_OVERSAMPLE_N];
static uint32_t s_vdda_mv;

static uint16_t adc_hw_read_channel(uint8_t channel)
{
    ADC_RegularChannelConfig(ADC1, channel, 1, ADC_SampleTime_239Cycles5);

    DMA_DeInit(ADC_DMA_CHANNEL);
    DMA_InitTypeDef dma = {0};
    dma.DMA_PeripheralBaseAddr = (uint32_t)&ADC1->RDATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)s_dma_buf;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = ADC_OVERSAMPLE_N;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(ADC_DMA_CHANNEL, &dma);
    DMA_Cmd(ADC_DMA_CHANNEL, ENABLE);

    ADC_DMACmd(ADC1, ENABLE);
    ADC1->CTLR2 |= ADC_CONT;
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);

    while(DMA_GetFlagStatus(DMA1_FLAG_TC1) == RESET)
    {
    }
    DMA_ClearFlag(DMA1_FLAG_TC1);

    ADC1->CTLR2 &= ~ADC_CONT;
    ADC_SoftwareStartConvCmd(ADC1, DISABLE);
    ADC_DMACmd(ADC1, DISABLE);
    DMA_Cmd(ADC_DMA_CHANNEL, DISABLE);

    uint32_t sum = 0U;
    for(uint32_t i = 0U; i < ADC_OVERSAMPLE_N; ++i)
    {
        sum += s_dma_buf[i];
    }
    return (uint16_t)(sum / ADC_OVERSAMPLE_N);
}

void adc_hw_init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_ADC1, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = BATT_ADC_GPIO_PIN | VBUS_ADC_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(BATT_ADC_GPIO_PORT, &gpio);

    ADC_DeInit(ADC1);

    ADC_InitTypeDef adc = {0};
    ADC_StructInit(&adc);
    adc.ADC_Mode = ADC_Mode_Independent;
    adc.ADC_ScanConvMode = DISABLE;
    adc.ADC_ContinuousConvMode = DISABLE;
    adc.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    adc.ADC_DataAlign = ADC_DataAlign_Right;
    adc.ADC_NbrOfChannel = 1;
    adc.ADC_OutputBuffer = ADC_OutputBuffer_Disable;
    adc.ADC_Pga = ADC_Pga_1;
    ADC_Init(ADC1, &adc);

    ADC_Cmd(ADC1, ENABLE);

    // Calibrate before enabling TSVREFE (buffer forced on by TSVREFE)
    ADC_ResetCalibration(ADC1);
    while(ADC_GetResetCalibrationStatus(ADC1))
    {
    }
    ADC_StartCalibration(ADC1);
    while(ADC_GetCalibrationStatus(ADC1))
    {
    }

    ADC_TempSensorVrefintCmd(ENABLE);

    uint16_t vrefint_raw = adc_hw_read_channel(VREF_ADC_CHANNEL);
    if(vrefint_raw > 0U)
    {
        s_vdda_mv = (VREFINT_NOMINAL_MV * 4096U) / (uint32_t)vrefint_raw;
    }
    else
    {
        s_vdda_mv = 3300U;
    }

    printf("ADC: VREFINT raw=%u, VDDA=%lu mV\r\n",
           vrefint_raw, (unsigned long)s_vdda_mv);
}

uint16_t adc_hw_read_batt_raw(void)
{
    return adc_hw_read_channel(BATT_ADC_CHANNEL);
}

uint16_t adc_hw_read_vbus_raw(void)
{
    return adc_hw_read_channel(VBUS_ADC_CHANNEL);
}

uint16_t adc_hw_read_temp_raw(void)
{
    return adc_hw_read_channel(TEMP_ADC_CHANNEL);
}

uint32_t adc_hw_vdda_mv(void)
{
    return s_vdda_mv;
}
