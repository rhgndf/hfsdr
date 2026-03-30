#ifndef SPI_MANUAL_H
#define SPI_MANUAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Low-level SPI1 to the ST7789 bus: CS (PD2) + RS/DC (PC12) + MOSI/SCK.
 * Call spi_manual_init() once (safe after reset). Compose transactions yourself:
 *
 *   spi_manual_cs_begin();
 *   spi_manual_rs_cmd();
 *   spi_manual_transfer_u8(0x2A);          // CASET command
 *   spi_manual_rs_data();
 *   spi_manual_transfer_u8(0x00);
 *   spi_manual_transfer_u8(0x00);
 *   spi_manual_transfer_u8(0x00);
 *   spi_manual_transfer_u8(0xEF);
 *   spi_manual_cs_end();
 */

void spi_manual_init(void);

void spi_manual_cs_begin(void);
void spi_manual_cs_end(void);

/* RS low = command, RS high = data (same as ST7789 DC). */
void spi_manual_rs_cmd(void);
void spi_manual_rs_data(void);

/* One SPI frame; returns MISO (often unused on write-only links). */
uint8_t spi_manual_transfer_u8(uint8_t tx);

#endif
