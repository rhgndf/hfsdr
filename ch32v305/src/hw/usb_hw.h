#ifndef USB_HW_H
#define USB_HW_H

#include <stdint.h>

void usb_hw_init(void);
void usb_hw_task(void);

uint32_t usb_send_data(uint8_t const *buffer, uint32_t len);
uint32_t usb_receive_data(uint8_t *buffer, uint32_t len);

#endif
