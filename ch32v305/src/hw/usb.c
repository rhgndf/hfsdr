#include "usb.h"

#include <string.h>

#include "debug.h"
#include "si5351.h"
#include "tlv320adc6120.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define USB_ECHO_BUF_SIZE  512
#define USB_ROOT_HUB_PORT  0
#define USB_WEB_URL        "rhgndf.github.io/hfsdr/"
#define USB_HW_CLK_FREQ_PAYLOAD_SIZE 8U
#define USB_HW_CLK_FREQ_STATE_SIZE   (1U + USB_HW_CLK_FREQ_PAYLOAD_SIZE)
#define USB_HW_TLV320_GAIN_REQ_SIZE  1U
#define USB_HW_PLL_LOCK_STATE_SIZE   2U
#define USB_VENDOR_STREAM_INDEX      0U

static uint8_t usb_hw_clk_freq_req[USB_HW_CLK_FREQ_PAYLOAD_SIZE];
static uint8_t usb_hw_clk_freq_state[USB_HW_CLK_FREQ_STATE_SIZE];
static uint8_t usb_hw_tlv320_gain_req[USB_HW_TLV320_GAIN_REQ_SIZE];
static uint8_t usb_hw_pll_lock_state[USB_HW_PLL_LOCK_STATE_SIZE];
static ErrorStatus usb_hw_clk_freq_status = NoREADY;
static uint8_t usb_hw_tlv320_gain_raw = 0x00U;
static int8_t usb_hw_tlv320_gain_db_x2 = 0;
static ErrorStatus usb_hw_tlv320_gain_status = NoREADY;
static volatile uint32_t usb_hw_vendor_total_word_count = 0U;
static volatile uint32_t usb_hw_vendor_dropped_word_count = 0U;
static tusb_desc_webusb_url_t const desc_url = {
    .bLength = 3 + sizeof(USB_WEB_URL) - 1,
    .bDescriptorType = 3,
    .bScheme = 1,
    .url = USB_WEB_URL
};

static bool usb_hw_vendor_control_target_valid(tusb_control_request_t const* request)
{
    if(request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE)
    {
        return request->wIndex == 0U;
    }

    if(request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_INTERFACE)
    {
        return request->wIndex == ITF_NUM_VENDOR;
    }

    return false;
}

static uint64_t usb_u64_from_le(uint8_t const *buf)
{
    uint64_t value = 0U;

    for(uint32_t i = 0U; i < USB_HW_CLK_FREQ_PAYLOAD_SIZE; ++i)
    {
        value |= ((uint64_t)buf[i]) << (8U * i);
    }

    return value;
}

static void usb_u64_to_le(uint64_t value, uint8_t *buf)
{
    for(uint32_t i = 0U; i < USB_HW_CLK_FREQ_PAYLOAD_SIZE; ++i)
    {
        buf[i] = (uint8_t)(value >> (8U * i));
    }
}

static void usb_hw_prepare_clk_freq_state(void)
{
    usb_hw_clk_freq_state[0] = (uint8_t)usb_hw_clk_freq_status;
    usb_u64_to_le(si5351_hw_clk0_get_freq_hz(), &usb_hw_clk_freq_state[1]);
}

ErrorStatus usb_hw_get_pll_lock(uint8_t *locked)
{
    return si5351_hw_get_plla_lock(locked);
}

static void usb_hw_prepare_pll_lock_state(void)
{
    uint8_t locked = 0U;
    ErrorStatus pll_status = usb_hw_get_pll_lock(&locked);

    usb_hw_pll_lock_state[0] = (uint8_t)pll_status;
    usb_hw_pll_lock_state[1] = locked;
}

static void usb_send_connected_banner(void)
{
    static uint8_t const msg[] = "\r\nWebUSB interface connected\r\n";
    usb_send_data(msg, sizeof(msg) - 1U);
}

void usb_hw_vendor_write_isr(volatile uint16_t const *src_words, size_t word_count)
{
    if((src_words == 0) || (word_count == 0U))
    {
        return;
    }

    usb_hw_vendor_total_word_count += (uint32_t)word_count;

    if(tud_vendor_n_write_available(USB_VENDOR_STREAM_INDEX) < (word_count * sizeof(uint16_t)))
    {
        usb_hw_vendor_dropped_word_count += (uint32_t)word_count;
        return;
    }

    (void)tud_vendor_n_write(USB_VENDOR_STREAM_INDEX,
                             (uint8_t const *)(uintptr_t)src_words,
                             (uint32_t)(word_count * sizeof(uint16_t)));
}

uint32_t usb_hw_vendor_total_words(void)
{
    return usb_hw_vendor_total_word_count;
}

uint32_t usb_hw_vendor_dropped_words(void)
{
    return usb_hw_vendor_dropped_word_count;
}

ErrorStatus usb_hw_set_clk_freq_hz(uint64_t hz)
{
    usb_hw_clk_freq_status = si5351_hw_clk0_set_freq_hz(hz);
    if(usb_hw_clk_freq_status == READY)
    {
        printf("Si5351: LO set to %lu Hz\r\n", (unsigned long)hz);
    }
    else
    {
        printf("Si5351: LO set failed for %lu Hz (status %u)\r\n",
               (unsigned long)hz,
               (unsigned int)usb_hw_clk_freq_status);
    }

    return usb_hw_clk_freq_status;
}

ErrorStatus usb_hw_set_tlv320_gain_raw(uint8_t gain_raw)
{
    usb_hw_tlv320_gain_status = tlv320adc6120_hw_set_ch_gain_raw(gain_raw);
    if(usb_hw_tlv320_gain_status == READY)
    {
        usb_hw_tlv320_gain_raw = gain_raw;
        printf("TLV320: CHx_CFG1 set to 0x%02X\r\n", (unsigned int)gain_raw);
    }
    else
    {
        printf("TLV320: CHx_CFG1 set failed for 0x%02X (status %u)\r\n",
               (unsigned int)gain_raw,
               (unsigned int)usb_hw_tlv320_gain_status);
    }

    return usb_hw_tlv320_gain_status;
}

ErrorStatus usb_hw_set_tlv320_gain_db_x2(int8_t gain_db_x2)
{
    uint8_t magnitude_db_x2 = (uint8_t)((gain_db_x2 < 0) ? -gain_db_x2 : gain_db_x2);
    char sign = (gain_db_x2 < 0) ? '-' : '+';

    usb_hw_tlv320_gain_status = tlv320adc6120_hw_set_ch_gain_db_x2(gain_db_x2);
    if(usb_hw_tlv320_gain_status == READY)
    {
        usb_hw_tlv320_gain_db_x2 = gain_db_x2;
        printf("TLV320: channel gain set to %c%lu.%lu dB\r\n",
               sign,
               (unsigned long)(magnitude_db_x2 / 2U),
               (unsigned long)((magnitude_db_x2 & 1U) * 5U));
    }
    else
    {
        printf("TLV320: channel gain set failed for %c%lu.%lu dB (status %u)\r\n",
               sign,
               (unsigned long)(magnitude_db_x2 / 2U),
               (unsigned long)((magnitude_db_x2 & 1U) * 5U),
               (unsigned int)usb_hw_tlv320_gain_status);
    }

    return usb_hw_tlv320_gain_status;
}

ErrorStatus usb_hw_get_clk_freq_status(void)
{
    return usb_hw_clk_freq_status;
}

void usb_hw_init(void)
{
    RCC_USBCLK48MConfig(RCC_USBCLK48MCLKSource_USBPHY);
    RCC_USBHSPLLCLKConfig(RCC_HSBHSPLLCLKSource_HSE);
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
    if((buffer == 0) || (bufsize == 0U))
    {
        return;
    }

    if(tud_vendor_n_write_available(idx) < bufsize)
    {
        return;
    }

    (void)tud_vendor_n_write(idx, buffer, bufsize);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request)
{
    switch(request->bmRequestType_bit.type)
    {
        case TUSB_REQ_TYPE_VENDOR:
            switch(request->bRequest)
            {
                case VENDOR_REQUEST_WEBUSB:
                    if(stage != CONTROL_STAGE_SETUP)
                    {
                        return true;
                    }
                    return tud_control_xfer(rhport, request, (void*)(uintptr_t)&desc_url, desc_url.bLength);

                case VENDOR_REQUEST_MICROSOFT:
                    if(stage != CONTROL_STAGE_SETUP)
                    {
                        return true;
                    }
                    if(request->wIndex == 7U)
                    {
                        uint16_t total_len;
                        memcpy(&total_len, desc_ms_os_20 + 8, 2);
                        return tud_control_xfer(rhport, request, (void*)(uintptr_t)desc_ms_os_20, total_len);
                    }
                    return false;

                case VENDOR_REQUEST_SET_CLK_FREQ:
                    if(request->bmRequestType_bit.direction != TUSB_DIR_OUT)
                    {
                        return false;
                    }
                    if((request->wValue != 0U) || !usb_hw_vendor_control_target_valid(request) || (request->wLength != USB_HW_CLK_FREQ_PAYLOAD_SIZE))
                    {
                        return false;
                    }

                    if(stage == CONTROL_STAGE_SETUP)
                    {
                        /* Browser-side shape:
                         * controlTransferOut({ requestType: 'vendor', recipient: 'interface', request: 3, value: 0, index: ITF_NUM_VENDOR }, freqHzLe64)
                         */
                        return tud_control_xfer(rhport, request, usb_hw_clk_freq_req, sizeof(usb_hw_clk_freq_req));
                    }
                    if(stage == CONTROL_STAGE_DATA)
                    {
                        return (usb_hw_set_clk_freq_hz(usb_u64_from_le(usb_hw_clk_freq_req)) == READY);
                    }
                    return true;

                case VENDOR_REQUEST_GET_CLK_FREQ:
                    if(stage != CONTROL_STAGE_SETUP)
                    {
                        return true;
                    }
                    if(request->bmRequestType_bit.direction != TUSB_DIR_IN)
                    {
                        return false;
                    }
                    if((request->wValue != 0U) || !usb_hw_vendor_control_target_valid(request))
                    {
                        return false;
                    }

                    /* Returns 1 status byte followed by uint64_t little-endian frequency in Hz. */
                    usb_hw_prepare_clk_freq_state();
                    return tud_control_xfer(rhport, request, usb_hw_clk_freq_state, sizeof(usb_hw_clk_freq_state));

                case VENDOR_REQUEST_SET_TLV320_GAIN:
                    if(request->bmRequestType_bit.direction != TUSB_DIR_OUT)
                    {
                        return false;
                    }
                    if((request->wValue != 0U) || !usb_hw_vendor_control_target_valid(request) || (request->wLength != USB_HW_TLV320_GAIN_REQ_SIZE))
                    {
                        return false;
                    }

                    if(stage == CONTROL_STAGE_SETUP)
                    {
                        return tud_control_xfer(rhport, request, usb_hw_tlv320_gain_req, sizeof(usb_hw_tlv320_gain_req));
                    }
                    if(stage == CONTROL_STAGE_DATA)
                    {
                        return (usb_hw_set_tlv320_gain_db_x2((int8_t)usb_hw_tlv320_gain_req[0]) == READY);
                    }
                    return true;

                case VENDOR_REQUEST_GET_PLL_LOCK:
                    if(stage != CONTROL_STAGE_SETUP)
                    {
                        return true;
                    }
                    if(request->bmRequestType_bit.direction != TUSB_DIR_IN)
                    {
                        return false;
                    }
                    if((request->wValue != 0U) || !usb_hw_vendor_control_target_valid(request))
                    {
                        return false;
                    }

                    usb_hw_prepare_pll_lock_state();
                    return tud_control_xfer(rhport, request, usb_hw_pll_lock_state, sizeof(usb_hw_pll_lock_state));

                default:
                    break;
            }
            break;

        case TUSB_REQ_TYPE_CLASS:
            if(stage != CONTROL_STAGE_SETUP)
            {
                return true;
            }
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
}

void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
    (void) itf;
    (void) sent_bytes;
}
