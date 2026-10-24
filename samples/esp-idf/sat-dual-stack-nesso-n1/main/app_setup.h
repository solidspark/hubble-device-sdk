/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_setup.h
 * @brief Keep the provisioned location and ephemeris in NVS across resets.
 *
 * Unix time is not saved: the Nesso N1 has no RTC, so after a reset the time
 * comes from SNTP (app_sntp.h) or from dual-stack-companion.py.
 */

#ifndef APP_SETUP_H
#define APP_SETUP_H

#include <stdbool.h>
#include <stdint.h>

#include <hubble/sat/pass_prediction.h>

/**
 * @brief Save the device location and satellite orbital parameters.
 *
 * @param saved_at_s Unix time of the save, in seconds.
 * @return 0 on success, negative on an NVS error.
 */
int app_setup_save(const struct hubble_sat_device_pos *pos,
		   const struct hubble_sat_orbital_params *orb, uint8_t count,
		   uint64_t saved_at_s);

/**
 * @brief Load a saved setup.
 *
 * @param max_count Capacity of @p orb.
 * @return true if a complete setup was found.
 */
bool app_setup_load(struct hubble_sat_device_pos *pos,
		    struct hubble_sat_orbital_params *orb, uint8_t max_count,
		    uint8_t *count, uint64_t *saved_at_s);

/** @brief Erase the saved setup. */
void app_setup_clear(void);

#endif /* APP_SETUP_H */
