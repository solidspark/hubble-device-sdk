/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* ESP-IDF components */
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include <sys/time.h>
#include "nvs_flash.h"

#include <hubble/hubble.h>
#include <hubble/sat.h>
#include <hubble/sat/packet.h>
#include <hubble/sat/pass_prediction.h>

#include "app_ble.h"
#include "app_power_log.h"
#include "app_setup.h"
#include "app_sntp.h"
#include "app_state.h"
#include "board.h"
#include "ui.h"

static const char *APP_TAG = "main";

#define MS_PER_SEC         1000U
#define US_PER_MS          1000U
#define MS_PER_DAY         (24ULL * 60 * 60 * MS_PER_SEC)

/* Retry counts and spacing per reliability mode (src/hubble_sat.c) */
#define SAT_HIGH_RETRIES   16U
#define SAT_HIGH_INTERVAL  10U
#define SAT_NORM_RETRIES   8U
#define SAT_NORM_INTERVAL  20U
#define SAT_MAX_DRIFT_MS   (90U * 60U * MS_PER_SEC)

/* Midpoint of the beacon's 1364-1636 ms advertising interval */
#define BEACON_INTERVAL_MS 1500U

#define DONE_SCREEN_MS     2600U
#define SETUP_MAX_AGE_S                                                        \
	((uint64_t)CONFIG_HUBBLE_SAMPLE_SETUP_MAX_AGE_DAYS * 24 * 60 * 60)
#define BATTERY_LOG_US     (60ULL * 1000 * 1000)
#define PASS_SEARCH_LIMIT  8

/* Device location and sat orbital parameters */
struct hubble_sat_device_pos device_pos;
struct hubble_sat_orbital_params orb_params[HUBBLE_MAX_SAT];
uint8_t orb_params_count;

/* Hubble device key and time */
static uint8_t _hubble_key[CONFIG_HUBBLE_KEY_SIZE];
uint64_t unix_time_ms;

/* Sem for sync time / orb params */
SemaphoreHandle_t sync_sem;

enum app_event {
	APP_EVT_SAT_TIMER,
	APP_EVT_RADIO,
};

static QueueHandle_t _evt_q;
static esp_timer_handle_t _sat_timer;

static struct app_state _state = {
	.phase = APP_PHASE_BOOT,
	.radio = APP_RADIO_SAT,
	.sat_high = true,
	.tx_interval_s = SAT_HIGH_INTERVAL,
	.beacon_interval_ms = BEACON_INTERVAL_MS,
};
static portMUX_TYPE _state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool _provisioned;
static uint64_t _synced_ms;

void app_state_get(struct app_state *out)
{
	portENTER_CRITICAL(&_state_lock);
	*out = _state;
	portEXIT_CRITICAL(&_state_lock);
}

void app_radio_request(enum app_radio radio)
{
	enum app_event evt = APP_EVT_RADIO;
	bool changed;

	portENTER_CRITICAL(&_state_lock);
	changed = _state.radio != radio;
	_state.radio = radio;
	portEXIT_CRITICAL(&_state_lock);

	if (changed) {
		(void)xQueueSend(_evt_q, &evt, 0);
	}
}

void app_sat_high_set(bool high)
{
	portENTER_CRITICAL(&_state_lock);
	_state.sat_high = high;
	portEXIT_CRITICAL(&_state_lock);
}

void app_beacon_interval_set(uint32_t interval_ms)
{
	uint64_t now = app_time_ms();

	portENTER_CRITICAL(&_state_lock);
	/* Bank the beacons sent at the old rate before the rate changes. */
	if (_state.beacon_since_ms != 0 && now > _state.beacon_since_ms) {
		_state.beacons_before += (now - _state.beacon_since_ms) /
					 _state.beacon_interval_ms;
		_state.beacon_since_ms = now;
	}
	_state.beacon_interval_ms = interval_ms;
	portEXIT_CRITICAL(&_state_lock);

	(void)ble_adv_interval_set(interval_ms - interval_ms / 11,
				   interval_ms + interval_ms / 11);
}

void app_setup_forget(void)
{
	app_setup_clear();
	esp_restart();
}

uint64_t app_time_ms(void)
{
	return _provisioned ? hubble_time_get() : 0;
}

static void _phase_set(enum app_phase phase)
{
	portENTER_CRITICAL(&_state_lock);
	_state.phase = phase;
	portEXIT_CRITICAL(&_state_lock);
}

static void _setup_state_set(uint64_t saved_at_s, uint8_t sats)
{
	portENTER_CRITICAL(&_state_lock);
	_state.setup_saved_at_s = saved_at_s;
	_state.setup_sats = sats;
	portEXIT_CRITICAL(&_state_lock);
}

/* System time, which SNTP has set if app_sntp_time_get() succeeded. */
static uint64_t _system_time_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * MS_PER_SEC + tv.tv_usec / US_PER_MS;
}

/*
 * Resume without the companion when a saved setup exists, Wi-Fi gives the
 * time, and the ephemeris is not too old. Otherwise leave orb_params empty
 * for the companion to fill.
 */
static bool _setup_resume(void)
{
	uint64_t saved_at_s;
	uint64_t now_s;
	uint8_t count;

	if (!app_setup_load(&device_pos, orb_params, HUBBLE_MAX_SAT, &count,
			    &saved_at_s)) {
		ESP_LOGI(APP_TAG, "No saved setup");
		goto need_companion;
	}
	_setup_state_set(saved_at_s, count);

	if (!app_sntp_configured()) {
		ESP_LOGI(APP_TAG, "Saved setup found; set "
				  "CONFIG_HUBBLE_SAMPLE_WIFI_SSID to resume "
				  "without the companion");
		goto need_companion;
	}

	_phase_set(APP_PHASE_SYNCING);
	now_s = app_sntp_time_get() / MS_PER_SEC;
	if (now_s == 0) {
		goto need_companion;
	}

	if (now_s < saved_at_s || now_s - saved_at_s > SETUP_MAX_AGE_S) {
		ESP_LOGW(APP_TAG, "Saved setup is %llu days old; waiting for "
				  "the companion",
			 (now_s - saved_at_s) / (24 * 60 * 60));
		goto need_companion;
	}

	orb_params_count = count;
	ESP_LOGI(APP_TAG, "Resumed from saved setup (%u satellites)", count);
	return true;

need_companion:
	orb_params_count = 0;
	return false;
}

static void _battery_log_cb(void *arg)
{
	board_battery_log();
}

static void _sat_timer_cb(void *arg)
{
	enum app_event evt = APP_EVT_SAT_TIMER;

	(void)xQueueSend(_evt_q, &evt, 0);
}

/*
 * The beacon count is an estimate: NimBLE does not report each legacy
 * advertising event, so it is advertising time over the mean interval.
 */
static void _beacons_start(uint64_t now)
{
	portENTER_CRITICAL(&_state_lock);
	if (_state.beacon_since_ms / MS_PER_DAY != now / MS_PER_DAY) {
		_state.beacons_before = 0;
	}
	_state.beacon_since_ms = now;
	portEXIT_CRITICAL(&_state_lock);
}

static void _beacons_stop(uint64_t now)
{
	portENTER_CRITICAL(&_state_lock);
	if (_state.beacon_since_ms != 0) {
		_state.beacons_before += (now - _state.beacon_since_ms) /
					 _state.beacon_interval_ms;
	}
	_state.beacon_since_ms = 0;
	portEXIT_CRITICAL(&_state_lock);
}

/* Mirrors the SDK's drift compensation so the UI shows the real count. */
static uint8_t _sat_tx_total(uint64_t now, uint8_t retries, uint8_t interval_s)
{
	uint64_t drift_ms = ((now - _synced_ms) *
			     CONFIG_HUBBLE_SAT_NETWORK_DEVICE_TDR) /
			    1000000ULL;

	if (drift_ms > SAT_MAX_DRIFT_MS) {
		drift_ms = SAT_MAX_DRIFT_MS;
	}

	return retries + drift_ms / (interval_s * MS_PER_SEC);
}

/* Fills the next few future passes; returns how many were found. */
static uint8_t _passes_compute(uint64_t now,
			       struct hubble_sat_pass_info *first)
{
	struct hubble_sat_pass_info pass;
	uint64_t t = now;
	uint8_t n = 0;

	for (int i = 0; i < PASS_SEARCH_LIMIT && n < APP_PASSES_SHOWN; i++) {
		if (hubble_sat_next_pass_get(t, &device_pos, &pass) != 0) {
			break;
		}

		t = pass.start + pass.duration + 1;

		/* An ongoing pass is too late to join; take the next one. */
		if (pass.start <= now) {
			continue;
		}

		if (n == 0) {
			*first = pass;
		}

		portENTER_CRITICAL(&_state_lock);
		_state.pass_start_ms[n] = pass.start;
		_state.pass_elevation[n] = pass.max_elevation_angle;
		portEXIT_CRITICAL(&_state_lock);
		n++;
	}

	portENTER_CRITICAL(&_state_lock);
	_state.pass_count = n;
	portEXIT_CRITICAL(&_state_lock);

	return n;
}

void app_payload_build(uint8_t payload[APP_PAYLOAD_LEN])
{
	int16_t temp = board_battery_temp_centi_c();
	int battery = board_battery_percent();

	payload[0] = (uint16_t)temp >> 8;
	payload[1] = (uint16_t)temp & 0xFF;
	payload[2] = battery < 0 ? 0xFF : (uint8_t)battery;
	payload[3] = 0;
}

static enum app_event _event_wait(void)
{
	enum app_event evt;

	xQueueReceive(_evt_q, &evt, portMAX_DELAY);
	return evt;
}

static void _terrestrial_run(void)
{
	_phase_set(APP_PHASE_TERRESTRIAL);

	if (ble_adv_start() != 0) {
		ESP_LOGE(APP_TAG, "Failed to start advertising");
	}
	_beacons_start(hubble_time_get());
	ESP_LOGI(APP_TAG, "Terrestrial mode: beaconing");

	while (_event_wait() != APP_EVT_RADIO) {
	}

	_beacons_stop(hubble_time_get());
	(void)ble_adv_stop();
}

static void _satellite_run(void)
{
	struct hubble_sat_pass_info pass = {0};
	struct hubble_sat_packet packet = {0};
	uint8_t payload[APP_PAYLOAD_LEN];
	enum hubble_sat_transmission_mode mode;
	uint8_t interval_s;
	uint64_t now_ms = hubble_time_get();
	uint64_t wait_ms;
	uint8_t total;
	int ret;

	if (_passes_compute(now_ms, &pass) == 0) {
		ESP_LOGE(APP_TAG, "No upcoming pass found, retrying in 60 s");
		vTaskDelay(pdMS_TO_TICKS(60 * MS_PER_SEC));
		return;
	}

#ifdef CONFIG_HUBBLE_SAMPLE_DEBUG
	wait_ms = 120 * MS_PER_SEC;
	ESP_LOGI(APP_TAG, "Next pass in 120 seconds");
#else
	wait_ms = pass.start - now_ms;
	ESP_LOGI(APP_TAG, "Next pass at: %llu (unix epoch seconds)",
		 pass.start / MS_PER_SEC);
#endif

	portENTER_CRITICAL(&_state_lock);
	_state.next_tx_ms = now_ms + wait_ms;
	_state.phase = APP_PHASE_COUNTDOWN;
	portEXIT_CRITICAL(&_state_lock);

	xQueueReset(_evt_q);
	ESP_ERROR_CHECK(esp_timer_start_once(_sat_timer, wait_ms * US_PER_MS));

	if (ble_adv_start() != 0) {
		ESP_LOGE(APP_TAG, "Failed to start advertising");
	}
	_beacons_start(now_ms);

	if (_event_wait() == APP_EVT_RADIO) {
		(void)esp_timer_stop(_sat_timer);
		_beacons_stop(hubble_time_get());
		(void)ble_adv_stop();
		return;
	}

	_beacons_stop(hubble_time_get());
	ret = ble_adv_stop();
	if (ret != 0) {
		ESP_LOGE(APP_TAG, "Failed to stop advertising, ret: %d", ret);
	}

	app_payload_build(payload);
	ret = hubble_sat_packet_get(&packet, payload, sizeof(payload));
	if (ret != 0) {
		ESP_LOGE(APP_TAG, "Failed to get sat packet, ret: %d", ret);
		return;
	}

	now_ms = hubble_time_get();

	portENTER_CRITICAL(&_state_lock);
	mode = _state.sat_high ? HUBBLE_SAT_RELIABILITY_HIGH
			       : HUBBLE_SAT_RELIABILITY_NORMAL;
	interval_s = _state.sat_high ? SAT_HIGH_INTERVAL : SAT_NORM_INTERVAL;
	total = _sat_tx_total(now_ms,
			      _state.sat_high ? SAT_HIGH_RETRIES
					      : SAT_NORM_RETRIES,
			      interval_s);
	_state.tx_start_ms = now_ms;
	_state.tx_total = total;
	_state.tx_interval_s = interval_s;
	_state.phase = APP_PHASE_TX;
	portEXIT_CRITICAL(&_state_lock);

	ESP_LOGI(APP_TAG, "Starting satellite transmission (%u packets)...",
		 total);
	board_led_set(true);
	ret = hubble_sat_packet_send(&packet, mode);
	board_led_set(false);
	if (ret != 0) {
		ESP_LOGE(APP_TAG, "Failed to send sat packet, ret: %d", ret);
	}

	_phase_set(APP_PHASE_DONE);
	vTaskDelay(pdMS_TO_TICKS(DONE_SCREEN_MS));
}

/*
 * NOTE: this function has many early returns.
 * For production code, we should clean up resources (sem, timers, deinit stack, etc.)
 * on each failure case.
 * For simplicity of the sample, we just exit on failure.
 */
void app_main(void)
{
	esp_timer_create_args_t sat_timer_args = {0};
	struct app_state state;
	bool resumed;
	esp_err_t err;
	int ret;

	/* NVS flash init – dependency of NimBLE stack */
	err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
	    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	/* Decode device key */
	if (strlen(CONFIG_HUBBLE_DEVICE_KEY) != 0) {
		size_t outlen = 0;

		ret = mbedtls_base64_decode(
			_hubble_key, sizeof(_hubble_key), &outlen,
			(const unsigned char *)CONFIG_HUBBLE_DEVICE_KEY,
			strlen(CONFIG_HUBBLE_DEVICE_KEY));

		if (ret != 0) {
			ESP_LOGE(APP_TAG, "Invalid key provided!");
			return;
		}

		if (outlen != sizeof(_hubble_key)) {
			ESP_LOGE(APP_TAG, "Invalid key length provided!");
			return;
		}
	} else {
		ESP_LOGW(APP_TAG, "CONFIG_HUBBLE_DEVICE_KEY is empty; packets "
				  "will not decode in the cloud");
	}

	_evt_q = xQueueCreate(4, sizeof(enum app_event));
	sync_sem = xSemaphoreCreateBinary();
	if (_evt_q == NULL || sync_sem == NULL) {
		ESP_LOGE(APP_TAG, "Failed to create queue or semaphore");
		return;
	}

	sat_timer_args.callback = _sat_timer_cb;
	sat_timer_args.name = "sat_timer";
	err = esp_timer_create(&sat_timer_args, &_sat_timer);
	if (err != ESP_OK) {
		ESP_LOGE(APP_TAG, "Failed to create sat timer (rc=%d)", err);
		return;
	}

	/* The radio loop still works headless if the display fails. */
	err = board_init();
	if (err != ESP_OK) {
		ESP_LOGE(APP_TAG, "Board init failed (%s)",
			 esp_err_to_name(err));
	} else {
		esp_timer_create_args_t log_args = {
			.callback = _battery_log_cb,
			.name = "battery_log",
		};
		esp_timer_handle_t log_timer;

		board_battery_log();
		if (esp_timer_create(&log_args, &log_timer) == ESP_OK) {
			(void)esp_timer_start_periodic(log_timer,
						       BATTERY_LOG_US);
		}
		if (ui_init() != ESP_OK) {
			ESP_LOGE(APP_TAG, "UI init failed");
		}
		app_power_log_start();
	}

	resumed = _setup_resume();

	/* Init and enable BLE */
	ret = ble_init(!resumed);
	if (ret != 0) {
		ESP_LOGE(APP_TAG, "Failed to init BLE, ret: %d", ret);
		return;
	}

	if (resumed) {
		unix_time_ms = _system_time_ms();
	} else {
		_phase_set(APP_PHASE_PROVISIONING);
		ESP_LOGI(APP_TAG, "Waiting for provisioning over BLE...");

		/* We sit here waiting to have time */
		xSemaphoreTake(sync_sem, portMAX_DELAY);

		if (app_setup_save(&device_pos, orb_params, orb_params_count,
				   unix_time_ms / MS_PER_SEC) == 0) {
			_setup_state_set(unix_time_ms / MS_PER_SEC,
					 orb_params_count);
		}
	}

	ret = hubble_init(unix_time_ms, _hubble_key);
	if (ret != 0) {
		ESP_LOGE(APP_TAG,
			 "Failed to initialize Hubble Network, ret: %d", ret);
		return;
	}

	ret = hubble_sat_satellites_set(orb_params, orb_params_count);
	if (ret != 0) {
		ESP_LOGE(APP_TAG,
			 "Failed to set satellite orbital params data, ret: %d",
			 ret);
		return;
	}

	_synced_ms = hubble_time_get();
	_provisioned = true;
	ESP_LOGI(APP_TAG, "Hubble Network initialized");

	for (;;) {
		app_state_get(&state);
		if (state.radio == APP_RADIO_TER) {
			_terrestrial_run();
		} else {
			_satellite_run();
		}
	}
}
