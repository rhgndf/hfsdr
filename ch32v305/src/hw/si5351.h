#ifndef SI5351_HW_H
#define SI5351_HW_H

/*
 * Silicon Labs Si5351A/B/C — I2C clock generator (typ. addr 0x60).
 *
 * This driver assumes a 24 MHz crystal on XA/XB (hfsdr RF board). For each
 * requested frequency it programs CLK0 and CLK1 from PLLA with the same output
 * divider, and derives CLK1 with a 90-degree phase offset relative to CLK0.
 * It uses the Si5351 phase registers when the requested frequency is high
 * enough, and a task-timed temporary divider offset below that range. The final
 * low-frequency state keeps CLK0/CLK1 on equal integer MultiSynth dividers.
 *
 * si5351_hw_clk0_set_freq_hz(): quadrature CLK0/CLK1 output in Hz. Returns the
 * actual programmed frequency, or zero on failure.
 */

#ifndef SI5351_XTAL_FREQ_HZ
#define SI5351_XTAL_FREQ_HZ 24000000UL
#endif

#define SI5351_I2C_ADDR_7BIT 0x60U

/* Si5351A practical lower clock-output limit. */
#define SI5351_MIN_OUTPUT_HZ 2500U
#define SI5351_MAX_OUTPUT_HZ 225000000U

#include "debug.h"

ErrorStatus si5351_init();
uint32_t si5351_hw_clk0_set_freq_hz(uint32_t hz);
ErrorStatus si5351_hw_get_plla_lock(uint8_t *locked);

#endif
