/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_power_test.h
 * @brief Self-running A/B battery-draw test (CONFIG_HUBBLE_SAMPLE_POWER_TEST).
 *
 * Puts the charger in Hi-Z so the board runs from its battery while still
 * on USB, steps through configurations, and logs the draw of each at the
 * next boot. Runs once per firmware build.
 */

#ifndef APP_POWER_TEST_H
#define APP_POWER_TEST_H

/** @brief Print saved results, or start the test if this build has none. */
void app_power_test_start(void);

#endif /* APP_POWER_TEST_H */
