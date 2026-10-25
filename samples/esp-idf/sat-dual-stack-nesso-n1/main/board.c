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
#define PIN_LORA_BUSY   19
#define PIN_IR_TX       9
#define PIN_BUZZER      11

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

/* Expander 0 (0x43) LoRa control pins (M5Stack Nesso N1 pin map) */
#define EXP0_LORA_LNA   5
#define EXP0_LORA_ANT   6
#define EXP0_LORA_NRST  7

#define SX1262_SET_SLEEP 0x84
#define LORA_SPI_HZ     (8 * 1000 * 1000)

/* BMI270 IMU */
#define IMU_ADDR        0x68
#define IMU_REG_CHIP_ID 0x00
#define IMU_REG_PWR_CONF 0x7C
#define IMU_REG_PWR_CTRL 0x7D
#define IMU_ADV_PWR_SAVE 0x01

/* Expander 1 (0x44) pins */
#define EXP1_LCD_RESET  1
#define EXP1_GROVE_PWR  2
#define EXP1_VIN_DETECT 5
#define EXP1_LCD_BL     6
#define EXP1_LED        7

#define TOUCH_ADDR      0x38
#define TOUCH_REG_STAT  0x02
/* FT6336 power mode: 0 active, 1 monitor (low power). */
#define TOUCH_REG_PMODE 0xA5

/* AW32001 charger */
#define CHG_ADDR        0x49
#define CHG_REG_PWR_CFG 0x01
#define CHG_REG_TIMER   0x05
#define CHG_REG_STATUS  0x08
#define CHG_CEB_BIT     3
#define CHG_HIZ_BIT     4
#define CHG_WD_MASK     0x60

#define GAUGE_ADDR      0x55
#define GAUGE_REG_VOLT  0x08
#define GAUGE_REG_CURR  0x0C
#define GAUGE_REG_AVG   0x14
#define GAUGE_REG_TEMP  0x28
#define GAUGE_REG_RM    0x10
#define GAUGE_REG_FCC   0x12

/*
 * Same bus settings as HubbleNetwork/hitl-shell's Nesso N1 port. At 400 kHz
 * with a 50 ms timeout the bus locked up for minutes at a time, likely while
 * the BQ27220 held the clock low during an internal update.
 */
#define I2C_SPEED_HZ    100000
#define I2C_TIMEOUT_MS  1000

static i2c_master_bus_handle_t _i2c_bus;
static i2c_master_dev_handle_t _exp0;
static i2c_master_dev_handle_t _exp1;
static i2c_master_dev_handle_t _touch;
static i2c_master_dev_handle_t _gauge;
static i2c_master_dev_handle_t _chg;
static i2c_master_dev_handle_t _imu;

static esp_lcd_panel_io_handle_t _lcd_io;
static esp_lcd_panel_handle_t _lcd_panel;

/* Failed transfers per chip, logged with the battery to find a bad actor. */
static uint32_t _errs_exp0;
static uint32_t _errs_exp1;
static uint32_t _errs_touch;
static uint32_t _errs_chg;
static uint32_t _errs_gauge;

static void _count_error(i2c_master_dev_handle_t dev)
{
	if (dev == _exp0) {
		_errs_exp0++;
	} else if (dev == _exp1) {
		_errs_exp1++;
	} else if (dev == _touch) {
		_errs_touch++;
	} else if (dev == _chg) {
		_errs_chg++;
	} else if (dev == _gauge) {
		_errs_gauge++;
	}
}

static esp_err_t _reg_write(i2c_master_dev_handle_t dev, uint8_t reg,
			    uint8_t val)
{
	uint8_t buf[2] = {reg, val};
	esp_err_t err = i2c_master_transmit(dev, buf, sizeof(buf),
					    I2C_TIMEOUT_MS);

	if (err != ESP_OK) {
		_count_error(dev);
	}
	return err;
}

static esp_err_t _reg_read(i2c_master_dev_handle_t dev, uint8_t reg,
			   uint8_t *out, size_t len)
{
	esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, out, len,
						    I2C_TIMEOUT_MS);

	if (err != ESP_OK) {
		_count_error(dev);
	}
	return err;
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
		.scl_speed_hz = I2C_SPEED_HZ,
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
	dev_cfg.device_address = CHG_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(_i2c_bus, &dev_cfg,
						      &_chg),
			    TAG, "charger");
	dev_cfg.device_address = IMU_ADDR;
	ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(_i2c_bus, &dev_cfg,
						      &_imu),
			    TAG, "imu");
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

/*
 * Same setup as the Arduino library's NessoBattery::begin(): charging on,
 * input not in Hi-Z, and the charger watchdog off. With the watchdog on and
 * nobody feeding it, the charger resets its settings every 160 s.
 */
static esp_err_t _charger_init(void)
{
	uint8_t timer;

	ESP_RETURN_ON_ERROR(_reg_update(_chg, CHG_REG_PWR_CFG, CHG_CEB_BIT,
					false),
			    TAG, "charge enable");
	ESP_RETURN_ON_ERROR(_reg_update(_chg, CHG_REG_PWR_CFG, CHG_HIZ_BIT,
					false),
			    TAG, "hi-z off");
	ESP_RETURN_ON_ERROR(_reg_read(_chg, CHG_REG_TIMER, &timer, 1), TAG,
			    "timer");
	return _reg_write(_chg, CHG_REG_TIMER, timer & ~CHG_WD_MASK);
}

/*
 * The sample never uses LoRa, but the SX1262 powers up in standby with its
 * LNA and antenna switch possibly enabled. Turn those off, reset it to a
 * known state, and put it into sleep (cold start) over the shared SPI bus.
 */
static esp_err_t _lora_sleep(void)
{
	spi_device_interface_config_t dev_cfg = {
		.clock_speed_hz = LORA_SPI_HZ,
		.mode = 0,
		.spics_io_num = PIN_LORA_CS,
		.queue_size = 1,
	};
	gpio_config_t busy = {
		.pin_bit_mask = 1ULL << PIN_LORA_BUSY,
		.mode = GPIO_MODE_INPUT,
	};
	uint8_t cmd[2] = {SX1262_SET_SLEEP, 0x00};
	spi_transaction_t t = {
		.length = sizeof(cmd) * 8,
		.tx_buffer = cmd,
	};
	spi_device_handle_t lora;
	int waited_ms = 0;

	ESP_RETURN_ON_ERROR(_exp_output(_exp0, EXP0_LORA_LNA, false), TAG,
			    "lna");
	ESP_RETURN_ON_ERROR(_exp_output(_exp0, EXP0_LORA_ANT, false), TAG,
			    "ant");
	ESP_RETURN_ON_ERROR(_exp_output(_exp0, EXP0_LORA_NRST, false), TAG,
			    "nrst");
	vTaskDelay(pdMS_TO_TICKS(2));
	ESP_RETURN_ON_ERROR(_reg_update(_exp0, EXP_REG_OUT, EXP0_LORA_NRST,
					true),
			    TAG, "nrst high");

	ESP_RETURN_ON_ERROR(gpio_config(&busy), TAG, "busy");
	while (gpio_get_level(PIN_LORA_BUSY) && waited_ms < 50) {
		vTaskDelay(pdMS_TO_TICKS(1));
		waited_ms++;
	}

	ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_HOST, &dev_cfg, &lora), TAG,
			    "lora device");
	ESP_RETURN_ON_ERROR(spi_device_polling_transmit(lora, &t), TAG,
			    "set sleep");
	ESP_LOGI(TAG, "LoRa radio asleep (busy cleared after %d ms)",
		 waited_ms);
	return ESP_OK;
}

/*
 * Without a loaded config the BMI270 should already idle, but check it and
 * make sure the accelerometer and gyro are off with advanced power save on.
 */
static void _imu_idle(void)
{
	uint8_t id = 0;
	uint8_t conf = 0;
	uint8_t ctrl = 0;

	if (_reg_read(_imu, IMU_REG_CHIP_ID, &id, 1) != ESP_OK ||
	    _reg_read(_imu, IMU_REG_PWR_CONF, &conf, 1) != ESP_OK ||
	    _reg_read(_imu, IMU_REG_PWR_CTRL, &ctrl, 1) != ESP_OK) {
		ESP_LOGW(TAG, "IMU did not answer");
		return;
	}
	ESP_LOGI(TAG, "IMU chip 0x%02x, PWR_CONF 0x%02x, PWR_CTRL 0x%02x", id,
		 conf, ctrl);

	if (ctrl != 0) {
		(void)_reg_write(_imu, IMU_REG_PWR_CTRL, 0);
	}
	if (!(conf & IMU_ADV_PWR_SAVE)) {
		vTaskDelay(pdMS_TO_TICKS(1));
		(void)_reg_write(_imu, IMU_REG_PWR_CONF,
				 conf | IMU_ADV_PWR_SAVE);
	}
}

/*
 * The IR LED (GPIO 9) and the active buzzer (GPIO 11) are both active high
 * (Zephyr's arduino_nesso_n1 devicetree). GPIO 9 is the boot strap pin, so
 * its pull-up is on at reset and the IR LED would draw continuously. Drive
 * both low, and keep that configuration in light sleep instead of IDF's
 * default isolation, which would float them. No gpio_hold: a hold that
 * survived a reset would strap GPIO 9 low and boot into download mode.
 */
static void _quiet_pins_init(void)
{
	gpio_config_t out = {
		.pin_bit_mask = (1ULL << PIN_IR_TX) | (1ULL << PIN_BUZZER),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
	};

	if (gpio_config(&out) != ESP_OK) {
		ESP_LOGW(TAG, "Could not drive the IR and buzzer pins low");
		return;
	}
	gpio_set_level(PIN_IR_TX, 0);
	gpio_set_level(PIN_BUZZER, 0);
	(void)gpio_sleep_sel_dis(PIN_IR_TX);
	(void)gpio_sleep_sel_dis(PIN_BUZZER);
}

esp_err_t board_init(void)
{
	_quiet_pins_init();
	ESP_RETURN_ON_ERROR(_i2c_init(), TAG, "i2c");
	ESP_RETURN_ON_ERROR(_expander_init(), TAG, "expander");
	ESP_RETURN_ON_ERROR(_lcd_init(), TAG, "lcd");
	if (_lora_sleep() != ESP_OK) {
		ESP_LOGW(TAG, "Could not put the LoRa radio to sleep");
	}
	/*
	 * Arduino names E0.P7 LORA_ENABLE, M5Stack SX_NRST; either way, low
	 * keeps the unused radio from drawing. Measured in an unplugged run.
	 */
	board_lora_enable(false);
	/* The sample uses nothing on the Grove port; keep its supply off. */
	board_grove_power(false);
	_imu_idle();
	if (_charger_init() != ESP_OK) {
		ESP_LOGW(TAG, "Charger did not answer; charging not configured");
	}
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

/*
 * Monitor mode rather than hibernate: leaving hibernate needs the
 * controller's reset line, which this board does not route to a pin the
 * sample controls.
 */
void board_touch_set_active(bool active)
{
	(void)_reg_write(_touch, TOUCH_REG_PMODE, active ? 0 : 1);
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

void board_lora_enable(bool on)
{
	(void)_exp_output(_exp0, EXP0_LORA_NRST, on);
}

void board_grove_power(bool on)
{
	(void)_exp_output(_exp1, EXP1_GROVE_PWR, on);
}

/* Hi-Z cuts USB input from the system, so it runs from the battery. */
void board_charger_hiz(bool hiz)
{
	(void)_reg_update(_chg, CHG_REG_PWR_CFG, CHG_HIZ_BIT, hiz);
}

bool board_usb_powered(void)
{
	uint8_t in;

	if (_reg_read(_exp1, EXP_REG_INPUT, &in, 1) != ESP_OK) {
		return true;
	}

	return (in >> EXP1_VIN_DETECT) & 1;
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

enum board_charge board_charge_state(void)
{
	uint8_t status;

	if (_reg_read(_chg, CHG_REG_STATUS, &status, 1) != ESP_OK) {
		return BOARD_CHARGE_UNKNOWN;
	}

	return (enum board_charge)((status >> 3) & 0x03);
}

bool board_battery_read(uint16_t *mv, int16_t *ma)
{
	uint8_t v[2];
	uint8_t c[2];

	if (_reg_read(_gauge, GAUGE_REG_VOLT, v, 2) != ESP_OK ||
	    _reg_read(_gauge, GAUGE_REG_CURR, c, 2) != ESP_OK) {
		return false;
	}

	*mv = v[0] | (v[1] << 8);
	*ma = (int16_t)(c[0] | (c[1] << 8));
	return true;
}

bool board_battery_avg_ma(int16_t *ma)
{
	uint8_t c[2];

	if (_reg_read(_gauge, GAUGE_REG_AVG, c, 2) != ESP_OK) {
		return false;
	}

	*ma = (int16_t)(c[0] | (c[1] << 8));
	return true;
}

void board_battery_log(void)
{
	static const char *const names[] = {"not charging", "pre-charge",
					    "charging", "full"};
	enum board_charge chg = board_charge_state();
	uint16_t mv = 0;
	int16_t ma = 0;
	bool gauge = board_battery_read(&mv, &ma);

	ESP_LOGI(TAG, "battery: %d%%, %s%u mV, %d mA, charger %s, usb %s",
		 board_battery_percent(), gauge ? "" : "gauge error, ", mv, ma,
		 chg == BOARD_CHARGE_UNKNOWN ? "not answering" : names[chg],
		 board_usb_powered() ? "in" : "out");
	ESP_LOGI(TAG,
		 "i2c errors: exp0=%lu exp1=%lu touch=%lu charger=%lu "
		 "gauge=%lu",
		 (unsigned long)_errs_exp0, (unsigned long)_errs_exp1,
		 (unsigned long)_errs_touch, (unsigned long)_errs_chg,
		 (unsigned long)_errs_gauge);
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
