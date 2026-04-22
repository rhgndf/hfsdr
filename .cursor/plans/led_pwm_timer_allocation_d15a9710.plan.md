---
name: LED PWM Timer Allocation
overview: Plan PWM-capable LED control by selecting timers/channels that are not currently used by the firmware and defining a safe fallback for pins without clear timer AF mapping.
todos:
  - id: audit-timer-conflicts
    content: Confirm active timers and reserve TIM6/TIM7/TIM8 from LED use
    status: completed
  - id: assign-led-pwm-backends
    content: Assign PA3 to TIM2_CH4 hardware PWM and PC0/PC1 to software PWM
    status: completed
  - id: define-duty-interface
    content: Define per-LED duty API for mode renderer and Link/USB override
    status: completed
  - id: validation-matrix
    content: Prepare regression checks for audio timing and LED visual quality
    status: completed
isProject: false
---

# LED PWM Allocation Plan

## Current Timer Usage (Do Not Reuse)

From current firmware sources:

- `TIM6` is used for DAC square-wave interrupt toggling in [C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c).
- `TIM7` is used as DAC streaming trigger base (TRGO) in [C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/dac_hw.c).
- `TIM8` is used for alternate 24 MHz I2S clock PWM output on PC6 in [C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/i2s_hw.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/i2s_hw.c).

Implication: avoid `TIM6`, `TIM7`, `TIM8` for LED PWM.

## LED Pins and PWM Feasibility

From [C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/hw/pinout.h](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/hw/pinout.h):

- `LED_GPIO_PIN`: `PA3`
- `LED1_GPIO_PIN`: `PC0`
- `LED2_GPIO_PIN`: `PC1`

Based on bundled peripheral headers:

- `PA3` has a clear hardware PWM option: `TIM2_CH4` (no remap required).
- `PC0` / `PC1` do not have clear timer-channel mapping in repo headers.

## Recommended PWM Strategy

- Use **hardware PWM** on `PA3` with `TIM2_CH4`.
- Keep `PC0` and `PC1` on **software PWM** (timer-tick driven duty cycle), unless board documentation confirms a hardware timer AF mapping.
- Use one periodic timebase for software PWM updates (prefer `SysTick`/existing loop tick to avoid burning another timer).

This gives immediate PWM capability without conflicting with active timing peripherals.

## Timer Allocation Proposal

- `TIM2`: reserved for LED hardware PWM (`PA3` only).
- `TIM6`, `TIM7`, `TIM8`: untouched (existing audio/clock stack).
- No new timer allocation required for `PC0/PC1` if software PWM is used.

## Implementation Steps

1. Add LED-PWM backend abstraction in [C:/Users/zunmun/Documents/Stuff/Github/GROUP PROJECTS/hfsdr/ch32v305/src/main.c](C:/Users/zunmun/Documents/Stuff/Github/GROUP%20PROJECTS/hfsdr/ch32v305/src/main.c):
  - `led_set_duty(led_id, duty_percent)`
  - hardware path for `PA3` (`TIM2_CH4` CCR update)
  - software path for `PC0/PC1`
2. Initialize `TIM2` PWM channel for `PA3` at a flicker-safe rate (e.g., 1-4 kHz).
3. Add software PWM accumulator for `PC0/PC1` in existing non-blocking LED task.
4. Refactor mode renderer to output per-LED duty targets (0-100), not only binary ON/OFF.
5. Keep Link/USB activity override behavior, but express LED output as duty pulses.

## Validation Plan

- Confirm no regressions in DAC streaming and I2S clock paths (timers 6/7/8 unchanged).
- Verify `PA3` brightness is smooth via hardware PWM.
- Verify `PC0`/`PC1` software PWM has no visible flicker and preserves distinct per-mode patterns.
- Stress-test with USB traffic + frequency changes to ensure Link/USB override remains responsive.

## Optional Hardening

If you want true hardware PWM on all 3 LEDs:

- Verify CH32V305 datasheet AF table for `PC0`/`PC1` timer channels.
- If unavailable, consider rerouting LED nets to known timer-channel pins in next hardware spin.

