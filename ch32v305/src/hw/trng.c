#include "trng.h"

#include "ch32v30x_rcc.h"
#include "ch32v30x_rng.h"

static void trng_hw_clear_interrupt_status(uint32_t status_bits)
{
    /* Sticky RNG status bits clear by writing 0; keep all other bits at 1. */
    RNG->SR = ~status_bits;
}

static void trng_hw_restart_after_seed_error(void)
{
    trng_hw_clear_interrupt_status(RNG_SR_SEIS);
    RNG_Cmd(DISABLE);
    RNG_Cmd(ENABLE);
}

static trng_hw_status_t trng_hw_handle_errors(void)
{
    uint32_t sr = RNG->SR;

    if((sr & (RNG_SR_SECS | RNG_SR_SEIS)) != 0U)
    {
        trng_hw_restart_after_seed_error();
        return TRNG_HW_SEED_ERROR;
    }

    if((sr & RNG_SR_CECS) != 0U)
    {
        trng_hw_clear_interrupt_status(RNG_SR_CEIS);
        return TRNG_HW_CLOCK_ERROR;
    }

    if((sr & RNG_SR_CEIS) != 0U)
    {
        trng_hw_clear_interrupt_status(RNG_SR_CEIS);
    }

    return TRNG_HW_OK;
}

void trng_hw_init(void)
{
    RCC_RNGCLKConfig(RCC_RNGCLKSource_SYSCLK);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_RNG, ENABLE);

    RNG_ITConfig(DISABLE);
    RNG_Cmd(DISABLE);
    trng_hw_clear_interrupt_status(RNG_SR_CEIS | RNG_SR_SEIS);
    RNG_Cmd(ENABLE);
}

void trng_hw_deinit(void)
{
    RNG_ITConfig(DISABLE);
    RNG_Cmd(DISABLE);
    trng_hw_clear_interrupt_status(RNG_SR_CEIS | RNG_SR_SEIS);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_RNG, DISABLE);
}

trng_hw_status_t trng_hw_status(void)
{
    uint32_t sr = RNG->SR;

    if((sr & (RNG_SR_SECS | RNG_SR_SEIS)) != 0U)
    {
        return TRNG_HW_SEED_ERROR;
    }

    if((sr & (RNG_SR_CECS | RNG_SR_CEIS)) != 0U)
    {
        return TRNG_HW_CLOCK_ERROR;
    }

    if((sr & RNG_SR_DRDY) == 0U)
    {
        return TRNG_HW_NOT_READY;
    }

    return TRNG_HW_OK;
}

trng_hw_status_t trng_hw_read_u32_nonblocking(uint32_t *value)
{
    if(value == NULL)
    {
        return TRNG_HW_INVALID_ARGUMENT;
    }

    trng_hw_status_t error_status = trng_hw_handle_errors();
    if(error_status != TRNG_HW_OK)
    {
        return error_status;
    }

    if((RNG->SR & RNG_SR_DRDY) == 0U)
    {
        return TRNG_HW_NOT_READY;
    }

    *value = RNG_GetRandomNumber();
    return TRNG_HW_OK;
}

trng_hw_status_t trng_hw_read_u32_timeout(uint32_t *value, uint32_t timeout_iterations)
{
    if(value == NULL)
    {
        return TRNG_HW_INVALID_ARGUMENT;
    }

    while(timeout_iterations > 0U)
    {
        --timeout_iterations;

        trng_hw_status_t status = trng_hw_read_u32_nonblocking(value);
        if(status == TRNG_HW_OK)
        {
            return TRNG_HW_OK;
        }

        if(status != TRNG_HW_NOT_READY)
        {
            return status;
        }
    }

    return TRNG_HW_TIMEOUT;
}

trng_hw_status_t trng_hw_read_u32(uint32_t *value)
{
    return trng_hw_read_u32_timeout(value, TRNG_HW_DEFAULT_TIMEOUT_ITERATIONS);
}

trng_hw_status_t trng_hw_fill_timeout(void *buffer, size_t length, uint32_t timeout_iterations)
{
    if((buffer == NULL) && (length > 0U))
    {
        return TRNG_HW_INVALID_ARGUMENT;
    }

    uint8_t *bytes = (uint8_t *)buffer;
    size_t bytes_written = 0U;

    while(bytes_written < length)
    {
        uint32_t word = 0U;
        trng_hw_status_t status = trng_hw_read_u32_timeout(&word, timeout_iterations);
        if(status != TRNG_HW_OK)
        {
            return status;
        }

        for(uint32_t i = 0U; (i < 4U) && (bytes_written < length); ++i)
        {
            bytes[bytes_written] = (uint8_t)(word >> (i * 8U));
            ++bytes_written;
        }
    }

    return TRNG_HW_OK;
}

trng_hw_status_t trng_hw_fill(void *buffer, size_t length)
{
    return trng_hw_fill_timeout(buffer, length, TRNG_HW_DEFAULT_TIMEOUT_ITERATIONS);
}

const char *trng_hw_status_name(trng_hw_status_t status)
{
    switch(status)
    {
        case TRNG_HW_OK:
            return "ok";
        case TRNG_HW_NOT_READY:
            return "not ready";
        case TRNG_HW_CLOCK_ERROR:
            return "clock error";
        case TRNG_HW_SEED_ERROR:
            return "seed error";
        case TRNG_HW_TIMEOUT:
            return "timeout";
        case TRNG_HW_INVALID_ARGUMENT:
            return "invalid argument";
        default:
            return "unknown";
    }
}
