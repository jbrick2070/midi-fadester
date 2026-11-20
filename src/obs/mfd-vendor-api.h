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
#define MFD_MIX_NAMESPACE "io.github.jbrick2070.fadester.mix.v1"
void mfd_vendor_send_mix_set(struct mfd_engine *eng, int slot_index, uint64_t revision, float db);
void mfd_vendor_handle_mix_reply(obs_data_t *request, obs_data_t *response, void *priv);

typedef void (*mfd_vendor_event_sink_fn)(const char *event_name, obs_data_t *event_data, void *user_data);

/*
 * Registers the in-process proc_handler / signal_handler bridge on the provided handlers:
 *   - proc `midi_fadester_get_slot_value` (in: string slot -> out: float value, bool ok, string reason)
 *   - proc `midi_fadester_set_fader_value` (in: string slot, float value -> out: bool ok, string reason)
 *   - signal `midi_fadester_fader_changed` (string slot, float value, string origin, string display_value)
 */
bool mfd_bridge_register_with_handlers(proc_handler_t *ph, signal_handler_t *sh, struct mfd_engine *eng);

/*
 * Registers the "midi-fadester" vendor API with obs-websocket 5.x if loaded.
 * Must be called from obs_module_post_load().
 * If obs-websocket is not loaded, logs one clear info line and returns false.
 */
bool mfd_vendor_api_register(struct mfd_engine *eng);

/*
 * Unregisters obs-websocket vendor requests and fences in-flight vendor/bridge calls.
 * Must be called first in obs_module_unload().
 */
void mfd_vendor_api_shutdown(struct mfd_engine *eng);

/*
 * Optional fake-websocket event sink for headless tests (test-vendor-api).
 */
void mfd_vendor_set_event_sink(mfd_vendor_event_sink_fn fn, void *user_data);

/*
 * Vendor event emitters called by the engine when slot values or state change.
 */
void mfd_vendor_emit_fader_changed(const char *slot_id, float value, const char *origin);
void mfd_vendor_emit_fader_changed_ex(const char *slot_id, float value, const char *display_value, const char *origin);
void mfd_vendor_emit_state_changed(struct mfd_engine *eng);
/* APC40 video speed for Fadester Companion: vendor event "vjster_action" with {"action": id} and, for
 * "vjster.speed.step", {"steps": n} (n x 0.05x, signed). Never called with the engine lock held. */
void mfd_vendor_emit_vjster_action(const char *action, int steps);
/* {"action": "vjster.speed.set", "rate": r} (0.25..4): the dock's preset speeds (Fadester Companion 0.1.17+). */
void mfd_vendor_emit_vjster_rate(double rate);

/*
 * An APC40 desk button for Visual Sidechain, through obs-websocket: "panic" (panics, or restores when it is panicked),
 * "tap_tempo", "lock_bpm" (toggles the lock) or "resync_downbeat". False when Visual Sidechain did not answer.
 * *out_panic: whether it is panicked afterwards (panic only). UI thread, no lock held.
 */
bool mfd_vendor_call_visual_sidechain_desk(const char *request, bool *out_panic);
/* Visual Sidechain's panic state as it answers get_safety. False when it did not answer. UI thread, no lock held. */
bool mfd_vendor_visual_sidechain_panic_state(bool *out_panic);

/*
 * Direct request handlers exposed for unit tests and in-process verification.
 * Conform to C:\streamdeck-fadester\API-CONTRACT.md:
 *   - get_state
 *   - set_fader_value
 *   - nudge_fader
 *   - auto_fade
 *   - set_slot
 *   - load_profile
 *   - get_catalog
 *   - reset_fx
 */
void mfd_vendor_handle_get_state(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_nudge_fader(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_auto_fade(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_set_slot(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_fill_from_scene(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_load_profile(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_get_catalog(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);
void mfd_vendor_handle_reset_fx(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

/* Makes every string in data (nested objects and arrays included) valid UTF-8 in place. Every reply and event goes
 * through it before obs-websocket, which cannot send a string that is not valid UTF-8 and stops OBS when asked to. */
void mfd_vendor_make_utf8_valid(obs_data_t *data);

#ifdef __cplusplus
}
#endif
