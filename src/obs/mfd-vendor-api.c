/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/midi-parser.h"
#include <obs-module.h>
#include "plugin-support.h"
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

static float clamp01(float v)
{
	if (!isfinite(v) || v <= 0.0f) {
		return 0.0f;
	}
	if (v >= 1.0f) {
		return 1.0f;
	}
	return v;
}

void mfd_vendor_handle_get_state(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "device_name",
			    eng->midi_dev.is_open ? eng->midi_dev.opened_name : eng->configured_device_name);
	obs_data_set_bool(response_data, "device_open", eng->midi_dev.is_open);
	obs_data_set_double(response_data, "smooth_tau_ms", (double)eng->smooth_tau_ms);
	obs_data_set_bool(response_data, "tbar_bidirectional_flip", eng->tbar_bidirectional_flip);
	obs_data_set_bool(response_data, "has_last_midi", eng->has_last_midi);

	char midi_buf[128] = {0};
	mfd_format_last_midi_readout(eng->has_last_midi, &eng->last_midi_decoded, midi_buf, sizeof(midi_buf));
	obs_data_set_string(response_data, "last_midi_text", midi_buf);

	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "slot", i);
		obs_data_set_bool(item, "enabled", s->enabled);
		obs_data_set_string(item, "label", s->label);
		obs_data_set_int(item, "midi_type", (int)s->addr.type);
		obs_data_set_int(item, "midi_channel", (int)s->addr.channel);
		obs_data_set_int(item, "midi_number", (int)s->addr.number);
		obs_data_set_bool(item, "midi_14bit", s->addr.is_14bit);
		obs_data_set_int(item, "target_type", (int)s->target_type);
		obs_data_set_string(item, "source_a", s->source_a_name);
		obs_data_set_string(item, "source_b", s->source_b_name);
		obs_data_set_int(item, "curve", (int)s->curve);
		obs_data_set_bool(item, "afv", s->audio_follows_video);
		obs_data_set_bool(item, "invert", s->invert);
		obs_data_set_double(item, "last_applied_val", (double)s->last_applied_val);
		obs_data_set_double(item, "last_alpha_a", (double)s->last_alpha_a);
		obs_data_set_double(item, "last_alpha_b", (double)s->last_alpha_b);
		obs_data_set_int(item, "takeover_state", (int)s->takeover.state);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(response_data, "slots", arr);
	obs_data_array_release(arr);
}

void mfd_vendor_handle_set_slot(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data) {
		return;
	}
	if (!request_data || !eng || !obs_data_has_user_value(request_data, "slot")) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "missing required 'slot' integer parameter");
		return;
	}

	int idx = (int)obs_data_get_int(request_data, "slot");
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "slot index out of range (0..9)");
		return;
	}

	struct mfd_fader_slot *s = &eng->fader_slots[idx];
	if (obs_data_has_user_value(request_data, "enabled")) {
		s->enabled = obs_data_get_bool(request_data, "enabled");
	}
	if (obs_data_has_user_value(request_data, "label")) {
		const char *lbl = obs_data_get_string(request_data, "label");
		if (lbl && *lbl) {
			snprintf(s->label, sizeof(s->label), "%s", lbl);
		}
	}
	if (obs_data_has_user_value(request_data, "target_type")) {
		int tt = (int)obs_data_get_int(request_data, "target_type");
		if (tt >= (int)MFD_TARGET_UNASSIGNED && tt <= (int)MFD_TARGET_AUDIO_VOLUME) {
			s->target_type = (enum mfd_target_type)tt;
		}
	}
	if (obs_data_has_user_value(request_data, "source_a")) {
		const char *sa = obs_data_get_string(request_data, "source_a");
		snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", sa ? sa : "");
	}
	if (obs_data_has_user_value(request_data, "source_b")) {
		const char *sb = obs_data_get_string(request_data, "source_b");
		snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", sb ? sb : "");
	}
	if (obs_data_has_user_value(request_data, "curve")) {
		int c = (int)obs_data_get_int(request_data, "curve");
		if (c >= (int)MFD_CURVE_DIP_FREE && c <= (int)MFD_CURVE_LINEAR) {
			s->curve = (enum mfd_crossfade_curve)c;
		}
	}
	if (obs_data_has_user_value(request_data, "afv")) {
		s->audio_follows_video = obs_data_get_bool(request_data, "afv");
	}
	if (obs_data_has_user_value(request_data, "invert")) {
		s->invert = obs_data_get_bool(request_data, "invert");
	}
	if (obs_data_has_user_value(request_data, "midi_type")) {
		s->addr.type = (enum mfd_midi_msg_type)obs_data_get_int(request_data, "midi_type");
	}
	if (obs_data_has_user_value(request_data, "midi_channel")) {
		s->addr.channel = (uint8_t)(obs_data_get_int(request_data, "midi_channel") & 0x0F);
	}
	if (obs_data_has_user_value(request_data, "midi_number")) {
		s->addr.number = (uint8_t)(obs_data_get_int(request_data, "midi_number") & 0x7F);
	}
	if (obs_data_has_user_value(request_data, "midi_14bit")) {
		s->addr.is_14bit = obs_data_get_bool(request_data, "midi_14bit");
	}

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_int(response_data, "slot", idx);
	obs_data_set_int(response_data, "target_type", (int)s->target_type);
	obs_data_set_string(response_data, "source_a", s->source_a_name);
	obs_data_set_string(response_data, "source_b", s->source_b_name);
}

void mfd_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data) {
		return;
	}
	if (!request_data || !eng || !obs_data_has_user_value(request_data, "slot") ||
	    !obs_data_has_user_value(request_data, "value")) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "missing required 'slot' or 'value' parameter");
		return;
	}

	int idx = (int)obs_data_get_int(request_data, "slot");
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "slot index out of range (0..9)");
		return;
	}

	float val = clamp01((float)obs_data_get_double(request_data, "value"));
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];

	/* Virtual fader engages slot directly and applies output immediately */
	slot->last_raw_norm = val;
	slot->takeover.state = MFD_TAKEOVER_ENGAGED;
	slot->takeover.hint = MFD_HINT_NONE;
	slot->takeover.target_obs = val;
	slot->takeover.last_fader = val;
	slot->takeover.has_last_fader = true;
	mfd_smoother_reset(&slot->smoother, val);

	mfd_engine_apply_slot_output(eng, slot, val);

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_int(response_data, "slot", idx);
	obs_data_set_double(response_data, "applied_value", (double)slot->last_applied_val);
	obs_data_set_double(response_data, "alpha_a", (double)slot->last_alpha_a);
	obs_data_set_double(response_data, "alpha_b", (double)slot->last_alpha_b);
}

void mfd_vendor_handle_load_apc40_defaults(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	mfd_engine_load_apc40_defaults(eng);
	obs_data_set_bool(response_data, "success", true);
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
	bool ok = proc_handler_call(ws_ph, "vendor_request_register", &cd);
	bool success = ok && calldata_bool(&cd, "success");
	calldata_free(&cd);
	return success;
}

bool mfd_vendor_api_register(struct mfd_engine *eng)
{
	proc_handler_t *global_ph = obs_get_proc_handler();
	if (!global_ph) {
		obs_log(LOG_INFO, "obs-websocket not available (no global proc_handler); skipping vendor API");
		return false;
	}

	struct calldata cd;
	calldata_init(&cd);
	bool has_ws = proc_handler_call(global_ph, "obs_websocket_api_get_ph", &cd);
	proc_handler_t *ws_ph = has_ws ? (proc_handler_t *)calldata_ptr(&cd, "ph") : NULL;
	calldata_free(&cd);

	if (!ws_ph) {
		obs_log(LOG_INFO, "obs-websocket is not loaded; skipping 'midi-fadester' vendor API registration");
		return false;
	}

	calldata_init(&cd);
	calldata_set_string(&cd, "name", MFD_VENDOR_NAME);
	bool reg_ok = proc_handler_call(ws_ph, "vendor_register", &cd);
	obs_websocket_vendor vendor = reg_ok ? (obs_websocket_vendor)calldata_ptr(&cd, "vendor") : NULL;
	calldata_free(&cd);

	if (!vendor) {
		obs_log(LOG_WARNING, "Failed to register obs-websocket vendor '%s'", MFD_VENDOR_NAME);
		return false;
	}

	bool r1 = register_one_vendor_request(ws_ph, vendor, "get_state", mfd_vendor_handle_get_state, eng);
	bool r2 = register_one_vendor_request(ws_ph, vendor, "set_slot", mfd_vendor_handle_set_slot, eng);
	bool r3 = register_one_vendor_request(ws_ph, vendor, "set_fader_value", mfd_vendor_handle_set_fader_value, eng);
	bool r4 = register_one_vendor_request(ws_ph, vendor, "load_apc40_defaults",
					      mfd_vendor_handle_load_apc40_defaults, eng);

	obs_log(LOG_INFO,
		"Registered obs-websocket vendor '%s' requests (get_state=%d, set_slot=%d, set_fader_value=%d, load_apc40_defaults=%d)",
		MFD_VENDOR_NAME, (int)r1, (int)r2, (int)r3, (int)r4);
	return r1 && r2 && r3 && r4;
}
