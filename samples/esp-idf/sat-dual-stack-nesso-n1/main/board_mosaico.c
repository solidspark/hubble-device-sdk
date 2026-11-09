/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ESP-Mosaico (ESP32-S31) board support. Pin map from espressif/
 * esp-board-manager esp_boards/esp_mosaico and the ESP-Mosaico user guide.
 *
 * Two CoreBoard revisions are in the field and swap pins:
 *
 *            I2C SDA/SCL  LCD SCLK  LCD RST  status LED
 *   v1.0     GPIO0/1      GPIO44    GPIO42   GPIO3 (active low)
 *   v1.1-1.2 GPIO56/3     GPIO42    GPIO44   none
 *
 * Espressif writes the revision to eFuse USER_DATA. A board without it is
 * probed instead: the revision whose I2C bus answers at the fuel gauge's
 * address is the one fitted.
 */

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"

#include "board.h"

static const char *TAG = "board";

#define PIN_VCC_EN      60 /* display and module 3V3, active low */
#define PIN_LCD_CS      50
#define PIN_LCD_D0      36
#define PIN_LCD_D1      51
#define PIN_LCD_D2      35
#define PIN_LCD_D3      9
#define PIN_TOUCH_INT   6
#define PIN_KEY_AI      7  /* front key, active low */
#define PIN_KEY_BOOT    61 /* back key, active low */
#define PIN_MOTOR       8

#define LCD_HOST        SPI2_HOST
#define LCD_DRAW_LINES  48

/* The panel's own soft start: a hard switch on GPIO60 browns out the rail. */
#define VCC_RAMP_HZ     100000
#define VCC_RAMP_MS     50

#define I2C_HZ          400000
#define GAUGE_ADDR      0x55
#define GAUGE_REG_VOLT  0x08
#define GAUGE_REG_CURR  0x0C
#define GAUGE_REG_RM    0x10
#define GAUGE_REG_FCC   0x12
#define GAUGE_REG_AVG   0x14
/* InternalTemperature(), as on the Nesso N1: no cell thermistor is assumed */
#define GAUGE_REG_TEMP  0x28

#define HW_VERSION(major, minor) ((uint16_t)(((major) << 8) | (minor)))

struct board_pins {
	const char *name;
	int sda;
	int scl;
	int lcd_sclk;
	int lcd_rst;
	int led;
};

static const struct board_pins _rev_v10 = {"v1.0", 0, 1, 44, 42, 3};
static const struct board_pins _rev_v12 = {"v1.2", 56, 3, 42, 44, -1};

static const struct board_pins *_pins;
static i2c_master_bus_handle_t _i2c_bus;
static i2c_master_dev_handle_t _gauge;
static esp_lcd_panel_io_handle_t _lcd_io;
static esp_lcd_panel_handle_t _lcd_panel;
static esp_lcd_touch_handle_t _touch;
static uint32_t _errs_gauge;

static const struct board_pins *_rev_from_efuse(void)
{
	uint16_t version = 0;

	if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version,
				      sizeof(version) * 8) != ESP_OK) {
		return NULL;
	}

	switch (version) {
	case HW_VERSION(1, 0):
		return &_rev_v10;
	case HW_VERSION(1, 1):
	case HW_VERSION(1, 2):
		return &_rev_v12;
	default:
		ESP_LOGW(TAG, "eFuse hardware version 0x%04x not known",
			 version);
		return NULL;
	}
}

static esp_err_t _i2c_open(const struct board_pins *pins)
{
	const i2c_master_bus_config_t bus_cfg = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = pins->sda,
		.scl_io_num = pins->scl,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};

	return i2c_new_master_bus(&bus_cfg, &_i2c_bus);
}

static void _i2c_close(void)
{
	if (_i2c_bus != NULL) {
		(void)i2c_del_master_bus(_i2c_bus);
		_i2c_bus = NULL;
	}
}

static esp_err_t _i2c_init(void)
{
	const struct board_pins *order[] = {_rev_from_efuse(), &_rev_v12,
					    &_rev_v10};

	for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		if (order[i] == NULL) {
			continue;
		}
		if (_i2c_open(order[i]) != ESP_OK) {
			continue;
		}
		if (i == 0 ||
		    i2c_master_probe(_i2c_bus, GAUGE_ADDR, 50) == ESP_OK) {
			_pins = order[i];
			ESP_LOGI(TAG, "ESP-Mosaico %s (%s)", _pins->name,
				 i == 0 ? "eFuse" : "probed");
			return ESP_OK;
		}
		_i2c_close();
	}

	return ESP_ERR_NOT_FOUND;
}

static esp_err_t _gauge_init(void)
{
	const i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = GAUGE_ADDR,
		.scl_speed_hz = I2C_HZ,
	};

	return i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_gauge);
}

static esp_err_t _gauge_read(uint8_t reg, uint8_t *buf, size_t len)
{
	esp_err_t err;

	if (_gauge == NULL) {
		return ESP_ERR_INVALID_STATE;
	}
	err = i2c_master_transmit_receive(_gauge, &reg, 1, buf, len, 50);
	if (err != ESP_OK) {
		_errs_gauge++;
	}
	return err;
}

static esp_err_t _vcc_ramp_on(void)
{
	const ledc_timer_config_t timer = {
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.duty_resolution = LEDC_TIMER_8_BIT,
		.timer_num = LEDC_TIMER_1,
		.freq_hz = VCC_RAMP_HZ,
		.clk_cfg = LEDC_AUTO_CLK,
	};
	const ledc_channel_config_t channel = {
		.gpio_num = PIN_VCC_EN,
		.speed_mode = LEDC_LOW_SPEED_MODE,
		.channel = LEDC_CHANNEL_1,
		.timer_sel = LEDC_TIMER_1,
		.duty = 255,
	};
	const gpio_config_t out = {
		.pin_bit_mask = 1ULL << PIN_VCC_EN,
		.mode = GPIO_MODE_OUTPUT,
	};
	esp_err_t err;

	/* Active low: fade the duty from 255 (off) to 0 (on). */
	ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "vcc timer");
	ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "vcc channel");
	err = ledc_fade_func_install(0);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		return err;
	}
	ESP_RETURN_ON_ERROR(ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
						    LEDC_CHANNEL_1, 0,
						    VCC_RAMP_MS),
			    TAG, "vcc fade");
	ESP_RETURN_ON_ERROR(ledc_fade_start(LEDC_LOW_SPEED_MODE,
					    LEDC_CHANNEL_1,
					    LEDC_FADE_WAIT_DONE),
			    TAG, "vcc fade start");
	(void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
	ledc_fade_func_uninstall();

	/* Hand the pin back to GPIO with the latch already low. */
	ESP_RETURN_ON_ERROR(gpio_set_level(PIN_VCC_EN, 0), TAG, "vcc level");
	return gpio_config(&out);
}

static esp_err_t _lcd_init(void)
{
	const spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
		_pins->lcd_sclk, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2,
		PIN_LCD_D3,
		BOARD_LCD_H_RES * LCD_DRAW_LINES * sizeof(uint16_t));
	const esp_lcd_panel_io_spi_config_t io_cfg =
		CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
	/*
	 * No 480x480 CO5300 init table is published, so the driver's default
	 * (written for 466x466 modules) runs. Every draw sets its own window,
	 * which covers the whole 480x480 panel.
	 */
	const co5300_vendor_config_t vendor_cfg = {
		.flags.use_qspi_interface = 1,
	};
	const esp_lcd_panel_dev_config_t panel_cfg = {
		.reset_gpio_num = _pins->lcd_rst,
		.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
		.bits_per_pixel = 16,
		.vendor_config = (void *)&vendor_cfg,
	};

	ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg,
					       SPI_DMA_CH_AUTO),
			    TAG, "lcd bus");
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(
				    (esp_lcd_spi_bus_handle_t)LCD_HOST,
				    &io_cfg, &_lcd_io),
			    TAG, "lcd io");
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(_lcd_io, &panel_cfg,
						     &_lcd_panel),
			    TAG, "lcd panel");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(_lcd_panel), TAG, "lcd reset");
	ESP_RETURN_ON_ERROR(esp_lcd_panel_init(_lcd_panel), TAG, "lcd init");
	return esp_lcd_panel_disp_on_off(_lcd_panel, true);
}

static esp_err_t _touch_init(void)
{
	esp_lcd_panel_io_i2c_config_t io_cfg =
		ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
	const esp_lcd_touch_config_t tp_cfg = {
		.x_max = BOARD_LCD_H_RES,
		.y_max = BOARD_LCD_V_RES,
		.rst_gpio_num = GPIO_NUM_NC,
		.int_gpio_num = PIN_TOUCH_INT,
	};
	esp_lcd_panel_io_handle_t io;

	io_cfg.scl_speed_hz = I2C_HZ;
	ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(_i2c_bus, &io_cfg, &io),
			    TAG, "touch io");
	return esp_lcd_touch_new_i2c_cst9217(io, &tp_cfg, &_touch);
}

static void _gpio_init(void)
{
	const gpio_config_t keys = {
		.pin_bit_mask = (1ULL << PIN_KEY_AI) | (1ULL << PIN_KEY_BOOT),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
	};
	gpio_config_t outs = {
		.pin_bit_mask = 1ULL << PIN_MOTOR,
		.mode = GPIO_MODE_OUTPUT,
	};

	(void)gpio_config(&keys);
	(void)gpio_set_level(PIN_MOTOR, 0);
	if (_pins->led >= 0) {
		outs.pin_bit_mask |= 1ULL << _pins->led;
		(void)gpio_set_level(_pins->led, 1);
	}
	(void)gpio_config(&outs);
}

esp_err_t board_init(void)
{
	esp_err_t err;

	ESP_RETURN_ON_ERROR(_i2c_init(), TAG, "no ESP-Mosaico I2C bus answered");
	_gpio_init();

	err = _gauge_init();
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "fuel gauge: %s", esp_err_to_name(err));
	}

	ESP_RETURN_ON_ERROR(_vcc_ramp_on(), TAG, "display power");
	ESP_RETURN_ON_ERROR(_lcd_init(), TAG, "display");

	err = _touch_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "touch: %s", esp_err_to_name(err));
		_touch = NULL;
	}

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

esp_lcd_touch_handle_t board_touch_handle(void)
{
	return _touch;
}

bool board_touch_read(uint16_t *x, uint16_t *y)
{
	esp_lcd_touch_point_data_t point;
	uint8_t count = 0;

	if (_touch == NULL || esp_lcd_touch_read_data(_touch) != ESP_OK ||
	    esp_lcd_touch_get_data(_touch, &point, &count, 1) != ESP_OK ||
	    count == 0) {
		return false;
	}

	*x = point.x;
	*y = point.y;
	return true;
}

/* The CST9220 sleeps on its own between scans; nothing to switch. */
void board_touch_set_active(bool active)
{
	(void)active;
}

void board_touch_reg_write(uint8_t reg, uint8_t val)
{
	(void)reg;
	(void)val;
}

void board_touch_reg_log(void)
{
}

void board_led_set(bool on)
{
	if (_pins != NULL && _pins->led >= 0) {
		(void)gpio_set_level(_pins->led, on ? 0 : 1);
	}
}

/*
 * An AMOLED has no backlight: brightness 0 plus display-off blanks it, and
 * black pixels draw no power anyway.
 */
void board_backlight_set(bool on)
{
	if (_lcd_panel == NULL) {
		return;
	}
	if (on) {
		(void)esp_lcd_panel_disp_on_off(_lcd_panel, true);
		(void)esp_lcd_panel_co5300_set_brightness(_lcd_panel, 100);
	} else {
		(void)esp_lcd_panel_co5300_set_brightness(_lcd_panel, 0);
		(void)esp_lcd_panel_disp_on_off(_lcd_panel, false);
	}
}

void board_vibrate(uint32_t ms)
{
	(void)gpio_set_level(PIN_MOTOR, 1);
	vTaskDelay(pdMS_TO_TICKS(ms));
	(void)gpio_set_level(PIN_MOTOR, 0);
}

/* No LoRa radio, Grove port or charger control on this board. */
void board_lora_enable(bool on)
{
	(void)on;
}

void board_grove_power(bool on)
{
	(void)on;
}

void board_charger_hiz(bool hiz)
{
	(void)hiz;
}

/*
 * No VBUS sense pin is documented; the TP4057 charges whenever USB is in,
 * so a charging current (or a full cell at rest on USB) means USB power.
 */
bool board_usb_powered(void)
{
	int16_t ma;

	return board_battery_avg_ma(&ma) && ma > 0;
}

bool board_key_pressed(void)
{
	return gpio_get_level(PIN_KEY_AI) == 0 ||
	       gpio_get_level(PIN_KEY_BOOT) == 0;
}

int board_battery_percent(void)
{
	uint8_t rm[2];
	uint8_t fcc[2];
	uint16_t full;

	if (_gauge_read(GAUGE_REG_RM, rm, 2) != ESP_OK ||
	    _gauge_read(GAUGE_REG_FCC, fcc, 2) != ESP_OK) {
		return -1;
	}

	full = fcc[0] | (fcc[1] << 8);
	if (full == 0) {
		return -1;
	}

	return ((rm[0] | (rm[1] << 8)) * 100U) / full;
}

/* The TP4057 has no I2C; its state shows only on the charge LED. */
enum board_charge board_charge_state(void)
{
	return BOARD_CHARGE_UNKNOWN;
}

bool board_battery_read(uint16_t *mv, int16_t *ma)
{
	uint8_t v[2];
	uint8_t c[2];

	if (_gauge_read(GAUGE_REG_VOLT, v, 2) != ESP_OK ||
	    _gauge_read(GAUGE_REG_CURR, c, 2) != ESP_OK) {
		return false;
	}

	*mv = v[0] | (v[1] << 8);
	*ma = (int16_t)(c[0] | (c[1] << 8));
	return true;
}

bool board_battery_avg_ma(int16_t *ma)
{
	uint8_t c[2];

	if (_gauge_read(GAUGE_REG_AVG, c, 2) != ESP_OK) {
		return false;
	}

	*ma = (int16_t)(c[0] | (c[1] << 8));
	return true;
}

void board_battery_log(void)
{
	uint16_t mv = 0;
	int16_t ma = 0;
	bool gauge = board_battery_read(&mv, &ma);

	ESP_LOGI(TAG, "battery: %d%%, %s%u mV, %d mA, usb %s, gauge errors %lu",
		 board_battery_percent(), gauge ? "" : "gauge error, ", mv, ma,
		 board_usb_powered() ? "in" : "out",
		 (unsigned long)_errs_gauge);
}

int16_t board_battery_temp_centi_c(void)
{
	uint8_t raw[2];

	if (_gauge_read(GAUGE_REG_TEMP, raw, 2) != ESP_OK) {
		return INT16_MIN;
	}

	/* The gauge reports tenths of a kelvin. */
	return (int16_t)((raw[0] | (raw[1] << 8)) * 10 - 27315);
}
