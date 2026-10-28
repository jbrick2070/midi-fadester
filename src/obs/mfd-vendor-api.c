/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/midi-parser.h"
#include "core/tbar-logic.h"
#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>
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
static volatile bool g_vendor_shutting_down = false;

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
		set_error_comment(
			response_data,
			"Missing required 'slot' id. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.");
		return -1;
	}
	int idx = mfd_slot_index_for_id(slot_id);
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		char msg[224];
		snprintf(msg, sizeof(msg),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.",
			 slot_id);
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
	char disp_val[64] = {0};
	mfd_engine_get_slot_status(eng, s, dyn_label, sizeof(dyn_label), &available, reason, sizeof(reason));
	mfd_engine_format_slot_display_value(s, s->last_applied_val, disp_val, sizeof(disp_val));

	obs_data_set_string(out, "id", s->id);
	obs_data_set_string(out, "name", s->label);
	obs_data_set_string(out, "kind", s->kind);
	obs_data_set_bool(out, "enabled", s->enabled);
	obs_data_set_string(out, "target_mode", mfd_target_mode_to_string(s->target_type));
	obs_data_set_string(out, "scene", (s->scene_name[0] != '\0') ? s->scene_name : s->resolved_scene_name);
	obs_data_set_string(out, "source_a", s->source_a_name);
	obs_data_set_string(out, "source_b", s->source_b_name);
	obs_data_set_string(out, "filter_name", s->filter_name);
	obs_data_set_string(out, "filter_prop", s->filter_prop);
	obs_data_set_double(out, "from_val", s->filter_from_val);
	obs_data_set_double(out, "to_val", s->filter_to_val);
	obs_data_set_bool(out, "bypass_at_zero", s->filter_bypass_at_zero);
	obs_data_set_string(out, "curve", mfd_curve_to_string(s->curve));
	obs_data_set_bool(out, "afv", s->audio_follows_video);
	obs_data_set_double(out, "value", (double)s->last_applied_val);
	obs_data_set_string(out, "display_value", disp_val);
	obs_data_set_int(out, "cal_min", s->cal.cal_min);
	obs_data_set_int(out, "cal_max", s->cal.cal_max);
	obs_data_set_string(out, "takeover_pending", takeover_pending_str(&s->takeover));
	obs_data_set_bool(out, "parked", s->takeover.state == MFD_TAKEOVER_PARKED);
	obs_data_set_string(out, "label", dyn_label);
	obs_data_set_bool(out, "available", available);
	obs_data_set_string(out, "reason", reason);
}

void mfd_vendor_set_event_sink(mfd_vendor_event_sink_fn fn, void *user_data)
{
	os_atomic_store_bool(&g_vendor_shutting_down, false);
	g_event_sink = fn;
	g_event_sink_user_data = user_data;
}

static void emit_vendor_event(const char *event_name, obs_data_t *event_data)
{
	if (!event_name || !event_data || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}
	if (g_event_sink) {
		g_event_sink(event_name, event_data, g_event_sink_user_data);
	}
	if (g_vendor && obs_get_proc_handler()) {
		obs_websocket_vendor_emit_event(g_vendor, event_name, event_data);
	}
}

void mfd_vendor_emit_fader_changed_ex(const char *slot_id, float value, const char *display_value, const char *origin)
{
	if (!slot_id || !*slot_id || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}
	float clamped = clamp01(value);
	const char *orig = (origin && *origin) ? origin : "api";
	char fallback_disp[32] = {0};
	const char *disp = (display_value && *display_value) ? display_value : fallback_disp;
	if (!display_value || !*display_value) {
		snprintf(fallback_disp, sizeof(fallback_disp), "%d%%", (int)lroundf(clamped * 100.0f));
	}

	/* 1. Emit in-process signal `midi_fadester_fader_changed` */
	if (g_bridge_sh) {
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "slot", slot_id);
		calldata_set_float(&cd, "value", (double)clamped);
		calldata_set_string(&cd, "origin", orig);
		calldata_set_string(&cd, "display_value", disp);
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
	obs_data_set_string(ev, "display_value", disp);
	obs_data_set_string(ev, "origin", orig);
	emit_vendor_event("fader_changed", ev);
	obs_data_release(ev);
}

void mfd_vendor_emit_fader_changed(const char *slot_id, float value, const char *origin)
{
	mfd_vendor_emit_fader_changed_ex(slot_id, value, NULL, origin);
}

void mfd_vendor_emit_state_changed(struct mfd_engine *eng)
{
	if (!eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
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
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		calldata_set_float(cd, "value", 0.0);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", "Midi Fadester is shutting down.");
		return;
	}

	const char *slot_id = calldata_string(cd, "slot");
	if (!slot_id || !*slot_id) {
		slot_id = calldata_string(cd, "slot_id");
	}
	int idx = mfd_slot_index_for_id(slot_id);
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		char msg[224];
		snprintf(msg, sizeof(msg),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.",
			 (slot_id && *slot_id) ? slot_id : "");
		calldata_set_float(cd, "value", 0.0);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", msg);
		return;
	}

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		calldata_set_float(cd, "value", 0.0);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", "Midi Fadester is shutting down.");
		return;
	}

	const struct mfd_fader_slot *s = &eng->fader_slots[idx];
	double val = (double)s->last_applied_val;
	bool available = false;
	char reason[160] = {0};
	mfd_engine_get_slot_status(eng, s, NULL, 0, &available, reason, sizeof(reason));
	UNUSED_PARAMETER(available);
	mfd_engine_unlock(eng);

	calldata_set_float(cd, "value", val);
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
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", "Midi Fadester is shutting down.");
		return;
	}

	const char *slot_id = calldata_string(cd, "slot");
	int idx = mfd_slot_index_for_id(slot_id);
	if (idx < 0 || idx >= MFD_MAX_FADER_SLOTS) {
		char msg[224];
		snprintf(msg, sizeof(msg),
			 "Slot '%s' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.",
			 (slot_id && *slot_id) ? slot_id : "");
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", msg);
		return;
	}

	float val = clamp01((float)calldata_float(cd, "value"));
	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", "Midi Fadester is shutting down.");
		return;
	}
	if (eng->setup_session.active && eng->setup_session.slot_index == idx) {
		mfd_engine_unlock(eng);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", "Strip is currently paused while Set up is open.");
		return;
	}
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	if (!mfd_engine_is_slot_writable(eng, slot)) {
		bool avail = false;
		char reason[160] = {0};
		mfd_engine_get_slot_status(eng, slot, NULL, 0U, &avail, reason, sizeof(reason));
		mfd_engine_unlock(eng);
		calldata_set_bool(cd, "ok", false);
		calldata_set_bool(cd, "success", false);
		calldata_set_string(cd, "reason", reason);
		return;
	}
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
			  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);

	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->takeover.dial_reversed = false;

	if (slot->target_type == MFD_TARGET_TBAR) {
		if (mfd_tbar_is_in_target_rail_zone(val, false)) {
			if (!was_parked) {
				if (slot->tbar_state == MFD_TBAR_IDLE) {
					mfd_engine_unlatch_tbar(eng);
				}
				mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
				mfd_engine_notify_fader_changed(eng, idx, "api");
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		} else if (mfd_tbar_is_in_origin_rail_zone(val, false)) {
			if (slot->tbar_state == MFD_TBAR_THROWING) {
				mfd_engine_apply_tbar_abort(eng, slot);
				mfd_engine_notify_fader_changed(eng, idx, "api");
				mfd_engine_notify_state_changed(eng);
			} else if (was_parked) {
				mfd_engine_unlatch_tbar(eng);
				slot->tbar_state = MFD_TBAR_IDLE;
				mfd_takeover_unpark_from_api(&slot->takeover, 0.0f);
				mfd_smoother_reset(&slot->smoother, 0.0f);
				slot->last_applied_val = 0.0f;
				mfd_engine_notify_fader_changed(eng, idx, "api");
				mfd_engine_notify_state_changed(eng);
			}
		} else {
			if (was_parked || slot->tbar_state == MFD_TBAR_IDLE) {
				mfd_engine_unlatch_tbar(eng);
			}
			slot->tbar_state = MFD_TBAR_THROWING;
			mfd_takeover_unpark_from_api(&slot->takeover, val);
			mfd_smoother_reset(&slot->smoother, val);
			mfd_engine_apply_slot_output(eng, slot, val);
			mfd_engine_notify_fader_changed(eng, idx, "api");
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	} else {
		mfd_takeover_unpark_from_api(&slot->takeover, val);
		mfd_smoother_reset(&slot->smoother, val);
		mfd_engine_apply_slot_output(eng, slot, val);
		if (slot->takeover.two_rail_mode && (val >= 1.0f || val <= 0.0f)) {
			mfd_takeover_notify_rail_completed(&slot->takeover, val >= 1.0f, val);
		}
		mfd_engine_notify_fader_changed(eng, idx, "api");
		if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
			mfd_engine_notify_state_changed(eng);
		}
	}

	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);

	calldata_set_bool(cd, "ok", true);
	calldata_set_bool(cd, "success", true);
	calldata_set_string(cd, "reason", "");
}

bool mfd_bridge_register_with_handlers(proc_handler_t *ph, signal_handler_t *sh, struct mfd_engine *eng)
{
	os_atomic_store_bool(&g_vendor_shutting_down, false);
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
		signal_handler_add(
			sh,
			"void midi_fadester_fader_changed(string slot, float value, string origin, string display_value)");
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
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}

	obs_data_set_string(response_data, "version", PLUGIN_VERSION);

	obs_data_t *build = obs_data_create();
	obs_data_set_string(build, "commit", PLUGIN_GIT_COMMIT);
	obs_data_set_string(build, "built_at", PLUGIN_BUILT_AT);
	obs_data_set_obj(response_data, "build", build);
	obs_data_release(build);

	obs_data_set_int(response_data, "api_version", MFD_API_VERSION);
	obs_data_set_bool(response_data, "studio_mode", mfd_engine_is_studio_mode_active(eng));

	const struct mfd_controller_profile_def *pdef = mfd_profile_get_def(mfd_engine_get_active_profile(eng));
	obs_data_set_string(response_data, "profile", pdef ? pdef->slug : "apc40mk2");
	obs_data_set_string(response_data, "profile_display_name", pdef ? pdef->display_name : "Akai APC40 mkII");
	obs_data_set_string(response_data, "profile_tier",
			    mfd_profile_tier_to_string(pdef ? pdef->tier : MFD_PROFILE_TIER_VERIFIED));

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

	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_set_fader_value(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
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

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	if ((eng->setup_session.active && eng->setup_session.slot_index == idx) ||
	    !mfd_engine_is_slot_writable(eng, slot)) {
		obs_data_set_string(response_data, "slot", slot->id);
		obs_data_set_double(response_data, "value", (double)slot->last_applied_val);
		mfd_engine_unlock(eng);
		return;
	}
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
			  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);
	bool prev_upper = slot->takeover.parked_at_upper_rail;

	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->takeover.dial_reversed = false;

	if (slot->target_type == MFD_TARGET_TBAR) {
		if (release || mfd_tbar_is_in_target_rail_zone(val, false)) {
			if (!was_parked) {
				if (slot->tbar_state == MFD_TBAR_IDLE) {
					mfd_engine_unlatch_tbar(eng);
				}
				mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
				mfd_engine_notify_fader_changed(eng, idx, "api");
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		} else if (mfd_tbar_is_in_origin_rail_zone(val, false)) {
			if (slot->tbar_state == MFD_TBAR_THROWING) {
				mfd_engine_apply_tbar_abort(eng, slot);
				mfd_engine_notify_fader_changed(eng, idx, "api");
				mfd_engine_notify_state_changed(eng);
			} else if (was_parked) {
				mfd_engine_unlatch_tbar(eng);
				slot->tbar_state = MFD_TBAR_IDLE;
				mfd_takeover_unpark_from_api(&slot->takeover, 0.0f);
				mfd_smoother_reset(&slot->smoother, 0.0f);
				slot->last_applied_val = 0.0f;
				mfd_engine_notify_fader_changed(eng, idx, "api");
				mfd_engine_notify_state_changed(eng);
			}
		} else {
			if (was_parked || slot->tbar_state == MFD_TBAR_IDLE) {
				mfd_engine_unlatch_tbar(eng);
			}
			slot->tbar_state = MFD_TBAR_THROWING;
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
				mfd_takeover_notify_rail_completed(&slot->takeover, at_upper, rail_val);
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

	mfd_engine_mark_ui_dirty(eng, false);
	obs_data_set_string(response_data, "slot", slot->id);
	obs_data_set_double(response_data, "value", (double)slot->last_applied_val);
	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_nudge_fader(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
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

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	if ((eng->setup_session.active && eng->setup_session.slot_index == idx) ||
	    !mfd_engine_is_slot_writable(eng, slot)) {
		obs_data_set_string(response_data, "slot", slot->id);
		obs_data_set_double(response_data, "value", (double)slot->last_applied_val);
		mfd_engine_unlock(eng);
		return;
	}
	float delta = (float)obs_data_get_double(request_data, "delta");
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
			  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);

	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;

	if (slot->target_type == MFD_TARGET_TBAR) {
		if (!(was_parked && fabsf(delta) <= 1e-6f)) {
			float eff_delta = delta;
			if (was_parked) {
				slot->takeover.dial_reversed = (delta < 0.0f);
				eff_delta = fabsf(delta);
				mfd_engine_unlatch_tbar(eng);
				slot->tbar_state = MFD_TBAR_IDLE;
			} else if (slot->takeover.dial_reversed) {
				eff_delta = -delta;
			}
			float val = clamp01(slot->last_applied_val + eff_delta);
			if (mfd_tbar_is_in_target_rail_zone(val, false)) {
				if (slot->tbar_state == MFD_TBAR_IDLE) {
					mfd_engine_unlatch_tbar(eng);
				}
				mfd_takeover_unpark_from_api(&slot->takeover, val);
				mfd_smoother_reset(&slot->smoother, 1.0f);
				mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
			} else if (mfd_tbar_is_in_origin_rail_zone(val, false)) {
				mfd_takeover_unpark_from_api(&slot->takeover, 0.0f);
				mfd_smoother_reset(&slot->smoother, 0.0f);
				if (slot->tbar_state == MFD_TBAR_THROWING) {
					mfd_engine_apply_tbar_abort(eng, slot);
				} else {
					slot->tbar_state = MFD_TBAR_IDLE;
					slot->last_applied_val = 0.0f;
				}
			} else {
				if (slot->tbar_state == MFD_TBAR_IDLE) {
					mfd_engine_unlatch_tbar(eng);
				}
				slot->tbar_state = MFD_TBAR_THROWING;
				mfd_takeover_unpark_from_api(&slot->takeover, val);
				mfd_smoother_reset(&slot->smoother, val);
				mfd_engine_apply_slot_output(eng, slot, val);
			}
			mfd_engine_notify_fader_changed(eng, idx, "api");
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	} else {
		slot->takeover.dial_reversed = false;
		float val = clamp01(slot->last_applied_val + delta);
		mfd_takeover_unpark_from_api(&slot->takeover, val);
		mfd_smoother_reset(&slot->smoother, val);
		mfd_engine_apply_slot_output(eng, slot, val);
		if (!was_parked && slot->takeover.two_rail_mode && (val >= 1.0f || val <= 0.0f)) {
			mfd_takeover_notify_rail_completed(&slot->takeover, val >= 1.0f, val);
		}
		mfd_engine_notify_fader_changed(eng, idx, "api");
		if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
			mfd_engine_notify_state_changed(eng);
		}
	}

	mfd_engine_mark_ui_dirty(eng, false);
	obs_data_set_string(response_data, "slot", slot->id);
	obs_data_set_double(response_data, "value", (double)slot->last_applied_val);
	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_auto_fade(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
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

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}
	struct mfd_fader_slot *slot = &eng->fader_slots[idx];
	if ((eng->setup_session.active && eng->setup_session.slot_index == idx) ||
	    !mfd_engine_is_slot_writable(eng, slot)) {
		obs_data_set_string(response_data, "slot", slot->id);
		mfd_engine_unlock(eng);
		return;
	}
	float to_val = clamp01((float)obs_data_get_double(request_data, "to"));
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
			  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);

	if (was_parked) {
		if (slot->target_type == MFD_TARGET_TBAR && to_val <= 0.0f && slot->last_applied_val <= 0.0f) {
			slot->auto_fade_active = false;
			slot->tbar_midi_active = false;
			obs_data_set_string(response_data, "slot", slot->id);
			mfd_engine_unlock(eng);
			return;
		}
		if (slot->target_type != MFD_TARGET_TBAR &&
		    ((slot->takeover.parked_at_upper_rail && to_val >= 1.0f) ||
		     (!slot->takeover.parked_at_upper_rail && to_val <= 0.0f)) &&
		    fabsf(slot->last_applied_val - to_val) < 1e-5f) {
			slot->auto_fade_active = false;
			slot->tbar_midi_active = false;
			obs_data_set_string(response_data, "slot", slot->id);
			mfd_engine_unlock(eng);
			return;
		}
	}

	slot->auto_fade_from = slot->last_applied_val;
	slot->auto_fade_to = to_val;
	slot->auto_fade_duration_sec = (float)ms / 1000.0f;
	slot->auto_fade_elapsed_sec = 0.0f;
	slot->auto_fade_active = true;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	if (was_parked) {
		if (slot->target_type == MFD_TARGET_TBAR) {
			mfd_engine_unlatch_tbar(eng);
			slot->tbar_state = MFD_TBAR_IDLE;
		}
		mfd_takeover_unpark_from_api(&slot->takeover, slot->auto_fade_from);
		mfd_engine_notify_state_changed(eng);
	}

	mfd_engine_mark_ui_dirty(eng, false);
	obs_data_set_string(response_data, "slot", slot->id);
	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_set_slot(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}

	int idx = resolve_slot_or_error(request_data, response_data);
	if (idx < 0) {
		return;
	}

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}

	struct mfd_fader_slot *s = &eng->fader_slots[idx];
	bool was_enabled = s->enabled;
	enum mfd_target_type new_target = s->target_type;
	enum mfd_crossfade_curve new_curve = s->curve;
	bool has_target = obs_data_has_user_value(request_data, "target_mode");
	bool has_enabled = obs_data_has_user_value(request_data, "enabled");
	bool has_curve = obs_data_has_user_value(request_data, "curve");
	bool has_scene = obs_data_has_user_value(request_data, "scene");
	bool has_sa = obs_data_has_user_value(request_data, "source_a");
	bool has_sb = obs_data_has_user_value(request_data, "source_b");
	bool has_fn = obs_data_has_user_value(request_data, "filter_name");
	bool has_fp = obs_data_has_user_value(request_data, "filter_prop");
	bool has_from = obs_data_has_user_value(request_data, "from_val");
	bool has_to = obs_data_has_user_value(request_data, "to_val");
	bool has_bypass = obs_data_has_user_value(request_data, "bypass_at_zero");
	bool has_cal_min = obs_data_has_user_value(request_data, "cal_min");
	bool has_cal_max = obs_data_has_user_value(request_data, "cal_max");
	int new_cal_min = has_cal_min ? (int)obs_data_get_int(request_data, "cal_min") : s->cal.cal_min;
	int new_cal_max = has_cal_max ? (int)obs_data_get_int(request_data, "cal_max") : s->cal.cal_max;

	if (has_enabled) {
		obs_data_item_t *item = obs_data_item_byname(request_data, "enabled");
		bool is_boolean = item && obs_data_item_gettype(item) == OBS_DATA_BOOLEAN;
		obs_data_item_release(&item);
		if (!is_boolean) {
			set_error_comment(response_data, "Parameter 'enabled' must be a boolean.");
			mfd_engine_unlock(eng);
			return;
		}
	}

	if ((has_cal_min || has_cal_max) && (new_cal_min < 0 || new_cal_max <= new_cal_min)) {
		set_error_comment(response_data,
				  "Invalid calibration range: 'cal_max' must be greater than 'cal_min' (>= 0).");
		mfd_engine_unlock(eng);
		return;
	}

	if (has_target) {
		const char *tm = obs_data_get_string(request_data, "target_mode");
		if (!mfd_target_mode_from_string(tm, &new_target)) {
			char msg[224];
			snprintf(
				msg, sizeof(msg),
				"Invalid target_mode '%s'. Valid modes: unassigned, tbar, crossfade_ab, layer_opacity, audio_volume, filter_param.",
				tm ? tm : "");
			set_error_comment(response_data, msg);
			mfd_engine_unlock(eng);
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
			mfd_engine_unlock(eng);
			return;
		}
	}

	const char *sc = has_scene ? obs_data_get_string(request_data, "scene") : NULL;
	if (has_scene && sc && sc[0] != '\0' && !mfd_engine_scene_exists(eng, sc)) {
		char msg[256];
		snprintf(msg, sizeof(msg), "Scene '%s' not found. Create the scene in OBS first or check its spelling.",
			 sc);
		set_error_comment(response_data, msg);
		mfd_engine_unlock(eng);
		return;
	}

	const char *sa = has_sa ? obs_data_get_string(request_data, "source_a") : NULL;
	if (has_sa && sa && sa[0] != '\0' && !mfd_engine_source_exists(eng, sa)) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Source '%s' not found. Create the source in OBS first or check its spelling.", sa);
		set_error_comment(response_data, msg);
		mfd_engine_unlock(eng);
		return;
	}

	const char *sb = has_sb ? obs_data_get_string(request_data, "source_b") : NULL;
	if (has_sb && sb && sb[0] != '\0' && !mfd_engine_source_exists(eng, sb)) {
		char msg[256];
		snprintf(msg, sizeof(msg),
			 "Source '%s' not found. Create the source in OBS first or check its spelling.", sb);
		set_error_comment(response_data, msg);
		mfd_engine_unlock(eng);
		return;
	}

	const char *fn = has_fn ? obs_data_get_string(request_data, "filter_name") : NULL;
	const char *fp = has_fp ? obs_data_get_string(request_data, "filter_prop") : NULL;
	const char *cand_sa = has_sa ? (sa ? sa : "") : s->source_a_name;
	const char *cand_fn = has_fn ? (fn ? fn : "") : s->filter_name;
	const char *cand_fp = has_fp ? (fp ? fp : "") : s->filter_prop;
	bool same_filter_binding = (s->target_type == MFD_TARGET_FILTER_PARAM &&
				    strcmp(s->source_a_name, cand_sa) == 0 && strcmp(s->filter_name, cand_fn) == 0 &&
				    strcmp(s->filter_prop, cand_fp) == 0);
	double cand_from = s->filter_from_val;
	double cand_to = s->filter_to_val;
	bool new_enabled = has_enabled ? obs_data_get_bool(request_data, "enabled") : (has_target ? true : s->enabled);
	/* A pure disable must remain possible after the existing filter disappears. */
	bool disable_unchanged_filter = has_enabled && !new_enabled && !has_target && !has_sa && !has_fn && !has_fp &&
					!has_from && !has_to && !has_bypass;

	if (new_target == MFD_TARGET_FILTER_PARAM && !disable_unchanged_filter) {
		char err_msg[256] = {0};
		struct mfd_filter_prop_info prop_info;
		memset(&prop_info, 0, sizeof(prop_info));
		if (!same_filter_binding && !(has_from && has_to)) {
			if (!mfd_engine_validate_filter_param_binding_locked(eng, idx, new_enabled, cand_sa, cand_fn,
									     cand_fp, false, &cand_from, &cand_to,
									     &prop_info, err_msg, sizeof(err_msg))) {
				set_error_comment(response_data, err_msg);
				mfd_engine_unlock(eng);
				return;
			}
			if (has_from) {
				cand_from = obs_data_get_double(request_data, "from_val");
			}
			if (has_to) {
				cand_to = obs_data_get_double(request_data, "to_val");
			}
			if ((has_from || has_to) &&
			    !mfd_engine_validate_filter_param_binding_locked(eng, idx, new_enabled, cand_sa, cand_fn,
									     cand_fp, true, &cand_from, &cand_to,
									     &prop_info, err_msg, sizeof(err_msg))) {
				set_error_comment(response_data, err_msg);
				mfd_engine_unlock(eng);
				return;
			}
		} else {
			if (has_from) {
				cand_from = obs_data_get_double(request_data, "from_val");
			}
			if (has_to) {
				cand_to = obs_data_get_double(request_data, "to_val");
			}
			if (!mfd_engine_validate_filter_param_binding_locked(eng, idx, new_enabled, cand_sa, cand_fn,
									     cand_fp, true, &cand_from, &cand_to,
									     &prop_info, err_msg, sizeof(err_msg))) {
				set_error_comment(response_data, err_msg);
				mfd_engine_unlock(eng);
				return;
			}
		}
	}

	mfd_engine_detach_filter_binding_locked(eng, idx);
	s->enabled = new_enabled;
	if (has_enabled && !new_enabled) {
		/* Explicit disable cancels intent instead of queuing an old reset for reenable. */
		s->auto_fade_active = false;
		s->auto_fade_is_reset_fx = false;
		s->pending_clean_apply_on_avail = false;
		s->has_last_written_mapped = false;
		mfd_smoother_reset(&s->smoother, s->last_applied_val);
		mfd_takeover_sync_external(&s->takeover, s->last_applied_val);
	}

	if (has_cal_min || has_cal_max) {
		mfd_engine_set_slot_calibration(eng, idx, new_cal_min, new_cal_max);
	}
	if (has_curve) {
		s->curve = new_curve;
	}
	if (has_scene) {
		snprintf(s->scene_name, sizeof(s->scene_name), "%s", sc ? sc : "");
		if (!sc || sc[0] == '\0') {
			s->resolved_scene_name[0] = '\0';
		}
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
	if (new_target == MFD_TARGET_FILTER_PARAM) {
		if (has_fn) {
			snprintf(s->filter_name, sizeof(s->filter_name), "%s", cand_fn);
		}
		if (has_fp) {
			snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", cand_fp);
		}
		s->filter_from_val = cand_from;
		s->filter_to_val = cand_to;
		if (has_bypass) {
			s->filter_bypass_at_zero = obs_data_get_bool(request_data, "bypass_at_zero");
		}
		s->restore_persisted_pre_bypass = false;
	} else if (has_target) {
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
	}

	if (has_target) {
		if (new_target == MFD_TARGET_FILTER_PARAM &&
		    (!same_filter_binding || has_from || has_to || s->target_type == MFD_TARGET_FILTER_PARAM)) {
			s->target_type = MFD_TARGET_UNASSIGNED;
		}
		mfd_engine_reset_slot_mode_state(eng, s, new_target);
	} else if (new_target == MFD_TARGET_FILTER_PARAM && (!same_filter_binding || has_from || has_to)) {
		s->target_type = MFD_TARGET_UNASSIGNED;
		mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_FILTER_PARAM);
	}

	mfd_engine_bump_slot_rev(eng, idx);
	mfd_engine_reevaluate_slots(eng, false);
	if (!was_enabled && new_enabled) {
		mfd_engine_sync_filter_slot_from_live_locked(eng, idx);
	}
	if (obs_initialized()) {
		eng->pending_config_save = true;
	}
	mfd_engine_notify_state_changed(eng);
	populate_slot_data(eng, s, response_data);
	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_load_profile(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}

	const char *name = request_data ? obs_data_get_string(request_data, "name") : NULL;
	const struct mfd_controller_profile_def *def = mfd_profile_find_by_slug(name);
	if (!def) {
		char msg[240];
		snprintf(
			msg, sizeof(msg),
			"Unknown profile '%s'. Valid profiles: apc40mk2, apcminimk2, midimix, nanokontrol2, launchcontrolxl, generic.",
			name ? name : "");
		set_error_comment(response_data, msg);
		return;
	}

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}
	mfd_engine_load_profile_defaults(eng, def->id);
	if (obs_initialized()) {
		eng->pending_config_save = true;
	}
	mfd_engine_notify_state_changed(eng);
	obs_data_set_string(response_data, "loaded", def->slug);
	obs_data_set_string(response_data, "profile_tier", mfd_profile_tier_to_string(def->tier));
	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_get_catalog(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}

	const char *source = (request_data && obs_data_has_user_value(request_data, "source"))
				     ? obs_data_get_string(request_data, "source")
				     : NULL;

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}

	char err_msg[256] = {0};
	if (!mfd_engine_fill_vendor_catalog_locked(eng, source, response_data, err_msg, sizeof(err_msg))) {
		set_error_comment(response_data, err_msg);
	}
	mfd_engine_unlock(eng);
}

void mfd_vendor_handle_reset_fx(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	struct mfd_engine *eng = (struct mfd_engine *)priv_data;
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (!response_data || !eng || os_atomic_load_bool(&g_vendor_shutting_down)) {
		return;
	}

	long long raw_ms = (request_data && obs_data_has_user_value(request_data, "ms"))
				   ? obs_data_get_int(request_data, "ms")
				   : 0LL;
	if (raw_ms != 0LL && (raw_ms < 50LL || raw_ms > 20000LL)) {
		set_error_comment(response_data, "Duration 'ms' must be 0 (instant) or between 50 and 20000 ms.");
		return;
	}

	mfd_engine_lock(eng);
	if (os_atomic_load_bool(&g_vendor_shutting_down)) {
		mfd_engine_unlock(eng);
		return;
	}

	int eligible_count = 0;
	int reset_count = 0;
	int queued_count = 0;
	char err_msg[192] = {0};
	if (!mfd_engine_reset_fx(eng, (int)raw_ms, &eligible_count, &reset_count, &queued_count, err_msg,
				 sizeof(err_msg))) {
		set_error_comment(response_data, err_msg);
		mfd_engine_unlock(eng);
		return;
	}
	obs_data_set_int(response_data, "eligible_count", eligible_count);
	obs_data_set_int(response_data, "reset_count", reset_count);
	obs_data_set_int(response_data, "queued_unavailable_count", queued_count);
	obs_data_set_int(response_data, "ms", (int)raw_ms);
	mfd_engine_unlock(eng);
}

bool mfd_vendor_api_register(struct mfd_engine *eng)
{
	os_atomic_store_bool(&g_vendor_shutting_down, false);
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
	bool r7 = obs_websocket_vendor_register_request(g_vendor, "get_catalog", mfd_vendor_handle_get_catalog, eng);
	bool r8 = obs_websocket_vendor_register_request(g_vendor, "reset_fx", mfd_vendor_handle_reset_fx, eng);

	obs_log(LOG_INFO,
		"Registered obs-websocket vendor '%s' requests (get_state=%d, set_fader_value=%d, nudge_fader=%d, auto_fade=%d, set_slot=%d, load_profile=%d, get_catalog=%d, reset_fx=%d)",
		MFD_VENDOR_NAME, (int)r1, (int)r2, (int)r3, (int)r4, (int)r5, (int)r6, (int)r7, (int)r8);
	return r1 && r2 && r3 && r4 && r5 && r6 && r7 && r8;
}

void mfd_vendor_api_shutdown(struct mfd_engine *eng)
{
	os_atomic_store_bool(&g_vendor_shutting_down, true);
	if (g_vendor && obs_get_proc_handler()) {
		obs_websocket_vendor_unregister_request(g_vendor, "get_state");
		obs_websocket_vendor_unregister_request(g_vendor, "set_fader_value");
		obs_websocket_vendor_unregister_request(g_vendor, "nudge_fader");
		obs_websocket_vendor_unregister_request(g_vendor, "auto_fade");
		obs_websocket_vendor_unregister_request(g_vendor, "set_slot");
		obs_websocket_vendor_unregister_request(g_vendor, "load_profile");
		obs_websocket_vendor_unregister_request(g_vendor, "get_catalog");
		obs_websocket_vendor_unregister_request(g_vendor, "reset_fx");
	}
	if (!eng) {
		eng = mfd_engine_get();
	}
	if (eng) {
		mfd_engine_lock(eng);
		mfd_engine_unlock(eng);
	}
	g_vendor = NULL;
	g_bridge_ph = NULL;
	g_bridge_sh = NULL;
	g_event_sink = NULL;
	g_event_sink_user_data = NULL;
}
