#ifndef SI5351_HW_H
#define SI5351_HW_H

/*
 * Silicon Labs Si5351A/B/C — I2C clock generator (typ. addr 0x60).
 *
 * This driver assumes a 24 MHz crystal on XA/XB (hfsdr RF board). For each
 * requested CLK0 frequency it uses R_DIV=64 below 1 MHz and R_DIV=1 otherwise,
 * targets a PLL VCO near 750 MHz, approximates outputClock / Fxtal with a
 * continued fraction, and keeps MS0 in integer mode.
 *
 * si5351_hw_clk0_set_freq_hz(): CLK0 output in Hz. Practical minimum is about
 * 5.2 kHz with the current divider plan (SI5351_MIN_OUTPUT_HZ); below that
 * returns NoREADY.
 */

#ifndef SI5351_XTAL_FREQ_HZ
#define SI5351_XTAL_FREQ_HZ 24000000UL
#endif

#define SI5351_I2C_ADDR_7BIT 0x60U

/* Minimum output frequency (Hz) for the current CLK0 divider plan. */
#define SI5351_MIN_OUTPUT_HZ 5209ULL

#include "debug.h"

ErrorStatus si5351_hw_clk0_set_freq_hz(uint64_t hz_hz);
ErrorStatus si5351_hw_clk0_set_94mhz(void);

#endif
