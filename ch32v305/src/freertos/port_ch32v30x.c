#include "FreeRTOS.h"
#include "task.h"

#include "debug.h"

#include <stdint.h>

#define CH32_SYSTICK_CTLR_STE      (1UL << 0)
#define CH32_SYSTICK_CTLR_STIE     (1UL << 1)
#define CH32_SYSTICK_CTLR_STCLK    (1UL << 2)
#define CH32_SYSTICK_COUNTS_PER_TICK \
    ((uint64_t)configCPU_CLOCK_HZ / (uint64_t)configTICK_RATE_HZ)

static uint64_t s_next_tick_deadline;

static uint64_t systick_count_read(void)
{
    volatile uint32_t const *count_words =
        (volatile uint32_t const *)(uintptr_t)&SysTick->CNT;
    uint32_t high_before;
    uint32_t low;
    uint32_t high_after;

    do
    {
        high_before = count_words[1];
        low = count_words[0];
        high_after = count_words[1];
    } while(high_before != high_after);

    return ((uint64_t)high_before << 32) | low;
}

static void systick_compare_write(uint64_t deadline)
{
    volatile uint32_t *compare_words =
        (volatile uint32_t *)(uintptr_t)&SysTick->CMP;

    /* Mask the low word while changing high, then publish the final low word. */
    compare_words[0] = UINT32_MAX;
    compare_words[1] = (uint32_t)(deadline >> 32);
    compare_words[0] = (uint32_t)deadline;
    __asm volatile("fence iorw, iorw" ::: "memory");
}

static BaseType_t pfic_has_enabled_pending_irq(void)
{
    for(uint32_t word = 0U; word < 4U; ++word)
    {
        if((NVIC->ISR[word] & NVIC->IPR[word]) != 0U)
        {
            return pdTRUE;
        }
    }

    return pdFALSE;
}

void vPortYield(void)
{
    NVIC_SetPendingIRQ(Software_IRQn);
}

void vPortSetupTimerInterrupt(void)
{
    configASSERT(configCPU_CLOCK_HZ == SystemCoreClock);
    configASSERT((configCPU_CLOCK_HZ % configTICK_RATE_HZ) == 0U);

    SysTick->CTLR = CH32_SYSTICK_CTLR_STE | CH32_SYSTICK_CTLR_STCLK;
    SysTick->SR = 0U;
    NVIC_ClearPendingIRQ(SysTicK_IRQn);
    NVIC_ClearPendingIRQ(Software_IRQn);

    s_next_tick_deadline = systick_count_read() + CH32_SYSTICK_COUNTS_PER_TICK;
    systick_compare_write(s_next_tick_deadline);

    /* PFIC priority values increase toward lower urgency. */
    NVIC_SetPriority(SysTicK_IRQn, 0xC0U);
    NVIC_SetPriority(Software_IRQn, 0xE0U);
    NVIC_EnableIRQ(SysTicK_IRQn);
    NVIC_EnableIRQ(Software_IRQn);

    SysTick->CTLR = CH32_SYSTICK_CTLR_STE |
                    CH32_SYSTICK_CTLR_STIE |
                    CH32_SYSTICK_CTLR_STCLK;
}

void SysTick_Handler(void) __attribute__((interrupt));
void SysTick_Handler(void)
{
    UBaseType_t saved_mie = portSET_INTERRUPT_MASK_FROM_ISR();
    BaseType_t switch_required = pdFALSE;
    uint64_t now;

    SysTick->SR = 0U;
    NVIC_ClearPendingIRQ(SysTicK_IRQn);

    now = systick_count_read();
    while(now >= s_next_tick_deadline)
    {
        if(xTaskIncrementTick() != pdFALSE)
        {
            switch_required = pdTRUE;
        }
        s_next_tick_deadline += CH32_SYSTICK_COUNTS_PER_TICK;
    }

    systick_compare_write(s_next_tick_deadline);
    portCLEAR_INTERRUPT_MASK_FROM_ISR(saved_mie);
    portYIELD_FROM_ISR(switch_required);
}

void vPortSuppressTicksAndSleep(TickType_t expected_idle_time)
{
    UBaseType_t saved_mie;
    uint64_t sleep_deadline;
    uint64_t now;
    uint64_t elapsed_tick_count = 0U;

    saved_mie = portSET_INTERRUPT_MASK_FROM_ISR();
    if(eTaskConfirmSleepModeStatus() == eAbortSleep)
    {
        portCLEAR_INTERRUPT_MASK_FROM_ISR(saved_mie);
        return;
    }

    /*
     * The next normal compare is the first pending tick boundary.  Reaching
     * expected_idle_time boundaries therefore requires expected_idle_time - 1
     * additional complete intervals.
     */
    sleep_deadline = s_next_tick_deadline;
    if(expected_idle_time > 1U)
    {
        sleep_deadline +=
            (uint64_t)(expected_idle_time - 1U) * CH32_SYSTICK_COUNTS_PER_TICK;
    }

    SysTick->SR = 0U;
    NVIC_ClearPendingIRQ(SysTicK_IRQn);
    systick_compare_write(sleep_deadline);

    /*
     * WFI is intentionally not used on this part.  Checking CNT >= deadline is
     * also essential because SysTick only documents an equality match: if the
     * 64-bit compare is installed after CNT passes it, waiting for CNTIF could
     * otherwise take an entire counter wrap.
     */
    for(;;)
    {
        now = systick_count_read();
        if((now >= sleep_deadline) ||
           (pfic_has_enabled_pending_irq() != pdFALSE))
        {
            break;
        }
    }

    now = systick_count_read();
    if(now >= s_next_tick_deadline)
    {
        elapsed_tick_count =
            ((now - s_next_tick_deadline) / CH32_SYSTICK_COUNTS_PER_TICK) + 1U;
        if(elapsed_tick_count > expected_idle_time)
        {
            elapsed_tick_count = expected_idle_time;
        }
    }

    s_next_tick_deadline +=
        elapsed_tick_count * CH32_SYSTICK_COUNTS_PER_TICK;

    SysTick->SR = 0U;
    NVIC_ClearPendingIRQ(SysTicK_IRQn);
    systick_compare_write(s_next_tick_deadline);

    if(elapsed_tick_count != 0U)
    {
        vTaskStepTick((TickType_t)elapsed_tick_count);
    }

    portCLEAR_INTERRUPT_MASK_FROM_ISR(saved_mie);
}
