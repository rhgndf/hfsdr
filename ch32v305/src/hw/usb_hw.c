#include "usb_hw.h"

#include <string.h>

#include "debug.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define USB_ECHO_BUF_SIZE  512
#define USB_ROOT_HUB_PORT  0
#define USB_WEB_URL        "example.tinyusb.org/webusb-serial/index.html"

static uint8_t usb_echo_buf[USB_ECHO_BUF_SIZE];
static uint8_t vendor_buf[512];
static tusb_desc_webusb_url_t const desc_url = {
    .bLength = 3 + sizeof(USB_WEB_URL) - 1,
    .bDescriptorType = 3,
    .bScheme = 1,
    .url = USB_WEB_URL
};

static void usb_send_connected_banner(void)
{
    static uint8_t const msg[] = "\r\nWebUSB interface connected\r\n";
    usb_send_data(msg, sizeof(msg) - 1U);
}

void usb_hw_init(void)
{
    RCC_USBCLK48MConfig(RCC_USBCLK48MCLKSource_USBPHY);
    RCC_USBHSPLLCLKConfig(RCC_HSBHSPLLCLKSource_HSE);
    //RCC_USBHSPLLCLKConfig(RCC_HSBHSPLLCLKSource_HSI);
    /* Board is fixed at 24 MHz HSE, so keep the USBHS PHY divider aligned to an 8 MHz ref. */
    RCC_USBHSConfig(RCC_USBPLL_Div3);
    RCC_USBHSPLLCKREFCLKConfig(RCC_USBHSPLLCKREFCLK_8M);
    RCC_USBHSPHYPLLALIVEcmd(ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBHS, ENABLE);

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };

    tusb_init(USB_ROOT_HUB_PORT, &dev_init);
}

void usb_hw_task(void)
{
    if(tud_vendor_n_write_available(0) != 0U)
    {
        tud_vendor_write(vendor_buf, sizeof(vendor_buf));
#if CFG_TUD_VENDOR_TXRX_BUFFERED
        tud_vendor_write_flush();
#endif
    }

    tud_task();
}

uint32_t usb_send_data(uint8_t const *buffer, uint32_t len)
{
    uint32_t written = tud_cdc_write(buffer, len);
    if(written > 0U)
    {
        tud_cdc_write_flush();
    }

    return written;
}

uint32_t usb_receive_data(uint8_t *buffer, uint32_t len)
{
    return tud_cdc_read(buffer, len);
}

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize)
{
    (void) idx;
    (void) buffer;
    (void) bufsize;
#if CFG_TUD_VENDOR_TXRX_BUFFERED
    tud_vendor_read(vendor_buf, sizeof(vendor_buf));
#endif
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request)
{
    if(stage != CONTROL_STAGE_SETUP)
    {
        return true;
    }

    switch(request->bmRequestType_bit.type)
    {
        case TUSB_REQ_TYPE_VENDOR:
            switch(request->bRequest)
            {
                case VENDOR_REQUEST_WEBUSB:
                    return tud_control_xfer(rhport, request, (void*)(uintptr_t)&desc_url, desc_url.bLength);

                case VENDOR_REQUEST_MICROSOFT:
                    if(request->wIndex == 7U)
                    {
                        uint16_t total_len;
                        memcpy(&total_len, desc_ms_os_20 + 8, 2);
                        return tud_control_xfer(rhport, request, (void*)(uintptr_t)desc_ms_os_20, total_len);
                    }
                    return false;

                default:
                    break;
            }
            break;

        case TUSB_REQ_TYPE_CLASS:
            if(request->bRequest == 0x22U)
            {
                if(request->wValue != 0U)
                {
                    usb_send_connected_banner();
                }

                return tud_control_status(rhport, request);
            }
            break;

        default:
            break;
    }

    return false;
}

void tud_cdc_rx_cb(uint8_t itf)
{
    (void) itf;

    while(1)
    {
        uint32_t rx_avail = tud_cdc_available();
        uint32_t tx_avail = tud_cdc_write_available();
        uint32_t chunk = rx_avail;

        if(chunk > tx_avail)
        {
            chunk = tx_avail;
        }

        if(chunk > sizeof(usb_echo_buf))
        {
            chunk = sizeof(usb_echo_buf);
        }

        if(chunk == 0U)
        {
            break;
        }

        chunk = usb_receive_data(usb_echo_buf, chunk);
        if(chunk == 0U)
        {
            break;
        }

        usb_send_data(usb_echo_buf, chunk);
    }
}

void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
    (void) itf;
    (void) sent_bytes;
}
