#include "rtc.h"

#include "ch32v30x_pwr.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_rtc.h"

#include <stddef.h>

#define RTC_HSE_DIVIDER            128U
#define RTC_CLOCK_HZ               (HSE_VALUE / RTC_HSE_DIVIDER)
#define RTC_PRESCALER              (RTC_CLOCK_HZ - 1U)
#define RTC_MAX_PRESCALER          0x000FFFFFU

static_assert((HSE_VALUE % RTC_HSE_DIVIDER) == 0U,
              "HSE must be exactly divisible by the RTC clock divider");
static_assert(RTC_CLOCK_HZ > 0U, "RTC clock must be nonzero");
static_assert(RTC_PRESCALER <= RTC_MAX_PRESCALER,
              "RTC prescaler does not fit in the hardware register");

static bool s_synchronized;
static int16_t s_local_offset_minutes;

static void rtc_backup_access(FunctionalState state)
{
    PWR_BackupAccessCmd(state);
}

void rtc_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    rtc_backup_access(ENABLE);

    if((RCC->BDCTLR & RCC_RTCSEL) != RCC_RTCSEL_HSE)
    {
        RCC_BackupResetCmd(ENABLE);
        RCC_BackupResetCmd(DISABLE);
        RCC_RTCCLKConfig(RCC_RTCCLKSource_HSE_Div128);
    }

    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
    RTC_WaitForLastTask();
    RTC_SetPrescaler(RTC_PRESCALER);
    RTC_WaitForLastTask();

    rtc_backup_access(DISABLE);
    s_local_offset_minutes = 0;
    s_synchronized = false;
}

void rtc_set_utc(uint32_t utc_seconds, int16_t local_offset_minutes)
{
    rtc_backup_access(ENABLE);
    RTC_WaitForLastTask();
    RTC_SetCounter(utc_seconds);
    RTC_WaitForLastTask();
    rtc_backup_access(DISABLE);

    s_local_offset_minutes = local_offset_minutes;
    s_synchronized = true;
}

bool rtc_get(uint32_t *utc_seconds, int16_t *local_offset_minutes)
{
    if(!s_synchronized)
    {
        return false;
    }

    if(utc_seconds != NULL)
    {
        *utc_seconds = RTC_GetCounter();
    }
    if(local_offset_minutes != NULL)
    {
        *local_offset_minutes = s_local_offset_minutes;
    }
    return true;
}
