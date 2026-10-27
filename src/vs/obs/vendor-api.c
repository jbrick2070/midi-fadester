/*
 * Visual Sidechain - obs-websocket 5 Vendor API & In-Process Signal Bridge
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
	obs_data_set_double(data, "beat_phase", (double)f->beat);
	obs_data_set_double(data, "spectral_centroid", (double)f->spectral_centroid);
	obs_data_set_double(data, "spectral_flux", (double)f->spectral_flux);
	obs_data_set_double(data, "estimated_bpm", (double)f->estimated_bpm);
	obs_data_set_double(data, "bpm", (double)f->estimated_bpm);

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

void vs_vendor_api_on_tick(float dt_sec, const struct vs_audio_frame *frame)
{
	if (!frame)
		return;

	if (!vs_fader_bridge_on_tick(dt_sec, frame))
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
	signal_handler_t *global_sh = obs_get_signal_handler();

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

	obs_log(LOG_INFO, "Registered obs-websocket vendor '%s' (get_signals=%d)", VS_VENDOR_NAME, (int)r1);
	return r1;
}
