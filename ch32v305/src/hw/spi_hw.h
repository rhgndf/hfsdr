#ifndef SPI_HW_H
#define SPI_HW_H

#include <stdint.h>

void spi_hw_init(void);
uint8_t spi_hw_transfer_u8(uint8_t tx_byte);

#endif
