/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_sntp.h
 * @brief One-shot Unix time from SNTP over Wi-Fi.
 */

#ifndef APP_SNTP_H
#define APP_SNTP_H

#include <stdbool.h>
#include <stdint.h>

/** @brief true if either Wi-Fi network is set in Kconfig. */
bool app_sntp_configured(void);

/**
 * @brief Join Wi-Fi, get the time over SNTP, then turn Wi-Fi off again.
 *
 * Joins the first configured network that a scan finds in range.
 *
 * Wi-Fi is fully deinitialized before returning so it holds no radio time
 * once BLE and the satellite radio start.
 *
 * @return Unix time in milliseconds, or 0 on failure.
 */
uint64_t app_sntp_time_get(void);

/**
 * @brief Scan for the first (home) Wi-Fi network without joining it.
 *
 * Scans only the channel it was found on at boot, if known, so the radio
 * is on for a fraction of a second.
 *
 * @return true if the network is in range.
 */
bool app_wifi_home_seen(void);

/** @brief true if the last app_sntp_time_get() joined the home network. */
bool app_wifi_joined_home(void);

#endif /* APP_SNTP_H */
