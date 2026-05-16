#ifndef SPI_HW_H
#define SPI_HW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPI3 master: 8-bit frames, MSB first (required for ST7789 command/data bytes). */

void spi_hw_init(void);
uint8_t spi_hw_transfer_u8(uint8_t tx_byte);
/* Starts a TX DMA transfer and returns before it completes. tx_buf must remain
 * valid until spi_hw_wait_dma(), spi_hw_transfer_u8(), or the next
 * spi_hw_transfer_dma() has waited for the transfer. */
void spi_hw_transfer_dma(const uint8_t *tx_buf, size_t len);
void spi_hw_transfer_dma_u16(const uint16_t *tx_buf, size_t count);
void spi_hw_transfer_dma_repeat_u16(uint16_t tx_word, size_t count);
void spi_hw_wait_dma(void);

#ifdef __cplusplus
}
#endif

#endif
