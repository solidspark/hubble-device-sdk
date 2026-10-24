/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_power_log.h
 * @brief Measure battery draw while unplugged, for tuning battery life.
 *
 * The fuel gauge only shows the device's own draw on battery, and the serial
 * log is only readable on USB, so the totals are kept in NVS and printed at
 * the next boot.
 */

#ifndef APP_POWER_LOG_H
#define APP_POWER_LOG_H

/** @brief Print and clear the saved totals, then start sampling. */
void app_power_log_start(void);

#endif /* APP_POWER_LOG_H */
