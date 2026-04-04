#ifndef USB_HW_H
#define USB_HW_H

#include <stddef.h>
#include <stdint.h>

#include "debug.h"

void usb_hw_init(void);
void usb_hw_vendor_write_isr(volatile uint16_t const *src_words, size_t word_count);
uint32_t usb_hw_vendor_total_words(void);
uint32_t usb_hw_vendor_dropped_words(void);

uint32_t usb_send_data(uint8_t const *buffer, uint32_t len);
uint32_t usb_receive_data(uint8_t *buffer, uint32_t len);

ErrorStatus usb_hw_set_clk_freq_hz(uint64_t hz);
uint64_t usb_hw_get_clk_freq_hz(void);
ErrorStatus usb_hw_get_clk_freq_status(void);

#endif
