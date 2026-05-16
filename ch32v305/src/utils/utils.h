#pragma once

extern "C" {
#include "debug.h"
}

#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

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
    const uint32_t trigger_ms;
    uint64_t last_trigger_tick = 0U;
    Callable f;
};

template<typename F>
PeriodicTrigger(uint32_t, F&&) -> PeriodicTrigger<std::decay_t<F>>;
