#pragma once

extern "C" {
#include "debug.h"
}
#include "utils/dsp.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>

enum ProfileName : uint8_t {
    PROFILE_FM_ISR,
    PROFILE_FFT_DRAW,
    PROFILE_UI_DRAW,
    PROFILE_COUNT
};

static constexpr std::array<const char*, PROFILE_COUNT> kProfileNames = {
    "FM_ISR",
    "FFT_DRAW",
    "UI_DRAW",
};

constexpr size_t TIMER_AVG_SAMPLES = 16;

class TimerRecorder
{
    struct Entry {
        CICFilter<uint32_t, TIMER_AVG_SAMPLES> avg;
        uint32_t min_ticks = std::numeric_limits<uint32_t>::max();
        uint32_t max_ticks = 0;
        uint32_t last_avg = 0;
        uint32_t sample_count = 0;
    };

    std::array<Entry, PROFILE_COUNT> entries = {};

public:
    void record(ProfileName name, uint32_t ticks) noexcept
    {
        auto& e = entries[name];
        e.last_avg = e.avg.push(ticks);
        e.min_ticks = std::min(e.min_ticks, ticks);
        e.max_ticks = std::max(e.max_ticks, ticks);
        ++e.sample_count;
    }

    void print() noexcept
    {
        uint32_t clk_mhz = std::max(SystemCoreClock / 1000000UL, 1UL);

        printf("Instrumentation:\r\n");
        for(size_t i = 0U; i < entries.size(); ++i)
        {
            auto& e = entries[i];
            if(e.sample_count == 0U) continue;
            printf("    %s: min=%lu max=%lu avg=%lu us\r\n",
                   kProfileNames[i],
                   (unsigned long)(e.min_ticks / clk_mhz),
                   (unsigned long)(e.max_ticks / clk_mhz),
                   (unsigned long)(e.last_avg / clk_mhz));
            e.min_ticks = std::numeric_limits<uint32_t>::max();
            e.max_ticks = 0;
            e.sample_count = 0;
        }
    }
};

inline TimerRecorder g_timer_recorder;

template<ProfileName P>
class ScopedTimer
{
    uint64_t start;
public:
    ScopedTimer() noexcept : start(SysTick->CNT) {}
    ~ScopedTimer() noexcept
    {
        uint32_t elapsed = static_cast<uint32_t>(SysTick->CNT - start);
        g_timer_recorder.record(P, elapsed);
    }
    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
};
