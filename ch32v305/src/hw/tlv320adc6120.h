#ifndef TLV320ADC6120_HW_H
#define TLV320ADC6120_HW_H

/*
 * Texas Instruments TLV320ADC6120 — I2C audio ADC (see SBASA92A).
 * - Fixed 7-bit I2C address 0b1001110 (0x4E).
 * - tlv320adc6120_hw_init() targets ASI controller + I2S + 24-bit to match CH32 SPI2
 *   I2S slave RX in i2s.c. The codec expects an external 24 MHz MCLK on GPIO1 and
 *   generates BCLK/FSYNC for 192 kHz stereo capture.
 *
 * AVDD / AREG: set TLV320ADC6120_USE_INTERNAL_AREG before including this header
 * (or in the build) if you use 3.3 V AVDD with the on-chip 1.8 V AREG regulator.
 * Use 0 for 1.8 V AVDD with AREG shorted to AVDD (external AREG path).
 */

#include <stdint.h>

#include "debug.h"

#ifndef TLV320ADC6120_USE_INTERNAL_AREG
#define TLV320ADC6120_USE_INTERNAL_AREG 1
#endif

#define TLV320ADC6120_I2C_ADDR_7BIT 0x4EU
#define TLV320ADC6120_CH_GAIN_MIN_DB_X2 (-22)
#define TLV320ADC6120_CH_GAIN_MAX_DB_X2 84U
#define TLV320ADC6120_CH_GAIN_CAL_MIN_DB_X10 (-8)
#define TLV320ADC6120_CH_GAIN_CAL_MAX_DB_X10 7

[[nodiscard]] ErrorStatus tlv320adc6120_hw_init(void);
[[nodiscard]] ErrorStatus tlv320adc6120_hw_set_ch_gain_raw(uint8_t gain_raw);
[[nodiscard]] ErrorStatus tlv320adc6120_hw_set_ch_gain_db_x2(int8_t gain_db_x2);
[[nodiscard]] ErrorStatus tlv320adc6120_hw_set_ch_calibration(int8_t ch1_gain_cal_db_x10,
                                                              uint8_t ch1_phase_cal_cycles,
                                                              int8_t ch2_gain_cal_db_x10,
                                                              uint8_t ch2_phase_cal_cycles);

#endif
