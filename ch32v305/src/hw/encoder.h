#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

void encoder_init(void);
[[nodiscard]] int16_t encoder_take_delta(void);
[[nodiscard]] int32_t encoder_get_position(void);

#endif
