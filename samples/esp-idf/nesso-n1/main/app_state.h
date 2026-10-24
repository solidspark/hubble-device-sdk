/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_state.h
 * @brief State shared between the radio loop (main.c) and the UI (ui.c).
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdint.h>

#define APP_PASSES_SHOWN 3

enum app_phase {
	APP_PHASE_BOOT,
	APP_PHASE_PROVISIONING,
	APP_PHASE_COUNTDOWN,
	APP_PHASE_TX,
	APP_PHASE_DONE,
	APP_PHASE_TERRESTRIAL,
};

enum app_radio {
	APP_RADIO_SAT,
	APP_RADIO_TER,
};

struct app_state {
	enum app_phase phase;
	enum app_radio radio;

	/* Countdown target; differs from pass_start[0] in debug mode. */
	uint64_t next_tx_ms;
	uint64_t pass_start_ms[APP_PASSES_SHOWN];
	double pass_elevation[APP_PASSES_SHOWN];
	uint8_t pass_count;

	uint64_t tx_start_ms;
	uint8_t tx_total;
	uint8_t tx_interval_s;

	/* Terrestrial beacons sent before beacon_since_ms, today (UTC). */
	uint32_t beacons_before;
	uint64_t beacon_since_ms;
	uint32_t beacon_interval_ms;
};

/** @brief Copy the current state; safe from any task. */
void app_state_get(struct app_state *out);

/** @brief Ask the radio loop to switch to satellite or terrestrial. */
void app_radio_request(enum app_radio radio);

/** @brief Unix time in milliseconds, or 0 before provisioning. */
uint64_t app_time_ms(void);

#endif /* APP_STATE_H */
