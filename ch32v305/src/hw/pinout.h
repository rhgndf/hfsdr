#ifndef PINOUT_H
#define PINOUT_H

#include "ch32v30x.h"

// --- nanoCH32V305 --------------------------------------
/* Global define */
#define LED_GPIO_PORT      GPIOA
#define LED_GPIO_PIN       GPIO_Pin_3

/* Change these if your BOOT button is on another GPIO */
#define BOOT_GPIO_PORT     GPIOA
#define BOOT_GPIO_PIN      GPIO_Pin_14

// --- hfsdr badge ---------------------------------------
/* LEDs */
#define LED1_GPIO_PORT     GPIOC
#define LED1_GPIO_PIN      GPIO_Pin_0
#define LED2_GPIO_PORT     GPIOC
#define LED2_GPIO_PIN      GPIO_Pin_1

/* DAC */
#define DAC1_OUT_GPIO_PORT GPIOA
#define DAC1_OUT_GPIO_PIN  GPIO_Pin_4
#define DAC2_OUT_GPIO_PORT GPIOA
#define DAC2_OUT_GPIO_PIN  GPIO_Pin_5

/* Encoder */
#define ENC_B_GPIO_PORT    GPIOC
#define ENC_B_GPIO_PIN     GPIO_Pin_13
#define ENC_A_GPIO_PORT    GPIOC
#define ENC_A_GPIO_PIN     GPIO_Pin_14
#define ENC_BTN_GPIO_PORT  GPIOC
#define ENC_BTN_GPIO_PIN   GPIO_Pin_15

/* ADC sense */
#define BATT_ADC_GPIO_PORT GPIOC
#define BATT_ADC_GPIO_PIN  GPIO_Pin_4
#define VBUS_ADC_GPIO_PORT GPIOC
#define VBUS_ADC_GPIO_PIN  GPIO_Pin_5

/* I2S - ADC */
#define I2S_WS_GPIO_PORT   GPIOB
#define I2S_WS_GPIO_PIN    GPIO_Pin_12
#define I2S_CK_GPIO_PORT   GPIOB
#define I2S_CK_GPIO_PIN    GPIO_Pin_13
#define I2S_SD_GPIO_PORT   GPIOB
#define I2S_SD_GPIO_PIN    GPIO_Pin_15
#define I2S_MCK_GPIO_PORT  GPIOC
#define I2S_MCK_GPIO_PIN   GPIO_Pin_6

/* SPI3 - Screen (remapped: SCK/MOSI on PB3/PB5; default SPI3 pins are PC10/PC11/PC12) */
#define SPI3_SCL_SCK_GPIO_PORT  GPIOB
#define SPI3_SCL_SCK_GPIO_PIN   GPIO_Pin_3
#define SPI3_SDA_MOSI_GPIO_PORT GPIOB
#define SPI3_SDA_MOSI_GPIO_PIN  GPIO_Pin_5

/* ST7789 + touch: command vs SPI data is RS (register select), not a separate DC pin. */
/* ST7789 reset: active-low (drive low to reset, high for normal run). */
#define ST7789_RST_GPIO_PORT  GPIOC
#define ST7789_RST_GPIO_PIN   GPIO_Pin_11
/* Touch panel reset (active-low): PC10 */
#define TP_RST_GPIO_PORT      GPIOC
#define TP_RST_GPIO_PIN       GPIO_Pin_10
/* Previous pinouts:
 * #define ST7789_CS_GPIO_PORT   GPIOD
 * #define ST7789_CS_GPIO_PIN    GPIO_Pin_2
 * #define ST7789_RS_GPIO_PORT   GPIOC
 * #define ST7789_RS_GPIO_PIN    GPIO_Pin_12
 */
#define ST7789_CS_GPIO_PORT   GPIOA
#define ST7789_CS_GPIO_PIN    GPIO_Pin_10
#define ST7789_RS_GPIO_PORT   GPIOA
#define ST7789_RS_GPIO_PIN    GPIO_Pin_9

/* I2C - Touchscreen, ADC (RS also drives LCD register select on the shared flex / routing) */
#define I2C_SCL_GPIO_PORT   GPIOB
#define I2C_SCL_GPIO_PIN    GPIO_Pin_10
#define I2C_SDA_GPIO_PORT   GPIOB
#define I2C_SDA_GPIO_PIN    GPIO_Pin_11

#endif
