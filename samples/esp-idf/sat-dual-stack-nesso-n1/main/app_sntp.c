/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "app_sntp.h"

static const char *TAG = "sntp";

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define SNTP_SYNC_TIMEOUT_MS    15000
#define WIFI_MAX_RETRIES        5

#define BIT_GOT_IP              BIT0
#define BIT_GAVE_UP             BIT1

static EventGroupHandle_t _events;
static int _retries;

static void _wifi_event_cb(void *arg, esp_event_base_t base, int32_t id,
			   void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		(void)esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		if (++_retries <= WIFI_MAX_RETRIES) {
			(void)esp_wifi_connect();
		} else {
			xEventGroupSetBits(_events, BIT_GAVE_UP);
		}
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(_events, BIT_GOT_IP);
	}
}

bool app_sntp_configured(void)
{
	return strlen(CONFIG_HUBBLE_SAMPLE_WIFI_SSID) > 0;
}

uint64_t app_sntp_time_get(void)
{
	wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_sntp_config_t sntp_cfg =
		ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_HUBBLE_SAMPLE_SNTP_SERVER);
	wifi_config_t wifi_cfg = {0};
	esp_event_handler_instance_t wifi_h = NULL;
	esp_event_handler_instance_t ip_h = NULL;
	esp_netif_t *netif = NULL;
	EventBits_t bits;
	struct timeval tv;
	uint64_t now_ms = 0;
	esp_err_t err;

	if (!app_sntp_configured()) {
		return 0;
	}

	_events = xEventGroupCreate();
	if (_events == NULL) {
		return 0;
	}
	_retries = 0;

	err = esp_netif_init();
	if (err == ESP_OK) {
		err = esp_event_loop_create_default();
		if (err == ESP_ERR_INVALID_STATE) {
			err = ESP_OK;
		}
	}
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Network init failed (%s)", esp_err_to_name(err));
		goto out_events;
	}

	netif = esp_netif_create_default_wifi_sta();
	if (esp_wifi_init(&init_cfg) != ESP_OK) {
		ESP_LOGE(TAG, "Wi-Fi init failed");
		goto out_netif;
	}

	(void)esp_event_handler_instance_register(
		WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_event_cb, NULL, &wifi_h);
	(void)esp_event_handler_instance_register(
		IP_EVENT, IP_EVENT_STA_GOT_IP, _wifi_event_cb, NULL, &ip_h);

	strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_HUBBLE_SAMPLE_WIFI_SSID,
		sizeof(wifi_cfg.sta.ssid));
	strlcpy((char *)wifi_cfg.sta.password,
		CONFIG_HUBBLE_SAMPLE_WIFI_PASSWORD,
		sizeof(wifi_cfg.sta.password));

	if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
	    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK ||
	    esp_wifi_start() != ESP_OK) {
		ESP_LOGE(TAG, "Wi-Fi start failed");
		goto out_wifi;
	}

	ESP_LOGI(TAG, "Joining \"%s\"...", CONFIG_HUBBLE_SAMPLE_WIFI_SSID);
	bits = xEventGroupWaitBits(_events, BIT_GOT_IP | BIT_GAVE_UP, pdFALSE,
				   pdFALSE,
				   pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
	if (!(bits & BIT_GOT_IP)) {
		ESP_LOGW(TAG, "Could not join Wi-Fi");
		goto out_stop;
	}

	if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
		if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(
			    SNTP_SYNC_TIMEOUT_MS)) == ESP_OK) {
			gettimeofday(&tv, NULL);
			now_ms = (uint64_t)tv.tv_sec * 1000ULL +
				 tv.tv_usec / 1000;
			ESP_LOGI(TAG, "Time synced: %llu", now_ms);
		} else {
			ESP_LOGW(TAG, "SNTP sync timed out");
		}
		esp_netif_sntp_deinit();
	}

out_stop:
	(void)esp_wifi_disconnect();
	(void)esp_wifi_stop();
out_wifi:
	if (wifi_h != NULL) {
		(void)esp_event_handler_instance_unregister(WIFI_EVENT,
							    ESP_EVENT_ANY_ID,
							    wifi_h);
	}
	if (ip_h != NULL) {
		(void)esp_event_handler_instance_unregister(
			IP_EVENT, IP_EVENT_STA_GOT_IP, ip_h);
	}
	(void)esp_wifi_deinit();
out_netif:
	if (netif != NULL) {
		esp_netif_destroy_default_wifi(netif);
	}
out_events:
	vEventGroupDelete(_events);
	return now_ms;
}
