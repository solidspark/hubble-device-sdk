/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "board.h"

static const char *TAG = "board";

/* Pin map from the Arduino Nesso N1 board variant */
#define PIN_I2C_SDA     10
#define PIN_I2C_SCL     8
#define PIN_LCD_MOSI    21
#define PIN_LCD_MISO    22
#define PIN_LCD_SCLK    20
#define PIN_LCD_DC      16
#define PIN_LCD_CS      17
#define PIN_LORA_CS     23

#define LCD_HOST        SPI2_HOST
#define LCD_PCLK_HZ     (40 * 1000 * 1000)

/*
 * The 135x240 panel sits inside the ST7789's 240x320 RAM. In landscape
 * (swap XY + mirror X) its window starts at column 40, row 53.
 */
#define LCD_GAP_X       40
#define LCD_GAP_Y       53

/* PI4IOE5V6408 IO expanders */
#define EXP0_ADDR       0x43
#define EXP1_ADDR       0x44
#define EXP_REG_RESET   0x01
#define EXP_REG_DIR     0x03
#define EXP_REG_OUT     0x05
#define EXP_REG_HIGH_Z  0x07
#define EXP_REG_PULL_EN 0x0B
#define EXP_REG_PULL_UP 0x0D
#define EXP_REG_INPUT   0x0F

/* Expander 0 (0x43) pins; the keys pull low when pressed */
#define EXP0_KEY1       0
#define EXP0_KEY2       1
#define EXP0_KEYS_MASK  ((1U << EXP0_KEY1) | (1U << EXP0_KEY2))

/* Expander 1 (0x44) pins */
#define EXP1_LCD_RESET  1
#define EXP1_LCD_BL     6
#define EXP1_LED        7

#define TOUCH_ADDR      0x38
#define TOUCH_REG_STAT  0x02

#define GAUGE_ADDR      0x55
#define GAUGE_REG_TEMP  0x28
#define GAUGE_REG_RM    0x10
#define GAUGE_REG_FCC   0x12

#define I2C_TIMEOUT_MS  50

static i2c_master_bus_handle_t _i2c_bus;
static i2c_master_dev_handle_t _exp0;
static i2c_master_dev_handle_t _exp1;
static i2c_master_dev_handle_t _touch;
static i2c_master_dev_handle_t _gauge;

static esp_lcd_panel_io_handle_t _lcd_io;
static esp_lcd_panel_handle_t _lcd_panel;

static esp_err_t _reg_write(i2c_master_dev_handle_t dev, uint8_t reg,
			    uint8_t val)
{
	uint8_t buf[2] = {reg, val};

	return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t _reg_read(i2c_master_dev_handle_t dev, uint8_t reg,
			   uint8_t *out, size_t len)
{
	return i2c_master_transmit_receive(dev, &reg, 1, out, len,
					   I2C_TIMEOUT_MS);
}

static esp_err_t _reg_update(i2c_master_dev_handle_t dev, uint8_t reg,
			     uint8_t bit, bool set)
{
	uint8_t val;

	ESP_RETURN_ON_ERROR(_reg_read(dev, reg, &val, 1), TAG, "read");
	val = set ? (val | (1U << bit)) : (val & ~(1U << bit));
	return _reg_write(dev, reg, val);
}

/* Latch the level before switching to output so the pin never glitches. */
static esp_err_t _exp_output(i2c_master_dev_handle_t dev, uint8_t pin,
			     bool level)
{
	ESP_RETURN_ON_ERROR(_reg_update(dev, EXP_REG_OUT, pin, level), TAG,
			    "out");
	ESP_RETURN_ON_ERROR(_reg_update(dev, EXP_REG_DIR, pin, true), TAG,
			    "dir");
	return _reg_update(dev, EXP_REG_HIGH_Z, pin, false);
}

static esp_err_t _i2c_init(void)
{
	i2c_master_bus_config_t bus_cfg = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = PIN_I2C_SDA,
		.scl_io_num = PIN_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.scl_speed_hz = 400000,
	};

	ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &_i2c_bus), TAG,
			    "i2c bus");

	dev_cfg.device_address = EXP0_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(_i2c_bus, &dev_cfg,
						      &_exp0),
			    TAG, "exp0");
	dev_cfg.device_address = EXP1_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(_i2c_bus, &dev_cfg,
						      &_exp1),
			    TAG, "exp1");
	dev_cfg.device_address = TOUCH_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(_i2c_bus, &dev_cfg,
						      &_touch),
			    TAG, "touch");
	dev_cfg.device_address = GAUGE_ADDR;
	return i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_gauge);
}

/*
 * Expander 1 also drives POWEROFF (P0) and expander 0 the LoRa enables, so
 * only the pins this board file owns are touched; the rest keep their reset
 * state (inputs).
 */
static esp_err_t _expander_init(void)
{
	for (uint8_t key = EXP0_KEY1; key <= EXP0_KEY2; key++) {
		ESP_RETURN_ON_ERROR(_reg_update(_exp0, EXP_REG_DIR, key, false),
				    TAG, "key dir");
		ESP_RETURN_ON_ERROR(_reg_update(_exp0, EXP_REG_PULL_UP, key,
						true),
				    TAG, "key pull-up");
		ESP_RETURN_ON_ERROR(_reg_update(_exp0, EXP_REG_PULL_EN, key,
						true),
				    TAG, "key pull enable");
	}

	ESP_RETURN_ON_ERROR(_exp_output(_exp1, EXP1_LCD_BL, false), TAG, "bl");
	ESP_RETURN_ON_ERROR(_exp_output(_exp1, EXP1_LED, true), TAG, "led");
	return _exp_output(_exp1, EXP1_LCD_RESET, true);
}

static esp_err_t _lcd_init(void)
{
	spi_bus_config_t bus_cfg = {
		.sclk_io_num = PIN_LCD_SCLK,
		.mosi_io_num = PIN_LCD_MOSI,
		.miso_io_num = PIN_LCD_MISO,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = BOARD_LCD_H_RES * 40 * sizeof(uint16_t),
	};
	esp_lcd_panel_io_spi_config_t io_cfg = {
		.dc_gpio_num = PIN_LCD_DC,
		.cs_gpio_num = PIN_LCD_CS,
		.pclk_hz = LCD_PCLK_HZ,
		.lcd_cmd_bits = 8,
		.lcd_param_bits = 8,
		.spi_mode = 0,
		.trans_queue_depth = 10,
	};
	esp_lcd_panel_dev_config_t panel_cfg = {
		.reset_gpio_num = -1,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
	};
	gpio_config_t lora_cs = {
		.pin_bit_mask = 1ULL << PIN_LORA_CS,
		.mode = GPIO_MODE_OUTPUT,
	};

	/* The SX1262 shares the SPI bus; keep it deselected. */
	ESP_RETURN_ON_ERROR(gpio_config(&lora_cs), TAG, "lora cs");
	gpio_set_level(PIN_LORA_CS, 1);

	ESP_RETURN_ON_ERROR(
		spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG,
		"spi bus");
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
				    (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg,
				    &_lcd_io),
			    TAG, "panel io");
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(_lcd_io, &panel_cfg,
						     &_lcd_panel),
			    TAG, "st7789");

	/* The panel reset line is on the expander, not a GPIO. */
	ESP_RETURN_ON_ERROR(_reg_update(_exp1, EXP_REG_OUT, EXP1_LCD_RESET,
					false),
			    TAG, "reset low");
	vTaskDelay(pdMS_TO_TICKS(20));
	ESP_RETURN_ON_ERROR(_reg_update(_exp1, EXP_REG_OUT, EXP1_LCD_RESET,
					true),
			    TAG, "reset high");
	vTaskDelay(pdMS_TO_TICKS(120));

	ESP_RETURN_ON_ERROR(esp_lcd_panel_init(_lcd_panel), TAG, "init");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(_lcd_panel, true), TAG,
			    "invert");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(_lcd_panel, true), TAG,
			    "swap");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(_lcd_panel, true, false), TAG,
			    "mirror");
	ESP_RETURN_ON_ERROR(
		esp_lcd_panel_set_gap(_lcd_panel, LCD_GAP_X, LCD_GAP_Y), TAG,
		"gap");
	return esp_lcd_panel_disp_on_off(_lcd_panel, true);
}

esp_err_t board_init(void)
{
	ESP_RETURN_ON_ERROR(_i2c_init(), TAG, "i2c");
	ESP_RETURN_ON_ERROR(_expander_init(), TAG, "expander");
	ESP_RETURN_ON_ERROR(_lcd_init(), TAG, "lcd");
	board_backlight_set(true);
	return ESP_OK;
}

esp_lcd_panel_io_handle_t board_lcd_io(void)
{
	return _lcd_io;
}

esp_lcd_panel_handle_t board_lcd_panel(void)
{
	return _lcd_panel;
}

bool board_touch_read(uint16_t *x, uint16_t *y)
{
	uint8_t buf[5];
	uint16_t raw_x;
	uint16_t raw_y;

	if (_reg_read(_touch, TOUCH_REG_STAT, buf, sizeof(buf)) != ESP_OK) {
		return false;
	}

	if ((buf[0] & 0x0F) == 0) {
		return false;
	}

	/* The FT6336 reports in the panel's native portrait orientation. */
	raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
	raw_y = ((buf[3] & 0x0F) << 8) | buf[4];

	*x = raw_y;
	*y = (BOARD_LCD_V_RES - 1) - raw_x;

	ESP_LOGD(TAG, "touch raw=(%u,%u) screen=(%u,%u)", raw_x, raw_y, *x,
		 *y);
	return true;
}

/* The LED is active-low (HubbleNetwork/hitl-shell hw_led.c). */
void board_led_set(bool on)
{
	(void)_reg_update(_exp1, EXP_REG_OUT, EXP1_LED, !on);
}

void board_backlight_set(bool on)
{
	(void)_reg_update(_exp1, EXP_REG_OUT, EXP1_LCD_BL, on);
}

bool board_key_pressed(void)
{
	uint8_t in;

	if (_reg_read(_exp0, EXP_REG_INPUT, &in, 1) != ESP_OK) {
		return false;
	}

	return (in & EXP0_KEYS_MASK) != EXP0_KEYS_MASK;
}

int board_battery_percent(void)
{
	uint8_t rm[2];
	uint8_t fcc[2];
	uint16_t full;

	if (_reg_read(_gauge, GAUGE_REG_RM, rm, 2) != ESP_OK ||
	    _reg_read(_gauge, GAUGE_REG_FCC, fcc, 2) != ESP_OK) {
		return -1;
	}

	full = fcc[0] | (fcc[1] << 8);
	if (full == 0) {
		return -1;
	}

	return ((rm[0] | (rm[1] << 8)) * 100U) / full;
}

int16_t board_battery_temp_centi_c(void)
{
	uint8_t raw[2];

	if (_reg_read(_gauge, GAUGE_REG_TEMP, raw, 2) != ESP_OK) {
		return INT16_MIN;
	}

	/* The gauge reports tenths of a kelvin. */
	return (int16_t)((raw[0] | (raw[1] << 8)) * 10 - 27315);
}
