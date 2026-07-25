#pragma once

extern "C" {
#include "FreeRTOS.h"
#include "debug.h"
#include "semphr.h"
}

#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

class FreeRTOSLock
{
public:
    FreeRTOSLock() = default;
    FreeRTOSLock(const FreeRTOSLock &) = delete;
    FreeRTOSLock &operator=(const FreeRTOSLock &) = delete;
    FreeRTOSLock(FreeRTOSLock &&) = delete;
    FreeRTOSLock &operator=(FreeRTOSLock &&) = delete;

    void init() noexcept
    {
        configASSERT(m_mutex == nullptr);
        m_mutex = xSemaphoreCreateMutexStatic(&m_mutex_storage);
        configASSERT(m_mutex != nullptr);
    }

    void lock() noexcept
    {
        configASSERT(m_mutex != nullptr);
        BaseType_t const taken = xSemaphoreTake(m_mutex, portMAX_DELAY);
        configASSERT(taken == pdTRUE);
        (void)taken;
    }

    void unlock() noexcept
    {
        configASSERT(m_mutex != nullptr);
        BaseType_t const given = xSemaphoreGive(m_mutex);
        configASSERT(given == pdTRUE);
        (void)given;
    }

private:
    StaticSemaphore_t m_mutex_storage{};
    SemaphoreHandle_t m_mutex = nullptr;
};

inline uint64_t ticks_from_ms(uint32_t ms) noexcept
{
    uint64_t ticks = (static_cast<uint64_t>(SystemCoreClock) * static_cast<uint64_t>(ms)) / 1000ULL;
    if(ticks == 0U)
    {
        ticks = 1U;
    }
    return ticks;
}

template<typename Callable>
class PeriodicTrigger
{
public:
    template<typename F>
    constexpr PeriodicTrigger(uint32_t trigger_ms, F&& f) :
        trigger_ms(trigger_ms),
        f(std::forward<F>(f))
    {
    }

    void operator()()
    {
        uint64_t now_tick = SysTick->CNT;
        uint64_t trigger_period_ticks = ticks_from_ms(trigger_ms);

        if((now_tick - last_trigger_tick) < trigger_period_ticks)
        {
            return;
        }

        last_trigger_tick = now_tick;
        std::invoke(f);
    }

    void reset() noexcept
    {
        last_trigger_tick = 0U;
    }

private:
    uint64_t last_trigger_tick = 0U;
    const uint32_t trigger_ms;
    Callable f;
};

template<typename F>
PeriodicTrigger(uint32_t, F&&) -> PeriodicTrigger<std::decay_t<F>>;
