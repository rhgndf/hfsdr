#ifndef RTC_H
#define RTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rtc_init(void);
void rtc_set_utc(uint32_t utc_seconds, int16_t local_offset_minutes);
bool rtc_get(uint32_t *utc_seconds, int16_t *local_offset_minutes);

#ifdef __cplusplus
}
#endif

#endif
