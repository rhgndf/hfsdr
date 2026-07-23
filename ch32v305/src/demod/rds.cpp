#include "demod/rds.h"

#include "hw/rtc.h"
#include "utils/dsp.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <numbers>

namespace
{
constexpr uint32_t kInputSampleRateHz = 192000U;
constexpr uint32_t kBasebandSampleRateHz = 12000U;
constexpr uint32_t kRdsSubcarrierHz = 57000U;
constexpr uint16_t kSymbolPhaseModulus = 192U;
constexpr uint16_t kSymbolPhaseIncrement = 19U;
constexpr uint16_t kSymbolHalfPhase = kSymbolPhaseModulus / 2U;
constexpr size_t kTimingHypothesisCount = 8U;
constexpr uint16_t kTimingHypothesisSpacing =
    kSymbolPhaseModulus / kTimingHypothesisCount;

constexpr uint32_t kGeneratorPolynomial = 0x5B9U;
constexpr uint32_t kBlockMask = (1UL << 26U) - 1U;
constexpr uint16_t kCheckMask = (1U << 10U) - 1U;

constexpr uint16_t kOffsetA = 0x0FCU;
constexpr uint16_t kOffsetB = 0x198U;
constexpr uint16_t kOffsetC = 0x168U;
constexpr uint16_t kOffsetCPrime = 0x350U;
constexpr uint16_t kOffsetD = 0x1B4U;

constexpr size_t kMixerTableSize = 64U;
constexpr size_t kMixerTableMask = kMixerTableSize - 1U;
constexpr uint8_t kMixerInputShift = 20U;
constexpr size_t kMovingAverageLength = 16U;
constexpr size_t kMovingAverageMask = kMovingAverageLength - 1U;
constexpr uint8_t kMovingAverageShift =
    static_cast<uint8_t>(std::countr_zero(kMovingAverageLength));
constexpr size_t kBasebandCicTapCount = 4U;
constexpr size_t kMaxBasebandSamplesPerSymbol =
    (kSymbolPhaseModulus + kSymbolPhaseIncrement - 1U) / kSymbolPhaseIncrement;
constexpr size_t kGroupQueueCapacity = 16U;
constexpr size_t kGroupQueueMask = kGroupQueueCapacity - 1U;
constexpr uint32_t kUnixEpochMjd = 40587U;
constexpr uint32_t kMaximumMjd = 99999U;
constexpr uint32_t kSecondsPerMinute = 60U;
constexpr uint32_t kMinutesPerHour = 60U;
constexpr uint32_t kHoursPerDay = 24U;
constexpr uint32_t kSecondsPerHour = kMinutesPerHour * kSecondsPerMinute;
constexpr uint32_t kSecondsPerDay = kHoursPerDay * kSecondsPerHour;
constexpr uint8_t kMaximumLocalOffsetHalfHours = 24U;
constexpr size_t kCorrectableBurstCount =
    26U + 25U + (24U * 2U) + (23U * 4U) + (22U * 8U);
constexpr int64_t kMixerMagnitudeBound =
    (static_cast<int64_t>(std::numeric_limits<int32_t>::max()) /
     (1LL << kMixerInputShift) + 1LL) * 32768LL;

static_assert(kInputSampleRateHz / kMovingAverageLength == kBasebandSampleRateHz);
static_assert(std::has_single_bit(kMovingAverageLength));
static_assert(kRdsSubcarrierHz * kMixerTableSize / kInputSampleRateHz == 19U);
static_assert((kGroupQueueCapacity & (kGroupQueueCapacity - 1U)) == 0U);
static_assert(kCorrectableBurstCount == 367U);
static_assert(kMixerMagnitudeBound * kMovingAverageLength <=
              std::numeric_limits<int32_t>::max());
static_assert(kMixerMagnitudeBound * kBasebandCicTapCount <=
              std::numeric_limits<int32_t>::max());
static_assert(kMixerMagnitudeBound * kMaxBasebandSamplesPerSymbol <=
              std::numeric_limits<int32_t>::max());

struct ComplexSample
{
    int32_t i = 0;
    int32_t q = 0;
};

constexpr int32_t clamp_i32(int64_t value)
{
    if(value > std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }
    if(value < std::numeric_limits<int32_t>::min())
    {
        return std::numeric_limits<int32_t>::min();
    }
    return static_cast<int32_t>(value);
}

constexpr uint16_t polynomial_remainder(uint32_t value)
{
    value &= kBlockMask;
    for(int bit = 25; bit >= 10; --bit)
    {
        uint32_t const bit_mask = 1UL << static_cast<unsigned int>(bit);
        if((value & bit_mask) != 0U)
        {
            value ^= kGeneratorPolynomial << static_cast<unsigned int>(bit - 10);
        }
    }
    return static_cast<uint16_t>(value & kCheckMask);
}

constexpr uint16_t polynomial_remainder_wide(uint32_t value,
                                             unsigned int highest_bit)
{
    for(int bit = static_cast<int>(highest_bit); bit >= 10; --bit)
    {
        uint32_t const bit_mask = 1UL << static_cast<unsigned int>(bit);
        if((value & bit_mask) != 0U)
        {
            value ^= kGeneratorPolynomial << static_cast<unsigned int>(bit - 10);
        }
    }
    return static_cast<uint16_t>(value & kCheckMask);
}

constexpr uint16_t kDroppedWindowBitSyndrome =
    polynomial_remainder_wide(1UL << 26U, 26U);

constexpr uint16_t polynomial_push_bit(uint16_t syndrome, bool bit)
{
    uint16_t next = static_cast<uint16_t>(
        (static_cast<uint16_t>(syndrome << 1U) |
         static_cast<uint16_t>(bit)) & 0x07FFU);
    if((next & (1U << 10U)) != 0U)
    {
        next = static_cast<uint16_t>(next ^ kGeneratorPolynomial);
    }
    return static_cast<uint16_t>(next & kCheckMask);
}

constexpr uint16_t polynomial_push_window_bit(uint16_t syndrome,
                                              bool dropped_bit,
                                              bool bit)
{
    uint16_t next = polynomial_push_bit(syndrome, bit);
    if(dropped_bit)
    {
        next = static_cast<uint16_t>(next ^ kDroppedWindowBitSyndrome);
    }
    return next;
}

consteval bool rolling_syndrome_matches_polynomial_division()
{
    uint32_t window = 0U;
    uint16_t syndrome = 0U;
    uint32_t sequence = 0xA5C39E71U;

    for(unsigned int i = 0U; i < 1024U; ++i)
    {
        sequence = sequence * 1664525U + 1013904223U;
        bool const bit = (sequence & 0x80000000U) != 0U;
        bool const dropped_bit = (window & (1UL << 25U)) != 0U;

        syndrome = polynomial_push_window_bit(syndrome, dropped_bit, bit);
        window = ((window << 1U) | static_cast<uint32_t>(bit)) & kBlockMask;
        if(syndrome != polynomial_remainder(window))
        {
            return false;
        }
    }
    return true;
}

static_assert(kDroppedWindowBitSyndrome == 0x0EEU);
static_assert(rolling_syndrome_matches_polynomial_division());

struct BurstTable
{
    std::array<uint32_t, kCorrectableBurstCount> masks{};
    std::array<uint16_t, kCorrectableBurstCount> syndromes{};
};

consteval BurstTable make_burst_table()
{
    BurstTable table{};
    size_t index = 0U;

    for(unsigned int span = 1U; span <= 5U; ++span)
    {
        unsigned int const interior_count = (span <= 2U) ? 1U : (1U << (span - 2U));
        for(unsigned int start = 0U; start + span <= 26U; ++start)
        {
            for(unsigned int interior = 0U; interior < interior_count; ++interior)
            {
                uint32_t mask = 1UL << start;
                if(span > 1U)
                {
                    mask |= 1UL << (start + span - 1U);
                    for(unsigned int bit = 1U; bit + 1U < span; ++bit)
                    {
                        if((interior & (1U << (bit - 1U))) != 0U)
                        {
                            mask |= 1UL << (start + bit);
                        }
                    }
                }

                table.masks[index] = mask;
                table.syndromes[index] = polynomial_remainder(mask);
                ++index;
            }
        }
    }

    for(size_t i = 1U; i < table.syndromes.size(); ++i)
    {
        uint16_t const syndrome = table.syndromes[i];
        uint32_t const mask = table.masks[i];
        size_t insert = i;
        while((insert > 0U) && (table.syndromes[insert - 1U] > syndrome))
        {
            table.syndromes[insert] = table.syndromes[insert - 1U];
            table.masks[insert] = table.masks[insert - 1U];
            --insert;
        }
        table.syndromes[insert] = syndrome;
        table.masks[insert] = mask;
    }

    return table;
}

static constexpr BurstTable kBurstTable = make_burst_table();

consteval bool burst_syndromes_are_unique()
{
    if(kBurstTable.syndromes[0U] == 0U)
    {
        return false;
    }
    for(size_t i = 1U; i < kBurstTable.syndromes.size(); ++i)
    {
        if(kBurstTable.syndromes[i - 1U] == kBurstTable.syndromes[i])
        {
            return false;
        }
    }
    return true;
}

static_assert(burst_syndromes_are_unique());
static_assert(polynomial_remainder((1UL << 10U) | 0x1B9U) == 0U);
static_assert(polynomial_remainder((1UL << 10U) | 0x021U) == kOffsetB);
static_assert(polynomial_remainder((0xFFFFUL << 10U) | 0x0CDU) == 0U);
static_assert(polynomial_remainder((0xFFFFUL << 10U) | 0x155U) == kOffsetB);

struct OscillatorSample
{
    int16_t cos_q15 = 0;
    int16_t sin_q15 = 0;
};

consteval int16_t unit_to_q15(double value)
{
    double const scaled = value * 32768.0;
    if(scaled >= 32767.0)
    {
        return 32767;
    }
    if(scaled <= -32768.0)
    {
        return -32768;
    }
    return static_cast<int16_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
}

consteval std::array<OscillatorSample, kMixerTableSize> make_mixer_table()
{
    std::array<OscillatorSample, kMixerTableSize> table{};
    for(size_t i = 0U; i < table.size(); ++i)
    {
        double const phase = 2.0 * std::numbers::pi_v<double> *
                             static_cast<double>(i) /
                             static_cast<double>(table.size());
        table[i] = {
            unit_to_q15(std::cos(phase)),
            unit_to_q15(-std::sin(phase)),
        };
    }
    return table;
}

static constexpr auto kMixerTable = make_mixer_table();

class ComplexDecimator
{
    std::array<ComplexSample, kMovingAverageLength> input_history{};
    std::array<ComplexSample, kMovingAverageLength> first_stage_history{};
    int32_t first_sum_i = 0;
    int32_t first_sum_q = 0;
    int32_t second_sum_i = 0;
    int32_t second_sum_q = 0;
    uint8_t index = 0U;

public:
    constexpr bool push(ComplexSample sample, ComplexSample &output)
    {
        ComplexSample &input_slot = input_history[index];
        first_sum_i += sample.i - input_slot.i;
        first_sum_q += sample.q - input_slot.q;
        input_slot = sample;

        ComplexSample const first_stage{
            first_sum_i >> kMovingAverageShift,
            first_sum_q >> kMovingAverageShift,
        };
        ComplexSample &first_stage_slot = first_stage_history[index];
        second_sum_i += first_stage.i - first_stage_slot.i;
        second_sum_q += first_stage.q - first_stage_slot.q;
        first_stage_slot = first_stage;

        index = static_cast<uint8_t>((index + 1U) & kMovingAverageMask);
        if(index != 0U)
        {
            return false;
        }

        output = {
            second_sum_i >> kMovingAverageShift,
            second_sum_q >> kMovingAverageShift,
        };
        return true;
    }

    void reset()
    {
        input_history = {};
        first_stage_history = {};
        first_sum_i = 0;
        first_sum_q = 0;
        second_sum_i = 0;
        second_sum_q = 0;
        index = 0U;
    }
};

consteval bool fused_decimator_matches_cascade()
{
    ComplexDecimator decimator;
    std::array<ComplexSample, kMovingAverageLength> input_history{};
    std::array<ComplexSample, kMovingAverageLength> first_stage_history{};
    int32_t first_sum_i = 0;
    int32_t first_sum_q = 0;
    int32_t second_sum_i = 0;
    int32_t second_sum_q = 0;
    size_t first_index = 0U;
    size_t second_index = 0U;
    uint8_t decimation_phase = 0U;
    uint32_t sequence = 0x6D2B79F5U;

    for(size_t i = 0U; i < 512U; ++i)
    {
        sequence = sequence * 1664525U + 1013904223U;
        ComplexSample const sample{
            static_cast<int32_t>((sequence >> 16U) & 0xFFFFU) - 32768,
            static_cast<int32_t>(sequence & 0xFFFFU) - 32768,
        };

        ComplexSample &input_slot = input_history[first_index];
        first_sum_i += sample.i - input_slot.i;
        first_sum_q += sample.q - input_slot.q;
        input_slot = sample;
        first_index = (first_index + 1U) & kMovingAverageMask;

        ComplexSample const first_stage{
            first_sum_i >> kMovingAverageShift,
            first_sum_q >> kMovingAverageShift,
        };
        ComplexSample &first_stage_slot = first_stage_history[second_index];
        second_sum_i += first_stage.i - first_stage_slot.i;
        second_sum_q += first_stage.q - first_stage_slot.q;
        first_stage_slot = first_stage;
        second_index = (second_index + 1U) & kMovingAverageMask;

        decimation_phase = static_cast<uint8_t>(
            (decimation_phase + 1U) & kMovingAverageMask);
        bool const expected_ready = decimation_phase == 0U;
        ComplexSample output{};
        bool const ready = decimator.push(sample, output);
        if(ready != expected_ready)
        {
            return false;
        }
        if(ready && ((output.i != (second_sum_i >> kMovingAverageShift)) ||
                     (output.q != (second_sum_q >> kMovingAverageShift))))
        {
            return false;
        }
    }
    return true;
}

static_assert(fused_decimator_matches_cascade());

class BiphaseCorrelator
{
    uint16_t phase = 0U;
    int32_t acc_i = 0;
    int32_t acc_q = 0;
    bool discarded_partial_symbol = false;

public:
    void reset(uint16_t initial_phase)
    {
        phase = static_cast<uint16_t>(initial_phase % kSymbolPhaseModulus);
        acc_i = 0;
        acc_q = 0;
        discarded_partial_symbol = false;
    }

    bool push(ComplexSample sample, ComplexSample &symbol)
    {
        int32_t const sign = (phase < kSymbolHalfPhase) ? 1 : -1;
        acc_i += sign * sample.i;
        acc_q += sign * sample.q;

        uint16_t next_phase = static_cast<uint16_t>(phase + kSymbolPhaseIncrement);
        if(next_phase < kSymbolPhaseModulus)
        {
            phase = next_phase;
            return false;
        }

        phase = static_cast<uint16_t>(next_phase - kSymbolPhaseModulus);
        symbol = {
            acc_i >> 3U,
            acc_q >> 3U,
        };
        acc_i = 0;
        acc_q = 0;

        if(!discarded_partial_symbol)
        {
            discarded_partial_symbol = true;
            return false;
        }
        return true;
    }

};

class DifferentialSampler
{
    BiphaseCorrelator correlator;
    ComplexSample previous_symbol{};
    bool previous_symbol_valid = false;

public:
    void reset(uint16_t initial_phase)
    {
        correlator.reset(initial_phase);
        previous_symbol = {};
        previous_symbol_valid = false;
    }

    bool push(ComplexSample sample, bool &bit)
    {
        ComplexSample symbol{};
        if(!correlator.push(sample, symbol))
        {
            return false;
        }

        if(!previous_symbol_valid)
        {
            previous_symbol = symbol;
            previous_symbol_valid = true;
            return false;
        }

        int64_t const dot = static_cast<int64_t>(previous_symbol.i) * symbol.i +
                            static_cast<int64_t>(previous_symbol.q) * symbol.q;
        bit = dot < 0;
        previous_symbol = symbol;
        return true;
    }

};

enum class OffsetKind : uint8_t
{
    A,
    B,
    C,
    CPrime,
    D,
    Invalid,
};

constexpr uint8_t offset_position(OffsetKind offset)
{
    switch(offset)
    {
        case OffsetKind::A:      return 0U;
        case OffsetKind::B:      return 1U;
        case OffsetKind::C:
        case OffsetKind::CPrime: return 2U;
        case OffsetKind::D:      return 3U;
        case OffsetKind::Invalid:
        default:                 return 0xFFU;
    }
}

constexpr OffsetKind identify_offset(uint16_t syndrome)
{
    switch(syndrome)
    {
        case kOffsetA:      return OffsetKind::A;
        case kOffsetB:      return OffsetKind::B;
        case kOffsetC:      return OffsetKind::C;
        case kOffsetCPrime: return OffsetKind::CPrime;
        case kOffsetD:      return OffsetKind::D;
        default:            return OffsetKind::Invalid;
    }
}

class SynchronizationSearch
{
    uint32_t window = 0U;
    uint16_t syndrome = 0U;
    uint32_t bit_count = 0U;
    uint32_t last_hit_bit = 0U;
    OffsetKind last_hit = OffsetKind::Invalid;

public:
    void reset()
    {
        window = 0U;
        syndrome = 0U;
        bit_count = 0U;
        last_hit_bit = 0U;
        last_hit = OffsetKind::Invalid;
    }

    bool push(bool bit, OffsetKind &acquired_offset, uint32_t &acquired_block)
    {
        bool const dropped_bit = (window & (1UL << 25U)) != 0U;
        syndrome = polynomial_push_window_bit(syndrome, dropped_bit, bit);
        window = ((window << 1U) | static_cast<uint32_t>(bit)) & kBlockMask;
        ++bit_count;
        if(bit_count < 26U)
        {
            return false;
        }

        OffsetKind const current_hit = identify_offset(syndrome);
        if(current_hit == OffsetKind::Invalid)
        {
            return false;
        }

        bool acquired = false;
        if(last_hit != OffsetKind::Invalid)
        {
            uint32_t const distance_bits = bit_count - last_hit_bit;
            if((distance_bits >= 26U) && ((distance_bits % 26U) == 0U))
            {
                uint32_t const distance_blocks = distance_bits / 26U;
                uint8_t const expected_position = static_cast<uint8_t>(
                    (offset_position(last_hit) + distance_blocks) & 3U);
                acquired = offset_position(current_hit) == expected_position;
            }
        }

        last_hit = current_hit;
        last_hit_bit = bit_count;
        if(acquired)
        {
            acquired_offset = current_hit;
            acquired_block = window;
        }
        return acquired;
    }
};

class AcquisitionChannel
{
public:
    DifferentialSampler sampler;
    SynchronizationSearch search;

    void reset(uint16_t phase)
    {
        sampler.reset(phase);
        search.reset();
    }

    bool push(ComplexSample sample, OffsetKind &offset, uint32_t &block)
    {
        bool bit = false;
        return sampler.push(sample, bit) && search.push(bit, offset, block);
    }
};

struct Group
{
    std::array<uint16_t, 4U> words{};
    uint8_t corrected_mask = 0U;
};

struct ClockTime
{
    uint32_t mjd = 0U;
    uint32_t utc_seconds = 0U;
    int16_t local_offset_minutes = 0;
    uint8_t hour = 0U;
    uint8_t minute = 0U;
    bool valid = false;
};

constexpr ClockTime decode_clock_time(uint16_t block_b,
                                      uint16_t block_c,
                                      uint16_t block_d)
{
    if((block_b & 0xF800U) != 0x4000U)
    {
        return {};
    }

    uint32_t const mjd =
        (static_cast<uint32_t>(block_b & 0x0003U) << 15U) |
        (static_cast<uint32_t>(block_c) >> 1U);
    uint8_t const hour = static_cast<uint8_t>(
        ((block_c & 0x0001U) << 4U) | ((block_d >> 12U) & 0x000FU));
    uint8_t const minute = static_cast<uint8_t>((block_d >> 6U) & 0x003FU);
    uint8_t const offset_half_hours = static_cast<uint8_t>(block_d & 0x001FU);

    if((mjd < kUnixEpochMjd) || (mjd > kMaximumMjd) ||
       (hour >= kHoursPerDay) || (minute >= kMinutesPerHour) ||
       (offset_half_hours > kMaximumLocalOffsetHalfHours))
    {
        return {};
    }

    uint64_t const utc_seconds =
        static_cast<uint64_t>(mjd - kUnixEpochMjd) * kSecondsPerDay +
        static_cast<uint64_t>(hour) * kSecondsPerHour +
        static_cast<uint64_t>(minute) * kSecondsPerMinute;
    if(utc_seconds > std::numeric_limits<uint32_t>::max())
    {
        return {};
    }

    int16_t local_offset_minutes =
        static_cast<int16_t>(offset_half_hours * 30U);
    if((block_d & 0x0020U) != 0U)
    {
        local_offset_minutes = static_cast<int16_t>(-local_offset_minutes);
    }

    return {
        mjd,
        static_cast<uint32_t>(utc_seconds),
        local_offset_minutes,
        hour,
        minute,
        true,
    };
}

constexpr uint16_t make_clock_time_block_b(uint32_t mjd)
{
    return static_cast<uint16_t>(0x4000U | ((mjd >> 15U) & 0x0003U));
}

constexpr uint16_t make_clock_time_block_c(uint32_t mjd, uint8_t hour)
{
    return static_cast<uint16_t>(((mjd & 0x7FFFU) << 1U) |
                                 ((hour >> 4U) & 0x0001U));
}

constexpr uint16_t make_clock_time_block_d(uint8_t hour,
                                            uint8_t minute,
                                            bool negative_offset,
                                            uint8_t offset_half_hours)
{
    return static_cast<uint16_t>(
        ((hour & 0x0FU) << 12U) |
        ((minute & 0x3FU) << 6U) |
        (negative_offset ? 0x0020U : 0U) |
        (offset_half_hours & 0x1FU));
}

constexpr ClockTime kUnixEpochClockTime = decode_clock_time(
    make_clock_time_block_b(kUnixEpochMjd),
    make_clock_time_block_c(kUnixEpochMjd, 0U),
    make_clock_time_block_d(0U, 0U, false, 0U));
static_assert(kUnixEpochClockTime.valid);
static_assert(kUnixEpochClockTime.utc_seconds == 0U);

constexpr ClockTime kBoundaryClockTime = decode_clock_time(
    make_clock_time_block_b(65535U),
    make_clock_time_block_c(65535U, 23U),
    make_clock_time_block_d(23U, 59U, true, 1U));
static_assert(kBoundaryClockTime.valid);
static_assert(kBoundaryClockTime.mjd == 65535U);
static_assert(kBoundaryClockTime.hour == 23U);
static_assert(kBoundaryClockTime.minute == 59U);
static_assert(kBoundaryClockTime.local_offset_minutes == -30);

static_assert(!decode_clock_time(
                   make_clock_time_block_b(0U),
                   make_clock_time_block_c(0U, 0U),
                   make_clock_time_block_d(0U, 0U, false, 0U)).valid);
static_assert(!decode_clock_time(
                   make_clock_time_block_b(kUnixEpochMjd),
                   make_clock_time_block_c(kUnixEpochMjd, 24U),
                   make_clock_time_block_d(24U, 0U, false, 0U)).valid);
static_assert(!decode_clock_time(
                   make_clock_time_block_b(kUnixEpochMjd),
                   make_clock_time_block_c(kUnixEpochMjd, 0U),
                   make_clock_time_block_d(0U, 60U, false, 0U)).valid);
static_assert(!decode_clock_time(
                   make_clock_time_block_b(kUnixEpochMjd),
                   make_clock_time_block_c(kUnixEpochMjd, 0U),
                   make_clock_time_block_d(0U, 0U, false, 25U)).valid);
static_assert(!decode_clock_time(
                   make_clock_time_block_b(kMaximumMjd),
                   make_clock_time_block_c(kMaximumMjd, 0U),
                   make_clock_time_block_d(0U, 0U, false, 0U)).valid);
static_assert(!decode_clock_time(
                   static_cast<uint16_t>(
                       make_clock_time_block_b(kUnixEpochMjd) | 0x0800U),
                   make_clock_time_block_c(kUnixEpochMjd, 0U),
                   make_clock_time_block_d(0U, 0U, false, 0U)).valid);

static std::array<Group, kGroupQueueCapacity> s_group_queue{};
static std::atomic<uint32_t> s_group_head{0U};
static std::atomic<uint32_t> s_group_tail{0U};
static std::atomic<uint32_t> s_group_dropped{0U};

static void process_clock_time(const Group &group)
{
    uint16_t const block_b = group.words[1U];
    if((block_b & 0xF800U) != 0x4000U)
    {
        return;
    }

    ClockTime const clock_time =
        decode_clock_time(block_b, group.words[2U], group.words[3U]);
    if(!clock_time.valid)
    {
        std::printf("RDS: CT invalid B=%04X C=%04X D=%04X\r\n",
                    static_cast<unsigned int>(block_b),
                    static_cast<unsigned int>(group.words[2U]),
                    static_cast<unsigned int>(group.words[3U]));
        return;
    }

    rtc_set_utc(clock_time.utc_seconds, clock_time.local_offset_minutes);

    int const offset = static_cast<int>(clock_time.local_offset_minutes);
    unsigned int const offset_magnitude =
        static_cast<unsigned int>((offset < 0) ? -offset : offset);
    std::printf("RDS: CT MJD=%lu UTC=%02u:%02u LTO=%c%02u:%02u RTC=%lu\r\n",
                static_cast<unsigned long>(clock_time.mjd),
                static_cast<unsigned int>(clock_time.hour),
                static_cast<unsigned int>(clock_time.minute),
                (offset < 0) ? '-' : '+',
                offset_magnitude / 60U,
                offset_magnitude % 60U,
                static_cast<unsigned long>(clock_time.utc_seconds));
}

static void enqueue_group(const Group &group)
{
    uint32_t const head = s_group_head.load(std::memory_order_relaxed);
    uint32_t const tail = s_group_tail.load(std::memory_order_acquire);
    if((head - tail) >= kGroupQueueCapacity)
    {
        s_group_dropped.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    s_group_queue[head & kGroupQueueMask] = group;
    s_group_head.store(head + 1U, std::memory_order_release);
}

struct DecodeResult
{
    uint16_t information = 0U;
    bool valid = false;
    bool corrected = false;
};

static constexpr DecodeResult decode_with_syndrome(uint32_t raw_block,
                                                   uint16_t raw_syndrome,
                                                   uint16_t offset)
{
    uint32_t const deoffset_block = raw_block ^ offset;
    uint16_t const syndrome = static_cast<uint16_t>(raw_syndrome ^ offset);
    if(syndrome == 0U)
    {
        return {
            static_cast<uint16_t>(deoffset_block >> 10U),
            true,
            false,
        };
    }

    size_t first = 0U;
    size_t last = kBurstTable.syndromes.size();
    while(first < last)
    {
        size_t const middle = first + ((last - first) / 2U);
        if(kBurstTable.syndromes[middle] < syndrome)
        {
            first = middle + 1U;
        }
        else
        {
            last = middle;
        }
    }

    if((first < kBurstTable.syndromes.size()) &&
       (kBurstTable.syndromes[first] == syndrome))
    {
        uint32_t const corrected_block = deoffset_block ^ kBurstTable.masks[first];
        return {
            static_cast<uint16_t>(corrected_block >> 10U),
            true,
            true,
        };
    }

    return {};
}

static constexpr DecodeResult decode_with_offset(uint32_t raw_block,
                                                 uint16_t offset)
{
    return decode_with_syndrome(raw_block,
                                polynomial_remainder(raw_block),
                                offset);
}

constexpr uint32_t encode_block(uint16_t information, uint16_t offset)
{
    uint32_t const information_bits = static_cast<uint32_t>(information) << 10U;
    uint16_t const checkword = static_cast<uint16_t>(
        polynomial_remainder(information_bits) ^ offset);
    return information_bits | checkword;
}

consteval bool decoder_corrects_every_short_burst()
{
    constexpr uint16_t kTestInformation = 0xA53CU;
    constexpr uint32_t kTestBlock = encode_block(kTestInformation, kOffsetA);

    DecodeResult const clean = decode_with_offset(kTestBlock, kOffsetA);
    if(!clean.valid || clean.corrected || (clean.information != kTestInformation))
    {
        return false;
    }

    for(uint32_t burst : kBurstTable.masks)
    {
        DecodeResult const corrected = decode_with_offset(kTestBlock ^ burst, kOffsetA);
        if(!corrected.valid || !corrected.corrected ||
           (corrected.information != kTestInformation))
        {
            return false;
        }
    }
    return true;
}

static_assert(decoder_corrects_every_short_burst());

class LinkDecoder
{
    uint32_t raw_block = 0U;
    uint16_t raw_syndrome = 0U;
    uint8_t bits_in_block = 0U;
    uint8_t expected_position = 0U;
    Group group{};
    std::array<bool, 4U> block_valid{};
    bool group_active = false;
    uint64_t invalid_history = 0U;
    uint8_t history_count = 0U;
    uint8_t consecutive_invalid = 0U;

    static constexpr uint64_t kInvalidHistoryMask = (1ULL << 45U) - 1ULL;

    DecodeResult decode_current_block(uint8_t position) const
    {
        switch(position)
        {
            case 0U:
                return decode_with_syndrome(raw_block, raw_syndrome, kOffsetA);
            case 1U:
                return decode_with_syndrome(raw_block, raw_syndrome, kOffsetB);
            case 2U:
                if(group_active && block_valid[1U])
                {
                    uint16_t const expected_offset =
                        ((group.words[1U] & 0x0800U) != 0U) ? kOffsetCPrime : kOffsetC;
                    return decode_with_syndrome(
                        raw_block, raw_syndrome, expected_offset);
                }
                else
                {
                    DecodeResult const c =
                        decode_with_syndrome(raw_block, raw_syndrome, kOffsetC);
                    DecodeResult const c_prime =
                        decode_with_syndrome(raw_block, raw_syndrome, kOffsetCPrime);
                    if(c.valid == c_prime.valid)
                    {
                        return {};
                    }
                    return c.valid ? c : c_prime;
                }
            case 3U:
                return decode_with_syndrome(raw_block, raw_syndrome, kOffsetD);
            default:
                return {};
        }
    }

    bool record_validity(bool valid)
    {
        invalid_history = ((invalid_history << 1U) |
                           static_cast<uint64_t>(!valid)) & kInvalidHistoryMask;
        if(history_count < 45U)
        {
            ++history_count;
        }

        if(valid)
        {
            consecutive_invalid = 0U;
        }
        else if(consecutive_invalid < std::numeric_limits<uint8_t>::max())
        {
            ++consecutive_invalid;
        }

        return (consecutive_invalid < 8U) &&
               ((history_count < 45U) || (std::popcount(invalid_history) < 43));
    }

    void record_block(uint8_t position, DecodeResult result)
    {
        if(position == 0U)
        {
            group = {};
            block_valid = {};
            group_active = true;
        }

        if(group_active)
        {
            block_valid[position] = result.valid;
            if(result.valid)
            {
                group.words[position] = result.information;
                if(result.corrected)
                {
                    group.corrected_mask =
                        static_cast<uint8_t>(group.corrected_mask | (1U << position));
                }
            }
        }

        if(position == 3U)
        {
            if(group_active && block_valid[0U] && block_valid[1U] &&
               block_valid[2U] && block_valid[3U])
            {
                enqueue_group(group);
            }
            group_active = false;
        }
    }

public:
    void reset()
    {
        raw_block = 0U;
        raw_syndrome = 0U;
        bits_in_block = 0U;
        expected_position = 0U;
        group = {};
        block_valid = {};
        group_active = false;
        invalid_history = 0U;
        history_count = 0U;
        consecutive_invalid = 0U;
    }

    void acquire(OffsetKind offset, uint32_t block)
    {
        reset();
        uint8_t const position = offset_position(offset);
        DecodeResult const result{
            static_cast<uint16_t>(block >> 10U),
            true,
            false,
        };
        record_block(position, result);
        (void)record_validity(true);
        expected_position = static_cast<uint8_t>((position + 1U) & 3U);
    }

    bool push(bool bit)
    {
        raw_block = ((raw_block << 1U) | static_cast<uint32_t>(bit)) & kBlockMask;
        raw_syndrome = polynomial_push_bit(raw_syndrome, bit);
        ++bits_in_block;
        if(bits_in_block < 26U)
        {
            return true;
        }

        bits_in_block = 0U;
        DecodeResult const result = decode_current_block(expected_position);
        raw_syndrome = 0U;
        record_block(expected_position, result);
        bool const synchronized = record_validity(result.valid);
        expected_position = static_cast<uint8_t>((expected_position + 1U) & 3U);
        return synchronized;
    }
};

static std::array<AcquisitionChannel, kTimingHypothesisCount> s_acquisition_channels{};
static DifferentialSampler s_selected_sampler;
static LinkDecoder s_link_decoder;
static int s_selected_timing = -1;

static ComplexDecimator s_decimator;
static CICComplexFilter<int32_t, kBasebandCicTapCount, int32_t> s_baseband_cic;
static size_t s_mixer_phase = 0U;

static void restart_acquisition()
{
    for(size_t i = 0U; i < s_acquisition_channels.size(); ++i)
    {
        s_acquisition_channels[i].reset(
            static_cast<uint16_t>(i * kTimingHypothesisSpacing));
    }
    s_selected_sampler.reset(0U);
    s_link_decoder.reset();
    s_selected_timing = -1;
}

static void process_selected_timing(ComplexSample sample)
{
    bool bit = false;
    if(s_selected_sampler.push(sample, bit) && !s_link_decoder.push(bit))
    {
        restart_acquisition();
    }
}

static void process_acquisition(ComplexSample sample)
{
    for(size_t i = 0U; i < s_acquisition_channels.size(); ++i)
    {
        OffsetKind offset = OffsetKind::Invalid;
        uint32_t block = 0U;
        if(!s_acquisition_channels[i].push(sample, offset, block))
        {
            continue;
        }

        s_selected_sampler = s_acquisition_channels[i].sampler;
        s_selected_timing = static_cast<int>(i);
        s_link_decoder.acquire(offset, block);
        break;
    }
}

static void process_baseband_sample(ComplexSample sample)
{
    auto const [filtered_i, filtered_q] = s_baseband_cic.push(sample.i, sample.q);
    sample = {filtered_i, filtered_q};
    if(s_selected_timing < 0)
    {
        process_acquisition(sample);
    }
    else
    {
        process_selected_timing(sample);
    }
}
}

namespace demod
{
void rds_push_sample(int32_t fm_q31)
{
    OscillatorSample const oscillator = kMixerTable[s_mixer_phase];
    s_mixer_phase = (s_mixer_phase + 19U) & kMixerTableMask;

    int32_t const mixer_input = fm_q31 >> kMixerInputShift;
    ComplexSample mixed{
        mixer_input * oscillator.cos_q15,
        mixer_input * oscillator.sin_q15,
    };

    ComplexSample decimated{};
    if(s_decimator.push(mixed, decimated))
    {
        process_baseband_sample(decimated);
    }
}

void rds_reset()
{
    s_decimator.reset();
    s_baseband_cic.reset();
    s_mixer_phase = 0U;
    restart_acquisition();

    s_group_head.store(0U, std::memory_order_relaxed);
    s_group_tail.store(0U, std::memory_order_relaxed);
    s_group_dropped.store(0U, std::memory_order_relaxed);
}

void rds_poll()
{
    uint32_t const dropped = s_group_dropped.exchange(0U, std::memory_order_relaxed);
    if(dropped != 0U)
    {
        std::printf("RDS: dropped %lu group(s)\r\n",
                    static_cast<unsigned long>(dropped));
    }

    uint32_t tail = s_group_tail.load(std::memory_order_relaxed);
    uint32_t const head = s_group_head.load(std::memory_order_acquire);
    while(tail != head)
    {
        Group const group = s_group_queue[tail & kGroupQueueMask];
        uint16_t const block_b = group.words[1U];
        unsigned int const type = static_cast<unsigned int>((block_b >> 12U) & 0x0FU);
        char const version = ((block_b & 0x0800U) != 0U) ? 'B' : 'A';

        process_clock_time(group);
        std::printf("RDS: PI=%04X TYPE=%u%c A=%04X B=%04X C=%04X D=%04X CORR=%X\r\n",
                    static_cast<unsigned int>(group.words[0U]),
                    type,
                    version,
                    static_cast<unsigned int>(group.words[0U]),
                    static_cast<unsigned int>(group.words[1U]),
                    static_cast<unsigned int>(group.words[2U]),
                    static_cast<unsigned int>(group.words[3U]),
                    static_cast<unsigned int>(group.corrected_mask));
        ++tail;
        s_group_tail.store(tail, std::memory_order_release);
    }
}
}
