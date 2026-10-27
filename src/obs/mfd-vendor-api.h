/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <obs-data.h>
#include <stdbool.h>
#include "obs/mfd-engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_VENDOR_NAME "midi-fadester"

/*
 * Registers the "midi-fadester" vendor and its requests (get_state, set_slot,
 * set_fader_value, load_apc40_defaults) with obs-websocket during obs_module_post_load().
 * If obs-websocket is not loaded, logs one clear line and returns false.
 */
bool mfd_vendor_api_register(struct mfd_engine *eng);

/*
 * Direct request handlers exposed for both obs-websocket callbacks and headless/harness testing.
 */
void mfd_vendor_handle_get_state(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_set_slot(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_load_apc40_defaults(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

#ifdef __cplusplus
}
#endif
