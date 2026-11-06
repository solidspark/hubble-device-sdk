# Copyright (c) 2026 Hubble Network, Inc.
# SPDX-License-Identifier: Apache-2.0

set(srcs)
if(CONFIG_DIAG_CLOCK_DRIFT)
	list(APPEND srcs "diag_clock_drift.c")
endif()

idf_component_register(SRCS ${srcs}
		       INCLUDE_DIRS "include"
		       PRIV_REQUIRES esp_timer esp_pm esp_driver_tsens soc)

# Logs each satellite packet without touching the SDK or the sample's TX path.
if(CONFIG_DIAG_CLOCK_DRIFT)
	target_link_libraries(${COMPONENT_LIB} INTERFACE
			      "-Wl,--wrap=hubble_sat_board_packet_send")
endif()
