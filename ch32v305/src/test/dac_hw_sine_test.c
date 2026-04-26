#include "test/dac_hw_sine_test.h"

#include "hw/dac.h"

void dac_hw_sine_test_stop(void)
{
    dac_hw_stream_stop();
}

void dac_hw_static_noise_stop(void)
{
    dac_hw_stream_stop();
}

void dac_hw_static_noise_start(uint32_t sample_rate_hz)
{
    dac_hw_stream_noise_start(sample_rate_hz);
}

void dac_hw_sine_test_start(uint32_t sine_freq_hz, uint32_t sample_rate_hz)
{
    dac_hw_stream_sine_start(sine_freq_hz, sample_rate_hz);
}
