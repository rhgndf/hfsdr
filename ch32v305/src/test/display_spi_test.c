#include "test/display_spi_test.h"

#include "debug.h"
#include "hw/spi_manual.h"

void display_spi_test_run(void)
{
    /* Example: one command per CS window (adjust for your panel). */
    spi_manual_cs_begin();
    spi_manual_rs_cmd();
    spi_manual_transfer_u8(0x01U); /* SWRESET */
    spi_manual_cs_end();
    Delay_Ms(150U);

    spi_manual_cs_begin();
    spi_manual_rs_cmd();
    spi_manual_transfer_u8(0x11U); /* SLPOUT */
    spi_manual_cs_end();
    Delay_Ms(120U);

    /* Turn on backlight */
    spi_manual_cs_begin();
    spi_manual_rs_cmd();
    spi_manual_transfer_u8(0x53U);
    spi_manual_transfer_u8(0xFFU);
    spi_manual_cs_end();
    Delay_Ms(150U);

    spi_manual_cs_begin();
    spi_manual_rs_cmd();
    spi_manual_transfer_u8(0x29U); /* DISPON */
    spi_manual_cs_end();
}
