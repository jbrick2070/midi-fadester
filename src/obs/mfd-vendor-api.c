/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/midi-parser.h"
#include <obs-module.h>
#include <util/bmem.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "obs/obs-websocket-api.h"
#include "plugin-support.h"

static obs_websocket_vendor g_vendor = NULL;
static proc_handler_t *g_bridge_ph = NULL;
static signal_handler_t *g_bridge_sh = NULL;
static mfd_vendor_event_sink_fn g_event_sink = NULL;
static void *g_event_sink_user_data = NULL;

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

static void set_error_comment(obs_data_t *response_data, const char *sentence)
{
	if (!response_data || !sentence) {
		return;
	}
	obs_data_set_string(response_data, "error", sentence);
	obs_data_set_string(response_data, "comment", sentence);
}

static int resolve_slot_or_error(obs_data_t *request_data, obs_data_t *response_data)
{
	const char *slot_id = request_data ? obs_data_get_string(request_data, "slot") : NULL;
	if (!slot_id || !*slot_id) {
		set_error_comment(response_data,
				  "Missing required 'slot' id. Valid slots: crossfader, fader_1 .. fader_8, master.");
		return -1;
	}
	int idx = mfd_slot_index_for_id(slot_id);
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		char msg[192];
		snprintf(msg, sizeof(msg),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.", slot_id);
		set_error_comment(response_data, msg);
		return -1;
	}
	return idx;
}

static const char *takeover_pending_str(const struct mfd_takeover *t)
{
	if (!t || t->state != MFD_TAKEOVER_WAITING) {
		return "none";
	}
	if (t->hint == MFD_HINT_MOVE_UP) {
		return "up";
	}
	if (t->hint == MFD_HINT_MOVE_DOWN) {
		return "down";
	}
	return "none";
}

static void populate_slot_data(const struct mfd_engine *eng, const struct mfd_fader_slot *s, obs_data_t *out)
{
	char dyn_label[128] = {0};
	bool available = false;
	char reason[160] = {0};
	mfd_engine_get_slot_status(eng, s, dyn_label, sizeof(dyn_label), &available, reason, sizeof(reason));

	obs_data_set_string(out, "id", s->id);
	obs_data_set_string(out, "name", s->label);
	obs_data_set_string(out, "kind", s->kind);
	obs_data_set_string(out, "target_mode", mfd_target_mode_to_string(s->target_type));
	obs_data_set_string(out, "source_a", s->source_a_name);
	obs_data_set_string(out, "source_b", s->source_b_name);
	obs_data_set_string(out, "curve", mfd_curve_to_string(s->curve));
	obs_data_set_bool(out, "afv", s->audio_follows_video);
	obs_data_set_double(out, "value", (double)s->last_applied_val);
	obs_data_set_string(out, "takeover_pending", takeover_pending_str(&s->takeover));
	obs_data_set_bool(out, "parked", s->takeover.state == MFD_TAKEOVER_PARKED);
	obs_data_set_string(out, "label", dyn_label);
	obs_data_set_bool(out, "available", available);
	obs_data_set_string(out, "reason", reason);
}

void mfd_vendor_set_event_sink(mfd_vendor_event_sink_fn fn, void *user_data)
{
	g_event_sink = fn;
	g_event_sink_user_data = user_data;
}

static void emit_vendor_event(const char *event_name, obs_data_t *event_data)
{
	if (!event_name || !event_data) {
		return;
	}
	if (g_event_sink) {
		g_event_sink(event_name, event_data, g_event_sink_user_data);
	}
	if (g_vendor && obs_get_proc_handler()) {
		obs_websocket_vendor_emit_event(g_vendor, event_name, event_data);
	}
}

void mfd_vendor_emit_fader_changed(const char *slot_id, float value, const char *origin)
{
	if (!slot_id || !*slot_id) {
		return;
	}
	float clamped = clamp01(value);
	const char *orig = (origin && *origin) ? origin : "api";

	/* 1. Emit in-process signal `midi_fadester_fader_changed` */
	if (g_bridge_sh) {
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "slot", slot_id);
		calldata_set_float(&cd, "value", (double)clamped);
		calldata_set_string(&cd, "origin", orig);
		signal_handler_signal(g_bridge_sh, "midi_fadester_fader_changed", &cd);
		calldata_free(&cd);
	}

	/* Also notify Visual Sidechain's fader slot proc if registered on the proc_handler */
	if (g_bridge_ph) {
		struct calldata vs_cd;
		calldata_init(&vs_cd);
		calldata_set_string(&vs_cd, "slot", slot_id);
		calldata_set_float(&vs_cd, "value", (double)clamped);
		proc_handler_call(g_bridge_ph, "visual_sidechain_set_fader_slot", &vs_cd);
		calldata_free(&vs_cd);
	}

	/* 2. Emit obs-websocket vendor event `fader_changed` */
	obs_data_t *ev = obs_data_create();
	obs_data_set_string(ev, "slot", slot_id);
	obs_data_set_double(ev, "value", (double)clamped);
	obs_data_set_string(ev, "origin", orig);
	emit_vendor_event("fader_changed", ev);
	obs_data_release(ev);
}

void mfd_vendor_emit_state_changed(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	obs_data_t *ev = obs_data_create();
	mfd_vendor_handle_get_state(NULL, ev, eng);
	emit_vendor_event("state_changed", ev);
	obs_data_release(ev);
}

static void proc_get_slot_value(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = (struct mfd_engine *)data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!cd || !eng) {
		return;
	}

	const char *slot_id = calldata_string(cd, "slot");
	if (!slot_id || !*slot_id) {
		slot_id = calldata_string(cd, "slot_id");
	}
	int idx = mfd_slot_index_for_id(slot_id);
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		char msg[192];
		snprintf(msg, sizeof(msg),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.",
			 (slot_id && *slot_id) ? slot_id : "");
		calldata_set_float(cd, "value", 0.0);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", msg);
		return;
	}

	const struct mfd_fader_slot *s = &eng->fader_slots[idx];
	bool available = false;
	char reason[160] = {0};
	mfd_engine_get_slot_status(eng, s, NULL, 0, &available, reason, sizeof(reason));
	UNUSED_PARAMETER(available);

	calldata_set_float(cd, "value", (double)s->last_applied_val);
	calldata_set_bool(cd, "ok", true);
	calldata_set_bool(cd, "success", true);
	calldata_set_string(cd, "reason", reason);
}

static void proc_set_fader_value(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = (struct mfd_engine *)data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!cd || !eng) {
		return;
	}

	const char *slot_id = calldata_string(cd, "slot");
	int idx = mfd_slot_index_for_id(slot_id);
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		char msg[192];
		snprintf(msg, sizeof(msg),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.",
			 (slot_id && *slot_id) ? slot_id : "");
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", msg);
		return;
	}

	float val = clamp01((float)calldata_float(cd, "value"));
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED);

	slot->auto_fade_active = false;
	slot->takeover.dial_reversed = false;
	mfd_takeover_unpark_from_api(&slot->takeover, val);
	mfd_smoother_reset(&slot->smoother, val);

	mfd_engine_apply_slot_output(eng, slot, val);
	if (slot->takeover.two_rail_mode && (val >= 1.0f || val <= 0.0f)) {
		mfd_takeover_notify_rail_completed(&slot->takeover, val >= 1.0f);
	}
	mfd_engine_notify_fader_changed(eng, idx, "api");
	if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
		mfd_engine_notify_state_changed(eng);
	}

	calldata_set_bool(cd, "ok", true);
	calldata_set_bool(cd, "success", true);
	calldata_set_string(cd, "reason", "");
}

bool mfd_bridge_register_with_handlers(proc_handler_t *ph, signal_handler_t *sh, struct mfd_engine *eng)
{
	g_bridge_ph = ph;
	g_bridge_sh = sh;

	if (ph) {
		proc_handler_add(
			ph,
			"void midi_fadester_get_slot_value(in string slot, out float value, out bool ok, out string reason)",
			proc_get_slot_value, eng);
		proc_handler_add(
			ph,
			"void midi_fadester_set_fader_value(in string slot, in float value, out bool ok, out string reason)",
			proc_set_fader_value, eng);
	}

	if (sh) {
		signal_handler_add(sh, "void midi_fadester_fader_changed(string slot, float value, string origin)");
	}

	return (ph != NULL) && (sh != NULL);
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

	obs_data_set_string(response_data, "version", PLUGIN_VERSION);
	obs_data_set_int(response_data, "api_version", MFD_API_VERSION);
	obs_data_set_bool(response_data, "studio_mode", mfd_engine_is_studio_mode_active(eng));

	obs_data_t *dev = obs_data_create();
	obs_data_set_string(dev, "name",
			    eng->midi_dev.is_open ? eng->midi_dev.opened_name : eng->configured_device_name);
	obs_data_set_bool(dev, "connected", eng->midi_dev.is_open);
	obs_data_set_string(dev, "error", eng->midi_dev.last_error);
	obs_data_set_obj(response_data, "device", dev);
	obs_data_release(dev);

	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		obs_data_t *item = obs_data_create();
		populate_slot_data(eng, &eng->fader_slots[i], item);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(response_data, "slots", arr);
	obs_data_array_release(arr);
}

void mfd_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	int idx = resolve_slot_or_error(request_data, response_data);
	if (idx < 0) {
		return;
	}
	if (!obs_data_has_user_value(request_data, "value")) {
		set_error_comment(response_data, "Missing required 'value' parameter (0.0 to 1.0).");
		return;
	}

	float val = clamp01((float)obs_data_get_double(request_data, "value"));
	bool release = obs_data_get_bool(request_data, "release");
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED);
	bool prev_upper = slot->takeover.parked_at_upper_rail;

	slot->auto_fade_active = false;
	slot->takeover.dial_reversed = false;

	if (slot->target_type == MFD_TARGET_TBAR) {
		if (release || val >= MFD_THROW_COMPLETE_THRESHOLD) {
			if (!(was_parked && slot->last_applied_val <= 0.0f)) {
				mfd_engine_apply_slot_output_ex(eng, slot, val, release);
				mfd_engine_notify_fader_changed(eng, idx, "api");
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		} else {
			mfd_takeover_unpark_from_api(&slot->takeover, val);
			mfd_smoother_reset(&slot->smoother, val);
			mfd_engine_apply_slot_output_ex(eng, slot, val, false);
			mfd_engine_notify_fader_changed(eng, idx, "api");
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	} else {
		bool at_upper = (val >= 1.0f) || (release && val >= 0.5f);
		bool at_lower = (val <= 0.0f) || (release && val < 0.5f);
		if (slot->takeover.two_rail_mode && (at_upper || at_lower)) {
			float rail_val = at_upper ? 1.0f : 0.0f;
			if (!(was_parked && prev_upper == at_upper &&
			      fabsf(slot->last_applied_val - rail_val) < 1e-5f)) {
				mfd_smoother_reset(&slot->smoother, rail_val);
				mfd_engine_apply_slot_output_ex(eng, slot, rail_val, false);
				mfd_takeover_notify_rail_completed(&slot->takeover, at_upper);
				mfd_engine_notify_fader_changed(eng, idx, "api");
				mfd_engine_notify_state_changed(eng);
			}
		} else {
			mfd_takeover_unpark_from_api(&slot->takeover, val);
			mfd_smoother_reset(&slot->smoother, val);
			mfd_engine_apply_slot_output_ex(eng, slot, val, false);
			mfd_engine_notify_fader_changed(eng, idx, "api");
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	}

	obs_data_set_string(response_data, "slot", slot->id);
	obs_data_set_double(response_data, "value", (double)slot->last_applied_val);
}

void mfd_vendor_handle_nudge_fader(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	int idx = resolve_slot_or_error(request_data, response_data);
	if (idx < 0) {
		return;
	}
	if (!obs_data_has_user_value(request_data, "delta")) {
		set_error_comment(response_data, "Missing required 'delta' parameter (-1.0 to 1.0).");
		return;
	}

	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	float delta = (float)obs_data_get_double(request_data, "delta");
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED);

	slot->auto_fade_active = false;

	if (slot->target_type == MFD_TARGET_TBAR) {
		float eff_delta = delta;
		if (was_parked) {
			slot->takeover.dial_reversed = (delta < 0.0f);
			eff_delta = fabsf(delta);
		} else if (slot->takeover.dial_reversed) {
			eff_delta = -delta;
		}
		float val = clamp01(slot->last_applied_val + eff_delta);
		mfd_takeover_unpark_from_api(&slot->takeover, val);
		mfd_smoother_reset(&slot->smoother, val);
		mfd_engine_apply_slot_output(eng, slot, val);
		mfd_engine_notify_fader_changed(eng, idx, "api");
		if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
			mfd_engine_notify_state_changed(eng);
		}
	} else {
		slot->takeover.dial_reversed = false;
		float val = clamp01(slot->last_applied_val + delta);
		mfd_takeover_unpark_from_api(&slot->takeover, val);
		mfd_smoother_reset(&slot->smoother, val);
		mfd_engine_apply_slot_output(eng, slot, val);
		if (!was_parked && slot->takeover.two_rail_mode && (val >= 1.0f || val <= 0.0f)) {
			mfd_takeover_notify_rail_completed(&slot->takeover, val >= 1.0f);
		}
		mfd_engine_notify_fader_changed(eng, idx, "api");
		if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
			mfd_engine_notify_state_changed(eng);
		}
	}

	obs_data_set_string(response_data, "slot", slot->id);
	obs_data_set_double(response_data, "value", (double)slot->last_applied_val);
}

void mfd_vendor_handle_auto_fade(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	int idx = resolve_slot_or_error(request_data, response_data);
	if (idx < 0) {
		return;
	}
	if (!obs_data_has_user_value(request_data, "to") || !obs_data_has_user_value(request_data, "ms")) {
		set_error_comment(response_data, "Missing required 'to' (0.0..1.0) or 'ms' (50..20000) parameter.");
		return;
	}

	long long ms = obs_data_get_int(request_data, "ms");
	if (ms < 50 || ms > 20000) {
		set_error_comment(response_data,
				  "Invalid 'ms' duration. Specify a fade duration between 50 and 20000 ms.");
		return;
	}

	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	float to_val = clamp01((float)obs_data_get_double(request_data, "to"));
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED);

	if (was_parked) {
		if (slot->target_type == MFD_TARGET_TBAR && to_val <= 0.0f && slot->last_applied_val <= 0.0f) {
			slot->auto_fade_active = false;
			obs_data_set_string(response_data, "slot", slot->id);
			return;
		}
		if (slot->target_type != MFD_TARGET_TBAR &&
		    ((slot->takeover.parked_at_upper_rail && to_val >= 1.0f) ||
		     (!slot->takeover.parked_at_upper_rail && to_val <= 0.0f)) &&
		    fabsf(slot->last_applied_val - to_val) < 1e-5f) {
			slot->auto_fade_active = false;
			obs_data_set_string(response_data, "slot", slot->id);
			return;
		}
	}

	slot->auto_fade_from = slot->last_applied_val;
	slot->auto_fade_to = to_val;
	slot->auto_fade_duration_sec = (float)ms / 1000.0f;
	slot->auto_fade_elapsed_sec = 0.0f;
	slot->auto_fade_active = true;
	if (was_parked) {
		mfd_takeover_unpark_from_api(&slot->takeover, slot->auto_fade_from);
		mfd_engine_notify_state_changed(eng);
	}

	obs_data_set_string(response_data, "slot", slot->id);
}

void mfd_vendor_handle_set_slot(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	int idx = resolve_slot_or_error(request_data, response_data);
	if (idx < 0) {
		return;
	}

	struct mfd_fader_slot *s = &eng->fader_slots[idx];
	enum mfd_target_type new_target = s->target_type;
	enum mfd_crossfade_curve new_curve = s->curve;
	bool has_target = obs_data_has_user_value(request_data, "target_mode");
	bool has_curve = obs_data_has_user_value(request_data, "curve");
	bool has_sa = obs_data_has_user_value(request_data, "source_a");
	bool has_sb = obs_data_has_user_value(request_data, "source_b");

	if (has_target) {
		const char *tm = obs_data_get_string(request_data, "target_mode");
		if (!mfd_target_mode_from_string(tm, &new_target)) {
			char msg[192];
			snprintf(
				msg, sizeof(msg),
				"Invalid target_mode '%s'. Valid modes: unassigned, tbar, crossfade_ab, layer_opacity, audio_volume.",
				tm ? tm : "");
			set_error_comment(response_data, msg);
			return;
		}
	}

	if (has_curve) {
		const char *cv = obs_data_get_string(request_data, "curve");
		if (!mfd_curve_from_string(cv, &new_curve)) {
			char msg[192];
			snprintf(msg, sizeof(msg),
				 "Invalid curve '%s'. Valid curves: dip_free, full_center, equal_power, linear, cut.",
				 cv ? cv : "");
			set_error_comment(response_data, msg);
			return;
		}
	}

	const char *sa = has_sa ? obs_data_get_string(request_data, "source_a") : NULL;
	if (has_sa && sa && sa[0] != '\0' && !mfd_engine_source_exists(eng, sa)) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Source '%s' not found. Create the source in OBS first or check its spelling.", sa);
		set_error_comment(response_data, msg);
		return;
	}

	const char *sb = has_sb ? obs_data_get_string(request_data, "source_b") : NULL;
	if (has_sb && sb && sb[0] != '\0' && !mfd_engine_source_exists(eng, sb)) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Source '%s' not found. Create the source in OBS first or check its spelling.", sb);
		set_error_comment(response_data, msg);
		return;
	}

	if (has_target) {
		enum mfd_target_type prev_target = s->target_type;
		s->target_type = new_target;
		s->enabled = true;
		bool is_tbar = (s->target_type == MFD_TARGET_TBAR);
		bool is_two_rail = (s->target_type == MFD_TARGET_TWO_SOURCE_MIX ||
				    s->target_type == MFD_TARGET_SINGLE_LAYER ||
				    s->target_type == MFD_TARGET_AUDIO_VOLUME);
		if (prev_target == MFD_TARGET_UNASSIGNED && new_target != MFD_TARGET_UNASSIGNED) {
			float init_val =
				(new_target == MFD_TARGET_SINGLE_LAYER || new_target == MFD_TARGET_AUDIO_VOLUME) ? 1.0f
														 : 0.0f;
			mfd_smoother_reset(&s->smoother, init_val);
			mfd_takeover_init(&s->takeover, init_val, MFD_DEFAULT_PICKUP_THRESHOLD,
					  is_tbar ? eng->tbar_bidirectional_flip : false);
			s->last_raw_norm = init_val;
			s->last_applied_val = init_val;
		}
		s->takeover.bidirectional_flip = is_tbar ? eng->tbar_bidirectional_flip : false;
		mfd_takeover_set_two_rail_mode(&s->takeover, is_two_rail);
	}
	if (has_curve) {
		s->curve = new_curve;
	}
	if (has_sa) {
		snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", sa ? sa : "");
	}
	if (has_sb) {
		snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", sb ? sb : "");
	}
	if (obs_data_has_user_value(request_data, "afv")) {
		s->audio_follows_video = obs_data_get_bool(request_data, "afv");
	}

	mfd_engine_notify_state_changed(eng);
	populate_slot_data(eng, s, response_data);
}

void mfd_vendor_handle_load_profile(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng) {
		return;
	}

	const char *name = request_data ? obs_data_get_string(request_data, "name") : NULL;
	if (!name || strcmp(name, "apc40mk2") != 0) {
		char msg[192];
		snprintf(msg, sizeof(msg), "Unknown profile '%s'. Valid profiles: apc40mk2.", name ? name : "");
		set_error_comment(response_data, msg);
		return;
	}

	mfd_engine_load_apc40_defaults(eng);
	mfd_engine_notify_state_changed(eng);
	obs_data_set_string(response_data, "loaded", "apc40mk2");
}

bool mfd_vendor_api_register(struct mfd_engine *eng)
{
	proc_handler_t *global_ph = obs_get_proc_handler();
	signal_handler_t *global_sh = obs_get_signal_handler();

	if (global_ph && global_sh) {
		mfd_bridge_register_with_handlers(global_ph, global_sh, eng);
		obs_log(LOG_INFO,
			"Registered in-process bridge procs (midi_fadester_get_slot_value, midi_fadester_set_fader_value) and signal (midi_fadester_fader_changed)");
	}

	if (!global_ph) {
		obs_log(LOG_INFO, "obs-websocket not available (no global proc_handler); skipping vendor API");
		return false;
	}

	g_vendor = obs_websocket_register_vendor(MFD_VENDOR_NAME);
	if (!g_vendor) {
		obs_log(LOG_INFO, "obs-websocket is not loaded; skipping 'midi-fadester' vendor API registration");
		return false;
	}

	bool r1 = obs_websocket_vendor_register_request(g_vendor, "get_state", mfd_vendor_handle_get_state, eng);
	bool r2 = obs_websocket_vendor_register_request(g_vendor, "set_fader_value", mfd_vendor_handle_set_fader_value,
							eng);
	bool r3 = obs_websocket_vendor_register_request(g_vendor, "nudge_fader", mfd_vendor_handle_nudge_fader, eng);
	bool r4 = obs_websocket_vendor_register_request(g_vendor, "auto_fade", mfd_vendor_handle_auto_fade, eng);
	bool r5 = obs_websocket_vendor_register_request(g_vendor, "set_slot", mfd_vendor_handle_set_slot, eng);
	bool r6 = obs_websocket_vendor_register_request(g_vendor, "load_profile", mfd_vendor_handle_load_profile, eng);

	obs_log(LOG_INFO,
		"Registered obs-websocket vendor '%s' requests (get_state=%d, set_fader_value=%d, nudge_fader=%d, auto_fade=%d, set_slot=%d, load_profile=%d)",
		MFD_VENDOR_NAME, (int)r1, (int)r2, (int)r3, (int)r4, (int)r5, (int)r6);
	return r1 && r2 && r3 && r4 && r5 && r6;
}
