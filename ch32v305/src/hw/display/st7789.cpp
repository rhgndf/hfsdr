#include "st7789.h"

#include "debug.h"
#include "hw/spi.h"

#include <algorithm>
#include <array>
#include <stddef.h>
#include <stdint.h>
#include <cstdlib>
#include <utility>

static constexpr uint16_t ST7789_PANEL_LONG_SIDE = 320U;
static constexpr uint16_t ST7789_PANEL_SHORT_SIDE = 240U;
static constexpr size_t ST7789_RGB565_BYTES_PER_PIXEL = 2U;

static uint16_t s_scroll_top = 0U;
static uint16_t s_scroll_bottom = ST7789_HEIGHT;
static uint16_t s_scroll_offset = 0U;
static uint8_t s_rotation = ST7789_ROTATION;

enum class ST7789TransactionKind : uint8_t
{
	Command,
	Data,
};

class ST7789TransactionGuard
{
public:
	explicit ST7789TransactionGuard(ST7789TransactionKind kind)
	{
		// Wait for the previous transaction to finish, we launch all DMA async
		spi_hw_wait_dma();
		// Do a SET/RESET to reset the SPI state machine on the display
		GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_SET);
		// TODO: should there be a delay here?
		GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_RESET);
		if(kind == ST7789TransactionKind::Command)
		{
			GPIO_WriteBit(ST7789_RS_GPIO_PORT, ST7789_RS_GPIO_PIN, Bit_RESET);
		}
		else
		{
			GPIO_WriteBit(ST7789_RS_GPIO_PORT, ST7789_RS_GPIO_PIN, Bit_SET);
		}
	}

	~ST7789TransactionGuard()
	{
	}

	ST7789TransactionGuard(const ST7789TransactionGuard &) = delete;
	ST7789TransactionGuard &operator=(const ST7789TransactionGuard &) = delete;
};

static uint16_t ST7789_ActiveWidth(void)
{
	return ((s_rotation & 1U) != 0U) ? ST7789_PANEL_LONG_SIDE : ST7789_PANEL_SHORT_SIDE;
}

static uint16_t ST7789_ActiveHeight(void)
{
	return ((s_rotation & 1U) != 0U) ? ST7789_PANEL_SHORT_SIDE : ST7789_PANEL_LONG_SIDE;
}

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

static std::array<uint8_t, 2U> ST7789_U16Bytes(uint16_t value)
{
	return {
		(uint8_t)(value >> 8),
		(uint8_t)(value & 0xFFU),
	};
}

static std::array<uint8_t, 4U> ST7789_U16RangeBytes(uint16_t start, uint16_t end)
{
	return {
		(uint8_t)(start >> 8), (uint8_t)(start & 0xFFU),
		(uint8_t)(end >> 8), (uint8_t)(end & 0xFFU),
	};
}

static uint16_t ST7789_ClipExtent(uint16_t start, uint16_t extent, uint16_t limit)
{
	if(start >= limit)
	{
		return 0U;
	}

	return std::clamp<uint16_t>(extent, 0U, (uint16_t)(limit - start));
}

static bool ST7789_ReadBitmap1bppBit(const uint8_t *row_bits, uint16_t x)
{
	return (((row_bits[x >> 3U] >> (7U - (x & 7U))) & 1U) != 0U);
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

	g.GPIO_Pin = ST7789_RS_GPIO_PIN;
	GPIO_Init(ST7789_RS_GPIO_PORT, &g);

	g.GPIO_Pin = ST7789_RST_GPIO_PIN;
	GPIO_Init(ST7789_RST_GPIO_PORT, &g);
	ST7789_RST_Release();

	g.GPIO_Pin = ST7789_CS_GPIO_PIN;
	GPIO_Init(ST7789_CS_GPIO_PORT, &g);
	GPIO_WriteBit(ST7789_CS_GPIO_PORT, ST7789_CS_GPIO_PIN, Bit_SET);

	GPIO_WriteBit(ST7789_RS_GPIO_PORT, ST7789_RS_GPIO_PIN, Bit_SET);

	spi_hw_init();
}

/**
 * @brief Write command to ST7789 controller
 * @param cmd -> command to write
 * @return none
 */
static void ST7789_WriteCommand(uint8_t cmd)
{
	ST7789TransactionGuard guard{ST7789TransactionKind::Command};
	ST7789_SPI_TxU8(cmd);
}

/**
 * @brief Write data to ST7789 controller via SPI DMA
 * @param buff -> pointer of data buffer
 * @param buff_size -> size of the data buffer
 * @return none
 */
static void ST7789_WriteData(const uint8_t *buff, size_t buff_size)
{
	if ((buff == nullptr) || (buff_size == 0U)) {
		return;
	}

	ST7789TransactionGuard guard{ST7789TransactionKind::Data};
	spi_hw_transfer_dma(buff, buff_size);
}

template<size_t N>
static void ST7789_WriteDataArray(const std::array<uint8_t, N> &data)
{
	ST7789_WriteData(data.data(), data.size());
}

static void ST7789_WritePixelData(uint16_t color)
{
	ST7789_WriteDataArray(ST7789_U16Bytes(color));
}

/* Stream a single RGB565 pixel value to the panel pixel_count times via DMA. */
static void ST7789_WriteRepeatedPixel(uint16_t color, uint32_t pixel_count)
{
	if (pixel_count == 0U) {
		return;
	}

	ST7789TransactionGuard guard{ST7789TransactionKind::Data};
	spi_hw_transfer_dma_repeat_u16(color, pixel_count);
}
/**
 * @brief Write data to ST7789 controller, simplify for 8bit data.
 * data -> data to write
 * @return none
 */
static void ST7789_WriteSmallData(uint8_t data)
{
	ST7789TransactionGuard guard{ST7789TransactionKind::Data};
	ST7789_SPI_TxU8(data);
}

/**
 * @brief Set the rotation direction of the display
 * @param m -> rotation parameter(please refer it in st7789.h)
 * @return none
 */
void ST7789_SetRotation(uint8_t m)
{
	s_rotation = m;

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
	uint16_t x_start = x0 + X_SHIFT;
	uint16_t x_end = x1 + X_SHIFT;
	uint16_t y_start = y0 + Y_SHIFT;
	uint16_t y_end = y1 + Y_SHIFT;
	
	/* Column Address set */
	ST7789_WriteCommand(ST7789_CASET); 
	ST7789_WriteDataArray(ST7789_U16RangeBytes(x_start, x_end));

	/* Row Address set */
	ST7789_WriteCommand(ST7789_RASET);
	ST7789_WriteDataArray(ST7789_U16RangeBytes(y_start, y_end));

	/* Write to RAM */
	ST7789_WriteCommand(ST7789_RAMWR);
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

	/* PVGAMCTRL (E0h): positive-polarity gamma curve.
	 * The 14 parameters tune the source-driver gray-scale voltage points
	 * VP0..VP63 plus interpolation controls JP0/JP1 for the panel. */
	ST7789_WriteCommand(0xE0);
	{
		// TODO: find out what actual gamma values we need
		uint8_t data[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
		ST7789_WriteData(data, sizeof(data));
	}

	/* NVGAMCTRL (E1h): negative-polarity gamma curve.
	 * Same layout as PVGAMCTRL, but sets VN0..VN63 and JN0/JN1 to balance
	 * the panel response on the opposite drive polarity. */
    ST7789_WriteCommand(0xE1);
	{
		// TODO: same here
		uint8_t data[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
		ST7789_WriteData(data, sizeof(data));
	}
	
    ST7789_WriteCommand (ST7789_INVON);		//	Inversion ON
	ST7789_WriteCommand (ST7789_SLPOUT);	//	Out of sleep mode
	Delay_Ms(5);
  	ST7789_WriteCommand (ST7789_NORON);		//	Normal Display on
  	ST7789_WriteCommand (ST7789_DISPON);	//	Main screen turned on	

	ST7789_Fill_Color(BLACK);				//	Fill with Black.
	GPIO_WriteBit(ST7789_LEDK_GPIO_PORT, ST7789_LEDK_GPIO_PIN, Bit_RESET);
}

/**
 * @brief Fill the DisplayWindow with single color
 * @param color -> color to Fill with
 * @return none
 */
uint16_t ST7789_GetWidth(void)
{
	return ST7789_ActiveWidth();
}

uint16_t ST7789_GetHeight(void)
{
	return ST7789_ActiveHeight();
}

void ST7789_Fill_Color(uint16_t color)
{
	uint16_t const w = ST7789_ActiveWidth();
	uint16_t const h = ST7789_ActiveHeight();

	ST7789_SetAddressWindow(0U, 0U, (uint16_t)(w - 1U), (uint16_t)(h - 1U));
	ST7789_WriteRepeatedPixel(color, (uint32_t)w * (uint32_t)h);
}

/**
 * @brief Draw a Pixel
 * @param x&y -> coordinate to Draw
 * @param color -> color of the Pixel
 * @return none
 */
void ST7789_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
	if((x >= ST7789_ActiveWidth()) || (y >= ST7789_ActiveHeight()))
	{
		return;
	}
	
	ST7789_SetAddressWindow(x, y, x, y);
	ST7789_WritePixelData(color);
}

void ST7789_DrawColorLine(uint16_t x, uint16_t y, const uint16_t *colors, uint16_t width)
{
	if ((colors == nullptr) || (width == 0U) || (x >= ST7789_WIDTH) || (y >= ST7789_HEIGHT)) {
		return;
	}

	uint16_t const visible_width = ST7789_ClipExtent(x, width, ST7789_WIDTH);

	ST7789_SetAddressWindow(x, y, x + visible_width - 1U, y);
	ST7789TransactionGuard guard{ST7789TransactionKind::Data};
	spi_hw_transfer_dma_u16(colors, visible_width);
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
		std::array<uint8_t, 6U> data = {
			(uint8_t)(scroll_top >> 8), (uint8_t)(scroll_top & 0xFFU),
			(uint8_t)(height >> 8), (uint8_t)(height & 0xFFU),
			(uint8_t)(bottom_fixed >> 8), (uint8_t)(bottom_fixed & 0xFFU),
		};

		s_scroll_top = top;
		s_scroll_bottom = bottom;
		s_scroll_offset = 0U;

		ST7789_WriteCommand(ST7789_VSCRDEF);
		ST7789_WriteDataArray(data);
	}

	int32_t offset = (int32_t)s_scroll_offset + (int32_t)scroll_rows;
	offset %= (int32_t)height;
	if(offset < 0) {
		offset += (int32_t)height;
	}
	s_scroll_offset = (uint16_t)offset;

	uint16_t scroll_start = scroll_top + s_scroll_offset;
	ST7789_WriteCommand(ST7789_VSCSAD);
	ST7789_WriteDataArray(ST7789_U16Bytes(scroll_start));

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
	std::array<uint8_t, 6U> data = {
		(uint8_t)(scroll_top >> 8), (uint8_t)(scroll_top & 0xFFU),
		(uint8_t)(height >> 8), (uint8_t)(height & 0xFFU),
		(uint8_t)(bottom_fixed >> 8), (uint8_t)(bottom_fixed & 0xFFU),
	};

	s_scroll_top = 0U;
	s_scroll_bottom = height;
	s_scroll_offset = 0U;

	ST7789_WriteCommand(ST7789_VSCRDEF);
	ST7789_WriteDataArray(data);

	ST7789_WriteCommand(ST7789_VSCSAD);
	ST7789_WriteDataArray(ST7789_U16Bytes(0U));
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
	if((xEnd >= ST7789_ActiveWidth()) || (yEnd >= ST7789_ActiveHeight()) ||
	    (xSta > xEnd) || (ySta > yEnd))
	{
		return;
	}

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
	ST7789_Fill(x - 1, y - 1, x + 1, y + 1, color);
}

/**
 * @brief Draw a line with single color
 * @param x1&y1 -> coordinate of the start point
 * @param x2&y2 -> coordinate of the end point
 * @param color -> color of the line to Draw
 * @return none
 */
void ST7789_DrawLine(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                     uint16_t color)
{
	bool const steep = std::abs(y1 - y0) > std::abs(x1 - x0);
	if(steep)
	{
		std::swap(x0, y0);
		std::swap(x1, y1);
	}

	if(x0 > x1)
	{
		std::swap(x0, x1);
		std::swap(y0, y1);
	}

	int16_t const dx = (int16_t)(x1 - x0);
	int16_t const dy = std::abs(y1 - y0);
	int16_t const ystep = (y0 < y1) ? 1 : -1;
	int16_t err = dx / 2;

	for(; x0 <= x1; ++x0)
	{
		if(steep)
		{
			ST7789_DrawPixel(y0, x0, color);
		}
		else
		{
			ST7789_DrawPixel(x0, y0, color);
		}
		err -= dy;
		if(err < 0)
		{
			y0 += ystep;
			err += dx;
		}
	}
}

/*
 * Rasterize line with Bresenham; consecutive pixels on same row (non-steep) or same
 * column (steep swapped case) merged into ST7789_Fill for SPI efficiency.
 */
void ST7789_DrawLineFills(uint16_t xl0,
                          uint16_t yl0,
                          uint16_t xl1,
                          uint16_t yl1,
                          uint16_t color)
{
	uint16_t steep = std::abs(yl1 - yl0) > std::abs(xl1 - xl0);

	if (steep)
	{
		std::swap(xl0, yl0);
		std::swap(xl1, yl1);
	}

	if(xl0 > xl1)
	{
		std::swap(xl0, xl1);
		std::swap(yl0, yl1);
	}

	int16_t dx = (int16_t)(xl1 - xl0);
	int16_t dy = std::abs((int16_t)(yl1 - yl0));

	int16_t err = dx / 2;
	int16_t ystep = ((int16_t)yl0 < (int16_t)yl1) ? 1 : -1;

	if(steep)
	{
		/* Screen (sx,sy)=(yl, xl); xl advances each step → merge columns on fixed sx */
		bool have_run = false;
		uint16_t sx_col = 0U;
		uint16_t ys_lo = 0U;
		uint16_t ys_hi = 0U;

		for(; xl0 <= xl1; ++xl0)
		{
			uint16_t const sx_run = yl0;
			uint16_t const sy_run = xl0;

			if(!have_run)
			{
				sx_col = sx_run;
				ys_lo = sy_run;
				ys_hi = sy_run;
				have_run = true;
			}
			else if(sx_run != sx_col || sy_run != (uint16_t)(ys_hi + 1U))
			{
				ST7789_Fill(sx_col, ys_lo, sx_col, ys_hi, color);
				sx_col = sx_run;
				ys_lo = sy_run;
				ys_hi = sy_run;
			}
			else
			{
				ys_hi = sy_run;
			}

			err -= dy;
			if(err < 0)
			{
				yl0 += ystep;
				err += dx;
			}
		}

		if(have_run)
		{
			ST7789_Fill(sx_col, ys_lo, sx_col, ys_hi, color);
		}
	}
	else
	{
		bool have_run = false;
		uint16_t row_y = 0U;
		uint16_t xs_lo = 0U;
		uint16_t xs_hi = 0U;

		for(; xl0 <= xl1; ++xl0)
		{
			if(!have_run)
			{
				row_y = yl0;
				xs_lo = xl0;
				xs_hi = xl0;
				have_run = true;
			}
			else if((yl0 != row_y) || (xl0 != (uint16_t)(xs_hi + 1U)))
			{
				ST7789_Fill(xs_lo, row_y, xs_hi, row_y, color);
				row_y = yl0;
				xs_lo = xl0;
				xs_hi = xl0;
			}
			else
			{
				xs_hi = xl0;
			}

			err -= dy;
			if(err < 0)
			{
				yl0 += ystep;
				err += dx;
			}
		}

		if(have_run)
		{
			ST7789_Fill(xs_lo, row_y, xs_hi, row_y, color);
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
	ST7789_DrawLine(x1, y1, x2, y1, color);
	ST7789_DrawLine(x1, y1, x1, y2, color);
	ST7789_DrawLine(x1, y2, x2, y2, color);
	ST7789_DrawLine(x2, y1, x2, y2, color);
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

	ST7789_SetAddressWindow(x, y, x + w - 1, y + h - 1);
	ST7789TransactionGuard guard{ST7789TransactionKind::Data};
	spi_hw_transfer_dma_u16(data, (size_t)w * (size_t)h);
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
	uint8_t row_buf[ST7789_PANEL_LONG_SIDE * ST7789_RGB565_BYTES_PER_PIXEL];

	if ((bits == nullptr) || (w == 0U) || (h == 0U) || (w > ST7789_PANEL_LONG_SIDE))
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
			bool const bit = ST7789_ReadBitmap1bppBit(row_bits, col);
			row_buf[2U * col]      = bit ? fg_hi : bg_hi;
			row_buf[2U * col + 1U] = bit ? fg_lo : bg_lo;
		}
		ST7789_WriteData(row_buf, (size_t)w * ST7789_RGB565_BYTES_PER_PIXEL);
	}
}

/**
 * @brief Invert Fullscreen color
 * @param invert -> Whether to invert
 * @return none
 */
void ST7789_InvertColors(uint8_t invert)
{
	ST7789_WriteCommand(invert ? 0x21 /* INVON */ : 0x20 /* INVOFF */);
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
	ST7789_SetAddressWindow(x, y, x + font.width - 1, y + font.height - 1);

	for (uint32_t row = 0U; row < font.height; ++row) {
		uint32_t const bits = font.data[((uint32_t)ch - 32U) * font.height + row];
		for (uint32_t col = 0U; col < font.width; ++col) {
			uint16_t const pixel_color = ((bits << col) & 0x8000U) ? color : bgcolor;
			ST7789_WritePixelData(pixel_color);
		}
	}
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
	static uint8_t row_buf[ST7789_PANEL_LONG_SIDE * ST7789_RGB565_BYTES_PER_PIXEL];
	const uint16_t row_bytes = (bitmap_w + 7U) / 8U;
	const uint8_t fg_hi = (uint8_t)(fg >> 8);
	const uint8_t fg_lo = (uint8_t)(fg & 0xFFU);
	const uint8_t bg_hi = (uint8_t)(bg >> 8);
	const uint8_t bg_lo = (uint8_t)(bg & 0xFFU);

	if((bits == nullptr) || (w == 0U) || (w > ST7789_PANEL_LONG_SIDE))
	{
		return;
	}

	const uint8_t *row_bits = bits + (uint32_t)y * (uint32_t)row_bytes;

	ST7789_SetAddressWindow(x, y, (uint16_t)(x + w - 1U), y);

	for(uint16_t col = 0U; col < w; ++col)
	{
		uint16_t px = (uint16_t)(x + col);
		bool const bit = ST7789_ReadBitmap1bppBit(row_bits, px);

		row_buf[2U * col]      = bit ? fg_hi : bg_hi;
		row_buf[2U * col + 1U] = bit ? fg_lo : bg_lo;
	}

	ST7789_WriteData(row_buf, (size_t)w * ST7789_RGB565_BYTES_PER_PIXEL);
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

	if((str == nullptr) || (shear_den == 0))
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

	if((str == nullptr) || (shear_den == 0))
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

	w = ST7789_ClipExtent(x, w, ST7789_WIDTH);
	h = ST7789_ClipExtent(y, h, ST7789_HEIGHT);

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
	/* Draw lines */
	ST7789_DrawLine(x1, y1, x2, y2, color);
	ST7789_DrawLine(x2, y2, x3, y3, color);
	ST7789_DrawLine(x3, y3, x1, y1, color);
}

/** 
 * @brief Draw a filled Triangle with single color
 * @param  xi&yi -> 3 coordinates of 3 top points.
 * @param color ->color of the triangle
 * @return  none
 */
void ST7789_DrawFilledTriangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t x3, uint16_t y3, uint16_t color)
{
	int16_t const deltax = std::abs(x2 - x1);
	int16_t const deltay = std::abs(y2 - y1);
	int16_t x = (int16_t)x1;
	int16_t y = (int16_t)y1;
	int16_t xinc1 = (x2 >= x1) ? 1 : -1;
	int16_t xinc2 = xinc1;
	int16_t yinc1 = (y2 >= y1) ? 1 : -1;
	int16_t yinc2 = yinc1;
	int16_t den;
	int16_t num;
	int16_t numadd;
	int16_t numpixels;

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

	for (int16_t curpixel = 0; curpixel <= numpixels; ++curpixel) {
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
}


/**
 * @brief Open/Close tearing effect line
 * @param tear -> Whether to tear
 * @return none
 */
void ST7789_TearEffect(uint8_t tear)
{
	ST7789_WriteCommand(tear ? 0x35 /* TEON */ : 0x34 /* TEOFF */);
}


/** 
 * @brief A Simple test function for ST7789
 * @param  none
 * @return  none
 */
void ST7789_Test(void)
{
	//	If FLASH cannot storage anymore datas, please delete codes below.
	ST7789_DrawImage(0, 0, 128, 128, (uint16_t *)saber);
	Delay_Ms(3000);
}
