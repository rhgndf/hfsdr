#include "trng.h"

#include "ch32v30x_rcc.h"
#include "ch32v30x_rng.h"

static void trng_hw_clear_interrupt_status(uint32_t status_bits)
{
    /* Sticky RNG status bits clear by writing 0; keep all other bits at 1. */
    RNG->SR = ~status_bits;
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

uint32_t trng_hw_read_u32(void)
{
    while(1)
    {
        uint32_t sr = RNG->SR;

        if((sr & (RNG_SR_SECS | RNG_SR_SEIS)) != 0U)
        {
            trng_hw_clear_interrupt_status(RNG_SR_SEIS);
            RNG_Cmd(DISABLE);
            RNG_Cmd(ENABLE);
            continue;
        }

        if((sr & RNG_SR_CEIS) != 0U)
        {
            trng_hw_clear_interrupt_status(RNG_SR_CEIS);
        }

        if((sr & RNG_SR_DRDY) != 0U)
        {
            return RNG_GetRandomNumber();
        }
    }
}
