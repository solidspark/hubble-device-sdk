/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_sleep_floor.h
 * @brief Deep-sleep current floor test for the Nesso N1.
 */

#ifndef APP_SLEEP_FLOOR_H
#define APP_SLEEP_FLOOR_H

/**
 * @brief Run the floor test in place of the sample. Never returns.
 *
 * On battery: quiet the board, save one fuel-gauge sample, deep sleep 60 s.
 * On USB: print the saved run, then wait for the next unplug.
 */
void app_sleep_floor_run(void);

#endif /* APP_SLEEP_FLOOR_H */
