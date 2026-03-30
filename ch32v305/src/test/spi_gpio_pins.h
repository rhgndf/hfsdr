#ifndef SPI_GPIO_PINS_H
#define SPI_GPIO_PINS_H

#include "ch32v30x.h"

/*
 * Manual GPIO control of SPI-related lines (PB3 SCK, PB5 MOSI, PD2 CS, PC12 RS, PC11 RST).
 * spi_gpio_pins_enable() turns SPI3 off and configures pins as push-pull outputs (MISO optional input).
 * spi_gpio_pins_restore_hw_spi() runs spi_hw_init() again for peripheral SPI.
 */

void spi_gpio_pins_enable(void);
void spi_gpio_pins_restore_hw_spi(void);

/* Drive SCK, MOSI, CS, RS, RST all high (call after spi_gpio_pins_enable()). */
void spi_gpio_pins_all_on(void);

void spi_gpio_sck_write(BitAction level);
void spi_gpio_mosi_write(BitAction level);
void spi_gpio_cs_write(BitAction level);
void spi_gpio_rs_write(BitAction level);
void spi_gpio_rst_write(BitAction level);

void spi_gpio_sck_toggle(void);
void spi_gpio_mosi_toggle(void);
void spi_gpio_cs_toggle(void);
void spi_gpio_rs_toggle(void);

#endif
