#ifndef SPI_HW_H
#define SPI_HW_H

#include <stddef.h>
#include <stdint.h>

/* SPI3 master: 8-bit frames, MSB first (required for ST7789 command/data bytes). */

void spi_hw_init(void);
uint8_t spi_hw_transfer_u8(uint8_t tx_byte);
void spi_hw_transfer_dma(const uint8_t *tx_buf, size_t len);

#endif
