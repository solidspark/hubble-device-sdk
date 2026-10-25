/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file board.h
 * @brief Arduino Nesso N1 board support: display, touch, LED and battery.
 */

#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/* The 1.14" ST7789 panel, used in landscape */
#define BOARD_LCD_H_RES 240
#define BOARD_LCD_V_RES 135

/**
 * @brief Bring up the I2C bus, the IO expanders and the LCD.
 *
 * Leaves the backlight on and the panel cleared to black.
 *
 * @return ESP_OK on success.
 */
esp_err_t board_init(void);

esp_lcd_panel_io_handle_t board_lcd_io(void);
esp_lcd_panel_handle_t board_lcd_panel(void);

/**
 * @brief Read the first touch point, in landscape screen coordinates.
 *
 * @return true while a finger is on the panel.
 */
bool board_touch_read(uint16_t *x, uint16_t *y);

/** @brief Switch the touch controller between active and hibernate. */
void board_touch_set_active(bool active);


void board_led_set(bool on);

/** @brief Turn the LCD backlight on or off; the panel keeps its contents. */
void board_backlight_set(bool on);

/** @brief Drive the LoRa block's expander pin E0.P7 (enable or reset). */
void board_lora_enable(bool on);

/**
 * @brief Choose whether board_init leaves the LoRa block's E0.P7 low.
 *
 * Default true. With false the radio stays in its SPI sleep instead.
 * Call before board_init.
 */
void board_lora_hold_reset(bool hold);

/** @brief Switch the Grove port's power (expander E1.P2). */
void board_grove_power(bool on);

/** @brief Disconnect USB input so the board runs from its battery. */
void board_charger_hiz(bool hiz);

/** @brief true while USB power is present (expander 0x44 P5). */
bool board_usb_powered(void);

/** @brief true while either front key (KEY1 or KEY2) is held down. */
bool board_key_pressed(void);

/**
 * @brief Battery state of charge from the BQ27220 fuel gauge.
 *
 * @return 0-100, or -1 if the gauge did not answer.
 */
int board_battery_percent(void);

enum board_charge {
	BOARD_CHARGE_UNKNOWN = -1,
	BOARD_CHARGE_OFF,
	BOARD_CHARGE_PRE,
	BOARD_CHARGE_ON,
	BOARD_CHARGE_FULL,
};

/** @brief Charge state from the AW32001 charger. */
enum board_charge board_charge_state(void);

/**
 * @brief Battery voltage and current from the BQ27220 fuel gauge.
 *
 * @param mv Battery voltage in millivolts.
 * @param ma Battery current in milliamps; positive while charging.
 * @return false if the gauge did not answer.
 */
bool board_battery_read(uint16_t *mv, int16_t *ma);

/**
 * @brief Filtered battery current from the BQ27220 fuel gauge.
 *
 * The gauge averages over tens of seconds, so a read just after a short
 * wake still reflects the time before it.
 *
 * @param ma Battery current in milliamps; positive while charging.
 * @return false if the gauge did not answer.
 */
bool board_battery_avg_ma(int16_t *ma);

/** @brief Log percent, voltage, current and charge state. */
void board_battery_log(void);

/**
 * @brief Battery temperature from the BQ27220 fuel gauge.
 *
 * @return Temperature in hundredths of a degree Celsius, or INT16_MIN if the
 *         gauge did not answer.
 */
int16_t board_battery_temp_centi_c(void);

#endif /* BOARD_H */
