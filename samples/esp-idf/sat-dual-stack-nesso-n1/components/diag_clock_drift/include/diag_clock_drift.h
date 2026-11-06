/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file diag_clock_drift.h
 * @brief Crystal offset against USB SOF, die temperature and TX log.
 *
 * Prints CSV lines on the console:
 *   DRIFT,<ms_since_boot>,<frames>,<ticks>,<ppm>,<die_temp_c>,<valid 0|1>
 *   TX,<ms_since_boot>,<seq>,<die_temp_c>
 * Ticks are esp_timer microseconds, 1000 per nominal 1 ms frame. Without
 * USB SOF, frames reads "nosof" and only the temperature is valid. A TX
 * line carries the temperature from the latest window, so the send path
 * does no sensor read.
 */

#ifndef DIAG_CLOCK_DRIFT_H
#define DIAG_CLOCK_DRIFT_H

#include "esp_err.h"

/** @brief Start the low-priority measurement task. */
esp_err_t diag_clock_drift_start(void);

#endif /* DIAG_CLOCK_DRIFT_H */
