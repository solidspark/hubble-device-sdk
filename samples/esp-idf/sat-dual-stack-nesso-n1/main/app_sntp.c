/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <strings.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"

#include "app_sntp.h"

static const char *TAG = "sntp";

#define WIFI_CONNECT_TIMEOUT_MS 15000
#define SNTP_SYNC_TIMEOUT_MS    10000
#define HTTP_TIMEOUT_MS         5000
#define HTTP_DATE_LEN           40

/*
 * Some networks block NTP (UDP 123) but allow web traffic, so the HTTP Date
 * header of this URL is the fallback, at 1 s precision.
 */
#define HTTP_TIME_URL           "http://www.google.com/generate_204"
#define WIFI_MAX_RETRIES        5

#define BIT_GOT_IP              BIT0
#define BIT_GAVE_UP             BIT1

struct wifi_net {
	const char *ssid;
	const char *password;
};

static const struct wifi_net _nets[] = {
	{CONFIG_HUBBLE_SAMPLE_WIFI_SSID, CONFIG_HUBBLE_SAMPLE_WIFI_PASSWORD},
	{CONFIG_HUBBLE_SAMPLE_WIFI2_SSID, CONFIG_HUBBLE_SAMPLE_WIFI2_PASSWORD},
};

static EventGroupHandle_t _events;
static int _retries;

/* Channel the home network was found on at boot; 0 scans all channels. */
static uint8_t _home_channel;
static bool _joined_home;

static void _wifi_event_cb(void *arg, esp_event_base_t base, int32_t id,
			   void *data)
{
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		if (++_retries <= WIFI_MAX_RETRIES) {
			(void)esp_wifi_connect();
		} else {
			xEventGroupSetBits(_events, BIT_GAVE_UP);
		}
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *ev = data;

		ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&ev->ip_info.ip));
		xEventGroupSetBits(_events, BIT_GOT_IP);
	}
}

/* Days since 1970-01-01 for a civil date; newlib has no timegm(). */
static bool _wifi_in_range(const char *ssid)
{
	wifi_scan_config_t scan = {
		.ssid = (uint8_t *)ssid,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
	};
	uint16_t found = 0;

	if (esp_wifi_scan_start(&scan, true) == ESP_OK) {
		(void)esp_wifi_scan_get_ap_num(&found);
	}
	(void)esp_wifi_clear_ap_list();
	return found > 0;
}

static int64_t _days_from_civil(int y, unsigned m, unsigned d)
{
	int64_t era;
	unsigned yoe;
	unsigned doy;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	return era * 146097 + (int64_t)(yoe * 365 + yoe / 4 - yoe / 100 + doy) -
	       719468;
}

static esp_err_t _http_event_cb(esp_http_client_event_t *e)
{
	if (e->event_id == HTTP_EVENT_ON_HEADER &&
	    strcasecmp(e->header_key, "Date") == 0) {
		strlcpy(e->user_data, e->header_value, HTTP_DATE_LEN);
	}
	return ESP_OK;
}

/* Reads the time from an HTTP Date header and sets the system clock. */
static uint64_t _http_time_get(void)
{
	char date[HTTP_DATE_LEN] = "";
	esp_http_client_config_t cfg = {
		.url = HTTP_TIME_URL,
		.method = HTTP_METHOD_HEAD,
		.timeout_ms = HTTP_TIMEOUT_MS,
		.event_handler = _http_event_cb,
		.user_data = date,
	};
	esp_http_client_handle_t client = esp_http_client_init(&cfg);
	struct timeval tv = {0};
	struct tm tm = {0};

	if (client == NULL) {
		return 0;
	}
	(void)esp_http_client_perform(client);
	esp_http_client_cleanup(client);

	if (strptime(date, "%a, %d %b %Y %H:%M:%S GMT", &tm) == NULL) {
		ESP_LOGW(TAG, "No usable Date header from %s", HTTP_TIME_URL);
		return 0;
	}

	tv.tv_sec = _days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1,
				     tm.tm_mday) *
			    86400 +
		    tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
	settimeofday(&tv, NULL);
	ESP_LOGI(TAG, "Time from HTTP Date header: %lld", (long long)tv.tv_sec);
	return (uint64_t)tv.tv_sec * 1000ULL;
}

bool app_wifi_home_seen(void)
{
	wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
	wifi_scan_config_t scan = {
		.ssid = (uint8_t *)CONFIG_HUBBLE_SAMPLE_WIFI_SSID,
		.channel = _home_channel,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active = {.min = 50, .max = 150},
	};
	uint16_t found = 0;
	esp_err_t err;

	if (strlen(CONFIG_HUBBLE_SAMPLE_WIFI_SSID) == 0) {
		return false;
	}

	err = esp_netif_init();
	if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
		err = esp_event_loop_create_default();
	}
	if ((err != ESP_OK && err != ESP_ERR_INVALID_STATE) ||
	    esp_wifi_init(&init_cfg) != ESP_OK) {
		return false;
	}

	if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK &&
	    esp_wifi_start() == ESP_OK &&
	    esp_wifi_scan_start(&scan, true) == ESP_OK) {
		(void)esp_wifi_scan_get_ap_num(&found);
	}
	(void)esp_wifi_clear_ap_list();
	(void)esp_wifi_stop();
	(void)esp_wifi_deinit();

	ESP_LOGI(TAG, "Home Wi-Fi %s", found ? "in range" : "not seen");
	return found > 0;
}

bool app_wifi_joined_home(void)
{
	return _joined_home;
}

bool app_sntp_configured(void)
{
	return strlen(CONFIG_HUBBLE_SAMPLE_WIFI_SSID) > 0 ||
	       strlen(CONFIG_HUBBLE_SAMPLE_WIFI2_SSID) > 0;
}

uint64_t app_sntp_time_get(void)
{
	wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
		3, ESP_SNTP_SERVER_LIST(CONFIG_HUBBLE_SAMPLE_SNTP_SERVER,
					"time.google.com",
					"time.cloudflare.com"));
	wifi_config_t wifi_cfg = {0};
	const struct wifi_net *net = NULL;
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

	if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
	    esp_wifi_start() != ESP_OK) {
		ESP_LOGE(TAG, "Wi-Fi start failed");
		goto out_wifi;
	}

	for (size_t i = 0; i < sizeof(_nets) / sizeof(_nets[0]); i++) {
		if (strlen(_nets[i].ssid) > 0 && _wifi_in_range(_nets[i].ssid)) {
			net = &_nets[i];
			break;
		}
	}
	if (net == NULL) {
		ESP_LOGW(TAG, "No configured Wi-Fi network in range");
		goto out_stop;
	}

	strlcpy((char *)wifi_cfg.sta.ssid, net->ssid,
		sizeof(wifi_cfg.sta.ssid));
	strlcpy((char *)wifi_cfg.sta.password, net->password,
		sizeof(wifi_cfg.sta.password));
	if (esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK ||
	    esp_wifi_connect() != ESP_OK) {
		ESP_LOGE(TAG, "Wi-Fi connect failed");
		goto out_stop;
	}

	ESP_LOGI(TAG, "Joining \"%s\"...", net->ssid);
	_joined_home = net == &_nets[0];
	bits = xEventGroupWaitBits(_events, BIT_GOT_IP | BIT_GAVE_UP, pdFALSE,
				   pdFALSE,
				   pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
	if (!(bits & BIT_GOT_IP)) {
		ESP_LOGW(TAG, "Could not join Wi-Fi");
		goto out_stop;
	}

	{
		wifi_ap_record_t ap;

		if (net == &_nets[0] && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
			_home_channel = ap.primary;
		}
	}

	if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
		if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(
			    SNTP_SYNC_TIMEOUT_MS)) == ESP_OK) {
			gettimeofday(&tv, NULL);
			now_ms = (uint64_t)tv.tv_sec * 1000ULL +
				 tv.tv_usec / 1000;
			ESP_LOGI(TAG, "Time from SNTP: %llu", now_ms);
		} else {
			ESP_LOGW(TAG, "SNTP sync timed out");
		}
		esp_netif_sntp_deinit();
	}

	if (now_ms == 0) {
		now_ms = _http_time_get();
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
