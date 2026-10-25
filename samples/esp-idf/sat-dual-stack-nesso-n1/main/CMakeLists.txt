# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0

set(SOURCES
	"main.c"
	"app_ble.c"
	"board.c"
	"ui.c"
	"logo.c"
	"app_setup.c"
	"app_sntp.c"
	"app_power_log.c"
	"app_power_test.c"
	"app_sleep_floor.c"
)

idf_component_register(SRCS ${SOURCES}
		       PRIV_REQUIRES bt console nvs_flash esp_timer mbedtls hubblenetwork-sdk
				     esp_lcd esp_driver_i2c esp_driver_spi esp_driver_gpio
				     esp_wifi esp_netif esp_event lwip esp_http_client esp_app_format
		       INCLUDE_DIRS ".")
