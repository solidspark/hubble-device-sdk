/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "nvs.h"

#include "app_power_log.h"
#include "board.h"
#include "ui.h"

static const char *TAG = "power";

#define SAMPLE_S        10
#define SAVE_EVERY      6
#define PLOG_NS         "power_log"
#define PLOG_KEY        "totals"
#define PLOG_KEY_PM     "pm_locks"
#define PM_DUMP_LEN     1800

struct power_totals {
	/* Seconds on battery and milliamp-seconds drawn, by screen state */
	uint32_t s_on;
	uint32_t s_off;
	int64_t mas_on;
	int64_t mas_off;
	uint16_t first_mv;
	uint16_t last_mv;

	/*
	 * Light-sleep and total time from PM profiling at the first and the
	 * latest sample on battery; the stats count from boot, so only the
	 * difference covers the time unplugged.
	 */
	int64_t sleep0_us;
	int64_t total0_us;
	int64_t sleep1_us;
	int64_t total1_us;
};

static char _pm_dump[PM_DUMP_LEN];

static struct power_totals _t;
static int _since_save;

/* Fills _pm_dump and returns light-sleep time; *total_us is all modes. */
static int64_t _pm_sleep_us(int64_t *total_us)
{
	FILE *f = fmemopen(_pm_dump, sizeof(_pm_dump), "w");
	int64_t sleep_us = 0;
	char *line;

	*total_us = 0;
	if (f == NULL) {
		return 0;
	}
	(void)esp_pm_dump_locks(f);
	fclose(f);
	_pm_dump[sizeof(_pm_dump) - 1] = '\0';

	/* Mode rows look like "SLEEP     40 M        123456  12%". */
	line = strstr(_pm_dump, "Mode stats:");
	while (line != NULL && (line = strchr(line, '\n')) != NULL) {
		char name[12];
		char *m;
		int64_t us;

		line++;
		if (sscanf(line, "%11s", name) != 1 ||
		    (m = strchr(line, 'M')) == NULL || m > strchr(line, '\n')) {
			continue;
		}
		us = strtoll(m + 1, NULL, 10);
		*total_us += us;
		if (strcmp(name, "SLEEP") == 0) {
			sleep_us = us;
		}
	}
	return sleep_us;
}

static void _save(void)
{
	nvs_handle_t h;

	_t.sleep1_us = _pm_sleep_us(&_t.total1_us);

	if (nvs_open(PLOG_NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}
	(void)nvs_set_blob(h, PLOG_KEY, &_t, sizeof(_t));
	(void)nvs_set_str(h, PLOG_KEY_PM, _pm_dump);
	(void)nvs_commit(h);
	nvs_close(h);
}

static void _print_and_clear(void)
{
	struct power_totals t;
	size_t len = sizeof(t);
	nvs_handle_t h;

	if (nvs_open(PLOG_NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}

	if (nvs_get_blob(h, PLOG_KEY, &t, &len) == ESP_OK &&
	    len == sizeof(t) && t.s_on + t.s_off > 0) {
		ESP_LOGI(TAG,
			 "on battery %lu s: screen off %lu s at %lld mA avg, "
			 "screen on %lu s at %lld mA avg, %u -> %u mV",
			 (unsigned long)(t.s_on + t.s_off),
			 (unsigned long)t.s_off,
			 t.s_off ? t.mas_off / t.s_off : 0,
			 (unsigned long)t.s_on, t.s_on ? t.mas_on / t.s_on : 0,
			 t.first_mv, t.last_mv);
		if (t.total1_us > t.total0_us) {
			ESP_LOGI(TAG, "asleep %lld%% of the time on battery",
				 (t.sleep1_us - t.sleep0_us) * 100 /
					 (t.total1_us - t.total0_us));
		}
		len = sizeof(_pm_dump);
		if (nvs_get_str(h, PLOG_KEY_PM, _pm_dump, &len) == ESP_OK) {
			ESP_LOGI(TAG, "power locks at the last sample "
				      "(counted from boot):\n%s",
				 _pm_dump);
		}
	} else {
		ESP_LOGI(TAG, "no time on battery recorded");
	}

	(void)nvs_erase_key(h, PLOG_KEY);
	(void)nvs_erase_key(h, PLOG_KEY_PM);
	(void)nvs_commit(h);
	nvs_close(h);
}

static void _sample_cb(void *arg)
{
	uint16_t mv;
	int16_t ma;

	if (board_usb_powered() || !board_battery_read(&mv, &ma)) {
		return;
	}

	/* The gauge reports discharge as negative current. */
	if (ui_screen_on()) {
		_t.s_on += SAMPLE_S;
		_t.mas_on += (int64_t)-ma * SAMPLE_S;
	} else {
		_t.s_off += SAMPLE_S;
		_t.mas_off += (int64_t)-ma * SAMPLE_S;
	}
	if (_t.first_mv == 0) {
		_t.first_mv = mv;
		_t.sleep0_us = _pm_sleep_us(&_t.total0_us);
	}
	_t.last_mv = mv;

	if (++_since_save >= SAVE_EVERY) {
		_since_save = 0;
		_save();
	}
}

void app_power_log_start(void)
{
	esp_timer_create_args_t args = {
		.callback = _sample_cb,
		.name = "power_log",
	};
	esp_timer_handle_t timer;

	_print_and_clear();

	if (esp_timer_create(&args, &timer) == ESP_OK) {
		(void)esp_timer_start_periodic(timer,
					       SAMPLE_S * 1000ULL * 1000ULL);
	}
}
