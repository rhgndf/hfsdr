#ifndef SI5351_HW_H
#define SI5351_HW_H

/*
 * Silicon Labs Si5351A/B/C — I2C clock generator (typ. addr 0x60).
 *
 * This driver assumes a 24 MHz crystal on XA/XB (hfsdr RF board). For each
 * requested frequency it programs CLK0 and CLK1 from PLLA with the same output
 * divider, and derives CLK1 with a 90-degree phase offset relative to CLK0.
 * It keeps R_DIV at 1 in quadrature mode, targets a PLL VCO near 400 MHz, and
 * approximates outputClock / Fxtal with a continued fraction.
 *
 * si5351_hw_clk0_set_freq_hz(): quadrature CLK0/CLK1 output in Hz. Practical
 * minimum is about 3.17 MHz with the current divider/phase plan
 * (SI5351_MIN_OUTPUT_HZ); below that returns NoREADY.
 */

#ifndef SI5351_XTAL_FREQ_HZ
#define SI5351_XTAL_FREQ_HZ 24000000UL
#endif

#define SI5351_I2C_ADDR_7BIT 0x60U

/* Minimum output frequency (Hz) for the current quadrature phase plan. */
#define SI5351_MIN_OUTPUT_HZ 3174604ULL

#include "debug.h"

ErrorStatus si5351_hw_clk0_set_freq_hz(uint64_t hz_hz);
ErrorStatus si5351_hw_clk0_set_94mhz(void);
ErrorStatus si5351_hw_get_plla_lock(uint8_t *locked);

#endif
