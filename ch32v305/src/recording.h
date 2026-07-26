#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Create the statically allocated recording task before starting the scheduler.
 */
void recording_init(void);

/* Called directly from the encoder's EXTI interrupt context. */
void recording_request_toggle_from_isr(void);

/* Called by the I2S task after each 512-byte DMA half has been normalized. */
void recording_submit_i2s(volatile uint16_t const *samples,
                          size_t sample_word_count);

/* True only while samples are actively being accepted into the recording. */
bool recording_is_active(void);

#ifdef __cplusplus
}
#endif
