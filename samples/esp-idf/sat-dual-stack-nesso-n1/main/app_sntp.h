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

/** @brief true if Wi-Fi credentials are set in Kconfig. */
bool app_sntp_configured(void);

/**
 * @brief Join Wi-Fi, get the time over SNTP, then turn Wi-Fi off again.
 *
 * Wi-Fi is fully deinitialized before returning so it holds no radio time
 * once BLE and the satellite radio start.
 *
 * @return Unix time in milliseconds, or 0 on failure.
 */
uint64_t app_sntp_time_get(void);

#endif /* APP_SNTP_H */
