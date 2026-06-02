#ifndef TRNG_HW_H
#define TRNG_HW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void trng_hw_init(void);
void trng_hw_deinit(void);
uint32_t trng_hw_read_u32(void);

#ifdef __cplusplus
}
#endif

#endif
