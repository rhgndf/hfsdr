#include "ui/hw_state.h"

#include "freertos/task_stacks.h"

#include <algorithm>

extern "C" {
#include "FreeRTOS.h"
#include "task.h"

#include "hw/si5351.h"
#include "hw/tlv320adc6120.h"
}

namespace
{

constexpr uint32_t FrequencyPending = 1U << 0;
constexpr uint32_t GainPending = 1U << 1;

TaskHandle_t s_task_handle;
StaticTask_t s_task_tcb;
StackType_t s_task_stack[HW_STATE_TASK_STACK_WORDS]
    __attribute__((aligned(portBYTE_ALIGNMENT)));
uint32_t s_frequency;
uint32_t s_requested_frequency;
ErrorStatus s_frequency_status = NoREADY;
int8_t s_gain;

int8_t clamp_gain(int8_t gain_db_x2)
{
    return std::clamp(gain_db_x2,
                      static_cast<int8_t>(TLV320ADC6120_CH_GAIN_MIN_DB_X2),
                      static_cast<int8_t>(TLV320ADC6120_CH_GAIN_MAX_DB_X2));
}

void notify_worker(TaskHandle_t task, uint32_t bits)
{
    if(task != nullptr)
    {
        (void)xTaskNotify(task, bits, eSetBits);
    }
}

} // namespace

extern "C" void hw_state_set_frequency(uint32_t frequency_hz)
{
    s_requested_frequency = frequency_hz;
    s_frequency_status = NoREADY;
    notify_worker(s_task_handle, FrequencyPending);
}

extern "C" void hw_state_set_gain_x2(int8_t gain_db_x2)
{
    gain_db_x2 = clamp_gain(gain_db_x2);

    s_gain = gain_db_x2;
    notify_worker(s_task_handle, GainPending);
}

extern "C" uint32_t hw_state_get_frequency(void)
{
    return s_frequency;
}

extern "C" uint32_t hw_state_get_requested_frequency(void)
{
    return s_requested_frequency;
}

extern "C" ErrorStatus hw_state_get_frequency_status(void)
{
    return s_frequency_status;
}

extern "C" int8_t hw_state_get_gain_x2(void)
{
    return s_gain;
}

extern "C" void hw_state_publish_boot_state(uint32_t requested_frequency_hz,
                                             uint32_t actual_frequency_hz,
                                             ErrorStatus frequency_status,
                                             int8_t actual_gain_db_x2)
{
    actual_gain_db_x2 = clamp_gain(actual_gain_db_x2);

    s_frequency = actual_frequency_hz;
    s_requested_frequency = requested_frequency_hz;
    s_frequency_status = frequency_status;

    s_gain = actual_gain_db_x2;
}

void hw_state_task(void *parameters)
{
    (void)parameters;

    for(;;)
    {
        uint32_t notification = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &notification, portMAX_DELAY);
        if((notification & FrequencyPending) != 0U)
        {
            uint32_t frequency_hz;
            frequency_hz = s_requested_frequency;

            uint64_t t1 = xTaskGetTickCount();
            uint32_t actual_hz = si5351_hw_clk0_set_freq_hz(frequency_hz);
            if(actual_hz != 0U)
            {
                s_frequency = actual_hz;
            }
            s_frequency_status = actual_hz != 0U ? READY : NoREADY;
        }

        if((notification & GainPending) != 0U)
        {
            int8_t gain_db_x2;
            gain_db_x2 = s_gain;

            (void)tlv320adc6120_hw_set_ch_gain_db_x2(gain_db_x2);
        }
    }
}

extern "C" void hw_state_init(void)
{
    configASSERT(s_task_handle == nullptr);
    s_task_handle =
        xTaskCreateStatic(hw_state_task,
                          "hw_state",
                          HW_STATE_TASK_STACK_WORDS,
                          nullptr,
                          2U,
                          s_task_stack,
                          &s_task_tcb);
    configASSERT(s_task_handle != nullptr);
}
