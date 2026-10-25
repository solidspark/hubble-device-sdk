/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_sleep_floor.h"
#include "board.h"

static const char *TAG = "floor";

#define SLEEP_S     60
#define MAX_SAMPLES 120
#define FLOOR_NS    "sleep_floor"
#define FLOOR_KEY   "run"

struct floor_sample {
	int16_t avg_ma;
	int16_t ma;
	uint16_t mv;
	int16_t awake_ms;
};

struct floor_run {
	uint16_t count;
	struct floor_sample s[MAX_SAMPLES];
};

static struct floor_run _run;

static void _load(void)
{
	nvs_handle_t h;
	size_t len = sizeof(_run);

	memset(&_run, 0, sizeof(_run));
	if (nvs_open(FLOOR_NS, NVS_READONLY, &h) != ESP_OK) {
		return;
	}
	if (nvs_get_blob(h, FLOOR_KEY, &_run, &len) != ESP_OK ||
	    len != sizeof(_run)) {
		memset(&_run, 0, sizeof(_run));
	}
	nvs_close(h);
}

static void _save(void)
{
	nvs_handle_t h;

	if (nvs_open(FLOOR_NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}
	(void)nvs_set_blob(h, FLOOR_KEY, &_run, sizeof(_run));
	(void)nvs_commit(h);
	nvs_close(h);
}

static void _print(void)
{
	int32_t sum = 0;
	int n = 0;

	if (_run.count == 0) {
		ESP_LOGI(TAG, "no samples saved");
		return;
	}

	for (int i = 0; i < _run.count; i++) {
		const struct floor_sample *s = &_run.s[i];

		ESP_LOGI(TAG, "#%d avg %d mA, now %d mA, %u mV, awake %d ms",
			 i, s->avg_ma, s->ma, s->mv, s->awake_ms);
		/*
		 * The first wakes still carry the unplug and the USB time in the
		 * gauge's filter, so leave them out of the floor.
		 */
		if (i >= 3) {
			sum += s->avg_ma;
			n++;
		}
	}

	if (n > 0) {
		ESP_LOGI(TAG, "floor: %ld mA avg over %d samples (%d s each), "
			 "%u -> %u mV", (long)(sum / n), n, SLEEP_S,
			 _run.s[0].mv, _run.s[_run.count - 1].mv);
	}
}

static void _quiet(void)
{
	esp_lcd_panel_handle_t panel = board_lcd_panel();

	board_backlight_set(false);
	if (panel != NULL) {
		(void)esp_lcd_panel_disp_on_off(panel, false);
		(void)esp_lcd_panel_disp_sleep(panel, true);
	}
	board_touch_set_active(false);
	board_led_set(false);
}

void app_sleep_floor_run(void)
{
	if (board_init() != ESP_OK) {
		ESP_LOGE(TAG, "board init failed");
	}
	_quiet();
	_load();

	if (board_usb_powered()) {
		ESP_LOGW(TAG, "sleep-floor test build: the sample does not run");
		_print();
		while (board_usb_powered()) {
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
		ESP_LOGI(TAG, "unplugged: new run");
		memset(&_run, 0, sizeof(_run));
		_save();
	} else if (_run.count < MAX_SAMPLES) {
		struct floor_sample *s = &_run.s[_run.count];

		(void)board_battery_avg_ma(&s->avg_ma);
		(void)board_battery_read(&s->mv, &s->ma);
		s->awake_ms = (int16_t)(esp_timer_get_time() / 1000);
		_run.count++;
		_save();
	}

	esp_deep_sleep((uint64_t)SLEEP_S * 1000000ULL);
}
