/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "app_setup.h"

static const char *TAG = "setup";

#define SETUP_NS       "hubble_setup"
#define KEY_POS        "pos"
#define KEY_ORB        "orb"
#define KEY_SAVED_AT   "saved_at"

int app_setup_save(const struct hubble_sat_device_pos *pos,
		   const struct hubble_sat_orbital_params *orb, uint8_t count,
		   uint64_t saved_at_s)
{
	nvs_handle_t h;
	esp_err_t err;

	err = nvs_open(SETUP_NS, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to open NVS (%s)", esp_err_to_name(err));
		return -EIO;
	}

	err = nvs_set_blob(h, KEY_POS, pos, sizeof(*pos));
	if (err == ESP_OK) {
		err = nvs_set_blob(h, KEY_ORB, orb, count * sizeof(*orb));
	}
	if (err == ESP_OK) {
		err = nvs_set_u64(h, KEY_SAVED_AT, saved_at_s);
	}
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);

	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to save setup (%s)", esp_err_to_name(err));
		return -EIO;
	}

	ESP_LOGI(TAG, "Saved location and %u satellites", count);
	return 0;
}

bool app_setup_load(struct hubble_sat_device_pos *pos,
		    struct hubble_sat_orbital_params *orb, uint8_t max_count,
		    uint8_t *count, uint64_t *saved_at_s)
{
	size_t pos_len = sizeof(*pos);
	size_t orb_len = max_count * sizeof(*orb);
	nvs_handle_t h;
	bool ok;

	if (nvs_open(SETUP_NS, NVS_READONLY, &h) != ESP_OK) {
		return false;
	}

	ok = nvs_get_blob(h, KEY_POS, pos, &pos_len) == ESP_OK &&
	     pos_len == sizeof(*pos) &&
	     nvs_get_blob(h, KEY_ORB, orb, &orb_len) == ESP_OK &&
	     orb_len > 0 && orb_len % sizeof(*orb) == 0 &&
	     nvs_get_u64(h, KEY_SAVED_AT, saved_at_s) == ESP_OK;
	nvs_close(h);

	if (ok) {
		*count = orb_len / sizeof(*orb);
	}
	return ok;
}

void app_setup_clear(void)
{
	nvs_handle_t h;

	if (nvs_open(SETUP_NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}
	(void)nvs_erase_all(h);
	(void)nvs_commit(h);
	nvs_close(h);
	ESP_LOGI(TAG, "Cleared saved setup");
}
