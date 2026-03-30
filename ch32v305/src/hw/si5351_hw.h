#ifndef SI5351_HW_H
#define SI5351_HW_H

/*
 * Silicon Labs Si5351A/B/C — I2C clock generator (typ. addr 0x60).
 *
 * si5351_hw_clk0_set_94mhz() assumes a 24 MHz crystal on XA/XB (hfsdr RF board).
 * It programs PLL A to 846 MHz and MS0 to divide by 9 → 94 MHz on CLK0.
 * Change SI5351_XTAL_FREQ_HZ only if you add a new frequency plan in si5351_hw.c.
 *
 * si5351_hw_clk0_set_freq_hz() / si5351_hw_clk1_set_freq_hz(): CLK0 / CLK1 output
 * in Hz. Practical minimum ~4 kHz (SI5351_MIN_OUTPUT_HZ); below that returns
 * NoREADY (e.g. 1 Hz is not supported).
 */

#ifndef SI5351_XTAL_FREQ_HZ
#define SI5351_XTAL_FREQ_HZ 24000000UL
#endif

#define SI5351_I2C_ADDR_7BIT 0x60U

/* Minimum output frequency (Hz) for CLK0..5 path (library / device regime). */
#define SI5351_MIN_OUTPUT_HZ 4000ULL

#include "debug.h"

ErrorStatus si5351_hw_clk0_set_freq_hz(uint64_t hz_hz);
ErrorStatus si5351_hw_clk1_set_freq_hz(uint64_t hz_hz);
/* Program CLK0 and CLK1 to the same LO (e.g. Taylor detector / demux S0 & S1). */
ErrorStatus si5351_hw_fm_lo_both_hz(uint64_t hz_hz);
ErrorStatus si5351_hw_clk0_set_94mhz(void);

#endif
