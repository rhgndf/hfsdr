#include "iq_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <utility>

extern "C" {
#include "ch32v30x.h"
#include "hw/display/st7789.h"
#include "hw/i2s.h"
#include "hw/si5351.h"
#include "hw/tlv320adc6120.h"
}

#include "utils/complex_dsp.h"

constexpr uint32_t kCalibrationSignalHz = 72000000U;
constexpr uint32_t kI2sSampleRateHz = 192000U;
constexpr size_t kMeasureComplexSamples = 240U;
constexpr uint8_t kBlocksPerCandidate = 4U;
constexpr uint8_t kDiscardBlocksAfterRegisterWrite = 1U;
constexpr uint8_t kPhaseNoBestStopCycles = 5U;
constexpr int16_t kPhaseMinCycles = -64;
constexpr int16_t kPhaseMaxCycles = 64;
constexpr int8_t kGainMinDbX10 = TLV320ADC6120_CH_GAIN_CAL_MIN_DB_X10;
constexpr int8_t kGainMaxDbX10 = TLV320ADC6120_CH_GAIN_CAL_MAX_DB_X10;

struct TlvCalibration
{
    int8_t ch1_gain_db_x10 = 0;
    int8_t ch2_gain_db_x10 = 0;
    uint8_t ch1_phase_cycles = 0;
    uint8_t ch2_phase_cycles = 0;
    int16_t relative_phase_cycles = 0;
    int8_t relative_gain_db_x10 = 0;
};

enum class CalibrationState : uint8_t
{
    Idle,
    ApplyBaseline,
    WaitBaseline,
    ApplyPhase,
    WaitPhase,
    ApplyGain,
    WaitGain,
    ApplyBest,
    DonePending,
    Done,
    FailedPending,
    Failed
};

enum class DisplayStatus : uint8_t
{
    Idle,
    Running,
    Done,
    Failed
};

static CalibrationState s_state = CalibrationState::Idle;
static DisplayStatus s_display_status = DisplayStatus::Idle;
static TlvCalibration s_current_cal;
static TlvCalibration s_best_cal;
static float s_target_if_hz = -20000.0f;
static float s_baseline_irr_db = complex_dsp::kDbFloor;
static float s_current_irr_db = complex_dsp::kDbFloor;
static float s_best_irr_db = complex_dsp::kDbFloor;
static float s_wanted_power_acc = 0.0f;
static float s_image_power_acc = 0.0f;
static uint8_t s_candidate_blocks = 0U;
static uint8_t s_discard_blocks = 0U;
static int16_t s_phase_cursor = kPhaseMinCycles;
static uint8_t s_phase_cycles_without_best = 0U;
static bool s_phase_found_best = false;
static int8_t s_gain_cursor = kGainMinDbX10;
static uint32_t s_display_version = 0U;
static uint32_t s_drawn_display_version = UINT32_MAX;
static bool s_calibration_signal_started = false;

static void mark_display_dirty(void)
{
    ++s_display_version;
}

static TlvCalibration make_zero_calibration(void)
{
    return {};
}

static TlvCalibration make_phase_calibration(int16_t phase_cycles)
{
    TlvCalibration cal{};
    cal.relative_phase_cycles = phase_cycles;
    if(phase_cycles < 0)
    {
        cal.ch1_phase_cycles = static_cast<uint8_t>(-phase_cycles);
    }
    else
    {
        cal.ch2_phase_cycles = static_cast<uint8_t>(phase_cycles);
    }
    return cal;
}

static TlvCalibration make_gain_calibration(int16_t phase_cycles, int8_t gain_db_x10)
{
    TlvCalibration cal = make_phase_calibration(phase_cycles);
    cal.relative_gain_db_x10 = gain_db_x10;
    cal.ch2_gain_db_x10 = gain_db_x10;
    return cal;
}

[[maybe_unused]] static ErrorStatus calibration_signal_init(void)
{
    RCC_ClocksTypeDef clocks = {0};

    RCC_GetClocksFreq(&clocks);

    uint32_t tim_clk_hz;
    if((RCC->CFGR0 & RCC_PPRE2) == RCC_PPRE2_DIV1)
    {
        tim_clk_hz = clocks.PCLK2_Frequency;
    }
    else
    {
        tim_clk_hz = clocks.PCLK2_Frequency * 2U;
    }

    if((tim_clk_hz % kCalibrationSignalHz) != 0U)
    {
        return NoREADY;
    }

    uint32_t period_ticks = tim_clk_hz / kCalibrationSignalHz;
    if(period_ticks < 2U)
    {
        return NoREADY;
    }

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO | RCC_APB2Periph_TIM8, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    TIM_DeInit(TIM8);
    TIM_TimeBaseInitTypeDef tim = {0};
    tim.TIM_Prescaler = 0U;
    tim.TIM_CounterMode = TIM_CounterMode_Up;
    tim.TIM_Period = period_ticks - 1U;
    tim.TIM_ClockDivision = TIM_CKD_DIV1;
    tim.TIM_RepetitionCounter = 0U;
    TIM_TimeBaseInit(TIM8, &tim);

    TIM_OCInitTypeDef oc = {0};
    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse = period_ticks / 2U;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(TIM8, &oc);
    TIM_OC1PreloadConfig(TIM8, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM8, ENABLE);
    TIM_CtrlPWMOutputs(TIM8, ENABLE);
    TIM_Cmd(TIM8, ENABLE);

    s_calibration_signal_started = true;
    return READY;
}

[[maybe_unused]] static void calibration_signal_deinit(void)
{
    TIM_Cmd(TIM8, DISABLE);
    TIM_CtrlPWMOutputs(TIM8, DISABLE);
    TIM_OC1PreloadConfig(TIM8, TIM_OCPreload_Disable);
    TIM_DeInit(TIM8);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &gpio);

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM8, DISABLE);
    s_calibration_signal_started = false;
}

static ErrorStatus apply_tlv_calibration(TlvCalibration cal)
{
    return tlv320adc6120_hw_set_ch_calibration(cal.ch1_gain_db_x10,
                                               cal.ch1_phase_cycles,
                                               cal.ch2_gain_db_x10,
                                               cal.ch2_phase_cycles);
}

static void start_candidate(TlvCalibration cal)
{
    s_current_cal = cal;
    s_wanted_power_acc = 0.0f;
    s_image_power_acc = 0.0f;
    s_candidate_blocks = 0U;
    s_discard_blocks = kDiscardBlocksAfterRegisterWrite;
    i2s_fft_sample_arr_reset();
    mark_display_dirty();
}

static bool finish_with_failure(char const *reason)
{
    printf("IQ calibration failed: %s\r\n", reason);
    (void)apply_tlv_calibration(make_zero_calibration());
    if(s_calibration_signal_started)
    {
        calibration_signal_deinit();
    }
    s_display_status = DisplayStatus::Failed;
    s_state = CalibrationState::FailedPending;
    mark_display_dirty();
    return true;
}

static int32_t db_to_db_x10(float db)
{
    if(!std::isfinite(db))
    {
        return 0;
    }
    float rounded = db * 10.0f;
    rounded += (rounded >= 0.0f) ? 0.5f : -0.5f;
    return static_cast<int32_t>(rounded);
}

static void format_db(char *buf, size_t len, float db)
{
    if(!std::isfinite(db))
    {
        snprintf(buf, len, " --.-");
        return;
    }

    int32_t db_x10 = db_to_db_x10(db);
    char sign = (db_x10 < 0) ? '-' : ' ';
    uint32_t mag = static_cast<uint32_t>((db_x10 < 0) ? -db_x10 : db_x10);
    snprintf(buf, len, "%c%lu.%lu",
             sign,
             static_cast<unsigned long>(mag / 10U),
             static_cast<unsigned long>(mag % 10U));
}

static void print_db_value(float db)
{
    int32_t db_x10 = db_to_db_x10(db);
    char sign = (db_x10 < 0) ? '-' : '+';
    uint32_t mag = static_cast<uint32_t>((db_x10 < 0) ? -db_x10 : db_x10);
    printf("%c%lu.%lu",
           sign,
           static_cast<unsigned long>(mag / 10U),
           static_cast<unsigned long>(mag % 10U));
}

std::optional<std::pair<float, float>> iq_calibration_measure_ready_block(void)
{
    if(!i2s_fft_sample_arr_ready())
    {
        return std::nullopt;
    }

    if(s_discard_blocks > 0U)
    {
        --s_discard_blocks;
        i2s_fft_sample_arr_reset();
        return std::nullopt;
    }

    auto wanted = complex_dsp::measure_interleaved_iq_tone(i2s_fft_sample_arr,
                                                           kMeasureComplexSamples,
                                                           static_cast<float>(kI2sSampleRateHz),
                                                           s_target_if_hz);
    auto image = complex_dsp::measure_interleaved_iq_tone(i2s_fft_sample_arr,
                                                          kMeasureComplexSamples,
                                                          static_cast<float>(kI2sSampleRateHz),
                                                          -s_target_if_hz);

    i2s_fft_sample_arr_reset();
    return std::make_pair(wanted.power, image.power);
}

static bool update_candidate_measurement(bool *candidate_done)
{
    *candidate_done = false;
    auto measurement = iq_calibration_measure_ready_block();
    if(!measurement)
    {
        return true;
    }

    s_wanted_power_acc += measurement->first;
    s_image_power_acc += measurement->second;
    ++s_candidate_blocks;

    float avg_wanted_power = s_wanted_power_acc / static_cast<float>(s_candidate_blocks);
    float avg_image_power = s_image_power_acc / static_cast<float>(s_candidate_blocks);
    s_current_irr_db = complex_dsp::ratio_to_db(avg_wanted_power, avg_image_power);
    mark_display_dirty();

    if(s_candidate_blocks >= kBlocksPerCandidate)
    {
        *candidate_done = true;
    }

    return true;
}

static bool commit_candidate_result(void)
{
    if(s_current_irr_db > s_best_irr_db)
    {
        s_best_irr_db = s_current_irr_db;
        s_best_cal = s_current_cal;
        mark_display_dirty();
        return true;
    }

    return false;
}

static bool apply_current_candidate(TlvCalibration cal, CalibrationState wait_state)
{
    if(apply_tlv_calibration(cal) != READY)
    {
        return finish_with_failure("TLV register write");
    }

    start_candidate(cal);
    s_state = wait_state;
    return true;
}

static bool calibration_start(void)
{
    s_display_status = DisplayStatus::Running;
    s_current_cal = make_zero_calibration();
    s_best_cal = make_zero_calibration();
    s_baseline_irr_db = complex_dsp::kDbFloor;
    s_current_irr_db = complex_dsp::kDbFloor;
    s_best_irr_db = complex_dsp::kDbFloor;
    s_phase_cursor = kPhaseMinCycles;
    s_phase_cycles_without_best = 0U;
    s_phase_found_best = false;
    s_gain_cursor = kGainMinDbX10;

    int64_t target_if_hz = (int64_t)(2U * kCalibrationSignalHz) - (int64_t)si5351_hw_clk0_get_freq_hz();
    if(target_if_hz == 0)
    {
        return finish_with_failure("zero IF");
    }
    s_target_if_hz = static_cast<float>(target_if_hz);

    if(calibration_signal_init() != READY)
    {
        return finish_with_failure("signal init");
    }

    printf("IQ calibration: IF %ld Hz, measuring wanted/image\r\n", static_cast<long>(target_if_hz));
    s_state = CalibrationState::ApplyBaseline;
    mark_display_dirty();
    return true;
}

static bool calibration_apply_best(void)
{
    if(apply_tlv_calibration(s_best_cal) != READY)
    {
        return finish_with_failure("final TLV write");
    }

    if(s_calibration_signal_started)
    {
        calibration_signal_deinit();
    }

    printf("IQ calibration done: baseline ");
    print_db_value(s_baseline_irr_db);
    printf(" dB, best ");
    print_db_value(s_best_irr_db);
    printf(" dB, CH1_GCAL %d CH1_PCAL %u CH2_GCAL %d CH2_PCAL %u\r\n",
           static_cast<int>(s_best_cal.ch1_gain_db_x10),
           static_cast<unsigned int>(s_best_cal.ch1_phase_cycles),
           static_cast<int>(s_best_cal.ch2_gain_db_x10),
           static_cast<unsigned int>(s_best_cal.ch2_phase_cycles));

    s_current_cal = s_best_cal;
    s_current_irr_db = s_best_irr_db;
    s_display_status = DisplayStatus::Done;
    s_state = CalibrationState::DonePending;
    mark_display_dirty();
    return true;
}

static char const *status_text(void)
{
    switch(s_display_status)
    {
        case DisplayStatus::Done:
            return "DONE";
        case DisplayStatus::Failed:
            return "FAILED";
        case DisplayStatus::Running:
            switch(s_state)
            {
                case CalibrationState::ApplyBaseline:
                case CalibrationState::WaitBaseline:
                    return "RUN baseline";
                case CalibrationState::ApplyPhase:
                case CalibrationState::WaitPhase:
                    return "RUN phase";
                case CalibrationState::ApplyGain:
                case CalibrationState::WaitGain:
                    return "RUN gain";
                case CalibrationState::ApplyBest:
                    return "RUN final";
                default:
                    return "RUN";
            }
        default:
            return "IDLE";
    }
}

bool iq_calibration_run(void)
{
    bool candidate_done;

    switch(s_state)
    {
        case CalibrationState::Idle:
            return calibration_start();

        case CalibrationState::ApplyBaseline:
            return apply_current_candidate(make_zero_calibration(), CalibrationState::WaitBaseline);

        case CalibrationState::WaitBaseline:
            if(!update_candidate_measurement(&candidate_done) || !candidate_done)
            {
                return true;
            }
            s_baseline_irr_db = s_current_irr_db;
            s_best_cal = s_current_cal;
            printf("IQ calibration baseline IRR ");
            print_db_value(s_baseline_irr_db);
            printf(" dB\r\n");
            s_state = CalibrationState::ApplyPhase;
            mark_display_dirty();
            return true;

        case CalibrationState::ApplyPhase:
            if(s_phase_cursor > kPhaseMaxCycles)
            {
                s_gain_cursor = kGainMinDbX10;
                s_state = CalibrationState::ApplyGain;
                return true;
            }
            return apply_current_candidate(make_phase_calibration(s_phase_cursor), CalibrationState::WaitPhase);

        case CalibrationState::WaitPhase:
            if(!update_candidate_measurement(&candidate_done) || !candidate_done)
            {
                return true;
            }
            if(commit_candidate_result())
            {
                s_phase_found_best = true;
                s_phase_cycles_without_best = 0U;
            }
            else if(s_phase_found_best)
            {
                ++s_phase_cycles_without_best;
                if(s_phase_cycles_without_best >= kPhaseNoBestStopCycles)
                {
                    s_gain_cursor = kGainMinDbX10;
                    s_state = CalibrationState::ApplyGain;
                    return true;
                }
            }
            ++s_phase_cursor;
            s_state = CalibrationState::ApplyPhase;
            return true;

        case CalibrationState::ApplyGain:
            if(s_gain_cursor > kGainMaxDbX10)
            {
                s_state = CalibrationState::ApplyBest;
                return true;
            }
            return apply_current_candidate(make_gain_calibration(s_best_cal.relative_phase_cycles, s_gain_cursor),
                                           CalibrationState::WaitGain);

        case CalibrationState::WaitGain:
            if(!update_candidate_measurement(&candidate_done) || !candidate_done)
            {
                return true;
            }
            (void)commit_candidate_result();
            ++s_gain_cursor;
            s_state = CalibrationState::ApplyGain;
            return true;

        case CalibrationState::ApplyBest:
            return calibration_apply_best();

        case CalibrationState::DonePending:
            s_state = CalibrationState::Done;
            return false;

        case CalibrationState::FailedPending:
            s_state = CalibrationState::Failed;
            return false;

        case CalibrationState::Done:
        case CalibrationState::Failed:
            return false;
    }

    return false;
}

void iq_calibration_display(void)
{
    if(s_drawn_display_version == s_display_version)
    {
        return;
    }
    s_drawn_display_version = s_display_version;

    char base_db[12];
    char current_db[12];
    char best_db[12];
    char line[32];

    format_db(base_db, sizeof(base_db), s_baseline_irr_db);
    format_db(current_db, sizeof(current_db), s_current_irr_db);
    format_db(best_db, sizeof(best_db), s_best_irr_db);

    ST7789_WriteString(0U, 5U, "IQ Calibration", Font_11x18, WHITE, BLACK);

    snprintf(line, sizeof(line), "Base %5s dB", base_db);
    ST7789_WriteString(0U, 27U, line, Font_11x18, WHITE, BLACK);

    snprintf(line, sizeof(line), "Curr %5s dB", current_db);
    ST7789_WriteString(0U, 49U, line, Font_11x18, WHITE, BLACK);

    snprintf(line, sizeof(line), "Best %5s dB", best_db);
    ST7789_WriteString(0U, 71U, line, Font_11x18, WHITE, BLACK);

    snprintf(line, sizeof(line), "Cur G %d,%d P %2u,%2u",
             static_cast<int>(s_current_cal.ch1_gain_db_x10),
             static_cast<int>(s_current_cal.ch2_gain_db_x10),
             static_cast<unsigned int>(s_current_cal.ch1_phase_cycles),
             static_cast<unsigned int>(s_current_cal.ch2_phase_cycles));
    ST7789_WriteString(0U, 93U, line, Font_11x18, WHITE, BLACK);

    snprintf(line, sizeof(line), "BestG %d,%d P %2u,%2u",
             static_cast<int>(s_best_cal.ch1_gain_db_x10),
             static_cast<int>(s_best_cal.ch2_gain_db_x10),
             static_cast<unsigned int>(s_best_cal.ch1_phase_cycles),
             static_cast<unsigned int>(s_best_cal.ch2_phase_cycles));
    ST7789_WriteString(0U, 115U, line, Font_11x18, WHITE, BLACK);

    snprintf(line, sizeof(line), "%-10s", status_text());
    ST7789_WriteString(0U, 137U, line, Font_11x18,
                       (s_display_status == DisplayStatus::Failed) ? RED : CYAN,
                       BLACK);
}
