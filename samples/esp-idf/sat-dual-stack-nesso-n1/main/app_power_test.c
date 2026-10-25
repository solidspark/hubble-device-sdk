/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_ble.h"
#include "app_power_test.h"
#include "board.h"
#include "ui.h"

#ifdef CONFIG_HUBBLE_SAMPLE_POWER_TEST

static const char *TAG = "power_test";

#define AB_NS         "power_test"
#define AB_KEY        "results"
#define AB_STEPS      4
#define BOOT_SETTLE_S 20
#define STEP_SETTLE_S 30
#define STEP_SAMPLE_S 120
#define SAMPLE_S      2
#define ELF_ID_LEN    16
/* Restart anyway if the test has not finished by then. */
#define FAILSAFE_S    (13 * 60)

/*
 * USB Serial/JTAG stops working in light sleep, and this build turns off
 * IDF's keep-awake-on-USB option. Hold the chip awake except during the
 * measured steps, so the board stays reachable for flashing and logs.
 */
static esp_pm_lock_handle_t _awake_lock;

static const char *const _step_names[AB_STEPS] = {
	"A: as built",
	"B: + LoRa pin E0.P7 low",
	"C: + Grove power off",
	"D: + BLE beacon off",
};

struct ab_results {
	char elf[ELF_ID_LEN + 1];
	int16_t avg_ma[AB_STEPS];
	int16_t min_ma[AB_STEPS];
	int16_t max_ma[AB_STEPS];
	uint16_t mv[AB_STEPS];
	uint8_t samples[AB_STEPS];
	uint8_t usb_seen[AB_STEPS];
	uint8_t steps_done;
};

static void _failsafe_cb(void *arg)
{
	ESP_LOGW(TAG, "Failsafe: test overran, restarting");
	esp_restart();
}

static void _elf_id(char out[ELF_ID_LEN + 1])
{
	(void)esp_app_get_elf_sha256(out, ELF_ID_LEN + 1);
}

static void _print(const struct ab_results *r)
{
	ESP_LOGI(TAG, "A/B results (battery draw, charger in Hi-Z), %u of %d "
		      "steps done:",
		 r->steps_done, AB_STEPS);
	for (int i = 0; i < r->steps_done && i < AB_STEPS; i++) {
		ESP_LOGI(TAG, "  %-26s %4d mA avg (%d..%d), %u mV, %u samples%s",
			 _step_names[i], r->avg_ma[i], r->min_ma[i],
			 r->max_ma[i], r->mv[i], r->samples[i],
			 r->usb_seen[i] ? ", USB present" : "");
	}
}

static void _save(const struct ab_results *r)
{
	nvs_handle_t h;

	if (nvs_open(AB_NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}
	(void)nvs_set_blob(h, AB_KEY, r, sizeof(*r));
	(void)nvs_commit(h);
	nvs_close(h);
}

/* Runs the steps once, saves the results, then restarts. */
static void _test_task(void *arg)
{
	esp_timer_create_args_t failsafe_args = {
		.callback = _failsafe_cb,
		.name = "test_failsafe",
	};
	esp_timer_handle_t failsafe;
	struct ab_results r = {0};
	uint16_t mv;
	int16_t ma;

	_elf_id(r.elf);
	if (esp_timer_create(&failsafe_args, &failsafe) == ESP_OK) {
		(void)esp_timer_start_once(failsafe,
					   FAILSAFE_S * 1000ULL * 1000ULL);
	}
	vTaskDelay(pdMS_TO_TICKS(BOOT_SETTLE_S * 1000));

	ESP_LOGI(TAG, "Starting: USB input off, screen off, ~10 min");
	ui_screen_force_off();
	board_charger_hiz(true);
	esp_pm_lock_release(_awake_lock);

	for (int step = 0; step < AB_STEPS; step++) {
		int32_t sum = 0;
		int16_t lo = INT16_MAX;
		int16_t hi = INT16_MIN;
		uint8_t n = 0;

		if (step == 1) {
			board_lora_enable(false);
		} else if (step == 2) {
			board_grove_power(false);
		} else if (step == 3) {
			(void)ble_adv_stop();
		}
		vTaskDelay(pdMS_TO_TICKS(STEP_SETTLE_S * 1000));

		for (int t = 0; t < STEP_SAMPLE_S; t += SAMPLE_S) {
			if (board_battery_read(&mv, &ma)) {
				/* The gauge reports discharge as negative. */
				sum += -ma;
				lo = -ma < lo ? -ma : lo;
				hi = -ma > hi ? -ma : hi;
				n++;
				r.mv[step] = mv;
			}
			if (board_usb_powered()) {
				r.usb_seen[step] = 1;
			}
			vTaskDelay(pdMS_TO_TICKS(SAMPLE_S * 1000));
		}

		r.samples[step] = n;
		r.avg_ma[step] = n ? sum / n : 0;
		r.min_ma[step] = n ? lo : 0;
		r.max_ma[step] = n ? hi : 0;
		r.steps_done = step + 1;
		/* Saved per step so a stall still leaves what was measured. */
		_save(&r);
	}

	esp_pm_lock_acquire(_awake_lock);
	board_charger_hiz(false);
	_save(&r);
	esp_restart();
}

void app_power_test_start(void)
{
	struct ab_results saved;
	size_t len = sizeof(saved);
	char elf[ELF_ID_LEN + 1];
	nvs_handle_t h;
	bool have = false;

	if (nvs_open(AB_NS, NVS_READONLY, &h) == ESP_OK) {
		have = nvs_get_blob(h, AB_KEY, &saved, &len) == ESP_OK &&
		       len == sizeof(saved);
		nvs_close(h);
	}

	if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "test_awake",
			       &_awake_lock) != ESP_OK) {
		ESP_LOGE(TAG, "No PM lock; not running");
		return;
	}
	esp_pm_lock_acquire(_awake_lock);

	_elf_id(elf);
	if (have) {
		_print(&saved);
	}
	if (have && strcmp(saved.elf, elf) == 0 &&
	    saved.steps_done == AB_STEPS) {
		return;
	}

	if (xTaskCreate(_test_task, "power_test", 4096, NULL, 2, NULL) !=
	    pdPASS) {
		ESP_LOGE(TAG, "Could not start");
	}
}

#else

void app_power_test_start(void)
{
}

#endif /* CONFIG_HUBBLE_SAMPLE_POWER_TEST */
