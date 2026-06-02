#ifndef TRNG_HW_H
#define TRNG_HW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRNG_HW_DEFAULT_TIMEOUT_ITERATIONS 100000U

typedef enum
{
    TRNG_HW_OK = 0,
    TRNG_HW_NOT_READY,
    TRNG_HW_CLOCK_ERROR,
    TRNG_HW_SEED_ERROR,
    TRNG_HW_TIMEOUT,
    TRNG_HW_INVALID_ARGUMENT
} trng_hw_status_t;

void trng_hw_init(void);
void trng_hw_deinit(void);

trng_hw_status_t trng_hw_status(void);
trng_hw_status_t trng_hw_read_u32_nonblocking(uint32_t *value);
trng_hw_status_t trng_hw_read_u32_timeout(uint32_t *value, uint32_t timeout_iterations);
trng_hw_status_t trng_hw_read_u32(uint32_t *value);
trng_hw_status_t trng_hw_fill_timeout(void *buffer, size_t length, uint32_t timeout_iterations);
trng_hw_status_t trng_hw_fill(void *buffer, size_t length);

const char *trng_hw_status_name(trng_hw_status_t status);

#ifdef __cplusplus
}
#endif

#endif
