/*
 * Visual Sidechain - obs-websocket 5 Vendor API & In-Process Signal Bridge
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vendor-api.h"
#include "tubester/deck-filter.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef void *obs_websocket_vendor;
typedef void (*obs_websocket_request_callback_function)(obs_data_t *request_data, obs_data_t *response_data,
							void *priv_data);

struct obs_websocket_request_callback {
	obs_websocket_request_callback_function callback;
	void *priv_data;
};

static proc_handler_t *g_ws_ph = NULL;
static obs_websocket_vendor g_vendor = NULL;
static vs_safety_walk_schedule_fn g_safety_walk_scheduler = NULL;

void vs_vendor_api_set_safety_walk_scheduler(vs_safety_walk_schedule_fn fn)
{
	g_safety_walk_scheduler = fn;
}

void vs_vendor_api_reset(void)
{
	g_ws_ph = NULL;
	g_vendor = NULL;
	g_safety_walk_scheduler = NULL;
}

static void fill_safety_data(obs_data_t *data, bool panic, int64_t since_ms)
{
	obs_data_set_string(data, "version", PLUGIN_VERSION);
	obs_data_set_int(data, "api_version", 1);
	obs_data_set_bool(data, "panic", panic);
	obs_data_set_int(data, "since_ms", (long long)(panic ? since_ms : 0));
}

static void fill_signals_data(obs_data_t *data, const struct vs_audio_frame *f)
{
	obs_data_set_string(data, "version", PLUGIN_VERSION);
	obs_data_set_int(data, "api_version", 1);
	obs_data_set_bool(data, "panic", vs_fader_bridge_is_panic_active());
	obs_data_set_double(data, "level", (double)f->level);
	obs_data_set_double(data, "sub_bass", (double)f->sub_bass);
	obs_data_set_double(data, "bass", (double)f->bass);
	obs_data_set_double(data, "mid", (double)f->mid);
	obs_data_set_double(data, "high", (double)f->high);
	obs_data_set_double(data, "kick", (double)f->kick);
	obs_data_set_double(data, "snare", (double)f->snare);
	obs_data_set_double(data, "beat", (double)f->beat);
	obs_data_set_double(data, "beat_phase", (double)f->beat);
	obs_data_set_int(data, "beat_index", (long long)f->beat_index);
	obs_data_set_double(data, "bar_phase", (double)f->bar_phase);
	obs_data_set_int(data, "bar_index", (long long)f->bar_index);
	obs_data_set_double(data, "spectral_centroid", (double)f->spectral_centroid);
	obs_data_set_double(data, "spectral_flux", (double)f->spectral_flux);
	obs_data_set_double(data, "estimated_bpm", (double)f->estimated_bpm);
	obs_data_set_double(data, "bpm", (double)f->estimated_bpm);
	obs_data_set_bool(data, "bpm_locked", f->bpm_locked);
	obs_data_set_bool(data, "audio_present", f->audio_present);
	obs_data_set_double(data, "beat_confidence", (double)f->beat_confidence);

	obs_data_array_t *spec_arr = obs_data_array_create();
	for (size_t i = 0; i < VS_SPECTRUM_BANDS; i++) {
		obs_data_t *b = obs_data_create();
		obs_data_set_int(b, "band", (long long)i);
		obs_data_set_double(b, "value", (double)f->spectrum[i]);
		obs_data_array_push_back(spec_arr, b);
		obs_data_release(b);
	}
	obs_data_set_array(data, "spectrum", spec_arr);
	obs_data_array_release(spec_arr);
}

void vs_vendor_handle_get_signals(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	struct vs_audio_frame frame;
	vs_fader_bridge_get_signals(&frame);

	obs_data_set_bool(response_data, "success", true);
	fill_signals_data(response_data, &frame);
}

/* Every Tape Deck as its page last reported it (Tubester R-INT-4): read-only, for tools that show deck state. */
void vs_vendor_handle_get_decks(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (response_data)
		tub_decks_describe(response_data);
}

void vs_vendor_handle_get_safety(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	bool panic = false;
	int64_t since_ms = 0;
	vs_fader_bridge_get_safety(&panic, &since_ms);

	obs_data_set_bool(response_data, "success", true);
	fill_safety_data(response_data, panic, since_ms);
}

void vs_vendor_api_emit_safety_changed(void)
{
	if (!g_ws_ph || !g_vendor)
		return;

	bool panic = false;
	int64_t since_ms = 0;
	vs_fader_bridge_get_safety(&panic, &since_ms);

	obs_data_t *event_data = obs_data_create();
	fill_safety_data(event_data, panic, since_ms);

	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "vendor", g_vendor);
	calldata_set_string(&cd, "type", "safety_changed");
	calldata_set_ptr(&cd, "data", event_data);
	proc_handler_call(g_ws_ph, "vendor_event_emit", &cd);
	calldata_free(&cd);

	obs_data_release(event_data);
}

void vs_vendor_handle_panic(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	vs_fader_bridge_trigger_panic();
	if (g_safety_walk_scheduler)
		g_safety_walk_scheduler(true);

	bool panic = false;
	int64_t since_ms = 0;
	vs_fader_bridge_get_safety(&panic, &since_ms);

	if (response_data) {
		obs_data_set_bool(response_data, "success", true);
		fill_safety_data(response_data, panic, since_ms);
	}
	vs_vendor_api_emit_safety_changed();
}

void vs_vendor_handle_restore(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	vs_fader_bridge_trigger_restore();
	if (g_safety_walk_scheduler)
		g_safety_walk_scheduler(false);

	bool panic = false;
	int64_t since_ms = 0;
	vs_fader_bridge_get_safety(&panic, &since_ms);

	if (response_data) {
		obs_data_set_bool(response_data, "success", true);
		fill_safety_data(response_data, panic, since_ms);
	}
	vs_vendor_api_emit_safety_changed();
}

static void fill_tempo_response(obs_data_t *data, const struct vs_tempo_snapshot *snap)
{
	if (!data || !snap)
		return;
	obs_data_set_bool(data, "success", true);
	obs_data_set_string(data, "version", PLUGIN_VERSION);
	obs_data_set_int(data, "api_version", 1);
	obs_data_set_double(data, "bpm", (double)snap->bpm);
	obs_data_set_double(data, "estimated_bpm", (double)snap->bpm);
	obs_data_set_bool(data, "bpm_locked", snap->bpm_locked);
	obs_data_set_double(data, "beat", (double)snap->beat_phase);
	obs_data_set_double(data, "beat_phase", (double)snap->beat_phase);
	obs_data_set_int(data, "beat_index", (long long)snap->beat_index);
	obs_data_set_double(data, "bar_phase", (double)snap->bar_phase);
	obs_data_set_int(data, "bar_index", (long long)snap->bar_index);
	obs_data_set_int(data, "tap_count", (long long)snap->tap_count);
}

static void fill_vendor_error(obs_data_t *data, const char *sentence)
{
	if (!data || !sentence)
		return;
	obs_data_set_bool(data, "success", false);
	obs_data_set_string(data, "version", PLUGIN_VERSION);
	obs_data_set_int(data, "api_version", 1);
	obs_data_set_string(data, "error", sentence);
	obs_data_set_string(data, "comment", sentence);
}

void vs_vendor_api_emit_signals_changed(void)
{
	if (!g_ws_ph || !g_vendor)
		return;

	struct vs_audio_frame snap;
	vs_fader_bridge_get_signals(&snap);

	obs_data_t *event_data = obs_data_create();
	fill_signals_data(event_data, &snap);

	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "vendor", g_vendor);
	calldata_set_string(&cd, "type", "signals_changed");
	calldata_set_ptr(&cd, "data", event_data);
	proc_handler_call(g_ws_ph, "vendor_event_emit", &cd);
	calldata_free(&cd);

	obs_data_release(event_data);
}

void vs_vendor_tap_tempo_at(double now_sec, obs_data_t *response_data)
{
	struct vs_tempo_snapshot snap;
	const bool accepted = vs_tempo_global_tap(now_sec, &snap);
	if (accepted) {
		vs_fader_bridge_apply_tempo_snapshot(&snap);
		vs_vendor_api_emit_signals_changed();
	}
	fill_tempo_response(response_data, &snap);
}

void vs_vendor_handle_tap_tempo(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	const double now_sec = (double)os_gettime_ns() * 1e-9;
	vs_vendor_tap_tempo_at(now_sec, response_data);
}

void vs_vendor_handle_set_bpm(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	obs_data_item_t *item = request_data ? obs_data_item_byname(request_data, "bpm") : NULL;
	if (!item || obs_data_item_gettype(item) != OBS_DATA_NUMBER) {
		if (item)
			obs_data_item_release(&item);
		fill_vendor_error(response_data, "Field 'bpm' must be a number between 30 and 300.");
		return;
	}

	const double raw = (obs_data_item_numtype(item) == OBS_DATA_NUM_INT) ? (double)obs_data_item_get_int(item)
									     : obs_data_item_get_double(item);
	obs_data_item_release(&item);

	if (!isfinite(raw) || raw < (double)VS_TEMPO_MIN_BPM || raw > (double)VS_TEMPO_MAX_BPM) {
		fill_vendor_error(response_data, "Field 'bpm' must be between 30 and 300.");
		return;
	}

	struct vs_tempo_snapshot snap;
	if (!vs_tempo_global_set_bpm((float)raw, &snap)) {
		fill_vendor_error(response_data, "Field 'bpm' must be between 30 and 300.");
		return;
	}

	vs_fader_bridge_apply_tempo_snapshot(&snap);
	fill_tempo_response(response_data, &snap);
	vs_vendor_api_emit_signals_changed();
}

void vs_vendor_handle_lock_bpm(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	obs_data_item_t *item = request_data ? obs_data_item_byname(request_data, "locked") : NULL;
	if (!item || obs_data_item_gettype(item) != OBS_DATA_BOOLEAN) {
		if (item)
			obs_data_item_release(&item);
		fill_vendor_error(response_data, "Field 'locked' must be true or false.");
		return;
	}

	const bool locked = obs_data_item_get_bool(item);
	obs_data_item_release(&item);

	struct vs_tempo_snapshot snap;
	vs_tempo_global_set_locked(locked, &snap);
	vs_fader_bridge_apply_tempo_snapshot(&snap);
	fill_tempo_response(response_data, &snap);
	vs_vendor_api_emit_signals_changed();
}

void vs_vendor_handle_resync_downbeat(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	struct vs_tempo_snapshot snap;
	vs_tempo_global_resync_downbeat(&snap);
	vs_fader_bridge_apply_tempo_snapshot(&snap);
	fill_tempo_response(response_data, &snap);
	vs_vendor_api_emit_signals_changed();
}

void vs_vendor_api_on_tick(uint64_t video_frame_ts, float dt_sec, const struct vs_audio_frame *frame,
			   bool is_primary_track)
{
	if (!frame)
		return;

	if (!vs_fader_bridge_on_frame_tick(video_frame_ts, dt_sec, frame, is_primary_track))
		return;

	vs_vendor_api_emit_signals_changed();
}

static bool register_one_vendor_request(proc_handler_t *ws_ph, obs_websocket_vendor vendor, const char *req_name,
					obs_websocket_request_callback_function fn, void *priv_data)
{
	struct obs_websocket_request_callback cb = {fn, priv_data};
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "vendor", vendor);
	calldata_set_string(&cd, "type", req_name);
	calldata_set_ptr(&cd, "callback", &cb);
	const bool ok = proc_handler_call(ws_ph, "vendor_request_register", &cd);
	const bool success = ok && calldata_bool(&cd, "success");
	calldata_free(&cd);
	return success;
}

bool vs_vendor_api_register_with_ph(proc_handler_t *global_ph, signal_handler_t *global_sh)
{
	vs_fader_bridge_attach(global_ph, global_sh);

	if (!global_ph) {
		obs_log(LOG_INFO, "Global proc_handler unavailable; skipping vendor API");
		return false;
	}

	struct calldata cd;
	calldata_init(&cd);
	const bool has_ws = proc_handler_call(global_ph, "obs_websocket_api_get_ph", &cd);
	proc_handler_t *ws_ph = has_ws ? (proc_handler_t *)calldata_ptr(&cd, "ph") : NULL;
	calldata_free(&cd);

	if (!ws_ph) {
		obs_log(LOG_INFO, "obs-websocket is not loaded; skipping 'visual-sidechain' vendor API registration");
		return false;
	}

	calldata_init(&cd);
	calldata_set_string(&cd, "name", VS_VENDOR_NAME);
	const bool reg_ok = proc_handler_call(ws_ph, "vendor_register", &cd);
	obs_websocket_vendor vendor = reg_ok ? (obs_websocket_vendor)calldata_ptr(&cd, "vendor") : NULL;
	calldata_free(&cd);

	if (!vendor) {
		obs_log(LOG_WARNING, "Failed to register obs-websocket vendor '%s'", VS_VENDOR_NAME);
		return false;
	}

	g_ws_ph = ws_ph;
	g_vendor = vendor;

	const bool r1 = register_one_vendor_request(ws_ph, vendor, "get_signals", vs_vendor_handle_get_signals, NULL);
	const bool r2 = register_one_vendor_request(ws_ph, vendor, "get_decks", vs_vendor_handle_get_decks, NULL);
	const bool r3 = register_one_vendor_request(ws_ph, vendor, "get_safety", vs_vendor_handle_get_safety, NULL);
	const bool r4 = register_one_vendor_request(ws_ph, vendor, "panic", vs_vendor_handle_panic, NULL);
	const bool r5 = register_one_vendor_request(ws_ph, vendor, "restore", vs_vendor_handle_restore, NULL);
	const bool r6 = register_one_vendor_request(ws_ph, vendor, "tap_tempo", vs_vendor_handle_tap_tempo, NULL);
	const bool r7 = register_one_vendor_request(ws_ph, vendor, "set_bpm", vs_vendor_handle_set_bpm, NULL);
	const bool r8 = register_one_vendor_request(ws_ph, vendor, "lock_bpm", vs_vendor_handle_lock_bpm, NULL);
	const bool r9 =
		register_one_vendor_request(ws_ph, vendor, "resync_downbeat", vs_vendor_handle_resync_downbeat, NULL);

	obs_log(LOG_INFO,
		"Registered obs-websocket vendor '%s' (get_signals=%d, get_decks=%d, get_safety=%d, panic=%d, restore=%d, tap_tempo=%d, set_bpm=%d, lock_bpm=%d, resync_downbeat=%d)",
		VS_VENDOR_NAME, (int)r1, (int)r2, (int)r3, (int)r4, (int)r5, (int)r6, (int)r7, (int)r8, (int)r9);
	return r1 && r2 && r3 && r4 && r5 && r6 && r7 && r8 && r9;
}

bool vs_vendor_api_register(void)
{
	return vs_vendor_api_register_with_ph(obs_get_proc_handler(), obs_get_signal_handler());
}
