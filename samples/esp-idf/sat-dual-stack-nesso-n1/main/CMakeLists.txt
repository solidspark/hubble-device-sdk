# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0

set(SOURCES
	"main.c"
	"app_ble.c"
	"logo.c"
	"app_setup.c"
	"app_sntp.c"
	"app_power_log.c"
	"app_power_test.c"
	"app_sleep_floor.c"
)
set(REQUIRES bt console nvs_flash esp_timer mbedtls hubblenetwork-sdk
	     esp_lcd esp_driver_i2c esp_driver_spi esp_driver_gpio
	     esp_wifi esp_netif esp_event lwip esp_http_client esp_app_format)
set(EMBED)

# The IDF target picks the board: esp32c6 is the Arduino Nesso N1,
# esp32s31 is the ESP-Mosaico.
if(IDF_TARGET STREQUAL "esp32s31")
	list(APPEND SOURCES "board_mosaico.c" "ui_mosaico.c" "logo_48.c")
	list(APPEND REQUIRES efuse esp_driver_ledc usb_hs_console)
	list(APPEND EMBED "fonts/manrope_200.ttf" "fonts/manrope_400.ttf")
else()
	list(APPEND SOURCES "board.c" "ui.c")
endif()

idf_component_register(SRCS ${SOURCES}
		       PRIV_REQUIRES ${REQUIRES}
		       EMBED_FILES ${EMBED}
		       INCLUDE_DIRS ".")
