#ifndef ADC_HW_H
#define ADC_HW_H

#include <stdint.h>

#include "ch32v30x.h"

void adc_hw_init(void);

uint16_t adc_hw_read_batt_raw(void);
uint16_t adc_hw_read_vbus_raw(void);
uint16_t adc_hw_read_temp_raw(void);

uint32_t adc_hw_vdda_mv(void);

#endif
