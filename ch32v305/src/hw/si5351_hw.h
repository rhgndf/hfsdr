#ifndef SI5351_HW_H
#define SI5351_HW_H

/*
 * Silicon Labs Si5351A/B/C — I2C clock generator (typ. addr 0x60).
 *
 * si5351_hw_clk0_set_94mhz() assumes a 24 MHz crystal on XA/XB (hfsdr RF board).
 * It programs PLL A to 846 MHz and MS0 to divide by 9 → 94 MHz on CLK0.
 * Change SI5351_XTAL_FREQ_HZ only if you add a new frequency plan in si5351_hw.c.
 */

#ifndef SI5351_XTAL_FREQ_HZ
#define SI5351_XTAL_FREQ_HZ 24000000UL
#endif

#define SI5351_I2C_ADDR_7BIT 0x60U

#include "debug.h"

ErrorStatus si5351_hw_clk0_set_94mhz(void);

#endif
