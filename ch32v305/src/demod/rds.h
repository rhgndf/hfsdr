#pragma once

#include <cstdint>

namespace demod
{
/* Feed the 192 kHz, pre-deemphasis WBFM discriminator output from the DMA ISR. */
void rds_push_sample(int32_t fm_q31);

/* Reset receiver, synchronization, and queued-output state. */
void rds_reset();

/* Print completed groups from foreground context. */
void rds_poll();
}
