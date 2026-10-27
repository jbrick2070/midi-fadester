/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <obs.h>
#include <obs-data.h>
#include "obs/mfd-engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_VENDOR_NAME "midi-fadester"
#define MFD_API_VERSION 1

typedef void (*mfd_vendor_event_sink_fn)(const char *event_name, obs_data_t *event_data, void *user_data);

/*
 * Registers the in-process proc_handler / signal_handler bridge on the provided handlers:
 *   - proc `midi_fadester_get_slot_value` (in: string slot -> out: float value, bool ok, string reason)
 *   - proc `midi_fadester_set_fader_value` (in: string slot, float value -> out: bool ok, string reason)
 *   - signal `midi_fadester_fader_changed` (string slot, float value, string origin)
 */
bool mfd_bridge_register_with_handlers(proc_handler_t *ph, signal_handler_t *sh, struct mfd_engine *eng);

/*
 * Registers the "midi-fadester" vendor API with obs-websocket 5.x if loaded.
 * Must be called from obs_module_post_load().
 * If obs-websocket is not loaded, logs one clear info line and returns false.
 */
bool mfd_vendor_api_register(struct mfd_engine *eng);

/*
 * Optional fake-websocket event sink for headless tests (test-vendor-api).
 */
void mfd_vendor_set_event_sink(mfd_vendor_event_sink_fn fn, void *user_data);

/*
 * Vendor event emitters called by the engine when slot values or state change.
 */
void mfd_vendor_emit_fader_changed(const char *slot_id, float value, const char *origin);
void mfd_vendor_emit_state_changed(struct mfd_engine *eng);

/*
 * Direct request handlers exposed for unit tests and in-process verification.
 * Conform to C:\streamdeck-fadester\API-CONTRACT.md:
 *   - get_state
 *   - set_fader_value
 *   - nudge_fader
 *   - auto_fade
 *   - set_slot
 *   - load_profile
 */
void mfd_vendor_handle_get_state(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_nudge_fader(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_auto_fade(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_set_slot(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_load_profile(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

#ifdef __cplusplus
}
#endif
