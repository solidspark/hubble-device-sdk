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

#include <stdbool.h>
#include <stdint.h>

#define APP_PASSES_SHOWN 3

/* Temperature (int16, centi-degC, big endian) + battery % + reserved */
#define APP_PAYLOAD_LEN  4U

enum app_phase {
	APP_PHASE_BOOT,
	APP_PHASE_SYNCING,
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

	/* HUBBLE_SAT_RELIABILITY_HIGH when set, NORMAL otherwise */
	bool sat_high;
	uint64_t tx_start_ms;
	uint8_t tx_total;
	uint8_t tx_interval_s;

	/* Terrestrial beacons sent before beacon_since_ms, today (UTC). */
	uint32_t beacons_before;
	uint64_t beacon_since_ms;
	uint32_t beacon_interval_ms;

	/* Home Wi-Fi in range: beacons slow to the home interval. */
	bool home;

	/* Saved location and ephemeris; setup_saved_at_s is 0 if none. */
	uint64_t setup_saved_at_s;
	uint8_t setup_sats;
};

/** @brief Copy the current state; safe from any task. */
void app_state_get(struct app_state *out);

/** @brief Ask the radio loop to switch to satellite or terrestrial. */
void app_radio_request(enum app_radio radio);

/** @brief Use 16 packets 10 s apart (high) or 8 packets 20 s apart. */
void app_sat_high_set(bool high);

/** @brief Set the mean beacon interval; the beacon spans it +/-10%. */
void app_beacon_interval_set(uint32_t interval_ms);

/** @brief Fill the satellite payload from the current sensor readings. */
void app_payload_build(uint8_t payload[APP_PAYLOAD_LEN]);

/** @brief Erase the saved setup and restart into dual-stack-companion.py setup. */
void app_setup_forget(void);

/** @brief Unix time in milliseconds, or 0 before provisioning. */
uint64_t app_time_ms(void);

/* NimBLE does not report each advertising event, so this is an estimate. */
static inline uint32_t app_beacons_today(const struct app_state *s,
					 uint64_t now)
{
	uint32_t count = s->beacons_before;

	if (s->beacon_since_ms != 0 && now > s->beacon_since_ms) {
		count += (now - s->beacon_since_ms) / s->beacon_interval_ms;
	}

	return count;
}

#endif /* APP_STATE_H */
