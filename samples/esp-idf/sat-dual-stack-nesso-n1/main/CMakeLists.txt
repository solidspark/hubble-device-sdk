# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0

set(SOURCES
	"main.c"
	"app_ble.c"
	"board.c"
	"ui.c"
	"logo.c"
)

idf_component_register(SRCS ${SOURCES}
		       PRIV_REQUIRES bt console nvs_flash esp_timer mbedtls hubblenetwork-sdk
				     esp_lcd esp_driver_i2c esp_driver_spi esp_driver_gpio
		       INCLUDE_DIRS ".")
