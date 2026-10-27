/*
 * Visual Sidechain - obs-websocket 5 Vendor API & In-Process Signal/Fader Bus
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vendor-api.h"

#include <obs-module.h>
#include <plugin-support.h>

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

static void fill_signals_data(obs_data_t *data, const struct vs_audio_frame *f)
{
	obs_data_set_string(data, "version", PLUGIN_VERSION);
	obs_data_set_int(data, "api_version", 1);
	obs_data_set_double(data, "level", (double)f->level);
	obs_data_set_double(data, "sub_bass", (double)f->sub_bass);
	obs_data_set_double(data, "bass", (double)f->bass);
	obs_data_set_double(data, "mid", (double)f->mid);
	obs_data_set_double(data, "high", (double)f->high);
	obs_data_set_double(data, "kick", (double)f->kick);
	obs_data_set_double(data, "snare", (double)f->snare);
	obs_data_set_double(data, "beat", (double)f->beat);
	obs_data_set_double(data, "spectral_centroid", (double)f->spectral_centroid);
	obs_data_set_double(data, "spectral_flux", (double)f->spectral_flux);
	obs_data_set_double(data, "estimated_bpm", (double)f->estimated_bpm);

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

	obs_data_t *faders = obs_data_create();
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		obs_data_set_double(faders, vs_fader_slot_id(i), (double)vs_fader_bridge_get_slot(i));
	}
	obs_data_set_obj(data, "faders", faders);
	obs_data_release(faders);
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

void vs_vendor_handle_get_faders(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	obs_data_set_bool(response_data, "success", true);
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", vs_fader_slot_id(i));
		obs_data_set_int(item, "slot", i);
		obs_data_set_double(item, "value", (double)vs_fader_bridge_get_slot(i));
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(response_data, "slots", arr);
	obs_data_array_release(arr);
}

void vs_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);
	if (!response_data)
		return;

	if (!request_data || !obs_data_has_user_value(request_data, "slot") ||
	    !obs_data_has_user_value(request_data, "value")) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(
			response_data, "error",
			"Missing required 'slot' or 'value' parameter. Valid slots: crossfader, fader_1 .. fader_8, master.");
		return;
	}

	int slot_idx = -1;
	const char *slot_str = obs_data_get_string(request_data, "slot");
	if (slot_str && slot_str[0] != '\0') {
		slot_idx = vs_fader_slot_from_name(slot_str);
	} else {
		slot_idx = (int)obs_data_get_int(request_data, "slot");
		if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
			slot_idx = -1;
	}

	if (slot_idx < 0) {
		char err[192];
		snprintf(err, sizeof(err),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.",
			 slot_str ? slot_str : "(invalid)");
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", err);
		return;
	}

	const float val = (float)obs_data_get_double(request_data, "value");
	vs_fader_bridge_set_slot(slot_idx, val);

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "id", vs_fader_slot_id(slot_idx));
	obs_data_set_int(response_data, "slot", slot_idx);
	obs_data_set_double(response_data, "value", (double)vs_fader_bridge_get_slot(slot_idx));
}

static void proc_get_signals(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	struct vs_audio_frame f;
	vs_fader_bridge_get_signals(&f);
	calldata_set_float(cd, "level", (double)f.level);
	calldata_set_float(cd, "sub_bass", (double)f.sub_bass);
	calldata_set_float(cd, "bass", (double)f.bass);
	calldata_set_float(cd, "mid", (double)f.mid);
	calldata_set_float(cd, "high", (double)f.high);
	calldata_set_float(cd, "kick", (double)f.kick);
	calldata_set_float(cd, "snare", (double)f.snare);
	calldata_set_float(cd, "beat", (double)f.beat);
	calldata_set_float(cd, "bpm", (double)f.estimated_bpm);
}

static void proc_set_fader_slot(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	const char *slot_id = calldata_string(cd, "slot");
	const float value = (float)calldata_float(cd, "value");
	const bool ok = vs_fader_bridge_set_slot_by_name(slot_id, value);
	calldata_set_bool(cd, "success", ok);
}

static void poll_midi_fadester_slots_if_available(void)
{
	proc_handler_t *global_ph = obs_get_proc_handler();
	if (!global_ph)
		return;

	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_int(&cd, "slot", i);
		calldata_set_string(&cd, "slot_id", vs_fader_slot_id(i));
		if (proc_handler_call(global_ph, "midi_fadester_get_slot_value", &cd)) {
			if (calldata_bool(&cd, "success")) {
				const float v = (float)calldata_float(&cd, "value");
				vs_fader_bridge_set_slot(i, v);
			}
		} else {
			calldata_free(&cd);
			break;
		}
		calldata_free(&cd);
	}
}

void vs_vendor_api_on_tick(float dt_sec, const struct vs_audio_frame *frame)
{
	if (!frame)
		return;

	vs_fader_bridge_publish_signals(frame);
	poll_midi_fadester_slots_if_available();

	if (!vs_fader_bridge_should_emit_signals(dt_sec, frame))
		return;

	if (!g_ws_ph || !g_vendor)
		return;

	obs_data_t *event_data = obs_data_create();
	fill_signals_data(event_data, frame);

	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "vendor", g_vendor);
	calldata_set_string(&cd, "type", "signals_changed");
	calldata_set_ptr(&cd, "data", event_data);
	proc_handler_call(g_ws_ph, "vendor_event_emit", &cd);
	calldata_free(&cd);

	obs_data_release(event_data);
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

bool vs_vendor_api_register(void)
{
	proc_handler_t *global_ph = obs_get_proc_handler();
	if (!global_ph) {
		blog(LOG_INFO, "[visual-sidechain] Global proc_handler unavailable; skipping vendor API");
		return false;
	}

	/* Register in-process proc_handler endpoints for Midi Fadester / Playlister integration */
	proc_handler_add(
		global_ph,
		"void visual_sidechain_get_signals(out float level, out float sub_bass, out float bass, out float mid, out float high, out float kick, out float snare, out float beat, out float bpm)",
		proc_get_signals, NULL);
	proc_handler_add(global_ph,
			 "void visual_sidechain_set_fader_slot(in string slot, in float value, out bool success)",
			 proc_set_fader_slot, NULL);

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
	const bool r2 = register_one_vendor_request(ws_ph, vendor, "get_faders", vs_vendor_handle_get_faders, NULL);
	const bool r3 =
		register_one_vendor_request(ws_ph, vendor, "set_fader_value", vs_vendor_handle_set_fader_value, NULL);

	obs_log(LOG_INFO, "Registered obs-websocket vendor '%s' (get_signals=%d, get_faders=%d, set_fader_value=%d)",
		VS_VENDOR_NAME, (int)r1, (int)r2, (int)r3);
	return r1 && r2 && r3;
}
