/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ui.h
 * @brief LVGL screens for the Nesso N1 pass demo.
 */

#ifndef UI_H
#define UI_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Start LVGL on the board's LCD and touch panel and show the
 *        loading screen.
 *
 * Call after board_init(). The UI then follows app_state on its own.
 *
 * @return ESP_OK on success.
 */
esp_err_t ui_init(void);

/** @brief Turn the screen off now, as the idle timeout would. */
void ui_screen_force_off(void);

/** @brief true while the backlight is on. */
bool ui_screen_on(void);

#endif /* UI_H */
