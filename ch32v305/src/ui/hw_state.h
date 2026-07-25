#ifndef HFSDR_UI_HW_STATE_H
#define HFSDR_UI_HW_STATE_H

#include <stdint.h>

#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

void hw_state_init(void);

void hw_state_set_frequency(uint32_t frequency_hz);
void hw_state_set_gain_x2(int8_t gain_db_x2);

uint32_t hw_state_get_frequency(void);
uint32_t hw_state_get_requested_frequency(void);
ErrorStatus hw_state_get_frequency_status(void);
int8_t hw_state_get_gain_x2(void);

void hw_state_publish_boot_state(uint32_t requested_frequency_hz,
                                 uint32_t actual_frequency_hz,
                                 ErrorStatus frequency_status,
                                 int8_t actual_gain_db_x2);

#ifdef __cplusplus
}
#endif

#endif
