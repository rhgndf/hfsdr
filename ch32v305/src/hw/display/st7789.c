#include "st7789.h"

#include "debug.h"
#include "hw/spi.h"

#include <stddef.h>
#include <stdint.h>

#define ST7789_DMA_COLOR_LINE_CHUNK_PIXELS 32U
#define ST7789_DMA_FILL_CHUNK_PIXELS       64U

static uint16_t s_scroll_top = 0U;
static uint16_t s_scroll_bottom = ST7789_HEIGHT;
static uint16_t s_scroll_offset = 0U;

static uint16_t ST7789_ScrollYToLogicalY(uint16_t y)
{
#if ST7789_ROTATION == 0
	return (uint16_t)(ST7789_HEIGHT - 1U - y);
#else
	return y;
#endif
}

static void ST7789_SPI_TxU8(uint8_t v)
{
	(void)spi_hw_transfer_u8(v);
}

static void ST7789_HW_Init(void)
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD,
						   ENABLE);

	GPIO_InitTypeDef g = {0};
	g.GPIO_Mode = GPIO_Mode_Out_PP;
	g.GPIO_Speed = GPIO_Speed_50MHz;

	g.GPIO_Pin = ST7789_LEDK_GPIO_PIN;
	GPIO_Init(ST7789_LEDK_GPIO_PORT, &g);
	GPIO_WriteBit(ST7789_LEDK_GPIO_PORT, ST7789_LEDK_GPIO_PIN, Bit_SET);

	g.GPIO_Pin = ST7789_DC_PIN;
	GPIO_Init(ST7789_DC_PORT, &g);

	g.GPIO_Pin = ST7789_RST_PIN;
	GPIO_Init(ST7789_RST_PORT, &g);
	ST7789_RST_Release();

#ifndef CFG_NO_CS
	g.GPIO_Pin = ST7789_CS_PIN;
	GPIO_Init(ST7789_CS_PORT, &g);
	ST7789_UnSelect();
#endif
	ST7789_DC_Set();

	spi_hw_init();
}

/**
 * @brief Write command to ST7789 controller
 * @param cmd -> command to write
 * @return none
 */
static void ST7789_WriteCommand(uint8_t cmd)
{
	ST7789_Select();
	ST7789_DC_Clr();
	ST7789_SPI_TxU8(cmd);
	ST7789_UnSelect();
}

/**
 * @brief Write data to ST7789 controller via SPI DMA
 * @param buff -> pointer of data buffer
 * @param buff_size -> size of the data buffer
 * @return none
 */
static void ST7789_WriteData(uint8_t *buff, size_t buff_size)
{
	if ((buff == NULL) || (buff_size == 0U)) {
		return;
	}

	ST7789_Select();
	ST7789_DC_Set();
	spi_hw_transfer_dma(buff, buff_size);
	ST7789_UnSelect();
}

/* Stream a single RGB565 pixel value to the panel pixel_count times via DMA. */
static void ST7789_WriteRepeatedPixel(uint16_t color, uint32_t pixel_count)
{
	if (pixel_count == 0U) {
		return;
	}

	uint8_t tx_buf[ST7789_DMA_FILL_CHUNK_PIXELS * 2U];
	uint32_t chunk_pixels = (pixel_count > ST7789_DMA_FILL_CHUNK_PIXELS)
	                           ? ST7789_DMA_FILL_CHUNK_PIXELS : pixel_count;

	for (uint32_t i = 0U; i < chunk_pixels; ++i) {
		tx_buf[2U * i] = (uint8_t)(color >> 8);
		tx_buf[(2U * i) + 1U] = (uint8_t)(color & 0xFFU);
	}

	ST7789_Select();
	ST7789_DC_Set();
	while (pixel_count > 0U) {
		uint32_t this_chunk = (pixel_count > ST7789_DMA_FILL_CHUNK_PIXELS)
		                         ? ST7789_DMA_FILL_CHUNK_PIXELS : pixel_count;
		spi_hw_transfer_dma(tx_buf, (size_t)this_chunk * 2U);
		pixel_count -= this_chunk;
	}
	ST7789_UnSelect();
}
/**
 * @brief Write data to ST7789 controller, simplify for 8bit data.
 * data -> data to write
 * @return none
 */
static void ST7789_WriteSmallData(uint8_t data)
{
	ST7789_Select();
	ST7789_DC_Set();
	ST7789_SPI_TxU8(data);
	ST7789_UnSelect();
}

/**
 * @brief Set the rotation direction of the display
 * @param m -> rotation parameter(please refer it in st7789.h)
 * @return none
 */
void ST7789_SetRotation(uint8_t m)
{
	ST7789_WriteCommand(ST7789_MADCTL);	// MADCTL
	switch (m) {
	case 0:
		ST7789_WriteSmallData(ST7789_MADCTL_MX | ST7789_MADCTL_MY | ST7789_MADCTL_RGB);
		break;
	case 1:
		ST7789_WriteSmallData(ST7789_MADCTL_MY | ST7789_MADCTL_MV | ST7789_MADCTL_RGB);
		break;
	case 2:
		ST7789_WriteSmallData(ST7789_MADCTL_RGB);
		break;
	case 3:
		ST7789_WriteSmallData(ST7789_MADCTL_MX | ST7789_MADCTL_MV | ST7789_MADCTL_RGB);
		break;
	default:
		break;
	}
}

/**
 * @brief Set address of DisplayWindow
 * @param xi&yi -> coordinates of window
 * @return none
 */
static void ST7789_SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	ST7789_Select();
	uint16_t x_start = x0 + X_SHIFT;
	uint16_t x_end = x1 + X_SHIFT;
	uint16_t y_start = y0 + Y_SHIFT;
	uint16_t y_end = y1 + Y_SHIFT;
	
	/* Column Address set */
	ST7789_WriteCommand(ST7789_CASET); 
	{
		uint8_t data[] = {x_start >> 8, x_start & 0xFF, x_end >> 8, x_end & 0xFF};
		ST7789_WriteData(data, sizeof(data));
	}

	/* Row Address set */
	ST7789_WriteCommand(ST7789_RASET);
	{
		uint8_t data[] = {y_start >> 8, y_start & 0xFF, y_end >> 8, y_end & 0xFF};
		ST7789_WriteData(data, sizeof(data));
	}
	/* Write to RAM */
	ST7789_WriteCommand(ST7789_RAMWR);
	ST7789_UnSelect();
}

/**
 * @brief Initialize ST7789 controller
 * @param none
 * @return none
 */
void ST7789_Init(void)
{
	ST7789_HW_Init();

	Delay_Ms(1);
    ST7789_RST_Assert();
    Delay_Ms(1);
    ST7789_RST_Release();
    Delay_Ms(120);

    ST7789_WriteCommand(ST7789_COLMOD);		//	Set color mode
    ST7789_WriteSmallData(ST7789_COLOR_MODE_16bit);
  	ST7789_WriteCommand(0xB2);				//	Porch control
	{
		uint8_t data[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
		ST7789_WriteData(data, sizeof(data));
	}
	ST7789_SetRotation(ST7789_ROTATION);	//	MADCTL (Display Rotation)
	
	/* Internal LCD Voltage generator settings */
    ST7789_WriteCommand(0XB7);				//	Gate Control
    ST7789_WriteSmallData(0x35);			//	Default value
    ST7789_WriteCommand(0xBB);				//	VCOM setting
    ST7789_WriteSmallData(0x19);			//	0.725v (default 0.75v for 0x20)
    ST7789_WriteCommand(0xC0);				//	LCMCTRL	
    ST7789_WriteSmallData (0x2C);			//	Default value
    ST7789_WriteCommand (ST7789_VRHEN);		//	VRH command Enable
	{
		uint8_t data[] = {0x01, 0xFF};
		ST7789_WriteData(data, sizeof(data));
	}
    ST7789_WriteCommand (0xC3);				//	VRH set
    ST7789_WriteSmallData (0x12);			//	+-4.45v (defalut +-4.1v for 0x0B)
    ST7789_WriteCommand (ST7789_VCMOFSET);	//	VCOMS offset set
    ST7789_WriteSmallData (0x20);			//	0V offset
    ST7789_WriteCommand (0xC6);				//	Frame rate control in normal mode
    ST7789_WriteSmallData (0x0F);			//	Default value (60HZ)
    ST7789_WriteCommand (0xD0);				//	Power control
    ST7789_WriteSmallData (0xA4);			//	Default value
    ST7789_WriteSmallData (0xA1);			//	Default value
	/**************** Division line ****************/

	ST7789_WriteCommand(0xE0);
	{
		uint8_t data[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
		ST7789_WriteData(data, sizeof(data));
	}

    ST7789_WriteCommand(0xE1);
	{
		uint8_t data[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
		ST7789_WriteData(data, sizeof(data));
	}
    ST7789_WriteCommand (ST7789_INVON);		//	Inversion ON
	ST7789_WriteCommand (ST7789_SLPOUT);	//	Out of sleep mode
	Delay_Ms(5);
  	ST7789_WriteCommand (ST7789_NORON);		//	Normal Display on
  	ST7789_WriteCommand (ST7789_DISPON);	//	Main screen turned on	
	ST7789_UnSelect();

	ST7789_Fill_Color(BLACK);				//	Fill with Black.
	GPIO_WriteBit(ST7789_LEDK_GPIO_PORT, ST7789_LEDK_GPIO_PIN, Bit_RESET);
}

/**
 * @brief Fill the DisplayWindow with single color
 * @param color -> color to Fill with
 * @return none
 */
void ST7789_Fill_Color(uint16_t color)
{
	ST7789_SetAddressWindow(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);
	ST7789_WriteRepeatedPixel(color, (uint32_t)ST7789_WIDTH * (uint32_t)ST7789_HEIGHT);
}

/**
 * @brief Draw a Pixel
 * @param x&y -> coordinate to Draw
 * @param color -> color of the Pixel
 * @return none
 */
void ST7789_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
	if ((x < 0) || (x >= ST7789_WIDTH) ||
		 (y < 0) || (y >= ST7789_HEIGHT))	return;
	
	ST7789_SetAddressWindow(x, y, x, y);
	uint8_t data[] = {color >> 8, color & 0xFF};
	ST7789_Select();
	ST7789_WriteData(data, sizeof(data));
	ST7789_UnSelect();
}

void ST7789_DrawColorLine(uint16_t x, uint16_t y, const uint16_t *colors, uint16_t width)
{
	if ((colors == NULL) || (width == 0U) || (x >= ST7789_WIDTH) || (y >= ST7789_HEIGHT)) {
		return;
	}

	uint16_t visible_width = width;
	if(((uint32_t)x + (uint32_t)visible_width) > ST7789_WIDTH) {
		visible_width = ST7789_WIDTH - x;
	}

	ST7789_SetAddressWindow(x, y, x + visible_width - 1U, y);
	ST7789_Select();
	ST7789_DC_Set();
	for(uint16_t offset = 0U; offset < visible_width;) {
		uint16_t chunk_pixels = visible_width - offset;
		if(chunk_pixels > ST7789_DMA_COLOR_LINE_CHUNK_PIXELS) {
			chunk_pixels = ST7789_DMA_COLOR_LINE_CHUNK_PIXELS;
		}

		uint8_t tx_buf[ST7789_DMA_COLOR_LINE_CHUNK_PIXELS * 2U];
		for(uint16_t i = 0U; i < chunk_pixels; ++i) {
			uint16_t color = colors[offset + i];
			tx_buf[2U * i] = (uint8_t)(color >> 8);
			tx_buf[(2U * i) + 1U] = (uint8_t)(color & 0xFFU);
		}

		spi_hw_transfer_dma(tx_buf, (size_t)chunk_pixels * 2U);
		offset += chunk_pixels;
	}
	ST7789_UnSelect();
}

uint16_t ST7789_ScrollRows(uint16_t top, uint16_t bottom, int16_t rows)
{
	if((top >= bottom) || (bottom > ST7789_HEIGHT)) {
		return top;
	}

	uint16_t height = bottom - top;
	if(height == 0U) {
		return top;
	}

	uint16_t scroll_top = top;
	int16_t scroll_rows = rows;
#if ST7789_ROTATION == 0
	scroll_top = ST7789_HEIGHT - bottom;
#endif

	if((top != s_scroll_top) || (bottom != s_scroll_bottom) || (rows == 0)) {
		uint16_t bottom_fixed = ST7789_HEIGHT - scroll_top - height;
		uint8_t data[] = {
			(uint8_t)(scroll_top >> 8), (uint8_t)(scroll_top & 0xFFU),
			(uint8_t)(height >> 8), (uint8_t)(height & 0xFFU),
			(uint8_t)(bottom_fixed >> 8), (uint8_t)(bottom_fixed & 0xFFU),
		};

		s_scroll_top = top;
		s_scroll_bottom = bottom;
		s_scroll_offset = 0U;

		ST7789_WriteCommand(ST7789_VSCRDEF);
		ST7789_WriteData(data, sizeof(data));
	}

	int32_t offset = (int32_t)s_scroll_offset + (int32_t)scroll_rows;
	offset %= (int32_t)height;
	if(offset < 0) {
		offset += (int32_t)height;
	}
	s_scroll_offset = (uint16_t)offset;

	uint16_t scroll_start = scroll_top + s_scroll_offset;
	uint8_t data[] = {(uint8_t)(scroll_start >> 8), (uint8_t)(scroll_start & 0xFFU)};
	ST7789_WriteCommand(ST7789_VSCSAD);
	ST7789_WriteData(data, sizeof(data));

	uint16_t write_scroll_y;
	if(scroll_rows >= 0) {
		write_scroll_y = (uint16_t)(scroll_top + ((s_scroll_offset + height - 1U) % height));
	} else {
		write_scroll_y = (uint16_t)(scroll_top + s_scroll_offset);
	}
	return ST7789_ScrollYToLogicalY(write_scroll_y);
}

void ST7789_VerticalScrollDisable(void)
{
	uint16_t const height = ST7789_HEIGHT;
	uint16_t const scroll_top = 0U;
	uint16_t const bottom_fixed = (uint16_t)(ST7789_HEIGHT - height);
	uint8_t data[] = {
		(uint8_t)(scroll_top >> 8), (uint8_t)(scroll_top & 0xFFU),
		(uint8_t)(height >> 8), (uint8_t)(height & 0xFFU),
		(uint8_t)(bottom_fixed >> 8), (uint8_t)(bottom_fixed & 0xFFU),
	};

	s_scroll_top = 0U;
	s_scroll_bottom = height;
	s_scroll_offset = 0U;

	ST7789_WriteCommand(ST7789_VSCRDEF);
	ST7789_WriteData(data, sizeof(data));

	uint8_t vscsad[] = {0U, 0U};
	ST7789_WriteCommand(ST7789_VSCSAD);
	ST7789_WriteData(vscsad, sizeof(vscsad));
}

/**
 * @brief Fill an Area with single color
 * @param xSta&ySta -> coordinate of the start point
 * @param xEnd&yEnd -> coordinate of the end point
 * @param color -> color to Fill with
 * @return none
 */
void ST7789_Fill(uint16_t xSta, uint16_t ySta, uint16_t xEnd, uint16_t yEnd, uint16_t color)
{
	if ((xEnd >= ST7789_WIDTH) || (yEnd >= ST7789_HEIGHT) ||
	    (xSta > xEnd) || (ySta > yEnd))	return;

	ST7789_SetAddressWindow(xSta, ySta, xEnd, yEnd);
	uint32_t pixel_count = (uint32_t)(xEnd - xSta + 1U) * (uint32_t)(yEnd - ySta + 1U);
	ST7789_WriteRepeatedPixel(color, pixel_count);
}

/**
 * @brief Draw a big Pixel at a point
 * @param x&y -> coordinate of the point
 * @param color -> color of the Pixel
 * @return none
 */
void ST7789_DrawPixel_4px(uint16_t x, uint16_t y, uint16_t color)
{
	if ((x == 0U) || (x >= ST7789_WIDTH - 1U) ||
		 (y == 0U) || (y >= ST7789_HEIGHT - 1U))	return;
	ST7789_Select();
	ST7789_Fill(x - 1, y - 1, x + 1, y + 1, color);
	ST7789_UnSelect();
}

/**
 * @brief Draw a line with single color
 * @param x1&y1 -> coordinate of the start point
 * @param x2&y2 -> coordinate of the end point
 * @param color -> color of the line to Draw
 * @return none
 */
void ST7789_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
        uint16_t color) {
	uint16_t swap;
    uint16_t steep = ABS(y1 - y0) > ABS(x1 - x0);
    if (steep) {
		swap = x0;
		x0 = y0;
		y0 = swap;

		swap = x1;
		x1 = y1;
		y1 = swap;
        //_swap_int16_t(x0, y0);
        //_swap_int16_t(x1, y1);
    }

    if (x0 > x1) {
		swap = x0;
		x0 = x1;
		x1 = swap;

		swap = y0;
		y0 = y1;
		y1 = swap;
        //_swap_int16_t(x0, x1);
        //_swap_int16_t(y0, y1);
    }

    int16_t dx, dy;
    dx = x1 - x0;
    dy = ABS(y1 - y0);

    int16_t err = dx / 2;
    int16_t ystep;

    if (y0 < y1) {
        ystep = 1;
    } else {
        ystep = -1;
    }

    for (; x0<=x1; x0++) {
        if (steep) {
            ST7789_DrawPixel(y0, x0, color);
        } else {
            ST7789_DrawPixel(x0, y0, color);
        }
        err -= dy;
        if (err < 0) {
            y0 += ystep;
            err += dx;
        }
    }
}

/**
 * @brief Draw a Rectangle with single color
 * @param xi&yi -> 2 coordinates of 2 top points.
 * @param color -> color of the Rectangle line
 * @return none
 */
void ST7789_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
	ST7789_Select();
	ST7789_DrawLine(x1, y1, x2, y1, color);
	ST7789_DrawLine(x1, y1, x1, y2, color);
	ST7789_DrawLine(x1, y2, x2, y2, color);
	ST7789_DrawLine(x2, y1, x2, y2, color);
	ST7789_UnSelect();
}

/** 
 * @brief Draw a circle with single color
 * @param x0&y0 -> coordinate of circle center
 * @param r -> radius of circle
 * @param color -> color of circle line
 * @return  none
 */
void ST7789_DrawCircle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
{
	int16_t f = 1 - r;
	int16_t ddF_x = 1;
	int16_t ddF_y = -2 * r;
	int16_t x = 0;
	int16_t y = r;

	ST7789_Select();
	ST7789_DrawPixel(x0, y0 + r, color);
	ST7789_DrawPixel(x0, y0 - r, color);
	ST7789_DrawPixel(x0 + r, y0, color);
	ST7789_DrawPixel(x0 - r, y0, color);

	while (x < y) {
		if (f >= 0) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}
		x++;
		ddF_x += 2;
		f += ddF_x;

		ST7789_DrawPixel(x0 + x, y0 + y, color);
		ST7789_DrawPixel(x0 - x, y0 + y, color);
		ST7789_DrawPixel(x0 + x, y0 - y, color);
		ST7789_DrawPixel(x0 - x, y0 - y, color);

		ST7789_DrawPixel(x0 + y, y0 + x, color);
		ST7789_DrawPixel(x0 - y, y0 + x, color);
		ST7789_DrawPixel(x0 + y, y0 - x, color);
		ST7789_DrawPixel(x0 - y, y0 - x, color);
	}
	ST7789_UnSelect();
}

/**
 * @brief Draw an Image on the screen
 * @param x&y -> start point of the Image
 * @param w&h -> width & height of the Image to Draw
 * @param data -> pointer of the Image array
 * @return none
 */
void ST7789_DrawImage(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *data)
{
	if ((x >= ST7789_WIDTH) || (y >= ST7789_HEIGHT))
		return;
	if ((x + w - 1) >= ST7789_WIDTH)
		return;
	if ((y + h - 1) >= ST7789_HEIGHT)
		return;

	ST7789_Select();
	ST7789_SetAddressWindow(x, y, x + w - 1, y + h - 1);
	ST7789_WriteData((uint8_t *)data, sizeof(uint16_t) * w * h);
	ST7789_UnSelect();
}

/**
 * @brief Draw a packed 1bpp bitmap, expanded to fg/bg colors. Bits are MSB-first
 *        within each byte, row-major. Uses raw SetAddressWindow + DMA so it works
 *        with whatever rotation is currently active (caller picks the orientation).
 *        Maximum supported width is 320 (long axis of the panel).
 */
void ST7789_DrawBitmap1bpp(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           const uint8_t *bits, uint16_t fg, uint16_t bg)
{
	static uint8_t row_buf[320U * 2U];

	if ((bits == NULL) || (w == 0U) || (h == 0U) || (w > 320U))
		return;

	const uint16_t row_bytes = (w + 7U) / 8U;
	const uint8_t fg_hi = (uint8_t)(fg >> 8);
	const uint8_t fg_lo = (uint8_t)(fg & 0xFFU);
	const uint8_t bg_hi = (uint8_t)(bg >> 8);
	const uint8_t bg_lo = (uint8_t)(bg & 0xFFU);

	ST7789_SetAddressWindow(x, y, x + w - 1U, y + h - 1U);

	for (uint16_t row = 0U; row < h; ++row) {
		const uint8_t *row_bits = bits + (uint32_t)row * (uint32_t)row_bytes;
		for (uint16_t col = 0U; col < w; ++col) {
			uint8_t bit = (row_bits[col >> 3] >> (7U - (col & 7U))) & 1U;
			row_buf[2U * col]      = bit ? fg_hi : bg_hi;
			row_buf[2U * col + 1U] = bit ? fg_lo : bg_lo;
		}
		ST7789_WriteData(row_buf, (size_t)w * 2U);
	}
}

/**
 * @brief Invert Fullscreen color
 * @param invert -> Whether to invert
 * @return none
 */
void ST7789_InvertColors(uint8_t invert)
{
	ST7789_Select();
	ST7789_WriteCommand(invert ? 0x21 /* INVON */ : 0x20 /* INVOFF */);
	ST7789_UnSelect();
}

/** 
 * @brief Write a char
 * @param  x&y -> cursor of the start point.
 * @param ch -> char to write
 * @param font -> fontstyle of the string
 * @param color -> color of the char
 * @param bgcolor -> background color of the char
 * @return  none
 */
void ST7789_WriteChar(uint16_t x, uint16_t y, char ch, FontDef font, uint16_t color, uint16_t bgcolor)
{
	uint32_t i, b, j;
	ST7789_Select();
	ST7789_SetAddressWindow(x, y, x + font.width - 1, y + font.height - 1);

	for (i = 0; i < font.height; i++) {
		b = font.data[(ch - 32) * font.height + i];
		for (j = 0; j < font.width; j++) {
			if ((b << j) & 0x8000) {
				uint8_t data[] = {color >> 8, color & 0xFF};
				ST7789_WriteData(data, sizeof(data));
			}
			else {
				uint8_t data[] = {bgcolor >> 8, bgcolor & 0xFF};
				ST7789_WriteData(data, sizeof(data));
			}
		}
	}
	ST7789_UnSelect();
}

/** 
 * @brief Write a string 
 * @param  x&y -> cursor of the start point.
 * @param str -> string to write
 * @param font -> fontstyle of the string
 * @param color -> color of the string
 * @param bgcolor -> background color of the string
 * @return  none
 */
void ST7789_FillQuad(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                    uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, uint16_t color)
{
	ST7789_DrawFilledTriangle(x0, y0, x1, y1, x2, y2, color);
	ST7789_DrawFilledTriangle(x0, y0, x2, y2, x3, y3, color);
}

void ST7789_BlitBitmap1bppRow(uint16_t x, uint16_t y, uint16_t w, uint16_t bitmap_w,
                              const uint8_t *bits, uint16_t fg, uint16_t bg)
{
	static uint8_t row_buf[320U * 2U];
	const uint16_t row_bytes = (bitmap_w + 7U) / 8U;
	const uint8_t fg_hi = (uint8_t)(fg >> 8);
	const uint8_t fg_lo = (uint8_t)(fg & 0xFFU);
	const uint8_t bg_hi = (uint8_t)(bg >> 8);
	const uint8_t bg_lo = (uint8_t)(bg & 0xFFU);

	if((bits == NULL) || (w == 0U) || (w > 320U))
	{
		return;
	}

	const uint8_t *row_bits = bits + (uint32_t)y * (uint32_t)row_bytes;

	ST7789_SetAddressWindow(x, y, (uint16_t)(x + w - 1U), y);

	for(uint16_t col = 0U; col < w; ++col)
	{
		uint16_t px = (uint16_t)(x + col);
		uint8_t bit = (row_bits[px >> 3] >> (7U - (px & 7U))) & 1U;

		row_buf[2U * col]      = bit ? fg_hi : bg_hi;
		row_buf[2U * col + 1U] = bit ? fg_lo : bg_lo;
	}

	ST7789_WriteData(row_buf, (size_t)w * 2U);
}

static int32_t ST7789_QuadCross(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py)
{
	return ((bx - ax) * (py - ay)) - ((by - ay) * (px - ax));
}

static bool ST7789_PointInQuad(int32_t px, int32_t py,
                               uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                               uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3)
{
	int32_t c0 = ST7789_QuadCross((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1, px, py);
	int32_t c1 = ST7789_QuadCross((int32_t)x1, (int32_t)y1, (int32_t)x2, (int32_t)y2, px, py);
	int32_t c2 = ST7789_QuadCross((int32_t)x2, (int32_t)y2, (int32_t)x3, (int32_t)y3, px, py);
	int32_t c3 = ST7789_QuadCross((int32_t)x3, (int32_t)y3, (int32_t)x0, (int32_t)y0, px, py);
	bool has_neg = (c0 < 0) || (c1 < 0) || (c2 < 0) || (c3 < 0);
	bool has_pos = (c0 > 0) || (c1 > 0) || (c2 > 0) || (c3 > 0);

	return !(has_neg && has_pos);
}

void ST7789_WriteStringSlanted(int16_t x0, int16_t y0, const char *str, FontDef font, uint16_t color,
                               int16_t along_dx, int16_t along_dy, int16_t shear_num, int16_t shear_den)
{
	int32_t cx;
	int32_t cy;

	if((str == NULL) || (shear_den == 0))
	{
		return;
	}

	cx = x0;
	cy = y0;

	while(*str != '\0')
	{
		char ch = *str++;

		if((ch < 32) || (ch > 126))
		{
			continue;
		}

		for(uint32_t row = 0U; row < font.height; ++row)
		{
			uint16_t bits = font.data[((uint32_t)ch - 32U) * font.height + row];

			for(uint32_t col = 0U; col < font.width; ++col)
			{
				if((bits << col) & 0x8000U)
				{
					int32_t sx = cx + (int32_t)col + ((int32_t)row * shear_num) / shear_den;
					int32_t sy = cy + (int32_t)row;

					if((sx >= 0) && (sy >= 0))
					{
						ST7789_DrawPixel((uint16_t)sx, (uint16_t)sy, color);
					}
				}
			}
		}

		cx += along_dx;
		cy += along_dy;
	}
}

void ST7789_WriteStringSlantedInQuad(int16_t x0, int16_t y0, const char *str, FontDef font, uint16_t color,
                                     int16_t along_dx, int16_t along_dy, int16_t shear_num, int16_t shear_den,
                                     uint16_t qx0, uint16_t qy0, uint16_t qx1, uint16_t qy1,
                                     uint16_t qx2, uint16_t qy2, uint16_t qx3, uint16_t qy3)
{
	int32_t cx;
	int32_t cy;

	if((str == NULL) || (shear_den == 0))
	{
		return;
	}

	cx = x0;
	cy = y0;

	while(*str != '\0')
	{
		char ch = *str++;

		if((ch < 32) || (ch > 126))
		{
			continue;
		}

		for(uint32_t row = 0U; row < font.height; ++row)
		{
			uint16_t bits = font.data[((uint32_t)ch - 32U) * font.height + row];

			for(uint32_t col = 0U; col < font.width; ++col)
			{
				if((bits << col) & 0x8000U)
				{
					int32_t sx = cx + (int32_t)col + ((int32_t)row * shear_num) / shear_den;
					int32_t sy = cy + (int32_t)row;

					if((sx >= 0) && (sy >= 0) &&
					   ST7789_PointInQuad(sx, sy, qx0, qy0, qx1, qy1, qx2, qy2, qx3, qy3))
					{
						ST7789_DrawPixel((uint16_t)sx, (uint16_t)sy, color);
					}
				}
			}
		}

		cx += along_dx;
		cy += along_dy;
	}
}

void ST7789_WriteString(uint16_t x, uint16_t y, const char *str, FontDef font, uint16_t color, uint16_t bgcolor)
{
	ST7789_Select();
	while (*str) {
		if (x + font.width >= ST7789_WIDTH) {
			x = 0;
			y += font.height;
			if (y + font.height >= ST7789_HEIGHT) {
				break;
			}

			if (*str == ' ') {
				// skip spaces in the beginning of the new line
				str++;
				continue;
			}
		}
		ST7789_WriteChar(x, y, *str, font, color, bgcolor);
		x += font.width;
		str++;
	}
	ST7789_UnSelect();
}

/** 
 * @brief Draw a filled Rectangle with single color
 * @param  x&y -> coordinates of the starting point
 * @param w&h -> width & height of the Rectangle
 * @param color -> color of the Rectangle
 * @return  none
 */
void ST7789_DrawFilledRectangle(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT) {
		return;
	}

	if ((x + w) >= ST7789_WIDTH) {
		w = ST7789_WIDTH - x;
	}
	if ((y + h) >= ST7789_HEIGHT) {
		h = ST7789_HEIGHT - y;
	}

	ST7789_Fill(x, y, (uint16_t)(x + w - 1U), (uint16_t)(y + h - 1U), color);
}

/** 
 * @brief Draw a Triangle with single color
 * @param  xi&yi -> 3 coordinates of 3 top points.
 * @param color ->color of the lines
 * @return  none
 */
void ST7789_DrawTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, uint16_t color)
{
	ST7789_Select();
	/* Draw lines */
	ST7789_DrawLine(x1, y1, x2, y2, color);
	ST7789_DrawLine(x2, y2, x3, y3, color);
	ST7789_DrawLine(x3, y3, x1, y1, color);
	ST7789_UnSelect();
}

/** 
 * @brief Draw a filled Triangle with single color
 * @param  xi&yi -> 3 coordinates of 3 top points.
 * @param color ->color of the triangle
 * @return  none
 */
void ST7789_DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, uint16_t color)
{
	ST7789_Select();
	int16_t deltax = 0, deltay = 0, x = 0, y = 0, xinc1 = 0, xinc2 = 0,
			yinc1 = 0, yinc2 = 0, den = 0, num = 0, numadd = 0, numpixels = 0,
			curpixel = 0;

	deltax = ABS(x2 - x1);
	deltay = ABS(y2 - y1);
	x = x1;
	y = y1;

	if (x2 >= x1) {
		xinc1 = 1;
		xinc2 = 1;
	}
	else {
		xinc1 = -1;
		xinc2 = -1;
	}

	if (y2 >= y1) {
		yinc1 = 1;
		yinc2 = 1;
	}
	else {
		yinc1 = -1;
		yinc2 = -1;
	}

	if (deltax >= deltay) {
		xinc1 = 0;
		yinc2 = 0;
		den = deltax;
		num = deltax / 2;
		numadd = deltay;
		numpixels = deltax;
	}
	else {
		xinc2 = 0;
		yinc1 = 0;
		den = deltay;
		num = deltay / 2;
		numadd = deltax;
		numpixels = deltay;
	}

	for (curpixel = 0; curpixel <= numpixels; curpixel++) {
		ST7789_DrawLine(x, y, x3, y3, color);

		num += numadd;
		if (num >= den) {
			num -= den;
			x += xinc1;
			y += yinc1;
		}
		x += xinc2;
		y += yinc2;
	}
	ST7789_UnSelect();
}

/** 
 * @brief Draw a Filled circle with single color
 * @param x0&y0 -> coordinate of circle center
 * @param r -> radius of circle
 * @param color -> color of circle
 * @return  none
 */
void ST7789_DrawFilledCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color)
{
	ST7789_Select();
	int16_t f = 1 - r;
	int16_t ddF_x = 1;
	int16_t ddF_y = -2 * r;
	int16_t x = 0;
	int16_t y = r;

	ST7789_DrawPixel(x0, y0 + r, color);
	ST7789_DrawPixel(x0, y0 - r, color);
	ST7789_DrawPixel(x0 + r, y0, color);
	ST7789_DrawPixel(x0 - r, y0, color);
	ST7789_DrawLine(x0 - r, y0, x0 + r, y0, color);

	while (x < y) {
		if (f >= 0) {
			y--;
			ddF_y += 2;
			f += ddF_y;
		}
		x++;
		ddF_x += 2;
		f += ddF_x;

		ST7789_DrawLine(x0 - x, y0 + y, x0 + x, y0 + y, color);
		ST7789_DrawLine(x0 + x, y0 - y, x0 - x, y0 - y, color);

		ST7789_DrawLine(x0 + y, y0 + x, x0 - y, y0 + x, color);
		ST7789_DrawLine(x0 + y, y0 - x, x0 - y, y0 - x, color);
	}
	ST7789_UnSelect();
}


/**
 * @brief Open/Close tearing effect line
 * @param tear -> Whether to tear
 * @return none
 */
void ST7789_TearEffect(uint8_t tear)
{
	ST7789_Select();
	ST7789_WriteCommand(tear ? 0x35 /* TEON */ : 0x34 /* TEOFF */);
	ST7789_UnSelect();
}


/** 
 * @brief A Simple test function for ST7789
 * @param  none
 * @return  none
 */
void ST7789_Test(void)
{
	//	If FLASH cannot storage anymore datas, please delete codes below.
	ST7789_Fill_Color(WHITE);
	ST7789_DrawImage(0, 0, 128, 128, (uint16_t *)saber);
	Delay_Ms(3000);
}
