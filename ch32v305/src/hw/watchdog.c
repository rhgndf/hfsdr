#include "watchdog.h"

#include "ch32v30x_dbgmcu.h"
#include "ch32v30x_iwdg.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_wwdg.h"

#define IWDG_LSI_FREQ_HZ         32000U
#define IWDG_TIMEOUT_MS          5000U
#define IWDG_PRESCALER_DIV       256U
#define IWDG_RELOAD_VALUE        0xFFFFU

#define WWDG_COUNTER_VALUE        0x7FU
#define WWDG_WINDOW_VALUE         0x7FU

static void iwdg_wait_ready(void)
{
    while((IWDG_GetFlagStatus(IWDG_FLAG_PVU) != RESET) ||
          (IWDG_GetFlagStatus(IWDG_FLAG_RVU) != RESET))
    {
    }
}

static void debug_stop_on_halt(uint32_t dbg_periph)
{
    __set_DEBUG_CR(__get_DEBUG_CR() | dbg_periph);
}

static void iwdg_init(void)
{
    RCC_LSICmd(ENABLE);
    while(RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET)
    {
    }
    

    debug_stop_on_halt(DBGMCU_IWDG_STOP);

    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_256);
    IWDG_SetReload(IWDG_RELOAD_VALUE);
    iwdg_wait_ready();
    IWDG_ReloadCounter();
    IWDG_Enable();
}

static void iwdg_kick(void)
{
    IWDG_ReloadCounter();
}

static void wwdg_init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_WWDG, ENABLE);

    debug_stop_on_halt(DBGMCU_WWDG_STOP);

    WWDG_SetPrescaler(WWDG_Prescaler_8);
    WWDG_SetWindowValue(WWDG_WINDOW_VALUE);
    WWDG_Enable(WWDG_COUNTER_VALUE);
}

static void wwdg_kick(void)
{
    WWDG_SetCounter(WWDG_COUNTER_VALUE);
}

void watchdog_init(void)
{
    iwdg_init();
    //wwdg_init();
}

void watchdog_kick(void)
{
    //wwdg_kick();
    iwdg_kick();
}
