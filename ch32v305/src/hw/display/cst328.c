#include "cst328.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "hw/i2c.h"
#include "hw/pinout.h"
#include "ui/debug_overlay.h"

#define CST328_I2C_ADDR_7BIT                 0x1AU

#define CST328_CMD_DEBUG_INFO                0xD101U
#define CST328_CMD_NORMAL                    0xD109U

#define CST328_REG_TOUCH_INFO                0xD000U
#define CST328_REG_KEY_TX_RX_NUMBERS         0xD1F4U
#define CST328_REG_RESOLUTION                0xD1F8U
#define CST328_REG_CHECKSUM_BOOT_PROJECT     0xD1FCU
#define CST328_REG_CHIP_TYPE_PROJECT_ID      0xD204U
#define CST328_REG_FIRMWARE_VERSION          0xD208U
#define CST328_REG_FIRMWARE_CHECKSUM         0xD20CU
#define CST328_INFO_BLOCK_FIRST_REG          CST328_REG_KEY_TX_RX_NUMBERS
#define CST328_INFO_BLOCK_LEN                28U

#define CST328_TOUCH_REPORT_LEN              27U
#define CST328_MAX_TOUCHES                   5U
#define CST328_FINGER_RECORD_LEN             5U
#define CST328_FINGER_STATUS_TOUCH           0x06U

static uint16_t cst328_u16_from_le_bytes(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[1] << 8) | (uint16_t)data[0]);
}

static uint32_t cst328_u32_from_le_bytes(const uint8_t *data)
{
    return ((uint32_t)data[3] << 24) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[1] << 8) |
           (uint32_t)data[0];
}

static ErrorStatus cst328_enter_debug_info_mode(void)
{
    if(i2c_hw_scan_bus_at(CST328_I2C_ADDR_7BIT) != READY)
    {
        UI_DebugOverlay_Printf("CST328 no ACK 0x%02X", CST328_I2C_ADDR_7BIT);
        printf("CST328: no ACK at I2C 0x%02X\r\n", CST328_I2C_ADDR_7BIT);
        return NoREADY;
    }

    if(i2c_hw_write_command16(CST328_I2C_ADDR_7BIT, CST328_CMD_DEBUG_INFO) != READY)
    {
        UI_DebugOverlay_Printf("CST328 debug cmd fail");
        printf("CST328: failed to enter debug info mode (I2C 0x%02X)\r\n", CST328_I2C_ADDR_7BIT);
        return NoREADY;
    }

    Delay_Ms(10U);
    return READY;
}

static ErrorStatus cst328_read_info_block(uint8_t *data, size_t len)
{
    for(uint8_t attempt = 0U; attempt < 3U; ++attempt)
    {
        if((cst328_enter_debug_info_mode() == READY) &&
           (i2c_hw_read_register16_burst(CST328_I2C_ADDR_7BIT, CST328_INFO_BLOCK_FIRST_REG, data, len) == READY))
        {
            return READY;
        }

        printf("CST328: failed to read info block attempt %u\r\n", (unsigned int)(attempt + 1U));
        UI_DebugOverlay_Printf("CST328 read fail %u", (unsigned int)(attempt + 1U));
        Delay_Ms(20U);
    }

    return NoREADY;
}

static void cst328_hw_configure_pins(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC, ENABLE);

    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = CST328_RST_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(CST328_RST_GPIO_PORT, &gpio);

    gpio.GPIO_Pin = CST328_IRQ_GPIO_PIN;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(CST328_IRQ_GPIO_PORT, &gpio);
}

static void cst328_hw_reset(void)
{
    GPIO_WriteBit(CST328_RST_GPIO_PORT, CST328_RST_GPIO_PIN, Bit_RESET);
    Delay_Ms(2U);
    GPIO_WriteBit(CST328_RST_GPIO_PORT, CST328_RST_GPIO_PIN, Bit_SET);
    Delay_Ms(300U);
}

ErrorStatus cst328_hw_init(void)
{
    cst328_hw_configure_pins();
    cst328_hw_reset();

    if(i2c_hw_write_command16(CST328_I2C_ADDR_7BIT, CST328_CMD_NORMAL) != READY)
    {
        printf("CST328: normal mode command failed (I2C 0x%02X)\r\n", CST328_I2C_ADDR_7BIT);
        return NoREADY;
    }

    printf("CST328: I2C 0x%02X, RST PC13, IRQ PA12\r\n", CST328_I2C_ADDR_7BIT);
    return READY;
}

void cst328_hw_print_details(void)
{
    static uint8_t hw_ready = 0U;
    static uint32_t call_count = 0U;
    uint8_t info_block[CST328_INFO_BLOCK_LEN] = {0};

    ++call_count;
    UI_DebugOverlay_Printf("CST328 details %lu", (unsigned long)call_count);
    printf("CST328 print details call %lu\r\n", (unsigned long)call_count);

    if(hw_ready == 0U)
    {
        cst328_hw_configure_pins();
        cst328_hw_reset();
        hw_ready = 1U;
    }

    if(cst328_read_info_block(info_block, sizeof(info_block)) != READY)
    {
        UI_DebugOverlay_Printf("CST328 info read failed");
        printf("CST328: failed to read version information registers\r\n");
        return;
    }

    const uint8_t *key_tx_rx = &info_block[CST328_REG_KEY_TX_RX_NUMBERS - CST328_INFO_BLOCK_FIRST_REG];
    const uint8_t *resolution = &info_block[CST328_REG_RESOLUTION - CST328_INFO_BLOCK_FIRST_REG];
    const uint8_t *checksum_boot_project = &info_block[CST328_REG_CHECKSUM_BOOT_PROJECT - CST328_INFO_BLOCK_FIRST_REG];
    const uint8_t *chip_project = &info_block[CST328_REG_CHIP_TYPE_PROJECT_ID - CST328_INFO_BLOCK_FIRST_REG];
    const uint8_t *firmware_version = &info_block[CST328_REG_FIRMWARE_VERSION - CST328_INFO_BLOCK_FIRST_REG];
    const uint8_t *firmware_checksum = &info_block[CST328_REG_FIRMWARE_CHECKSUM - CST328_INFO_BLOCK_FIRST_REG];

    UI_DebugOverlay_Printf("CST328 %ux%u fw %u.%u.%u",
                           (unsigned int)cst328_u16_from_le_bytes(&resolution[0]),
                           (unsigned int)cst328_u16_from_le_bytes(&resolution[2]),
                           (unsigned int)firmware_version[3],
                           (unsigned int)firmware_version[2],
                           (unsigned int)cst328_u16_from_le_bytes(&firmware_version[0]));
    printf("CST328 info: key=%u tx=%u rx=%u res=%ux%u ic=0x%02X project=0x%06lX fw=%u.%u.%u boot=%u checksum=0x%08lX fw_checksum=0x%08lX raw D1F4=%02X %02X %02X %02X D1F8=%02X %02X %02X %02X\r\n",
           (unsigned int)key_tx_rx[3],
           (unsigned int)key_tx_rx[0],
           (unsigned int)key_tx_rx[2],
           (unsigned int)cst328_u16_from_le_bytes(&resolution[0]),
           (unsigned int)cst328_u16_from_le_bytes(&resolution[2]),
           (unsigned int)chip_project[3],
           (unsigned long)(cst328_u32_from_le_bytes(chip_project) & 0x00FFFFFFUL),
           (unsigned int)firmware_version[3],
           (unsigned int)firmware_version[2],
           (unsigned int)cst328_u16_from_le_bytes(&firmware_version[0]),
           (unsigned int)checksum_boot_project[3],
           (unsigned long)cst328_u32_from_le_bytes(checksum_boot_project),
           (unsigned long)cst328_u32_from_le_bytes(firmware_checksum),
           (unsigned int)key_tx_rx[0],
           (unsigned int)key_tx_rx[1],
           (unsigned int)key_tx_rx[2],
           (unsigned int)key_tx_rx[3],
           (unsigned int)resolution[0],
           (unsigned int)resolution[1],
           (unsigned int)resolution[2],
           (unsigned int)resolution[3]);
}

void cst328_hw_poll(void)
{
    uint8_t report[CST328_TOUCH_REPORT_LEN] = {0};
    uint8_t touch_count;

    if(GPIO_ReadInputDataBit(CST328_IRQ_GPIO_PORT, CST328_IRQ_GPIO_PIN) != Bit_RESET)
    {
        return;
    }

    if(i2c_hw_read_register16_burst(CST328_I2C_ADDR_7BIT, CST328_REG_TOUCH_INFO, report, sizeof(report)) != READY)
    {
        printf("CST328: failed to read touch report\r\n");
        return;
    }

    touch_count = report[5] & 0x0FU;
    if(touch_count > CST328_MAX_TOUCHES)
    {
        touch_count = CST328_MAX_TOUCHES;
    }

    for(uint8_t i = 0U; i < touch_count; ++i)
    {
        size_t offset = (size_t)i * CST328_FINGER_RECORD_LEN;
        uint8_t id_status = report[offset];
        uint8_t status = id_status & 0x0FU;

        if(status == CST328_FINGER_STATUS_TOUCH)
        {
            uint16_t x = (uint16_t)(((uint16_t)report[offset + 1U] << 4) | ((uint16_t)(report[offset + 3U] >> 4) & 0x0FU));
            uint16_t y = (uint16_t)(((uint16_t)report[offset + 2U] << 4) | ((uint16_t)report[offset + 3U] & 0x0FU));
            uint8_t finger_id = id_status >> 4;
            uint8_t pressure = report[offset + 4U];

            printf("CST328 touch: id=%u x=%u y=%u pressure=%u count=%u\r\n",
                   (unsigned int)finger_id,
                   (unsigned int)x,
                   (unsigned int)y,
                   (unsigned int)pressure,
                   (unsigned int)touch_count);
        }
    }
}
