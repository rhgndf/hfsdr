#ifndef TLV320ADC6120_HW_H
#define TLV320ADC6120_HW_H

/*
 * Texas Instruments TLV320ADC6120 — I2C audio ADC (see SBASA92A).
 * - Fixed 7-bit I2C address 0b1001110 (0x4E).
 * - tlv320adc6120_hw_init() targets ASI slave + I2S + 16-bit to match CH32 SPI2 I2S
 *   master (Philips, 96 kHz) in i2s_hw.c; PWR_CFG enables MICBIAS for electret mics.
 *
 * AVDD / AREG: set TLV320ADC6120_USE_INTERNAL_AREG before including this header
 * (or in the build) if you use 3.3 V AVDD with the on-chip 1.8 V AREG regulator.
 * Use 0 for 1.8 V AVDD with AREG shorted to AVDD (external AREG path).
 */

#include "debug.h"

#ifndef TLV320ADC6120_USE_INTERNAL_AREG
#define TLV320ADC6120_USE_INTERNAL_AREG 1
#endif

#define TLV320ADC6120_I2C_ADDR_7BIT 0x4EU

ErrorStatus tlv320adc6120_hw_init(void);

#endif
