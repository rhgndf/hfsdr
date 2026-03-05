/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : Main program body.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

/*
 *@Note
 GPIO routine:
 PA0 push-pull output.

*/

#include "debug.h"
#include "tusb.h"

/* Global define */
#define LED_GPIO_PORT      GPIOA
#define LED_GPIO_PIN       GPIO_Pin_3

/* Change these if your BOOT button is on another GPIO */
#define BOOT_GPIO_PORT     GPIOA
#define BOOT_GPIO_PIN      GPIO_Pin_14

#define USB_ECHO_BUF_SIZE  512

/* Global Variable */
static uint8_t cdc_buf[USB_ECHO_BUF_SIZE];

/*********************************************************************
 * @fn      GPIO_Toggle_INIT
 *
 * @brief   Initializes GPIOA.0
 *
 * @return  none
 */
void GPIO_Toggle_INIT(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = LED_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LED_GPIO_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = BOOT_GPIO_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(BOOT_GPIO_PORT, &GPIO_InitStructure);
}


/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */
int main(void)
{
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();

    //SysTick_Config(SystemCoreClock / 1000);
    USART_Printf_Init(115200);	
    printf("SystemClk:%d\r\n", SystemCoreClock);
    printf( "ChipID:%08x\r\n", DBGMCU_GetCHIPID() );

    printf("GPIO Toggle TEST\r\n");
    GPIO_Toggle_INIT();

    RCC_USBCLK48MConfig(RCC_USBCLK48MCLKSource_USBPHY);
    RCC_USBHSPLLCLKConfig(RCC_HSBHSPLLCLKSource_HSE);
    RCC_USBHSConfig(RCC_USBPLL_Div1);
    RCC_USBHSPLLCKREFCLKConfig(RCC_USBHSPLLCKREFCLK_8M);
    RCC_USBHSPHYPLLALIVEcmd(ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBHS, ENABLE);

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(0, &dev_init); // initialize device stack on roothub port 0

    while(1)
    {
        tud_task(); // device task
    }
}

void tud_cdc_rx_cb(uint8_t itf)
{
  //if (tud_cdc_connected())
  {
    if(tud_cdc_available())
    {
      tud_cdc_n_read_flush(itf);
      if (tud_vendor_n_write_available(0))
        tud_vendor_write(cdc_buf, 512);
    }
  }
}

//--------------------------------------------------------------------+
// WebUSB use vendor class
//--------------------------------------------------------------------+

#define URL  "example.tinyusb.org/webusb-serial/index.html"

const tusb_desc_webusb_url_t desc_url = {
  .bLength         = 3 + sizeof(URL) - 1,
  .bDescriptorType = 3, // WEBUSB URL type
  .bScheme         = 1, // 0: http, 1: https
  .url             = URL
};

// Invoked when a control transfer occurred on an interface of this class
// Driver response accordingly to the request and the transfer stage (setup/data/ack)
// return false to stall control endpoint (e.g unsupported request)
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
  // nothing to with DATA & ACK stage
  if (stage != CONTROL_STAGE_SETUP) return true;

  switch (request->bmRequestType_bit.type) {
    case TUSB_REQ_TYPE_VENDOR:
      switch (request->bRequest) {
        case VENDOR_REQUEST_WEBUSB:
          // match vendor request in BOS descriptor
          // Get landing page url
          return tud_control_xfer(rhport, request, (void*)(uintptr_t)&desc_url, desc_url.bLength);

        case VENDOR_REQUEST_MICROSOFT:
          if (request->wIndex == 7) {
            // Get Microsoft OS 2.0 compatible descriptor
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20 + 8, 2);

            return tud_control_xfer(rhport, request, (void*)(uintptr_t)desc_ms_os_20, total_len);
          } else {
            return false;
          }

        default: break;
      }
      break;

    case TUSB_REQ_TYPE_CLASS:
      if (request->bRequest == 0x22) {
        // Webserial simulate the CDC_REQUEST_SET_CONTROL_LINE_STATE (0x22) to connect and disconnect.
        bool web_serial_connected = (request->wValue != 0);

        // Always lit LED if connected
        if (web_serial_connected) {

          tud_vendor_write_str("\r\nWebUSB interface connected\r\n");
          tud_vendor_write_flush();
        } else {

        }

        // response with status OK
        return tud_control_status(rhport, request);
      }
      break;

    default: break;
  }

  // stall unknown request
  return false;
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const* buffer, uint32_t bufsize) {
  (void) itf;
  (void) buffer;
  (void) bufsize;

  while (1)
  {
    uint32_t rx_avail = tud_vendor_available();
    uint32_t tx_avail = tud_vendor_write_available();
    uint32_t chunk = rx_avail;

    if (chunk > tx_avail)
    {
      chunk = tx_avail;
    }

    if (chunk > sizeof(cdc_buf))
    {
      chunk = sizeof(cdc_buf);
    }

    if (chunk == 0)
    {
      break;
    }

    chunk = tud_vendor_read(cdc_buf, chunk);
    if (chunk == 0)
    {
      break;
    }

    tud_vendor_write(cdc_buf, chunk);
    tud_vendor_write_flush();
  }

}

void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes) {
  (void) itf;
  (void) sent_bytes;
}