#include "watchdog.h"

#include "ch32v30x_dbgmcu.h"
#include "ch32v30x_iwdg.h"
#include "ch32v30x_rcc.h"

#define IWDG_LSI_FREQ_HZ         32000U
#define IWDG_TIMEOUT_MS          5000U
#define IWDG_PRESCALER_DIV       256U
#define IWDG_RELOAD_VALUE        ((((IWDG_LSI_FREQ_HZ * IWDG_TIMEOUT_MS) / 1000U) / IWDG_PRESCALER_DIV) - 1U)

static void watchdog_wait_ready(void)
{
    while((IWDG_GetFlagStatus(IWDG_FLAG_PVU) != RESET) ||
          (IWDG_GetFlagStatus(IWDG_FLAG_RVU) != RESET))
    {
    }
}

void watchdog_init(void)
{
    RCC_LSICmd(ENABLE);
    while(RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET)
    {
    }
    

    // Possible library bug: it replaces the entire register, doesn't OR it in
    //DBGMCU_Config(DBGMCU_IWDG_STOP, ENABLE);

    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_256);
    IWDG_SetReload((uint16_t)IWDG_RELOAD_VALUE);
    watchdog_wait_ready();
    IWDG_ReloadCounter();
    IWDG_Enable();
}

void watchdog_kick(void)
{
    IWDG_ReloadCounter();
}
