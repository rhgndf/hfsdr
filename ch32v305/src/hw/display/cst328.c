#include "cst328.h"

#include "hw/i2c.h"
#include "hw/pinout.h"
#include "debug.h"

#include <stdio.h>
#include <string.h>

#define CST328_I2C_ADDR_7BIT       0x1AU

/* Mode command registers (write 16-bit address with no data payload). */
#define CST328_REG_MODE_DEBUG_INFO 0xD101U
#define CST328_REG_MODE_NORMAL     0xD109U

/* Debug-info registers (only valid in DEBUG_INFO mode). */
#define CST328_REG_RES_XY          0xD1F8U
#define CST328_REG_BOOT_CHECKSUM   0xD1FCU
#define CST328_REG_FW_VERSION      0xD208U

/* Touch report block (NORMAL mode). */
#define CST328_REG_TOUCH_BASE      0xD000U
#define CST328_TOUCH_REPORT_LEN    27U   /* 5 fingers + key/count + 0xAB sentinel */

#define CST328_FINGER_STATUS_TOUCH 0x06U
#define CST328_FIXED_BYTE_VALUE    0xABU

static uint8_t s_cst328_present = 0U;

static void cst328_hw_reset_pulse(void)
{
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_RESET);
    Delay_Ms(10);
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_SET);
    /* Datasheet TRON: 300 ms initialization after reset before host I2C access. */
    Delay_Ms(300);
}

static void cst328_hw_gpio_init(void)
{
    GPIO_InitTypeDef g = {0};

    /* Reset pin: push-pull output, held high in normal run. */
    g.GPIO_Mode = GPIO_Mode_Out_PP;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Pin = TP_RST_GPIO_PIN;
    GPIO_Init(TP_RST_GPIO_PORT, &g);
    GPIO_WriteBit(TP_RST_GPIO_PORT, TP_RST_GPIO_PIN, Bit_SET);

    /* IRQ pin: input with pull-up (chip drives it open-drain / push-pull low on data ready). */
    g.GPIO_Mode = GPIO_Mode_IPU;
    g.GPIO_Pin = TP_INT_GPIO_PIN;
    GPIO_Init(TP_INT_GPIO_PORT, &g);
}

ErrorStatus cst328_hw_init(void)
{
    cst328_hw_gpio_init();
    cst328_hw_reset_pulse();

    s_cst328_present = 0U;

    /* Probe: enter debug-info mode and read firmware version + resolution. */
    if(i2c_hw_write_register16(CST328_I2C_ADDR_7BIT, CST328_REG_MODE_DEBUG_INFO, NULL, 0U) != READY)
    {
        printf("CST328: probe failed (no ACK at 0x%02X)\r\n", CST328_I2C_ADDR_7BIT);
        return NoREADY;
    }

    uint8_t fw[4] = {0};
    uint8_t res[4] = {0};
    (void)i2c_hw_read_register16(CST328_I2C_ADDR_7BIT, CST328_REG_FW_VERSION, fw, sizeof(fw));
    (void)i2c_hw_read_register16(CST328_I2C_ADDR_7BIT, CST328_REG_RES_XY, res, sizeof(res));

    uint16_t res_x = (uint16_t)res[0] | ((uint16_t)res[1] << 8);
    uint16_t res_y = (uint16_t)res[2] | ((uint16_t)res[3] << 8);
    printf("CST328: FW build=0x%02X%02X minor=0x%02X major=0x%02X res=%ux%u\r\n",
           fw[1], fw[0], fw[2], fw[3], res_x, res_y);

    /* Switch to normal touch-reporting mode. */
    if(i2c_hw_write_register16(CST328_I2C_ADDR_7BIT, CST328_REG_MODE_NORMAL, NULL, 0U) != READY)
    {
        printf("CST328: failed to enter normal mode\r\n");
        return NoREADY;
    }

    s_cst328_present = 1U;
    return READY;
}

ErrorStatus cst328_hw_read_touch(cst328_touch_t *touch)
{
    if(touch == NULL)
    {
        return NoREADY;
    }

    uint8_t buf[CST328_TOUCH_REPORT_LEN];
    if(i2c_hw_read_register16(CST328_I2C_ADDR_7BIT, CST328_REG_TOUCH_BASE, buf, sizeof(buf)) != READY)
    {
        return NoREADY;
    }

    if(buf[6] != CST328_FIXED_BYTE_VALUE)
    {
        return NoREADY;
    }

    uint8_t finger_count = buf[5] & 0x0FU;
    if(finger_count > CST328_MAX_FINGERS)
    {
        finger_count = CST328_MAX_FINGERS;
    }

    memset(touch, 0, sizeof(*touch));
    touch->finger_count = finger_count;

    /* Per-finger blocks: finger 1 at [0..4], then 0xD005/0xD006 housekeeping at [5..6],
     * fingers 2..5 each take 5 bytes starting at [7], [12], [17], [22]. */
    for(uint8_t i = 0U; i < finger_count; ++i)
    {
        const uint8_t *p = (i == 0U) ? &buf[0] : &buf[7U + (i - 1U) * 5U];
        uint8_t status = p[0] & 0x0FU;
        if(status != CST328_FINGER_STATUS_TOUCH)
        {
            continue;
        }
        cst328_point_t *pt = &touch->points[i];
        pt->id = (p[0] >> 4) & 0x0FU;
        pt->x = ((uint16_t)p[1] << 4) | ((uint16_t)(p[3] >> 4) & 0x0FU);
        pt->y = ((uint16_t)p[2] << 4) | ((uint16_t)p[3] & 0x0FU);
        pt->pressure = p[4];
    }

    return READY;
}

void cst328_hw_poll(void)
{
    if(s_cst328_present == 0U)
    {
        return;
    }

    /* IRQ is active-low: chip pulls it down only when there is a valid touch report. */
    if(GPIO_ReadInputDataBit(TP_INT_GPIO_PORT, TP_INT_GPIO_PIN) != Bit_RESET)
    {
        return;
    }

    cst328_touch_t touch;
    if(cst328_hw_read_touch(&touch) != READY)
    {
        return;
    }

    if(touch.finger_count == 0U)
    {
        return;
    }

    printf("CST328: %u finger(s):", touch.finger_count);
    for(uint8_t i = 0U; i < touch.finger_count; ++i)
    {
        const cst328_point_t *pt = &touch.points[i];
        printf(" [id=%u x=%u y=%u p=%u]",
               pt->id, pt->x, pt->y, pt->pressure);
    }
    printf("\r\n");
}
