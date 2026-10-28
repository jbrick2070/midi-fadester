/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-engine.h"
#include "obs/mfd-layer-filter.h"
#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include "core/tbar-logic.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include "plugin-support.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MFD_SAVE_DATA_KEY "midi_fadester_scene_collection"
#define MFD_DEFAULT_PROFILE_NAME "apc40mk2"

static struct mfd_engine g_engine;

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

static int ascii_stricmp(const char *a, const char *b)
{
	if (!a && !b) {
		return 0;
	}
	if (!a) {
		return -1;
	}
	if (!b) {
		return 1;
	}
	while (*a && *b) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb) {
			return ca - cb;
		}
		++a;
		++b;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static bool slot_addr_is_14bit(const struct mfd_midi_addr *addr)
{
	if (!addr) {
		return false;
	}
	return addr->is_14bit || (addr->type == MFD_MIDI_PITCH_BEND);
}

static bool midi_addr_exact_equal(const struct mfd_midi_addr *a, const struct mfd_midi_addr *b)
{
	if (!a || !b) {
		return false;
	}
	if (a->type == MFD_MIDI_NONE || b->type == MFD_MIDI_NONE) {
		return false;
	}
	if (a->type != b->type || a->channel != b->channel) {
		return false;
	}
	if (a->type == MFD_MIDI_PITCH_BEND) {
		return true;
	}
	return (a->number == b->number) && (a->is_14bit == b->is_14bit);
}

static const char *effective_profile_name(const struct mfd_engine *eng)
{
	if (eng && eng->active_profile_name[0] != '\0') {
		return eng->active_profile_name;
	}
	return MFD_DEFAULT_PROFILE_NAME;
}

bool mfd_engine_init_mutex(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	if (eng->state_mutex_initialized) {
		return true;
	}
	if (pthread_mutex_init_recursive(&eng->state_mutex) != 0) {
		return false;
	}
	eng->state_mutex_initialized = true;
	eng->lock_depth = 0U;
	return true;
}

void mfd_engine_destroy_mutex(struct mfd_engine *eng)
{
	if (!eng || !eng->state_mutex_initialized) {
		return;
	}
	eng->state_mutex_initialized = false;
	eng->lock_depth = 0U;
	pthread_mutex_destroy(&eng->state_mutex);
}

void mfd_engine_lock(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	if (eng->state_mutex_initialized) {
		pthread_mutex_lock(&eng->state_mutex);
	}
	++eng->lock_depth;
}

void mfd_engine_unlock(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}

	bool emit_state = false;
	bool wake_ui = false;
	mfd_ui_notify_fn ui_fn = NULL;
	void *ui_ud = NULL;
	struct mfd_pending_fader_emit fader_emits[MFD_MAX_FADER_SLOTS];
	size_t fader_emit_count = 0U;

	if (eng->lock_depth > 0U) {
		--eng->lock_depth;
	}
	if (eng->lock_depth == 0U) {
		emit_state = eng->pending_state_event;
		eng->pending_state_event = false;

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			if (eng->pending_fader_emits[i].pending) {
				fader_emits[fader_emit_count++] = eng->pending_fader_emits[i];
				eng->pending_fader_emits[i].pending = false;
			}
		}

		wake_ui = eng->pending_ui_notify || eng->pending_config_save;
		eng->pending_ui_notify = false;
		ui_fn = eng->ui_notify_fn;
		ui_ud = eng->ui_notify_user_data;
	}

	if (eng->state_mutex_initialized) {
		pthread_mutex_unlock(&eng->state_mutex);
	}

	for (size_t k = 0U; k < fader_emit_count; ++k) {
		mfd_vendor_emit_fader_changed_ex(fader_emits[k].slot_id, fader_emits[k].value,
						 fader_emits[k].display_value, fader_emits[k].origin);
	}
	if (emit_state) {
		mfd_vendor_emit_state_changed(eng);
	}
	if (wake_ui && ui_fn) {
		ui_fn(ui_ud);
	}
}

void mfd_engine_mark_ui_dirty(struct mfd_engine *eng, bool is_topology)
{
	if (!eng) {
		return;
	}
	if (is_topology) {
		++eng->config_topology_rev;
	} else {
		++eng->telemetry_rev;
	}
	eng->pending_ui_notify = true;
}

void mfd_engine_bump_slot_rev(struct mfd_engine *eng, int slot_index)
{
	if (!eng) {
		return;
	}
	if (slot_index >= 0 && slot_index < MFD_MAX_FADER_SLOTS) {
		++eng->slot_config_rev[slot_index];
	} else {
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			++eng->slot_config_rev[i];
		}
	}
	mfd_engine_mark_ui_dirty(eng, true);
}

void mfd_engine_set_ui_notify_fn(struct mfd_engine *eng, mfd_ui_notify_fn fn, void *user_data)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->ui_notify_fn = fn;
	eng->ui_notify_user_data = user_data;
	mfd_engine_unlock(eng);
}

void mfd_engine_flush_pending_ui_work(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	bool do_save = false;
	bool check_health = false;
	mfd_engine_lock(eng);
	if (eng->pending_config_save) {
		eng->pending_config_save = false;
		do_save = true;
	}
	if (os_atomic_load_bool(&eng->midi_dev.device_closed_unexpectedly)) {
		check_health = true;
	}
	mfd_engine_unlock(eng);

	if (do_save && obs_initialized()) {
		mfd_engine_save_global_config(eng);
	}
	if (check_health) {
		mfd_engine_poll_midi_health_on_ui_thread(eng);
	}
}

static void unlock_and_persist_global_config(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	bool save_now = false;
	if (obs_initialized()) {
		if (eng->lock_depth == 1U) {
			eng->pending_config_save = false;
			save_now = true;
		} else {
			eng->pending_config_save = true;
		}
	}
	mfd_engine_unlock(eng);
	if (save_now) {
		mfd_engine_save_global_config(eng);
	}
}

static void refresh_apc40_detected_locked(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	if (eng->midi_dev.is_open && mfd_is_apc40_mk2_name(eng->midi_dev.opened_name)) {
		eng->apc40_controller_detected = true;
		return;
	}
	if (mfd_is_apc40_mk2_name(eng->configured_device_name)) {
		eng->apc40_controller_detected = true;
		return;
	}
	if (obs_initialized()) {
		struct mfd_midi_device_info list[MFD_MAX_MIDI_DEVICES];
		size_t count = mfd_winmm_enumerate(list, MFD_MAX_MIDI_DEVICES);
		for (size_t i = 0U; i < count; ++i) {
			if (list[i].is_apc40_mk2) {
				eng->apc40_controller_detected = true;
				return;
			}
		}
		eng->apc40_controller_detected = false;
	}
}

static void refresh_frontend_cache_on_ui_thread_locked(struct mfd_engine *eng)
{
	if (!eng || !obs_initialized()) {
		return;
	}
	eng->cached_studio_mode = obs_frontend_preview_program_mode_active();
	eng->cached_program_scene[0] = '\0';
	eng->cached_preview_scene[0] = '\0';

	obs_source_t *prog = obs_frontend_get_current_scene();
	if (prog) {
		const char *nm = obs_source_get_name(prog);
		if (nm) {
			snprintf(eng->cached_program_scene, sizeof(eng->cached_program_scene), "%s", nm);
		}
		obs_source_release(prog);
	}
	if (eng->cached_studio_mode) {
		obs_source_t *prev = obs_frontend_get_current_preview_scene();
		if (prev) {
			const char *nm = obs_source_get_name(prev);
			if (nm) {
				snprintf(eng->cached_preview_scene, sizeof(eng->cached_preview_scene), "%s", nm);
			}
			obs_source_release(prev);
		}
	}
}

static void store_persisted_calibration(struct mfd_engine *eng, const char *profile_name,
					const struct mfd_midi_addr *addr, const struct mfd_control_calibration *cal)
{
	if (!eng || !addr || !cal || addr->type == MFD_MIDI_NONE) {
		return;
	}
	const char *prof = (profile_name && profile_name[0] != '\0') ? profile_name : MFD_DEFAULT_PROFILE_NAME;
	int free_idx = -1;
	for (int i = 0; i < MFD_MAX_CAL_ENTRIES; ++i) {
		struct mfd_persisted_cal_entry *e = &eng->cal_entries[i];
		if (!e->in_use) {
			if (free_idx < 0) {
				free_idx = i;
			}
			continue;
		}
		if (strcmp(e->profile_name, prof) == 0 && e->addr.type == addr->type &&
		    e->addr.channel == addr->channel && e->addr.number == addr->number &&
		    e->addr.is_14bit == addr->is_14bit) {
			e->cal = *cal;
			return;
		}
	}
	if (free_idx >= 0) {
		struct mfd_persisted_cal_entry *e = &eng->cal_entries[free_idx];
		e->in_use = true;
		snprintf(e->profile_name, sizeof(e->profile_name), "%s", prof);
		e->addr = *addr;
		e->cal = *cal;
	}
}

bool mfd_engine_lookup_persisted_calibration(const struct mfd_engine *eng, const char *profile_name,
					     const struct mfd_midi_addr *addr, struct mfd_control_calibration *out_cal)
{
	if (!out_cal) {
		return false;
	}
	bool is_14 = slot_addr_is_14bit(addr);
	mfd_calibration_init(out_cal, is_14);
	if (!eng || !addr || addr->type == MFD_MIDI_NONE) {
		return false;
	}
	const char *prof = (profile_name && profile_name[0] != '\0') ? profile_name : MFD_DEFAULT_PROFILE_NAME;
	for (int i = 0; i < MFD_MAX_CAL_ENTRIES; ++i) {
		const struct mfd_persisted_cal_entry *e = &eng->cal_entries[i];
		if (!e->in_use) {
			continue;
		}
		if (strcmp(e->profile_name, prof) == 0 && e->addr.type == addr->type &&
		    e->addr.channel == addr->channel && e->addr.number == addr->number &&
		    e->addr.is_14bit == addr->is_14bit) {
			*out_cal = e->cal;
			return true;
		}
	}
	return false;
}

void mfd_engine_set_slot_calibration(struct mfd_engine *eng, int slot_index, int cal_min, int cal_max)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	mfd_calibration_set(&s->cal, slot_addr_is_14bit(&s->addr), cal_min, cal_max);
	store_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
	mfd_engine_bump_slot_rev(eng, slot_index);
	unlock_and_persist_global_config(eng);
}

struct mfd_engine *mfd_engine_get(void)
{
	return &g_engine;
}

void mfd_engine_set_tbar_driver(mfd_tbar_apply_fn driver_fn)
{
	mfd_engine_lock(&g_engine);
	g_engine.tbar_driver = driver_fn;
	mfd_engine_unlock(&g_engine);
}

void mfd_engine_set_tbar_unlatch_fn(mfd_tbar_unlatch_fn unlatch_fn)
{
	mfd_engine_lock(&g_engine);
	g_engine.tbar_unlatch = unlatch_fn;
	mfd_engine_unlock(&g_engine);
}

void mfd_engine_unlatch_tbar(struct mfd_engine *eng)
{
	if (eng && eng->tbar_unlatch) {
		eng->tbar_unlatch();
	}
}

static float query_live_filter_unmapped_deflection_locked(const struct mfd_engine *eng,
							  const struct mfd_fader_slot *slot);

void mfd_engine_format_slot_display_value(const struct mfd_fader_slot *slot, float norm_01, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	float val = clamp01(norm_01);
	if (!slot) {
		snprintf(out, out_size, "%d%%", (int)lroundf(val * 100.0f));
		return;
	}

	switch (slot->target_type) {
	case MFD_TARGET_FILTER_PARAM: {
		double pmin = slot->filter_prop_valid ? slot->filter_prop_min
						      : fmin(slot->filter_from_val, slot->filter_to_val);
		double pmax = slot->filter_prop_valid ? slot->filter_prop_max
						      : fmax(slot->filter_from_val, slot->filter_to_val);
		double step = (slot->filter_prop_valid && slot->filter_prop_step > 0.0) ? slot->filter_prop_step : 0.01;
		mfd_filter_format_display_value(val, slot->filter_from_val, slot->filter_to_val, pmin, pmax,
						slot->filter_prop_is_int, step, slot->filter_prop_suffix,
						slot->filter_bypass_at_zero, out, out_size);
		break;
	}
	case MFD_TARGET_AUDIO_VOLUME: {
		float db = mfd_deflection_to_db(val);
		if (!isfinite(db)) {
			snprintf(out, out_size, "-inf dB");
		} else if (fabsf(db) < 0.05f) {
			snprintf(out, out_size, "0.0 dB");
		} else {
			snprintf(out, out_size, "%.1f dB", (double)db);
		}
		break;
	}
	case MFD_TARGET_UNASSIGNED:
	case MFD_TARGET_TBAR:
	case MFD_TARGET_SINGLE_LAYER:
	case MFD_TARGET_TWO_SOURCE_MIX:
	default:
		snprintf(out, out_size, "%d%%", (int)lroundf(val * 100.0f));
		break;
	}
}

static void configure_slot_takeover_mode(const struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	if (!slot) {
		return;
	}
	bool is_tbar = (slot->target_type == MFD_TARGET_TBAR);
	bool is_two_rail =
		(slot->target_type == MFD_TARGET_TWO_SOURCE_MIX || slot->target_type == MFD_TARGET_SINGLE_LAYER ||
		 slot->target_type == MFD_TARGET_AUDIO_VOLUME || slot->target_type == MFD_TARGET_FILTER_PARAM);
	slot->takeover.bidirectional_flip = is_tbar ? (eng ? eng->tbar_bidirectional_flip : true) : false;
	mfd_takeover_set_two_rail_mode(&slot->takeover, is_two_rail);
}

static void init_slot_runtime(struct mfd_engine *eng, struct mfd_fader_slot *slot, float initial_val)
{
	bool bidir = (slot->target_type == MFD_TARGET_TBAR) ? eng->tbar_bidirectional_flip : false;
	mfd_smoother_init(&slot->smoother, eng->smooth_tau_ms, initial_val);
	mfd_takeover_init(&slot->takeover, initial_val, MFD_DEFAULT_PICKUP_THRESHOLD, bidir);
	configure_slot_takeover_mode(eng, slot);
	slot->tbar_state = MFD_TBAR_IDLE;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->last_raw_norm = initial_val;
	slot->last_applied_val = initial_val;
	slot->last_alpha_a = 1.0f;
	slot->last_alpha_b = 0.0f;
	slot->last_audio_mul = mfd_deflection_to_mul(initial_val);
	slot->last_audio_db = mfd_deflection_to_db(initial_val);
	slot->output_write_count = 0U;
	slot->has_last_written_mapped = false;
	slot->last_written_int = 0;
	slot->last_written_float = 0.0;
	slot->writing_filter_settings = false;
	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->auto_fade_from = initial_val;
	slot->auto_fade_to = initial_val;
	slot->auto_fade_duration_sec = 0.0f;
	slot->auto_fade_elapsed_sec = 0.0f;
	slot->sec_since_last_fader_event = 1.0f;
	slot->fader_event_pending = false;
	slot->pending_origin[0] = '\0';
	if (slot->target_type == MFD_TARGET_TBAR) {
		mfd_engine_unlatch_tbar(eng);
	}
}

void mfd_engine_reset_slot_mode_state(struct mfd_engine *eng, struct mfd_fader_slot *slot,
				      enum mfd_target_type new_target)
{
	if (!eng || !slot) {
		return;
	}
	mfd_engine_lock(eng);
	enum mfd_target_type prev_target = slot->target_type;
	slot->target_type = new_target;
	if (prev_target == new_target) {
		configure_slot_takeover_mode(eng, slot);
		if (new_target == MFD_TARGET_TBAR && slot->takeover.state != MFD_TAKEOVER_PARKED) {
			slot->tbar_state = MFD_TBAR_IDLE;
			mfd_engine_unlatch_tbar(eng);
		} else if (new_target != MFD_TARGET_TBAR) {
			slot->tbar_state = MFD_TBAR_IDLE;
		}
		mfd_engine_unlock(eng);
		return;
	}

	bool had_raw = slot->takeover.has_last_fader;
	float preserved_raw = slot->last_raw_norm;
	if (prev_target == MFD_TARGET_TBAR || new_target == MFD_TARGET_TBAR) {
		mfd_engine_unlatch_tbar(eng);
	}

	float init_val = 0.0f;
	if (new_target == MFD_TARGET_SINGLE_LAYER || new_target == MFD_TARGET_AUDIO_VOLUME) {
		init_val = 1.0f;
	} else if (new_target == MFD_TARGET_FILTER_PARAM) {
		init_val = query_live_filter_unmapped_deflection_locked(eng, slot);
	}
	bool bidir = (new_target == MFD_TARGET_TBAR) ? eng->tbar_bidirectional_flip : false;
	mfd_smoother_reset(&slot->smoother, init_val);
	mfd_takeover_init(&slot->takeover, init_val, MFD_DEFAULT_PICKUP_THRESHOLD, bidir);
	configure_slot_takeover_mode(eng, slot);

	slot->tbar_state = MFD_TBAR_IDLE;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->has_last_written_mapped = false;
	slot->last_written_int = 0;
	slot->last_written_float = 0.0;
	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->last_applied_val = init_val;
	slot->last_alpha_a = 1.0f;
	slot->last_alpha_b = 0.0f;
	slot->last_audio_mul = mfd_deflection_to_mul(init_val);
	slot->last_audio_db = mfd_deflection_to_db(init_val);

	if (had_raw) {
		slot->last_raw_norm = preserved_raw;
		slot->takeover.has_last_fader = true;
		slot->takeover.last_raw_fader = preserved_raw;
		slot->takeover.last_fader = preserved_raw;
		float diff = preserved_raw - init_val;
		if (fabsf(diff) > slot->takeover.pickup_threshold) {
			slot->takeover.hint = (diff < 0.0f) ? MFD_HINT_MOVE_UP : MFD_HINT_MOVE_DOWN;
		} else {
			slot->takeover.hint = MFD_HINT_NONE;
		}
	} else {
		slot->last_raw_norm = init_val;
	}
	mfd_engine_unlock(eng);
}

enum mfd_controller_profile mfd_engine_get_active_profile(const struct mfd_engine *eng)
{
	if (!eng) {
		return MFD_PROFILE_APC40_MK2;
	}
	if (eng->active_profile_name[0] != '\0') {
		const struct mfd_controller_profile_def *def = mfd_profile_find_by_slug(eng->active_profile_name);
		if (def) {
			return def->id;
		}
	}
	enum mfd_controller_profile from_dev =
		mfd_profile_for_device(eng->configured_device_name, eng->midi_dev.is_open, eng->midi_dev.opened_name);
	if (from_dev != MFD_PROFILE_GENERIC) {
		return from_dev;
	}
	return MFD_PROFILE_APC40_MK2;
}

bool mfd_engine_load_profile_defaults(struct mfd_engine *eng, enum mfd_controller_profile prof)
{
	if (!eng) {
		return false;
	}
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(prof);
	if (!def) {
		return false;
	}

	mfd_engine_lock(eng);
	for (int k = 0; k < MFD_MAX_FADER_SLOTS; ++k) {
		mfd_engine_detach_filter_binding_locked(eng, k);
	}
	snprintf(eng->active_profile_name, sizeof(eng->active_profile_name), "%s", def->slug);
	if (!eng->has_last_midi) {
		eng->last_moved_slot_index = -1;
	}

	for (size_t i = 0U; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		memset(s, 0, sizeof(*s));
		s->filter_pre_bypass_enabled = true;
		if (i < MFD_APC40_DEFAULT_SLOT_COUNT) {
			bool phys = mfd_profile_has_physical_slot(def->id, (int)i);
			s->enabled = (mfd_slot_is_fx_bank((int)i) || def->id == MFD_PROFILE_GENERIC)
					     ? true
					     : (phys && def->slots[i].addr.type != MFD_MIDI_NONE);
			snprintf(s->id, sizeof(s->id), "%s", def->slots[i].id);
			snprintf(s->kind, sizeof(s->kind), "%s", def->slots[i].kind);
			snprintf(s->label, sizeof(s->label), "%s", def->slots[i].label);
			s->addr = def->slots[i].addr;
			s->target_type = def->slots[i].default_target;
			s->curve = def->slots[i].default_curve;
			s->audio_follows_video = false;
			s->invert = false;
		} else {
			s->enabled = false;
			snprintf(s->id, sizeof(s->id), "fader_%u", (unsigned)(i + 1U));
			snprintf(s->kind, sizeof(s->kind), "fader");
			snprintf(s->label, sizeof(s->label), "Fader %u", (unsigned)(i + 1U));
			s->addr.type = MFD_MIDI_NONE;
			s->target_type = MFD_TARGET_UNASSIGNED;
			s->curve = MFD_CURVE_DIP_FREE;
		}
		mfd_engine_lookup_persisted_calibration(eng, eng->active_profile_name, &s->addr, &s->cal);
		float init_val =
			(s->target_type == MFD_TARGET_SINGLE_LAYER || s->target_type == MFD_TARGET_AUDIO_VOLUME) ? 1.0f
														 : 0.0f;
		init_slot_runtime(eng, s, init_val);
	}
	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_unlock(eng);
	return true;
}

bool mfd_engine_select_profile(struct mfd_engine *eng, enum mfd_controller_profile prof, bool reset_targets)
{
	if (!eng) {
		return false;
	}
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(prof);
	if (!def) {
		return false;
	}

	mfd_engine_lock(eng);
	mfd_engine_cancel_setup_session(eng);
	if (reset_targets) {
		mfd_engine_load_profile_defaults(eng, prof);
	} else {
		snprintf(eng->active_profile_name, sizeof(eng->active_profile_name), "%s", def->slug);
		if (def->id == MFD_PROFILE_GENERIC) {
			for (int i = 0; i < MFD_MAX_FADER_SLOTS && (size_t)i < MFD_APC40_DEFAULT_SLOT_COUNT; ++i) {
				if (mfd_slot_is_fx_bank(i)) {
					eng->fader_slots[i].enabled = true;
				}
			}
		} else {
			for (int i = 0; i < MFD_MAX_FADER_SLOTS && (size_t)i < MFD_APC40_DEFAULT_SLOT_COUNT; ++i) {
				struct mfd_fader_slot *s = &eng->fader_slots[i];
				if (mfd_slot_is_fx_bank(i)) {
					s->enabled = true;
					snprintf(s->label, sizeof(s->label), "%s", def->slots[i].label);
					s->addr = def->slots[i].addr;
					mfd_engine_lookup_persisted_calibration(eng, eng->active_profile_name, &s->addr,
										&s->cal);
				} else if (mfd_profile_has_physical_slot(def->id, i) &&
					   def->slots[i].addr.type != MFD_MIDI_NONE) {
					s->enabled = true;
					snprintf(s->label, sizeof(s->label), "%s", def->slots[i].label);
					s->addr = def->slots[i].addr;
					mfd_engine_lookup_persisted_calibration(eng, eng->active_profile_name, &s->addr,
										&s->cal);
					if (s->target_type == MFD_TARGET_UNASSIGNED &&
					    def->slots[i].default_target != MFD_TARGET_UNASSIGNED) {
						mfd_engine_reset_slot_mode_state(eng, s, def->slots[i].default_target);
					}
				} else {
					mfd_engine_detach_filter_binding_locked(eng, i);
					s->enabled = false;
					s->addr.type = MFD_MIDI_NONE;
					s->scene_name[0] = '\0';
					s->resolved_scene_name[0] = '\0';
					s->source_a_name[0] = '\0';
					s->source_b_name[0] = '\0';
					s->filter_name[0] = '\0';
					s->filter_prop[0] = '\0';
					s->filter_from_val = 0.0;
					s->filter_to_val = 0.0;
					s->filter_bypass_at_zero = false;
					s->filter_prop_valid = false;
					mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
				}
			}
		}
		mfd_engine_bump_slot_rev(eng, -1);
		mfd_engine_reevaluate_slots(eng, false);
	}
	if (def->id != MFD_PROFILE_APC40_MK2) {
		eng->first_run_offer_dismissed = true;
	}
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

void mfd_engine_load_apc40_defaults(struct mfd_engine *eng)
{
	(void)mfd_engine_load_profile_defaults(eng, MFD_PROFILE_APC40_MK2);
}

static bool slots_differ_from_apc40_defaults(const struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	struct mfd_apc40_default_slot defs[MFD_APC40_DEFAULT_SLOT_COUNT];
	size_t count = mfd_apc40_get_default_slots(defs, MFD_APC40_DEFAULT_SLOT_COUNT);
	for (size_t i = 0U; i < MFD_MAX_FADER_SLOTS && i < count; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (!s->enabled) {
			return true;
		}
		if (s->target_type != defs[i].default_target) {
			return true;
		}
		if (!midi_addr_exact_equal(&s->addr, &defs[i].addr)) {
			return true;
		}
		if (s->scene_name[0] != '\0' || s->source_a_name[0] != '\0' || s->source_b_name[0] != '\0' ||
		    s->filter_name[0] != '\0' || s->filter_prop[0] != '\0' || s->filter_bypass_at_zero) {
			return true;
		}
		if (s->curve != defs[i].default_curve || s->audio_follows_video || s->invert) {
			return true;
		}
		int max_def = slot_addr_is_14bit(&s->addr) ? MFD_CAL_DEFAULT_MAX_14BIT : MFD_CAL_DEFAULT_MAX_7BIT;
		if (s->cal.cal_min != MFD_CAL_DEFAULT_MIN || s->cal.cal_max != max_def) {
			return true;
		}
	}
	return false;
}

struct mfd_stack_find_ctx {
	const char *target_a;
	const char *target_b;
	int idx;
	int found_a;
	int found_b;
};

static bool enum_scene_items_stack_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct mfd_stack_find_ctx *ctx = (struct mfd_stack_find_ctx *)param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && !obs_source_removed(src)) {
		const char *nm = obs_source_get_name(src);
		if (nm) {
			if (ctx->target_a && strcmp(nm, ctx->target_a) == 0) {
				ctx->found_a = ctx->idx;
			}
			if (ctx->target_b && strcmp(nm, ctx->target_b) == 0) {
				ctx->found_b = ctx->idx;
			}
		}
	}
	++ctx->idx;
	return true;
}

static int live_scene_item_stack_index_on_source(obs_source_t *scene_src, const char *source_name)
{
	if (!scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src) || !source_name ||
	    source_name[0] == '\0') {
		return -1;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		return -1;
	}
	struct mfd_stack_find_ctx ctx = {source_name, NULL, 0, -1, -1};
	obs_scene_enum_items(scene, enum_scene_items_stack_cb, &ctx);
	return ctx.found_a;
}

struct mfd_enum_scene_layer_ctx {
	const char *source_a;
	const char *source_b;
	const char *exclude_scene;
	bool pair_mode;
	bool found;
	char scene_name[MFD_SOURCE_NAME_MAX];
	int idx_a;
	int idx_b;
};

static bool enum_scenes_find_cb(void *param, obs_source_t *scene_src)
{
	struct mfd_enum_scene_layer_ctx *ctx = (struct mfd_enum_scene_layer_ctx *)param;
	if (!ctx || !scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src)) {
		return true;
	}
	const char *sc_name = obs_source_get_name(scene_src);
	if (!sc_name || sc_name[0] == '\0') {
		return true;
	}
	if (ctx->exclude_scene && ctx->exclude_scene[0] != '\0' && strcmp(sc_name, ctx->exclude_scene) == 0) {
		return true;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		return true;
	}
	struct mfd_stack_find_ctx sctx = {ctx->source_a, ctx->pair_mode ? ctx->source_b : NULL, 0, -1, -1};
	obs_scene_enum_items(scene, enum_scene_items_stack_cb, &sctx);
	if (ctx->pair_mode) {
		if (sctx.found_a >= 0 && sctx.found_b >= 0) {
			ctx->found = true;
			snprintf(ctx->scene_name, sizeof(ctx->scene_name), "%s", sc_name);
			ctx->idx_a = sctx.found_a;
			ctx->idx_b = sctx.found_b;
			return false;
		}
	} else {
		if (sctx.found_a >= 0) {
			ctx->found = true;
			snprintf(ctx->scene_name, sizeof(ctx->scene_name), "%s", sc_name);
			ctx->idx_a = sctx.found_a;
			return false;
		}
	}
	return true;
}

bool mfd_engine_source_exists(const struct mfd_engine *eng, const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->source_exists) {
		return eng->scene_graph_ops->source_exists(source_name, eng->scene_graph_user_data);
	}
	if (eng && eng->source_exists_fn) {
		return eng->source_exists_fn(source_name, eng->source_exists_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	bool alive = !obs_source_removed(src);
	obs_source_release(src);
	return alive;
}

bool mfd_engine_scene_exists(const struct mfd_engine *eng, const char *scene_name)
{
	if (!scene_name || scene_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->scene_exists) {
		return eng->scene_graph_ops->scene_exists(scene_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(scene_name);
	if (!src) {
		return false;
	}
	bool ok = !obs_source_removed(src) && obs_source_is_scene(src);
	obs_source_release(src);
	return ok;
}

bool mfd_engine_source_has_audio(const struct mfd_engine *eng, const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->source_has_audio) {
		return eng->scene_graph_ops->source_has_audio(source_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	bool has_audio = !obs_source_removed(src) && ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0U);
	obs_source_release(src);
	return has_audio;
}

static int sg_scene_item_stack_index(const struct mfd_engine *eng, const char *scene_name, const char *source_name)
{
	if (!scene_name || scene_name[0] == '\0' || !source_name || source_name[0] == '\0') {
		return -1;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->scene_item_stack_index) {
		return eng->scene_graph_ops->scene_item_stack_index(scene_name, source_name,
								    eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return -1;
	}
	obs_source_t *scene_src = obs_get_source_by_name(scene_name);
	if (!scene_src) {
		return -1;
	}
	int idx = live_scene_item_stack_index_on_source(scene_src, source_name);
	obs_source_release(scene_src);
	return idx;
}

bool mfd_engine_find_other_scene_with_layer(const struct mfd_engine *eng, const char *source_name,
					    const char *exclude_scene, char *out_scene, size_t out_size)
{
	if (out_scene && out_size > 0U) {
		out_scene[0] = '\0';
	}
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->find_scene_with_layer) {
		return eng->scene_graph_ops->find_scene_with_layer(source_name, exclude_scene, out_scene, out_size,
								   eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	struct mfd_enum_scene_layer_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.source_a = source_name;
	ctx.exclude_scene = exclude_scene;
	ctx.pair_mode = false;
	obs_enum_scenes(enum_scenes_find_cb, &ctx);
	if (ctx.found && out_scene && out_size > 0U) {
		snprintf(out_scene, out_size, "%s", ctx.scene_name);
	}
	return ctx.found;
}

static bool sg_find_scene_with_pair(const struct mfd_engine *eng, const char *source_a, const char *source_b,
				    char *out_scene, size_t out_size, int *out_idx_a, int *out_idx_b)
{
	if (out_scene && out_size > 0U) {
		out_scene[0] = '\0';
	}
	if (out_idx_a) {
		*out_idx_a = -1;
	}
	if (out_idx_b) {
		*out_idx_b = -1;
	}
	if (!source_a || source_a[0] == '\0' || !source_b || source_b[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->find_scene_with_pair) {
		return eng->scene_graph_ops->find_scene_with_pair(source_a, source_b, out_scene, out_size, out_idx_a,
								  out_idx_b, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	struct mfd_enum_scene_layer_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.source_a = source_a;
	ctx.source_b = source_b;
	ctx.pair_mode = true;
	ctx.idx_a = -1;
	ctx.idx_b = -1;
	obs_enum_scenes(enum_scenes_find_cb, &ctx);
	if (ctx.found) {
		if (out_scene && out_size > 0U) {
			snprintf(out_scene, out_size, "%s", ctx.scene_name);
		}
		if (out_idx_a) {
			*out_idx_a = ctx.idx_a;
		}
		if (out_idx_b) {
			*out_idx_b = ctx.idx_b;
		}
	}
	return ctx.found;
}

bool mfd_engine_is_studio_mode_active(const struct mfd_engine *eng)
{
	if (eng && eng->studio_mode_override_set) {
		return eng->studio_mode_override;
	}
	if (!obs_initialized()) {
		return false;
	}
	return eng ? eng->cached_studio_mode : false;
}

bool mfd_is_internal_filter(const char *filter_name, const char *type_id)
{
	if (filter_name &&
	    (strncmp(filter_name, "__mfd_", 6) == 0 || strncmp(filter_name, "__midi_fadester_", 16) == 0 ||
	     strcmp(filter_name, MFD_INTERNAL_OPACITY_FILTER_NAME) == 0)) {
		return true;
	}
	if (type_id && strcmp(type_id, MFD_LAYER_FILTER_ID) == 0) {
		return true;
	}
	return false;
}

static bool is_internal_source_name(const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return true;
	}
	return strncmp(source_name, "__mfd_", 6) == 0 || strncmp(source_name, "__midi_fadester_", 16) == 0;
}

static void collect_filter_numeric_props_depth(obs_properties_t *props, obs_data_t *settings, mfd_numeric_prop_cb cb,
					       void *param, int depth)
{
	if (!props || !cb || depth > 16) {
		return;
	}
	for (obs_property_t *p = obs_properties_first(props); p != NULL; obs_property_next(&p)) {
		enum obs_property_type ptype = obs_property_get_type(p);
		if (ptype == OBS_PROPERTY_GROUP) {
			obs_properties_t *sub = obs_property_group_content(p);
			if (sub && sub != props) {
				collect_filter_numeric_props_depth(sub, settings, cb, param, depth + 1);
			}
			continue;
		}
		if (ptype != OBS_PROPERTY_FLOAT && ptype != OBS_PROPERTY_INT) {
			continue;
		}
		const char *nm = obs_property_name(p);
		if (!nm || nm[0] == '\0') {
			continue;
		}
		struct mfd_filter_prop_info info;
		memset(&info, 0, sizeof(info));
		snprintf(info.name, sizeof(info.name), "%s", nm);
		const char *desc = obs_property_description(p);
		snprintf(info.label, sizeof(info.label), "%s", (desc && desc[0] != '\0') ? desc : nm);
		info.is_int = (ptype == OBS_PROPERTY_INT);
		if (info.is_int) {
			mfd_filter_normalize_bounds((double)obs_property_int_min(p), (double)obs_property_int_max(p),
						    &info.min_val, &info.max_val);
			int st = obs_property_int_step(p);
			info.step = (st > 0) ? (double)st : 1.0;
			const char *sfx = obs_property_int_suffix(p);
			snprintf(info.suffix, sizeof(info.suffix), "%s", sfx ? sfx : "");
			info.current_val = settings ? (double)obs_data_get_int(settings, nm) : info.min_val;
		} else {
			mfd_filter_normalize_bounds(obs_property_float_min(p), obs_property_float_max(p), &info.min_val,
						    &info.max_val);
			double st = obs_property_float_step(p);
			info.step = (isfinite(st) && st > 0.0) ? st : 0.01;
			const char *sfx = obs_property_float_suffix(p);
			snprintf(info.suffix, sizeof(info.suffix), "%s", sfx ? sfx : "");
			info.current_val = settings ? obs_data_get_double(settings, nm) : info.min_val;
		}
		cb(&info, param);
	}
}

void mfd_collect_filter_numeric_props(obs_properties_t *props, obs_data_t *settings, mfd_numeric_prop_cb cb,
				      void *param)
{
	collect_filter_numeric_props_depth(props, settings, cb, param, 0);
}

static bool find_filter_prop_in_props_depth(obs_properties_t *props, obs_data_t *settings, const char *prop_name,
					    bool *out_found_non_numeric, struct mfd_filter_prop_info *out_info,
					    int depth)
{
	if (!props || !prop_name || prop_name[0] == '\0' || depth > 16) {
		return false;
	}
	for (obs_property_t *p = obs_properties_first(props); p != NULL; obs_property_next(&p)) {
		enum obs_property_type ptype = obs_property_get_type(p);
		const char *nm = obs_property_name(p);
		if (ptype == OBS_PROPERTY_GROUP) {
			obs_properties_t *sub = obs_property_group_content(p);
			if (sub && sub != props &&
			    find_filter_prop_in_props_depth(sub, settings, prop_name, out_found_non_numeric, out_info,
							    depth + 1)) {
				return true;
			}
			if (nm && strcmp(nm, prop_name) == 0 && out_found_non_numeric) {
				*out_found_non_numeric = true;
			}
			continue;
		}
		if (!nm || strcmp(nm, prop_name) != 0) {
			continue;
		}
		if (ptype != OBS_PROPERTY_FLOAT && ptype != OBS_PROPERTY_INT) {
			if (out_found_non_numeric) {
				*out_found_non_numeric = true;
			}
			return false;
		}
		if (out_info) {
			memset(out_info, 0, sizeof(*out_info));
			snprintf(out_info->name, sizeof(out_info->name), "%s", nm);
			const char *desc = obs_property_description(p);
			snprintf(out_info->label, sizeof(out_info->label), "%s", (desc && desc[0] != '\0') ? desc : nm);
			out_info->is_int = (ptype == OBS_PROPERTY_INT);
			if (out_info->is_int) {
				mfd_filter_normalize_bounds((double)obs_property_int_min(p),
							    (double)obs_property_int_max(p), &out_info->min_val,
							    &out_info->max_val);
				int st = obs_property_int_step(p);
				out_info->step = (st > 0) ? (double)st : 1.0;
				const char *sfx = obs_property_int_suffix(p);
				snprintf(out_info->suffix, sizeof(out_info->suffix), "%s", sfx ? sfx : "");
				out_info->current_val = settings ? (double)obs_data_get_int(settings, nm)
								 : out_info->min_val;
			} else {
				mfd_filter_normalize_bounds(obs_property_float_min(p), obs_property_float_max(p),
							    &out_info->min_val, &out_info->max_val);
				double st = obs_property_float_step(p);
				out_info->step = (isfinite(st) && st > 0.0) ? st : 0.01;
				const char *sfx = obs_property_float_suffix(p);
				snprintf(out_info->suffix, sizeof(out_info->suffix), "%s", sfx ? sfx : "");
				out_info->current_val = settings ? obs_data_get_double(settings, nm)
								 : out_info->min_val;
			}
		}
		return true;
	}
	return false;
}

struct mfd_live_enum_filters_ctx {
	mfd_source_filter_cb cb;
	void *cb_param;
};

static void live_enum_source_filters_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct mfd_live_enum_filters_ctx *ctx = (struct mfd_live_enum_filters_ctx *)param;
	if (!ctx || !ctx->cb || !child || obs_source_removed(child)) {
		return;
	}
	const char *fnm = obs_source_get_name(child);
	const char *fid = obs_source_get_id(child);
	if (!fnm || fnm[0] == '\0' || mfd_is_internal_filter(fnm, fid)) {
		return;
	}
	ctx->cb(fnm, fid ? fid : "", obs_source_enabled(child), ctx->cb_param);
}

static bool sg_enum_source_filters(const struct mfd_engine *eng, const char *source_name, bool *out_is_scene,
				   mfd_source_filter_cb cb, void *cb_param)
{
	if (out_is_scene) {
		*out_is_scene = false;
	}
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_source_filters) {
		return eng->scene_graph_ops->enum_source_filters(source_name, out_is_scene, cb, cb_param,
								 eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	if (obs_source_removed(src)) {
		obs_source_release(src);
		return false;
	}
	if (out_is_scene) {
		*out_is_scene = obs_source_is_scene(src);
	}
	if (cb) {
		struct mfd_live_enum_filters_ctx ctx = {cb, cb_param};
		obs_source_enum_filters(src, live_enum_source_filters_cb, &ctx);
	}
	obs_source_release(src);
	return true;
}

struct mfd_find_filter_cb_ctx {
	const char *target_filter;
	bool found;
	bool enabled;
};

static void find_filter_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	if (mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	struct mfd_find_filter_cb_ctx *ctx = (struct mfd_find_filter_cb_ctx *)param;
	if (ctx && ctx->target_filter && filter_name && strcmp(filter_name, ctx->target_filter) == 0) {
		ctx->found = true;
		ctx->enabled = enabled;
	}
}

static bool sg_filter_exists(const struct mfd_engine *eng, const char *source_name, const char *filter_name,
			     bool *out_enabled)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return false;
	}
	if (eng && eng->removing_filter_host && eng->removing_filter_name &&
	    strcmp(eng->removing_filter_host, source_name) == 0 &&
	    strcmp(eng->removing_filter_name, filter_name) == 0) {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->get_filter_enabled) {
		bool en = true;
		if (!eng->scene_graph_ops->get_filter_enabled(source_name, filter_name, &en,
							      eng->scene_graph_user_data)) {
			return false;
		}
		if (out_enabled) {
			*out_enabled = en;
		}
		return true;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_source_filters) {
		struct mfd_find_filter_cb_ctx ctx = {filter_name, false, true};
		if (!sg_enum_source_filters(eng, source_name, NULL, find_filter_cb, &ctx)) {
			return false;
		}
		if (ctx.found && out_enabled) {
			*out_enabled = ctx.enabled;
		}
		return ctx.found;
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	if (obs_source_removed(src)) {
		obs_source_release(src);
		return false;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (!flt) {
		obs_source_release(src);
		return false;
	}
	bool ok = !obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt));
	if (ok && out_enabled) {
		*out_enabled = obs_source_enabled(flt);
	}
	obs_source_release(flt);
	obs_source_release(src);
	return ok;
}

static obs_properties_t *sg_create_filter_properties(const struct mfd_engine *eng, const char *source_name,
						     const char *filter_name)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return NULL;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->create_filter_properties) {
		return eng->scene_graph_ops->create_filter_properties(source_name, filter_name,
								      eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return NULL;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return NULL;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (!flt) {
		obs_source_release(src);
		return NULL;
	}
	obs_properties_t *props = NULL;
	if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
		props = obs_source_properties(flt);
	}
	obs_source_release(flt);
	obs_source_release(src);
	return props;
}

static obs_data_t *sg_get_filter_settings(const struct mfd_engine *eng, const char *source_name,
					  const char *filter_name)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return NULL;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->get_filter_settings) {
		return eng->scene_graph_ops->get_filter_settings(source_name, filter_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return NULL;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return NULL;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (!flt) {
		obs_source_release(src);
		return NULL;
	}
	obs_data_t *settings = NULL;
	if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
		settings = obs_source_get_settings(flt);
	}
	obs_source_release(flt);
	obs_source_release(src);
	return settings;
}

static void sg_update_filter_settings(const struct mfd_engine *eng, const char *source_name, const char *filter_name,
				      obs_data_t *settings)
{
	if (!settings || !source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->update_filter_settings) {
		eng->scene_graph_ops->update_filter_settings(source_name, filter_name, settings,
							     eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (flt) {
		if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
			obs_source_update(flt, settings);
		}
		obs_source_release(flt);
	}
	obs_source_release(src);
}

static void sg_set_filter_enabled(const struct mfd_engine *eng, const char *source_name, const char *filter_name,
				  bool enabled)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->set_filter_enabled) {
		eng->scene_graph_ops->set_filter_enabled(source_name, filter_name, enabled, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (flt) {
		if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
			obs_source_set_enabled(flt, enabled);
		}
		obs_source_release(flt);
	}
	obs_source_release(src);
}

static float query_live_filter_unmapped_deflection_locked(const struct mfd_engine *eng,
							  const struct mfd_fader_slot *slot)
{
	if (!eng || !slot || slot->source_a_name[0] == '\0' || slot->filter_name[0] == '\0' ||
	    slot->filter_prop[0] == '\0') {
		return 0.0f;
	}
	if (fabs(slot->filter_to_val - slot->filter_from_val) < MFD_FILTER_MIN_RANGE_SPAN) {
		return 0.0f;
	}
	if (!mfd_engine_source_exists(eng, slot->source_a_name) ||
	    !sg_filter_exists(eng, slot->source_a_name, slot->filter_name, NULL)) {
		return 0.0f;
	}
	obs_data_t *settings = sg_get_filter_settings(eng, slot->source_a_name, slot->filter_name);
	if (!settings) {
		return 0.0f;
	}
	bool has_val = obs_data_has_user_value(settings, slot->filter_prop) ||
		       obs_data_has_default_value(settings, slot->filter_prop);
	float t = 0.0f;
	if (has_val) {
		bool is_int = slot->filter_prop_is_int;
		if (!slot->filter_prop_valid) {
			obs_data_item_t *it = obs_data_item_byname(settings, slot->filter_prop);
			if (it) {
				is_int = (obs_data_item_numtype(it) == OBS_DATA_NUM_INT);
				obs_data_item_release(&it);
			}
		}
		double live_val = is_int ? (double)obs_data_get_int(settings, slot->filter_prop)
					 : obs_data_get_double(settings, slot->filter_prop);
		t = mfd_filter_unmap_value(live_val, slot->filter_from_val, slot->filter_to_val);
	}
	obs_data_release(settings);
	return t;
}

bool mfd_engine_validate_filter_param_binding_locked(const struct mfd_engine *eng, int slot_index, bool enabled,
						     const char *source_a, const char *filter_name,
						     const char *filter_prop, bool has_from_to, double *inout_from_val,
						     double *inout_to_val, struct mfd_filter_prop_info *out_prop_info,
						     char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Invalid strip index.");
		}
		return false;
	}
	if (!source_a || source_a[0] == '\0') {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Missing required 'source_a' for filter_param.");
		}
		return false;
	}
	if (!filter_name || filter_name[0] == '\0') {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Missing required 'filter_name' for filter_param.");
		}
		return false;
	}
	if (mfd_is_internal_filter(filter_name, NULL)) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size,
				 "Filter '%s' is reserved by Midi Fadester and cannot be bound directly.", filter_name);
		}
		return false;
	}
	if (!filter_prop || filter_prop[0] == '\0') {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Missing required 'filter_prop' for filter_param.");
		}
		return false;
	}

	if (enabled) {
		for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
			if (j == slot_index) {
				continue;
			}
			const struct mfd_fader_slot *other = &eng->fader_slots[j];
			if (other->enabled && other->target_type == MFD_TARGET_FILTER_PARAM &&
			    strcmp(other->source_a_name, source_a) == 0 &&
			    strcmp(other->filter_name, filter_name) == 0 &&
			    strcmp(other->filter_prop, filter_prop) == 0) {
				if (out_err && err_size > 0U) {
					snprintf(out_err, err_size, "Setting '%s' on '%s' is already controlled by %s.",
						 filter_prop, filter_name,
						 (other->label[0] != '\0') ? other->label : other->id);
				}
				return false;
			}
		}
	}

	if (!mfd_engine_source_exists(eng, source_a)) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size,
				 "Source '%s' not found. Create the source in OBS first or check its spelling.",
				 source_a);
		}
		return false;
	}
	if (!sg_filter_exists(eng, source_a, filter_name, NULL)) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size,
				 "Filter '%s' not found on '%s'. Add the filter in OBS Filters first.", filter_name,
				 source_a);
		}
		return false;
	}

	obs_properties_t *props = sg_create_filter_properties(eng, source_a, filter_name);
	if (!props) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Filter '%s' on '%s' exposes no configurable properties.",
				 filter_name, source_a);
		}
		return false;
	}
	obs_data_t *settings = sg_get_filter_settings(eng, source_a, filter_name);
	bool found_non_numeric = false;
	struct mfd_filter_prop_info info;
	memset(&info, 0, sizeof(info));
	bool found_numeric =
		find_filter_prop_in_props_depth(props, settings, filter_prop, &found_non_numeric, &info, 0);
	if (settings) {
		obs_data_release(settings);
	}
	obs_properties_destroy(props);

	if (!found_numeric) {
		if (out_err && err_size > 0U) {
			if (found_non_numeric) {
				snprintf(
					out_err, err_size,
					"Setting '%s' on '%s' is not a numeric setting. Choose a float or integer filter property.",
					filter_prop, filter_name);
			} else {
				snprintf(
					out_err, err_size,
					"Setting '%s' not found on filter '%s'. Query get_catalog to list numeric settings.",
					filter_prop, filter_name);
			}
		}
		return false;
	}

	double fval = info.min_val;
	double tval = info.max_val;
	if (has_from_to) {
		fval = inout_from_val ? *inout_from_val : info.min_val;
		tval = inout_to_val ? *inout_to_val : info.max_val;
	}
	if (!isfinite(fval) || !isfinite(tval)) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "'from_val' and 'to_val' must be finite numbers.");
		}
		return false;
	}
	if (fabs(tval - fval) < MFD_FILTER_MIN_RANGE_SPAN) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size,
				 "'from_val' and 'to_val' must differ so the control has a range to sweep.");
		}
		return false;
	}
	if (!mfd_filter_range_is_valid(fval, tval, info.min_val, info.max_val)) {
		if (out_err && err_size > 0U) {
			snprintf(
				out_err, err_size,
				"Range [%.6g, %.6g] is outside '%s' limits [%.6g, %.6g]. Click Set up to update the range.",
				fval, tval, filter_prop, info.min_val, info.max_val);
		}
		return false;
	}

	if (inout_from_val) {
		*inout_from_val = fval;
	}
	if (inout_to_val) {
		*inout_to_val = tval;
	}
	if (out_prop_info) {
		*out_prop_info = info;
	}
	return true;
}

static struct mfd_filter_bypass_lease *find_filter_lease_locked(struct mfd_engine *eng, const char *source_name,
								const char *filter_name)
{
	if (!eng || !source_name || !source_name[0] || !filter_name || !filter_name[0]) {
		return NULL;
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (l->in_use && strcmp(l->source_name, source_name) == 0 && strcmp(l->filter_name, filter_name) == 0) {
			return l;
		}
	}
	return NULL;
}

static void invalidate_filter_leases_for_removed_locked(struct mfd_engine *eng, const char *source_name,
							const char *filter_name)
{
	if (!eng || !source_name || !source_name[0]) {
		return;
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (!l->in_use || strcmp(l->source_name, source_name) != 0) {
			continue;
		}
		if (!filter_name || filter_name[0] == '\0' || strcmp(l->filter_name, filter_name) == 0) {
			memset(l, 0, sizeof(*l));
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->target_type == MFD_TARGET_FILTER_PARAM && strcmp(s->source_a_name, source_name) == 0 &&
		    (!filter_name || filter_name[0] == '\0' || strcmp(s->filter_name, filter_name) == 0)) {
			s->restore_persisted_pre_bypass = false;
		}
	}
}

static void sync_filter_bypass_lease_ex_locked(struct mfd_engine *eng, const char *source_name, const char *filter_name,
					       int exclude_slot, bool apply_enable_write)
{
	if (!eng || !source_name || !source_name[0] || !filter_name || !filter_name[0]) {
		return;
	}

	bool live_en = true;
	bool flt_exists = sg_filter_exists(eng, source_name, filter_name, &live_en);
	if (!flt_exists) {
		invalidate_filter_leases_for_removed_locked(eng, source_name, filter_name);
		return;
	}

	int active_slot_count = 0;
	int bypass_owner_count = 0;
	bool all_bypassed_at_zero = true;
	bool has_persisted_restore = false;
	bool persisted_pre_en = true;

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (i == exclude_slot) {
			continue;
		}
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (!s->enabled || s->target_type != MFD_TARGET_FILTER_PARAM || !s->cached_available) {
			continue;
		}
		if (strcmp(s->source_a_name, source_name) != 0 || strcmp(s->filter_name, filter_name) != 0) {
			continue;
		}
		++active_slot_count;
		if (s->filter_bypass_at_zero) {
			++bypass_owner_count;
			if (s->restore_persisted_pre_bypass && !has_persisted_restore) {
				has_persisted_restore = true;
				persisted_pre_en = s->filter_pre_bypass_enabled;
			}
			if (s->last_applied_val > MFD_FILTER_BYPASS_ZERO_THRESHOLD) {
				all_bypassed_at_zero = false;
			}
		} else {
			all_bypassed_at_zero = false;
		}
	}

	struct mfd_filter_bypass_lease *lease = find_filter_lease_locked(eng, source_name, filter_name);
	if (bypass_owner_count == 0) {
		if (lease) {
			bool restore_en = lease->pre_mfd_enabled;
			lease->writing_enable = true;
			sg_set_filter_enabled(eng, source_name, filter_name, restore_en);
			lease->writing_enable = false;
			memset(lease, 0, sizeof(*lease));
		}
		return;
	}

	if (!lease) {
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			if (!eng->filter_leases[i].in_use) {
				lease = &eng->filter_leases[i];
				memset(lease, 0, sizeof(*lease));
				lease->in_use = true;
				snprintf(lease->source_name, sizeof(lease->source_name), "%s", source_name);
				snprintf(lease->filter_name, sizeof(lease->filter_name), "%s", filter_name);
				lease->pre_mfd_enabled = has_persisted_restore ? persisted_pre_en : live_en;
				lease->last_mfd_enabled = live_en;
				lease->has_written_enable = false;
				break;
			}
		}
	}
	if (!lease) {
		return;
	}

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (i == exclude_slot) {
			continue;
		}
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->enabled && s->target_type == MFD_TARGET_FILTER_PARAM &&
		    strcmp(s->source_a_name, source_name) == 0 && strcmp(s->filter_name, filter_name) == 0) {
			s->filter_pre_bypass_enabled = lease->pre_mfd_enabled;
			s->restore_persisted_pre_bypass = false;
		}
	}

	if (!apply_enable_write || active_slot_count <= 0) {
		return;
	}

	bool want_enabled = !all_bypassed_at_zero;
	if (!lease->has_written_enable || live_en != want_enabled || lease->last_mfd_enabled != want_enabled) {
		if (live_en != want_enabled) {
			lease->writing_enable = true;
			sg_set_filter_enabled(eng, source_name, filter_name, want_enabled);
			lease->writing_enable = false;
		}
		lease->has_written_enable = true;
		lease->last_mfd_enabled = want_enabled;
	}
}

void mfd_engine_detach_filter_binding_locked(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (s->target_type != MFD_TARGET_FILTER_PARAM || s->source_a_name[0] == '\0' || s->filter_name[0] == '\0') {
		return;
	}
	char prev_sa[MFD_SOURCE_NAME_MAX];
	char prev_fn[MFD_SOURCE_NAME_MAX];
	snprintf(prev_sa, sizeof(prev_sa), "%s", s->source_a_name);
	snprintf(prev_fn, sizeof(prev_fn), "%s", s->filter_name);
	sync_filter_bypass_lease_ex_locked(eng, prev_sa, prev_fn, slot_index, true);
}

void mfd_engine_restore_all_bypass_leases(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (!l->in_use) {
			continue;
		}
		for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
			struct mfd_fader_slot *s = &eng->fader_slots[j];
			if (s->target_type == MFD_TARGET_FILTER_PARAM &&
			    strcmp(s->source_a_name, l->source_name) == 0 &&
			    strcmp(s->filter_name, l->filter_name) == 0) {
				s->filter_pre_bypass_enabled = l->pre_mfd_enabled;
			}
		}
		if (sg_filter_exists(eng, l->source_name, l->filter_name, NULL)) {
			l->writing_enable = true;
			sg_set_filter_enabled(eng, l->source_name, l->filter_name, l->pre_mfd_enabled);
			l->writing_enable = false;
		}
		memset(l, 0, sizeof(*l));
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		eng->fader_slots[i].restore_persisted_pre_bypass = false;
	}
	mfd_engine_unlock(eng);
}

static void refresh_filter_slot_metadata_locked(struct mfd_engine *eng, int slot_index, bool *out_avail,
						char *out_reason, size_t reason_size)
{
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	bool avail = false;
	char rsn[160] = {0};

	slot->filter_prop_valid = false;
	if (!slot->enabled || slot->source_a_name[0] == '\0' || slot->filter_name[0] == '\0' ||
	    slot->filter_prop[0] == '\0') {
		snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
	} else if (mfd_is_internal_filter(slot->filter_name, NULL)) {
		snprintf(rsn, sizeof(rsn), "Filter '%s' is reserved by Midi Fadester and cannot be bound directly.",
			 slot->filter_name);
	} else if (!mfd_engine_source_exists(eng, slot->source_a_name)) {
		invalidate_filter_leases_for_removed_locked(eng, slot->source_a_name, NULL);
		snprintf(rsn, sizeof(rsn), "%s was deleted.", slot->source_a_name);
	} else if (!sg_filter_exists(eng, slot->source_a_name, slot->filter_name, NULL)) {
		invalidate_filter_leases_for_removed_locked(eng, slot->source_a_name, slot->filter_name);
		snprintf(rsn, sizeof(rsn), "Filter '%s' was removed from %s. Re-add it in OBS Filters or click Set up.",
			 slot->filter_name, slot->source_a_name);
	} else {
		obs_properties_t *props = sg_create_filter_properties(eng, slot->source_a_name, slot->filter_name);
		if (!props) {
			snprintf(rsn, sizeof(rsn),
				 "Setting '%s' is no longer available on '%s'. Click Set up to choose another setting.",
				 slot->filter_prop, slot->filter_name);
		} else {
			obs_data_t *settings = sg_get_filter_settings(eng, slot->source_a_name, slot->filter_name);
			bool found_non_num = false;
			struct mfd_filter_prop_info info;
			memset(&info, 0, sizeof(info));
			bool found_num = find_filter_prop_in_props_depth(props, settings, slot->filter_prop,
									 &found_non_num, &info, 0);
			if (settings) {
				obs_data_release(settings);
			}
			obs_properties_destroy(props);

			if (!found_num) {
				if (found_non_num) {
					snprintf(
						rsn, sizeof(rsn),
						"Setting '%s' on '%s' is not a numeric setting. Choose a float or integer filter property.",
						slot->filter_prop, slot->filter_name);
				} else {
					snprintf(
						rsn, sizeof(rsn),
						"Setting '%s' is no longer available on '%s'. Click Set up to choose another setting.",
						slot->filter_prop, slot->filter_name);
				}
			} else if (!mfd_filter_range_is_valid(slot->filter_from_val, slot->filter_to_val, info.min_val,
							      info.max_val)) {
				slot->filter_prop_is_int = info.is_int;
				slot->filter_prop_min = info.min_val;
				slot->filter_prop_max = info.max_val;
				slot->filter_prop_step = info.step;
				snprintf(slot->filter_prop_suffix, sizeof(slot->filter_prop_suffix), "%s", info.suffix);
				snprintf(slot->filter_prop_label, sizeof(slot->filter_prop_label), "%s", info.label);
				snprintf(
					rsn, sizeof(rsn),
					"Range [%.6g, %.6g] is outside '%s' limits [%.6g, %.6g]. Click Set up to update the range.",
					slot->filter_from_val, slot->filter_to_val, slot->filter_prop, info.min_val,
					info.max_val);
			} else {
				slot->filter_prop_valid = true;
				slot->filter_prop_is_int = info.is_int;
				slot->filter_prop_min = info.min_val;
				slot->filter_prop_max = info.max_val;
				slot->filter_prop_step = info.step;
				snprintf(slot->filter_prop_suffix, sizeof(slot->filter_prop_suffix), "%s", info.suffix);
				snprintf(slot->filter_prop_label, sizeof(slot->filter_prop_label), "%s", info.label);
				avail = true;
				rsn[0] = '\0';
			}
		}
	}

	if (out_avail) {
		*out_avail = avail;
	}
	if (out_reason && reason_size > 0U) {
		snprintf(out_reason, reason_size, "%s", rsn);
	}
}

static void evaluate_slot_preconditions_const(const struct mfd_engine *eng, const struct mfd_fader_slot *slot,
					      char *out_label, size_t label_size, bool *out_available, char *out_reason,
					      size_t reason_size, char *out_resolved_scene, size_t resolved_size,
					      bool *out_a_above_b)
{
	char lbl[128] = "Unassigned";
	bool avail = false;
	char rsn[160] = "Not used. Click to set up.";
	char res_scene[MFD_SOURCE_NAME_MAX] = {0};
	bool a_above_b = slot ? slot->a_is_above_b : false;

	if (slot) {
		snprintf(res_scene, sizeof(res_scene), "%s", slot->resolved_scene_name);
		if (!slot->enabled) {
			snprintf(lbl, sizeof(lbl), "Disabled");
			avail = false;
			snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
		} else {
			switch (slot->target_type) {
			case MFD_TARGET_UNASSIGNED:
				snprintf(lbl, sizeof(lbl), "Unassigned");
				avail = false;
				snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
				break;

			case MFD_TARGET_TBAR:
				snprintf(lbl, sizeof(lbl), "Studio Mode T-Bar");
				if (!mfd_engine_is_studio_mode_active(eng)) {
					avail = false;
					snprintf(rsn, sizeof(rsn), "Studio Mode is off.");
				} else if (eng && eng->tbar_error[0] != '\0') {
					avail = false;
					snprintf(rsn, sizeof(rsn), "%s", eng->tbar_error);
				} else {
					avail = true;
					rsn[0] = '\0';
				}
				break;

			case MFD_TARGET_TWO_SOURCE_MIX: {
				if (slot->source_a_name[0] != '\0' && slot->source_b_name[0] != '\0') {
					snprintf(lbl, sizeof(lbl), "%s -> %s", slot->source_a_name,
						 slot->source_b_name);
				} else {
					snprintf(lbl, sizeof(lbl), "Crossfade A/B");
				}
				if (slot->source_a_name[0] == '\0' || slot->source_b_name[0] == '\0') {
					avail = false;
					snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
				} else if (strcmp(slot->source_a_name, slot->source_b_name) == 0) {
					avail = false;
					snprintf(rsn, sizeof(rsn), "Left and Right cannot be the same layer.");
				} else if (slot->scene_name[0] != '\0') {
					if (!mfd_engine_scene_exists(eng, slot->scene_name)) {
						avail = false;
						snprintf(rsn, sizeof(rsn), "Scene %s was deleted.", slot->scene_name);
					} else {
						int idx_a = sg_scene_item_stack_index(eng, slot->scene_name,
										      slot->source_a_name);
						int idx_b = sg_scene_item_stack_index(eng, slot->scene_name,
										      slot->source_b_name);
						if (idx_a < 0) {
							avail = false;
							snprintf(rsn, sizeof(rsn), "%s is no longer in scene %s.",
								 slot->source_a_name, slot->scene_name);
						} else if (idx_b < 0) {
							avail = false;
							snprintf(rsn, sizeof(rsn), "%s is no longer in scene %s.",
								 slot->source_b_name, slot->scene_name);
						} else {
							avail = true;
							rsn[0] = '\0';
							a_above_b = (idx_a > idx_b);
							snprintf(res_scene, sizeof(res_scene), "%s", slot->scene_name);
						}
					}
				} else {
					char found_sc[MFD_SOURCE_NAME_MAX] = {0};
					int idx_a = -1;
					int idx_b = -1;
					if (sg_find_scene_with_pair(eng, slot->source_a_name, slot->source_b_name,
								    found_sc, sizeof(found_sc), &idx_a, &idx_b)) {
						avail = true;
						rsn[0] = '\0';
						a_above_b = (idx_a > idx_b);
						snprintf(res_scene, sizeof(res_scene), "%s", found_sc);
					} else if (slot->resolved_scene_name[0] != '\0') {
						if (!mfd_engine_scene_exists(eng, slot->resolved_scene_name)) {
							avail = false;
							snprintf(rsn, sizeof(rsn), "Scene %s was deleted.",
								 slot->resolved_scene_name);
						} else if (sg_scene_item_stack_index(eng, slot->resolved_scene_name,
										     slot->source_a_name) < 0) {
							avail = false;
							snprintf(rsn, sizeof(rsn), "%s is no longer in scene %s.",
								 slot->source_a_name, slot->resolved_scene_name);
						} else {
							avail = false;
							snprintf(rsn, sizeof(rsn), "%s is no longer in scene %s.",
								 slot->source_b_name, slot->resolved_scene_name);
						}
					} else {
						avail = false;
						snprintf(rsn, sizeof(rsn), "%s and %s are not in the same scene.",
							 slot->source_a_name, slot->source_b_name);
					}
				}
				break;
			}

			case MFD_TARGET_SINGLE_LAYER: {
				if (slot->source_a_name[0] != '\0') {
					snprintf(lbl, sizeof(lbl), "%s (Opacity)", slot->source_a_name);
				} else {
					snprintf(lbl, sizeof(lbl), "Layer Opacity");
				}
				if (slot->source_a_name[0] == '\0') {
					avail = false;
					snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
				} else if (slot->scene_name[0] != '\0') {
					if (!mfd_engine_scene_exists(eng, slot->scene_name)) {
						avail = false;
						snprintf(rsn, sizeof(rsn), "Scene %s was deleted.", slot->scene_name);
					} else if (sg_scene_item_stack_index(eng, slot->scene_name,
									     slot->source_a_name) < 0) {
						avail = false;
						snprintf(rsn, sizeof(rsn), "%s is no longer in scene %s.",
							 slot->source_a_name, slot->scene_name);
					} else {
						avail = true;
						rsn[0] = '\0';
						snprintf(res_scene, sizeof(res_scene), "%s", slot->scene_name);
					}
				} else {
					char found_sc[MFD_SOURCE_NAME_MAX] = {0};
					if (mfd_engine_find_other_scene_with_layer(eng, slot->source_a_name, NULL,
										   found_sc, sizeof(found_sc))) {
						avail = true;
						rsn[0] = '\0';
						snprintf(res_scene, sizeof(res_scene), "%s", found_sc);
					} else if (slot->resolved_scene_name[0] != '\0') {
						if (!mfd_engine_scene_exists(eng, slot->resolved_scene_name)) {
							avail = false;
							snprintf(rsn, sizeof(rsn), "Scene %s was deleted.",
								 slot->resolved_scene_name);
						} else {
							avail = false;
							snprintf(rsn, sizeof(rsn), "%s is no longer in scene %s.",
								 slot->source_a_name, slot->resolved_scene_name);
						}
					} else {
						avail = false;
						snprintf(rsn, sizeof(rsn), "%s is not in any scene.",
							 slot->source_a_name);
					}
				}
				break;
			}

			case MFD_TARGET_AUDIO_VOLUME:
				if (slot->source_a_name[0] != '\0') {
					snprintf(lbl, sizeof(lbl), "%s (Audio)", slot->source_a_name);
				} else {
					snprintf(lbl, sizeof(lbl), "Audio Volume");
				}
				if (slot->source_a_name[0] == '\0') {
					avail = false;
					snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
				} else if (!mfd_engine_source_exists(eng, slot->source_a_name)) {
					avail = false;
					snprintf(rsn, sizeof(rsn), "%s was deleted.", slot->source_a_name);
				} else if (!mfd_engine_source_has_audio(eng, slot->source_a_name)) {
					avail = false;
					snprintf(rsn, sizeof(rsn), "%s has no audio.", slot->source_a_name);
				} else {
					avail = true;
					rsn[0] = '\0';
				}
				break;

			case MFD_TARGET_FILTER_PARAM:
				if (slot->source_a_name[0] != '\0' && slot->filter_name[0] != '\0' &&
				    slot->filter_prop[0] != '\0') {
					snprintf(lbl, sizeof(lbl), "%s: %s (%s)", slot->source_a_name,
						 slot->filter_name, slot->filter_prop);
					avail = slot->cached_available;
					snprintf(rsn, sizeof(rsn), "%s", slot->cached_reason);
				} else {
					snprintf(lbl, sizeof(lbl), "Filter Setting");
					avail = false;
					snprintf(rsn, sizeof(rsn), "Not used. Click to set up.");
				}
				break;
			}
		}
	}

	if (out_label && label_size > 0U) {
		snprintf(out_label, label_size, "%s", lbl);
	}
	if (out_available) {
		*out_available = avail;
	}
	if (out_reason && reason_size > 0U) {
		snprintf(out_reason, reason_size, "%s", rsn);
	}
	if (out_resolved_scene && resolved_size > 0U) {
		snprintf(out_resolved_scene, resolved_size, "%s", res_scene);
	}
	if (out_a_above_b) {
		*out_a_above_b = a_above_b;
	}
}

void mfd_engine_reevaluate_slots(struct mfd_engine *eng, bool emit_if_changed)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	bool any_changed = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		bool prev_avail = slot->cached_available;
		bool prev_a_above_b = slot->a_is_above_b;
		char prev_reason[160];
		snprintf(prev_reason, sizeof(prev_reason), "%s", slot->cached_reason);

		bool new_avail = false;
		char new_reason[160] = {0};
		char new_resolved[MFD_SOURCE_NAME_MAX] = {0};
		bool new_a_above_b = slot->a_is_above_b;
		if (slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM) {
			refresh_filter_slot_metadata_locked(eng, i, &new_avail, new_reason, sizeof(new_reason));
		} else {
			slot->filter_prop_valid = false;
			evaluate_slot_preconditions_const(eng, slot, NULL, 0U, &new_avail, new_reason,
							  sizeof(new_reason), new_resolved, sizeof(new_resolved),
							  &new_a_above_b);
		}

		slot->cached_available = new_avail;
		snprintf(slot->cached_reason, sizeof(slot->cached_reason), "%s", new_reason);
		snprintf(slot->resolved_scene_name, sizeof(slot->resolved_scene_name), "%s", new_resolved);
		slot->a_is_above_b = new_a_above_b;

		if (prev_avail && !new_avail) {
			if (slot->auto_fade_active && slot->auto_fade_is_reset_fx) {
				slot->pending_clean_apply_on_avail = true;
				slot->last_applied_val = 0.0f;
				mfd_takeover_sync_external(&slot->takeover, 0.0f);
			}
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->has_last_written_mapped = false;
			mfd_smoother_reset(&slot->smoother, slot->last_applied_val);
			if (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_THROWING) {
				slot->tbar_state = MFD_TBAR_IDLE;
				slot->tbar_midi_active = false;
				slot->tbar_from_idle_band = false;
				slot->last_applied_val = 0.0f;
				mfd_smoother_reset(&slot->smoother, 0.0f);
			}
		} else if (!prev_avail && new_avail && slot->pending_clean_apply_on_avail &&
			   !(eng->setup_session.active && eng->setup_session.slot_index == i)) {
			slot->pending_clean_apply_on_avail = false;
			slot->has_last_written_mapped = false;
			slot->last_applied_val = 0.0f;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_engine_apply_slot_output_ex(eng, slot, 0.0f, false);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			mfd_engine_notify_fader_changed(eng, i, "api");
			any_changed = true;
		} else if (new_avail && slot->target_type == MFD_TARGET_TWO_SOURCE_MIX && prev_avail &&
			   prev_a_above_b != new_a_above_b &&
			   !(eng->setup_session.active && eng->setup_session.slot_index == i)) {
			mfd_engine_apply_slot_output(eng, slot, slot->last_applied_val);
		}

		if (prev_avail != new_avail || prev_a_above_b != new_a_above_b ||
		    strcmp(prev_reason, new_reason) != 0) {
			any_changed = true;
		}
	}

	/* Acquire or synchronize bypass_at_zero leases for all available filter_param slots */
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM && slot->cached_available) {
			/* Acquire lease without forcing an unsolicited enable write until a value/reset write runs */
			sync_filter_bypass_lease_ex_locked(eng, slot->source_a_name, slot->filter_name, -1, false);
		}
	}

	mfd_engine_mark_ui_dirty(eng, true);
	if (any_changed && emit_if_changed) {
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_source_rename(struct mfd_engine *eng, const char *prev_name, const char *new_name)
{
	if (!eng || !prev_name || prev_name[0] == '\0' || !new_name || new_name[0] == '\0' ||
	    strcmp(prev_name, new_name) == 0) {
		return;
	}
	mfd_engine_lock(eng);
	bool touched = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (l->in_use && strcmp(l->source_name, prev_name) == 0) {
			snprintf(l->source_name, sizeof(l->source_name), "%s", new_name);
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		bool slot_touched = false;
		if (strcmp(s->source_a_name, prev_name) == 0) {
			snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", new_name);
			slot_touched = true;
		}
		if (strcmp(s->source_b_name, prev_name) == 0) {
			snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", new_name);
			slot_touched = true;
		}
		if (strcmp(s->scene_name, prev_name) == 0) {
			snprintf(s->scene_name, sizeof(s->scene_name), "%s", new_name);
			slot_touched = true;
		}
		if (strcmp(s->resolved_scene_name, prev_name) == 0) {
			snprintf(s->resolved_scene_name, sizeof(s->resolved_scene_name), "%s", new_name);
			slot_touched = true;
		}
		if (slot_touched) {
			mfd_engine_bump_slot_rev(eng, i);
			touched = true;
		}
	}
	mfd_engine_reevaluate_slots(eng, false);
	if (touched) {
		mfd_engine_notify_state_changed(eng);
		unlock_and_persist_global_config(eng);
		return;
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_rename(struct mfd_engine *eng, const char *parent_source_name, const char *prev_name,
				     const char *new_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !prev_name || !prev_name[0] || !new_name ||
	    !new_name[0] || strcmp(prev_name, new_name) == 0) {
		return;
	}
	mfd_engine_lock(eng);
	bool touched = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (l->in_use && strcmp(l->source_name, parent_source_name) == 0 &&
		    strcmp(l->filter_name, prev_name) == 0) {
			snprintf(l->filter_name, sizeof(l->filter_name), "%s", new_name);
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->target_type == MFD_TARGET_FILTER_PARAM && strcmp(s->source_a_name, parent_source_name) == 0 &&
		    strcmp(s->filter_name, prev_name) == 0) {
			snprintf(s->filter_name, sizeof(s->filter_name), "%s", new_name);
			mfd_engine_bump_slot_rev(eng, i);
			touched = true;
		}
	}
	mfd_engine_reevaluate_slots(eng, false);
	if (touched) {
		mfd_engine_notify_state_changed(eng);
		unlock_and_persist_global_config(eng);
		return;
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_removed(struct mfd_engine *eng, const char *parent_source_name, const char *filter_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0]) {
		return;
	}
	mfd_engine_lock(eng);
	invalidate_filter_leases_for_removed_locked(eng, parent_source_name, filter_name);
	eng->removing_filter_host = parent_source_name;
	eng->removing_filter_name = filter_name;
	mfd_engine_reevaluate_slots(eng, true);
	eng->removing_filter_host = NULL;
	eng->removing_filter_name = NULL;
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_enable(struct mfd_engine *eng, const char *parent_source_name, const char *filter_name,
				     bool enabled)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !filter_name || !filter_name[0]) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_filter_bypass_lease *lease = find_filter_lease_locked(eng, parent_source_name, filter_name);
	if (lease) {
		if (lease->writing_enable) {
			mfd_engine_unlock(eng);
			return;
		}
		/* Operator manually toggled filter eye icon in OBS: respect & record as new pre-bypass baseline */
		lease->pre_mfd_enabled = enabled;
		lease->last_mfd_enabled = enabled;
		lease->has_written_enable = true;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *s = &eng->fader_slots[i];
			if (s->target_type == MFD_TARGET_FILTER_PARAM &&
			    strcmp(s->source_a_name, parent_source_name) == 0 &&
			    strcmp(s->filter_name, filter_name) == 0) {
				s->filter_pre_bypass_enabled = enabled;
			}
		}
		if (obs_initialized()) {
			eng->pending_config_save = true;
		}
	}
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_settings_updated(struct mfd_engine *eng, const char *parent_source_name,
					       const char *filter_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !filter_name || !filter_name[0]) {
		return;
	}
	mfd_engine_lock(eng);
	obs_data_t *settings = NULL;
	bool any_synced = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled || slot->target_type != MFD_TARGET_FILTER_PARAM || !slot->cached_available ||
		    !slot->filter_prop_valid) {
			continue;
		}
		if (strcmp(slot->source_a_name, parent_source_name) != 0 ||
		    strcmp(slot->filter_name, filter_name) != 0) {
			continue;
		}
		if (slot->writing_filter_settings) {
			continue;
		}
		if (!settings) {
			settings = sg_get_filter_settings(eng, parent_source_name, filter_name);
			if (!settings) {
				break;
			}
		}
		bool has_val = obs_data_has_user_value(settings, slot->filter_prop) ||
			       obs_data_has_default_value(settings, slot->filter_prop);
		if (!has_val) {
			continue;
		}
		double live_val = slot->filter_prop_is_int ? (double)obs_data_get_int(settings, slot->filter_prop)
							   : obs_data_get_double(settings, slot->filter_prop);
		double cur_mapped = mfd_filter_map_value(slot->last_applied_val, slot->filter_from_val,
							 slot->filter_to_val, slot->filter_prop_min,
							 slot->filter_prop_max, slot->filter_prop_is_int);
		bool same = slot->filter_prop_is_int ? (llround(live_val) == llround(cur_mapped))
						     : (fabs(live_val - cur_mapped) <= 1e-6);
		if (same) {
			continue;
		}
		float t_ext = mfd_filter_unmap_value(live_val, slot->filter_from_val, slot->filter_to_val);
		slot->auto_fade_active = false;
		slot->auto_fade_is_reset_fx = false;
		slot->pending_clean_apply_on_avail = false;
		slot->has_last_written_mapped = false;
		slot->last_applied_val = t_ext;
		mfd_smoother_reset(&slot->smoother, t_ext);
		mfd_takeover_sync_external(&slot->takeover, t_ext);
		slot->sec_since_last_fader_event = 1.0f;
		mfd_engine_notify_fader_changed(eng, i, "obs");
		any_synced = true;
	}
	if (settings) {
		obs_data_release(settings);
	}
	if (any_synced) {
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_properties_updated(struct mfd_engine *eng, const char *parent_source_name,
						 const char *filter_name)
{
	UNUSED_PARAMETER(parent_source_name);
	UNUSED_PARAMETER(filter_name);
	if (!eng) {
		return;
	}
	mfd_engine_reevaluate_slots(eng, true);
}

void mfd_engine_get_slot_status(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, char *out_label,
				size_t label_size, bool *out_available, char *out_reason, size_t reason_size)
{
	evaluate_slot_preconditions_const(eng, slot, out_label, label_size, out_available, out_reason, reason_size,
					  NULL, 0U, NULL);
}

void mfd_engine_notify_fader_changed(struct mfd_engine *eng, int slot_index, const char *origin)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	const char *orig = (origin && *origin) ? origin : "api";

	if (slot->sec_since_last_fader_event >= MFD_FADER_EVENT_MIN_INTERVAL_SEC) {
		slot->sec_since_last_fader_event = 0.0f;
		slot->fader_event_pending = false;
		eng->pending_fader_emits[slot_index].pending = true;
		snprintf(eng->pending_fader_emits[slot_index].slot_id,
			 sizeof(eng->pending_fader_emits[slot_index].slot_id), "%s", slot->id);
		eng->pending_fader_emits[slot_index].value = slot->last_applied_val;
		mfd_engine_format_slot_display_value(slot, slot->last_applied_val,
						     eng->pending_fader_emits[slot_index].display_value,
						     sizeof(eng->pending_fader_emits[slot_index].display_value));
		snprintf(eng->pending_fader_emits[slot_index].origin,
			 sizeof(eng->pending_fader_emits[slot_index].origin), "%s", orig);
	} else {
		slot->fader_event_pending = true;
		snprintf(slot->pending_origin, sizeof(slot->pending_origin), "%s", orig);
	}
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
}

void mfd_engine_notify_state_changed(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->pending_state_event = true;
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

bool mfd_engine_is_slot_writable(const struct mfd_engine *eng, const struct mfd_fader_slot *slot)
{
	if (!slot || !slot->enabled) {
		return false;
	}
	if (slot->target_type == MFD_TARGET_UNASSIGNED) {
		return true;
	}
	if (slot->target_type == MFD_TARGET_TBAR && eng && eng->studio_mode_override_set) {
		return eng->studio_mode_override && (eng->tbar_error[0] == '\0');
	}
	return slot->cached_available;
}

/* --- Heap-Only Atomic UI Catalog & Snapshot --- */

struct mfd_ui_catalog *mfd_ui_catalog_create(void)
{
	return (struct mfd_ui_catalog *)calloc(1U, sizeof(struct mfd_ui_catalog));
}

void mfd_ui_catalog_destroy(struct mfd_ui_catalog *cat)
{
	free(cat);
}

static int cmp_scene_entries(const void *a, const void *b)
{
	const struct mfd_ui_scene_entry *sa = (const struct mfd_ui_scene_entry *)a;
	const struct mfd_ui_scene_entry *sb = (const struct mfd_ui_scene_entry *)b;
	return ascii_stricmp(sa->name, sb->name);
}

static int cmp_audio_entries(const void *a, const void *b)
{
	const struct mfd_ui_audio_entry *aa = (const struct mfd_ui_audio_entry *)a;
	const struct mfd_ui_audio_entry *ab = (const struct mfd_ui_audio_entry *)b;
	return ascii_stricmp(aa->name, ab->name);
}

static int cmp_filter_host_entries(const void *a, const void *b)
{
	const struct mfd_ui_filter_host_summary *ha = (const struct mfd_ui_filter_host_summary *)a;
	const struct mfd_ui_filter_host_summary *hb = (const struct mfd_ui_filter_host_summary *)b;
	return ascii_stricmp(ha->name, hb->name);
}

static void count_user_filters_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	UNUSED_PARAMETER(enabled);
	if (mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	uint32_t *cnt = (uint32_t *)param;
	if (cnt) {
		++(*cnt);
	}
}

struct mfd_ui_catalog_host_ctx {
	const struct mfd_engine *eng;
	struct mfd_ui_catalog *cat;
};

static void add_ui_catalog_filter_host_cb(const char *source_name, bool is_scene, void *param)
{
	struct mfd_ui_catalog_host_ctx *ctx = (struct mfd_ui_catalog_host_ctx *)param;
	if (!ctx || !ctx->cat || is_internal_source_name(source_name)) {
		return;
	}
	for (size_t i = 0U; i < ctx->cat->filter_host_count; ++i) {
		if (strcmp(ctx->cat->filter_hosts[i].name, source_name) == 0) {
			return;
		}
	}
	if (ctx->cat->filter_host_count >= MFD_CATALOG_MAX_FILTER_HOSTS) {
		ctx->cat->filter_hosts_truncated = true;
		return;
	}
	uint32_t fcnt = 0U;
	(void)sg_enum_source_filters(ctx->eng, source_name, NULL, count_user_filters_cb, &fcnt);
	struct mfd_ui_filter_host_summary *h = &ctx->cat->filter_hosts[ctx->cat->filter_host_count++];
	memset(h, 0, sizeof(*h));
	snprintf(h->name, sizeof(h->name), "%s", source_name);
	h->is_scene = is_scene;
	h->filter_count = fcnt;
}

struct mfd_live_host_enum_ctx {
	mfd_filter_host_cb cb;
	void *cb_param;
	bool scenes_pass;
};

static bool live_enum_hosts_cb(void *param, obs_source_t *src)
{
	struct mfd_live_host_enum_ctx *ctx = (struct mfd_live_host_enum_ctx *)param;
	if (!ctx || !ctx->cb || !src || obs_source_removed(src)) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (is_internal_source_name(nm)) {
		return true;
	}
	ctx->cb(nm, ctx->scenes_pass || obs_source_is_scene(src), ctx->cb_param);
	return true;
}

static void sg_enum_filter_hosts(const struct mfd_engine *eng, mfd_filter_host_cb cb, void *cb_param)
{
	if (!cb) {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_filter_hosts) {
		eng->scene_graph_ops->enum_filter_hosts(cb, cb_param, eng->scene_graph_user_data);
		return;
	}
	if (!obs_initialized()) {
		return;
	}
	struct mfd_live_host_enum_ctx sctx = {cb, cb_param, true};
	obs_enum_scenes(live_enum_hosts_cb, &sctx);
	struct mfd_live_host_enum_ctx ictx = {cb, cb_param, false};
	obs_enum_sources(live_enum_hosts_cb, &ictx);
}

static bool enum_catalog_scene_items_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct mfd_ui_scene_entry *se = (struct mfd_ui_scene_entry *)param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src || obs_source_removed(src)) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (!nm || !*nm || strncmp(nm, "__mfd_", 6) == 0 || strncmp(nm, "__midi_fadester_", 16) == 0) {
		return true;
	}
	for (size_t k = 0U; k < se->layer_count; ++k) {
		if (strcmp(se->layers[k], nm) == 0) {
			return true;
		}
	}
	if (se->layer_count >= MFD_CATALOG_MAX_SCENE_LAYERS) {
		se->layers_truncated = true;
		return true;
	}
	size_t idx = se->layer_count++;
	snprintf(se->layers[idx], sizeof(se->layers[idx]), "%s", nm);
	se->layer_has_audio[idx] = ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0U);
	return true;
}

static bool enum_catalog_scenes_cb(void *param, obs_source_t *scene_src)
{
	struct mfd_ui_catalog *cat = (struct mfd_ui_catalog *)param;
	if (!cat || !scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src)) {
		return true;
	}
	const char *sc_name = obs_source_get_name(scene_src);
	if (!sc_name || !*sc_name) {
		return true;
	}
	for (size_t i = 0U; i < cat->scene_count; ++i) {
		if (strcmp(cat->scenes[i].name, sc_name) == 0) {
			return true;
		}
	}
	if (cat->scene_count >= MFD_CATALOG_MAX_SCENES) {
		cat->scenes_truncated = true;
		return true;
	}
	struct mfd_ui_scene_entry *se = &cat->scenes[cat->scene_count++];
	memset(se, 0, sizeof(*se));
	snprintf(se->name, sizeof(se->name), "%s", sc_name);
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		obs_scene_enum_items(scene, enum_catalog_scene_items_cb, se);
	}
	return true;
}

static bool enum_catalog_audio_cb(void *param, obs_source_t *src)
{
	struct mfd_ui_catalog *cat = (struct mfd_ui_catalog *)param;
	if (!cat || !src || obs_source_removed(src)) {
		return true;
	}
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0U) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (!nm || !*nm || strncmp(nm, "__mfd_", 6) == 0 || strncmp(nm, "__midi_fadester_", 16) == 0) {
		return true;
	}
	for (size_t i = 0U; i < cat->audio_count; ++i) {
		if (strcmp(cat->audio_sources[i].name, nm) == 0) {
			return true;
		}
	}
	if (cat->audio_count >= MFD_CATALOG_MAX_AUDIO_SOURCES) {
		cat->audio_truncated = true;
		return true;
	}
	struct mfd_ui_audio_entry *ae = &cat->audio_sources[cat->audio_count++];
	snprintf(ae->name, sizeof(ae->name), "%s", nm);
	ae->db = mfd_mul_to_db(obs_source_get_volume(src));
	return true;
}

void mfd_engine_get_ui_catalog(struct mfd_engine *eng, struct mfd_ui_catalog *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!eng) {
		return;
	}

	mfd_engine_lock(eng);
	out->config_topology_rev = eng->config_topology_rev;
	if (eng->scene_graph_ops && eng->scene_graph_ops->fill_catalog) {
		eng->scene_graph_ops->fill_catalog(out, eng->scene_graph_user_data);
		if (out->filter_host_count == 0U) {
			struct mfd_ui_catalog_host_ctx hctx = {eng, out};
			sg_enum_filter_hosts(eng, add_ui_catalog_filter_host_cb, &hctx);
		}
	} else if (obs_initialized()) {
		obs_enum_scenes(enum_catalog_scenes_cb, out);
		obs_enum_sources(enum_catalog_audio_cb, out);
		struct mfd_ui_catalog_host_ctx hctx = {eng, out};
		sg_enum_filter_hosts(eng, add_ui_catalog_filter_host_cb, &hctx);
	}

	if (out->scene_count > 1U) {
		qsort(out->scenes, out->scene_count, sizeof(out->scenes[0]), cmp_scene_entries);
	}
	if (out->audio_count > 1U) {
		qsort(out->audio_sources, out->audio_count, sizeof(out->audio_sources[0]), cmp_audio_entries);
	}
	if (out->filter_host_count > 1U) {
		qsort(out->filter_hosts, out->filter_host_count, sizeof(out->filter_hosts[0]), cmp_filter_host_entries);
	}
	mfd_engine_unlock(eng);
}

struct mfd_source_filter_catalog *mfd_source_filter_catalog_create(void)
{
	return (struct mfd_source_filter_catalog *)calloc(1U, sizeof(struct mfd_source_filter_catalog));
}

void mfd_source_filter_catalog_destroy(struct mfd_source_filter_catalog *cat)
{
	free(cat);
}

static void append_prop_to_filter_entry_cb(const struct mfd_filter_prop_info *info, void *param)
{
	struct mfd_filter_entry_info *fe = (struct mfd_filter_entry_info *)param;
	if (!fe || !info) {
		return;
	}
	if (fe->prop_count >= MFD_FILTER_MAX_PROPS) {
		fe->props_truncated = true;
		return;
	}
	fe->props[fe->prop_count++] = *info;
}

struct mfd_fill_source_filter_cat_ctx {
	const struct mfd_engine *eng;
	const char *source_name;
	struct mfd_source_filter_catalog *out;
};

static void fill_source_filter_cat_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	struct mfd_fill_source_filter_cat_ctx *ctx = (struct mfd_fill_source_filter_cat_ctx *)param;
	if (!ctx || !ctx->out || !filter_name || !filter_name[0] || mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	if (ctx->out->filter_count >= MFD_HOST_MAX_FILTERS) {
		ctx->out->filters_truncated = true;
		return;
	}
	struct mfd_filter_entry_info *fe = &ctx->out->filters[ctx->out->filter_count++];
	memset(fe, 0, sizeof(*fe));
	snprintf(fe->name, sizeof(fe->name), "%s", filter_name);
	snprintf(fe->type_id, sizeof(fe->type_id), "%s", type_id ? type_id : "");
	fe->enabled = enabled;

	obs_properties_t *props = sg_create_filter_properties(ctx->eng, ctx->source_name, filter_name);
	if (props) {
		obs_data_t *settings = sg_get_filter_settings(ctx->eng, ctx->source_name, filter_name);
		mfd_collect_filter_numeric_props(props, settings, append_prop_to_filter_entry_cb, fe);
		if (settings) {
			obs_data_release(settings);
		}
		obs_properties_destroy(props);
	}
}

bool mfd_engine_get_source_filter_catalog(struct mfd_engine *eng, const char *source_name,
					  struct mfd_source_filter_catalog *out)
{
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!eng || !source_name || source_name[0] == '\0') {
		return false;
	}
	snprintf(out->source_name, sizeof(out->source_name), "%s", source_name);
	mfd_engine_lock(eng);
	if (!mfd_engine_source_exists(eng, source_name)) {
		out->source_exists = false;
		mfd_engine_unlock(eng);
		return false;
	}
	out->source_exists = true;
	struct mfd_fill_source_filter_cat_ctx ctx = {eng, source_name, out};
	bool is_sc = false;
	(void)sg_enum_source_filters(eng, source_name, &is_sc, fill_source_filter_cat_cb, &ctx);
	out->is_scene = is_sc || mfd_engine_scene_exists(eng, source_name);
	mfd_engine_unlock(eng);
	return true;
}

static void append_prop_to_obs_array_cb(const struct mfd_filter_prop_info *info, void *param)
{
	obs_data_array_t *arr = (obs_data_array_t *)param;
	if (!arr || !info) {
		return;
	}
	obs_data_t *pobj = obs_data_create();
	obs_data_set_string(pobj, "name", info->name);
	obs_data_set_string(pobj, "label", info->label);
	obs_data_set_string(pobj, "type", info->is_int ? "int" : "float");
	obs_data_set_double(pobj, "min", info->min_val);
	obs_data_set_double(pobj, "max", info->max_val);
	obs_data_set_double(pobj, "step", info->step);
	obs_data_set_string(pobj, "suffix", info->suffix);
	if (info->is_int) {
		obs_data_set_int(pobj, "current", (long long)llround(info->current_val));
	} else {
		obs_data_set_double(pobj, "current", info->current_val);
	}
	obs_data_array_push_back(arr, pobj);
	obs_data_release(pobj);
}

struct mfd_vendor_filter_list_ctx {
	const struct mfd_engine *eng;
	const char *source_name;
	obs_data_array_t *filters_arr;
};

static void append_vendor_filter_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	struct mfd_vendor_filter_list_ctx *ctx = (struct mfd_vendor_filter_list_ctx *)param;
	if (!ctx || !ctx->filters_arr || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	obs_data_t *fobj = obs_data_create();
	obs_data_set_string(fobj, "name", filter_name);
	obs_data_set_string(fobj, "type_id", type_id ? type_id : "");
	obs_data_set_bool(fobj, "enabled", enabled);

	obs_data_array_t *parr = obs_data_array_create();
	obs_properties_t *props = sg_create_filter_properties(ctx->eng, ctx->source_name, filter_name);
	if (props) {
		obs_data_t *settings = sg_get_filter_settings(ctx->eng, ctx->source_name, filter_name);
		mfd_collect_filter_numeric_props(props, settings, append_prop_to_obs_array_cb, parr);
		if (settings) {
			obs_data_release(settings);
		}
		obs_properties_destroy(props);
	}
	obs_data_set_array(fobj, "properties", parr);
	obs_data_array_release(parr);

	obs_data_array_push_back(ctx->filters_arr, fobj);
	obs_data_release(fobj);
}

struct mfd_vendor_host_list_ctx {
	const struct mfd_engine *eng;
	obs_data_array_t *hosts_arr;
};

static bool obs_array_contains_string_field(obs_data_array_t *arr, const char *key, const char *val)
{
	if (!arr || !key || !val) {
		return false;
	}
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0U; i < n; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		const char *s = it ? obs_data_get_string(it, key) : NULL;
		bool match = (s && strcmp(s, val) == 0);
		if (it) {
			obs_data_release(it);
		}
		if (match) {
			return true;
		}
	}
	return false;
}

static void append_vendor_host_cb(const char *source_name, bool is_scene, void *param)
{
	struct mfd_vendor_host_list_ctx *ctx = (struct mfd_vendor_host_list_ctx *)param;
	if (!ctx || !ctx->hosts_arr || is_internal_source_name(source_name)) {
		return;
	}
	if (obs_array_contains_string_field(ctx->hosts_arr, "source", source_name)) {
		return;
	}
	uint32_t fcnt = 0U;
	(void)sg_enum_source_filters(ctx->eng, source_name, NULL, count_user_filters_cb, &fcnt);
	obs_data_t *hobj = obs_data_create();
	obs_data_set_string(hobj, "source", source_name);
	obs_data_set_bool(hobj, "is_scene", is_scene);
	obs_data_set_int(hobj, "filter_count", (long long)fcnt);
	obs_data_array_push_back(ctx->hosts_arr, hobj);
	obs_data_release(hobj);
}

static bool append_live_vendor_scene_items_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	obs_data_array_t *layers_arr = (obs_data_array_t *)param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!layers_arr || !src || obs_source_removed(src)) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (is_internal_source_name(nm) || obs_array_contains_string_field(layers_arr, "name", nm)) {
		return true;
	}
	obs_data_t *lobj = obs_data_create();
	obs_data_set_string(lobj, "name", nm);
	obs_data_set_bool(lobj, "has_audio", (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0U);
	obs_data_array_push_back(layers_arr, lobj);
	obs_data_release(lobj);
	return true;
}

static bool append_live_vendor_scenes_cb(void *param, obs_source_t *scene_src)
{
	obs_data_array_t *scenes_arr = (obs_data_array_t *)param;
	if (!scenes_arr || !scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src)) {
		return true;
	}
	const char *sc_name = obs_source_get_name(scene_src);
	if (!sc_name || !sc_name[0] || obs_array_contains_string_field(scenes_arr, "name", sc_name)) {
		return true;
	}
	obs_data_t *sobj = obs_data_create();
	obs_data_set_string(sobj, "name", sc_name);
	obs_data_array_t *layers_arr = obs_data_array_create();
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		obs_scene_enum_items(scene, append_live_vendor_scene_items_cb, layers_arr);
	}
	obs_data_set_array(sobj, "layers", layers_arr);
	obs_data_array_release(layers_arr);
	obs_data_array_push_back(scenes_arr, sobj);
	obs_data_release(sobj);
	return true;
}

static bool append_live_vendor_audio_cb(void *param, obs_source_t *src)
{
	obs_data_array_t *audio_arr = (obs_data_array_t *)param;
	if (!audio_arr || !src || obs_source_removed(src)) {
		return true;
	}
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0U) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (is_internal_source_name(nm) || obs_array_contains_string_field(audio_arr, "name", nm)) {
		return true;
	}
	obs_data_t *aobj = obs_data_create();
	obs_data_set_string(aobj, "name", nm);
	float db = mfd_mul_to_db(obs_source_get_volume(src));
	obs_data_set_double(aobj, "db", isfinite(db) ? (double)db : -120.0);
	obs_data_array_push_back(audio_arr, aobj);
	obs_data_release(aobj);
	return true;
}

bool mfd_engine_fill_vendor_catalog_locked(struct mfd_engine *eng, const char *source_name, obs_data_t *response_data,
					   char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	if (!eng || !response_data) {
		return false;
	}

	if (source_name && source_name[0] != '\0') {
		if (!mfd_engine_source_exists(eng, source_name)) {
			if (out_err && err_size > 0U) {
				snprintf(out_err, err_size,
					 "Source '%s' not found. Create the source in OBS first or check its spelling.",
					 source_name);
			}
			return false;
		}
		bool is_sc = false;
		obs_data_array_t *farr = obs_data_array_create();
		struct mfd_vendor_filter_list_ctx fctx = {eng, source_name, farr};
		(void)sg_enum_source_filters(eng, source_name, &is_sc, append_vendor_filter_cb, &fctx);
		obs_data_set_string(response_data, "source", source_name);
		obs_data_set_bool(response_data, "is_scene", is_sc || mfd_engine_scene_exists(eng, source_name));
		obs_data_set_array(response_data, "filters", farr);
		obs_data_array_release(farr);
		return true;
	}

	obs_data_array_t *scenes_arr = obs_data_array_create();
	obs_data_array_t *audio_arr = obs_data_array_create();
	obs_data_array_t *hosts_arr = obs_data_array_create();

	if (eng->scene_graph_ops && eng->scene_graph_ops->fill_catalog) {
		struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
		if (cat) {
			mfd_engine_get_ui_catalog(eng, cat);
			for (size_t i = 0U; i < cat->scene_count; ++i) {
				obs_data_t *sobj = obs_data_create();
				obs_data_set_string(sobj, "name", cat->scenes[i].name);
				obs_data_array_t *larr = obs_data_array_create();
				for (size_t j = 0U; j < cat->scenes[i].layer_count; ++j) {
					obs_data_t *lobj = obs_data_create();
					obs_data_set_string(lobj, "name", cat->scenes[i].layers[j]);
					obs_data_set_bool(lobj, "has_audio", cat->scenes[i].layer_has_audio[j]);
					obs_data_array_push_back(larr, lobj);
					obs_data_release(lobj);
				}
				obs_data_set_array(sobj, "layers", larr);
				obs_data_array_release(larr);
				obs_data_array_push_back(scenes_arr, sobj);
				obs_data_release(sobj);
			}
			for (size_t i = 0U; i < cat->audio_count; ++i) {
				obs_data_t *aobj = obs_data_create();
				obs_data_set_string(aobj, "name", cat->audio_sources[i].name);
				obs_data_set_double(
					aobj, "db",
					isfinite(cat->audio_sources[i].db) ? (double)cat->audio_sources[i].db : -120.0);
				obs_data_array_push_back(audio_arr, aobj);
				obs_data_release(aobj);
			}
			mfd_ui_catalog_destroy(cat);
		}
	} else if (obs_initialized()) {
		obs_enum_scenes(append_live_vendor_scenes_cb, scenes_arr);
		obs_enum_sources(append_live_vendor_audio_cb, audio_arr);
	}

	struct mfd_vendor_host_list_ctx hctx = {eng, hosts_arr};
	sg_enum_filter_hosts(eng, append_vendor_host_cb, &hctx);

	obs_data_set_array(response_data, "scenes", scenes_arr);
	obs_data_set_array(response_data, "audio_sources", audio_arr);
	obs_data_set_array(response_data, "filter_hosts", hosts_arr);
	obs_data_array_release(scenes_arr);
	obs_data_array_release(audio_arr);
	obs_data_array_release(hosts_arr);
	return true;
}

void mfd_engine_get_ui_snapshot(struct mfd_engine *eng, struct mfd_ui_snapshot *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!eng) {
		return;
	}

	mfd_engine_lock(eng);
	out->config_topology_rev = eng->config_topology_rev;
	out->telemetry_rev = eng->telemetry_rev;
	memcpy(out->slot_config_rev, eng->slot_config_rev, sizeof(out->slot_config_rev));
	snprintf(out->configured_device_name, sizeof(out->configured_device_name), "%s", eng->configured_device_name);
	snprintf(out->opened_device_name, sizeof(out->opened_device_name), "%s", eng->midi_dev.opened_name);
	out->midi_connected = eng->midi_dev.is_open;
	out->midi_error_kind = eng->midi_dev.error_kind;
	snprintf(out->midi_last_error, sizeof(out->midi_last_error), "%s", eng->midi_dev.last_error);
	out->active_profile = mfd_engine_get_active_profile(eng);
	const struct mfd_controller_profile_def *pdef = mfd_profile_get_def(out->active_profile);
	out->active_profile_tier = pdef ? pdef->tier : MFD_PROFILE_TIER_CUSTOM;
	snprintf(out->active_profile_name, sizeof(out->active_profile_name), "%s",
		 pdef ? pdef->slug : effective_profile_name(eng));
	out->smooth_tau_ms = eng->smooth_tau_ms;
	out->tbar_bidirectional_flip = eng->tbar_bidirectional_flip;
	out->frontend_finished_loading = eng->frontend_finished_loading;
	out->studio_mode_active = mfd_engine_is_studio_mode_active(eng);
	snprintf(out->preview_scene_name, sizeof(out->preview_scene_name), "%s", eng->cached_preview_scene);
	snprintf(out->program_scene_name, sizeof(out->program_scene_name), "%s", eng->cached_program_scene);
	out->first_run_offer_pending = eng->frontend_finished_loading && !eng->first_run_offer_dismissed &&
				       eng->apc40_controller_detected && (out->active_profile == MFD_PROFILE_APC40_MK2);
	out->learning_slot_index = eng->learning_slot_index;
	out->has_last_midi = eng->has_last_midi;
	out->last_midi_decoded = eng->last_midi_decoded;
	out->last_moved_slot_index = eng->last_moved_slot_index;
	out->midi_dropped_count = mfd_midi_queue_dropped(&eng->midi_queue);
	out->setup = eng->setup_session;
	memcpy(out->slots, eng->fader_slots, sizeof(out->slots));
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		char lbl[128] = {0};
		bool avail = false;
		char rsn[160] = {0};
		char res_sc[MFD_SOURCE_NAME_MAX] = {0};
		bool a_above = out->slots[i].a_is_above_b;
		evaluate_slot_preconditions_const(eng, &out->slots[i], lbl, sizeof(lbl), &avail, rsn, sizeof(rsn),
						  res_sc, sizeof(res_sc), &a_above);
		out->slots[i].cached_available = avail;
		snprintf(out->slots[i].cached_reason, sizeof(out->slots[i].cached_reason), "%s", rsn);
		snprintf(out->slots[i].resolved_scene_name, sizeof(out->slots[i].resolved_scene_name), "%s", res_sc);
		out->slots[i].a_is_above_b = a_above;
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_set_test_frontend_state(struct mfd_engine *eng, bool finished_loading, bool studio_mode,
					const char *preview_scene, const char *program_scene, bool apc40_detected)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->frontend_finished_loading = finished_loading;
	eng->studio_mode_override_set = true;
	eng->studio_mode_override = studio_mode;
	eng->cached_studio_mode = studio_mode;
	snprintf(eng->cached_preview_scene, sizeof(eng->cached_preview_scene), "%s",
		 preview_scene ? preview_scene : "");
	snprintf(eng->cached_program_scene, sizeof(eng->cached_program_scene), "%s",
		 program_scene ? program_scene : "");
	eng->apc40_controller_detected = apc40_detected;
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_request_studio_mode(struct mfd_engine *eng, bool enabled)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->scene_graph_ops && eng->scene_graph_ops->set_studio_mode) {
		eng->scene_graph_ops->set_studio_mode(enabled, eng->scene_graph_user_data);
	}
	if (eng->studio_mode_override_set || !obs_initialized()) {
		eng->studio_mode_override_set = true;
		eng->studio_mode_override = enabled;
		eng->cached_studio_mode = enabled;
		mfd_engine_reevaluate_slots(eng, true);
		mfd_engine_unlock(eng);
		return;
	}
	mfd_engine_unlock(eng);

	obs_frontend_set_preview_program_mode(enabled);
	mfd_engine_lock(eng);
	refresh_frontend_cache_on_ui_thread_locked(eng);
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

/* --- Setup Session Transaction & First-Run Offer --- */

void mfd_engine_begin_setup_session(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->learning_slot_index >= 0 || eng->setup_session.learning) {
		eng->learning_slot_index = -1;
		mfd_midi_parser_cancel_learn(&eng->midi_parser);
	}

	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	slot->auto_fade_active = false;
	mfd_smoother_reset(&slot->smoother, slot->last_applied_val);

	memset(&eng->setup_session, 0, sizeof(eng->setup_session));
	eng->setup_session.active = true;
	eng->setup_session.slot_index = slot_index;
	eng->setup_session.base_slot_rev = eng->slot_config_rev[slot_index];
	eng->setup_session.candidate_addr = slot->addr;
	eng->setup_session.candidate_cal = slot->cal;
	eng->setup_session.candidate_invert = slot->invert;
	eng->setup_session.learning = false;
	eng->setup_session.candidate_raw_norm = slot->last_raw_norm;
	eng->setup_session.has_candidate_input = slot->takeover.has_last_fader;

	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_start_setup_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (!eng->setup_session.active) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->learning_slot_index = -1;
	eng->setup_session.learning = true;
	mfd_midi_parser_start_learn(&eng->midi_parser);
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_cancel_setup_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.learning) {
		eng->setup_session.learning = false;
		if (eng->learning_slot_index < 0) {
			mfd_midi_parser_cancel_learn(&eng->midi_parser);
		}
		mfd_engine_mark_ui_dirty(eng, true);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_set_setup_invert(struct mfd_engine *eng, bool invert)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.active && eng->setup_session.candidate_invert != invert) {
		eng->setup_session.candidate_invert = invert;
		if (eng->setup_session.has_candidate_input) {
			eng->setup_session.candidate_raw_norm = 1.0f - eng->setup_session.candidate_raw_norm;
		}
		mfd_engine_mark_ui_dirty(eng, true);
		mfd_engine_mark_ui_dirty(eng, false);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_cancel_setup_session(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.active) {
		if (eng->setup_session.learning && eng->learning_slot_index < 0) {
			mfd_midi_parser_cancel_learn(&eng->midi_parser);
		}
		memset(&eng->setup_session, 0, sizeof(eng->setup_session));
		mfd_engine_mark_ui_dirty(eng, true);
	}
	mfd_engine_unlock(eng);
}

bool mfd_engine_unassign_slot_preserve_addr(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return false;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.active && eng->setup_session.slot_index == slot_index) {
		if (eng->setup_session.learning && eng->learning_slot_index < 0) {
			mfd_midi_parser_cancel_learn(&eng->midi_parser);
		}
		memset(&eng->setup_session, 0, sizeof(eng->setup_session));
	}
	mfd_engine_detach_filter_binding_locked(eng, slot_index);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	s->enabled = true;
	s->scene_name[0] = '\0';
	s->resolved_scene_name[0] = '\0';
	s->source_a_name[0] = '\0';
	s->source_b_name[0] = '\0';
	s->filter_name[0] = '\0';
	s->filter_prop[0] = '\0';
	s->filter_from_val = 0.0;
	s->filter_to_val = 0.0;
	s->filter_bypass_at_zero = false;
	s->filter_prop_valid = false;
	s->restore_persisted_pre_bypass = false;
	s->audio_follows_video = false;
	mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

size_t mfd_engine_find_addr_owners(struct mfd_engine *eng, const struct mfd_midi_addr *addr, int exclude_slot,
				   int *out_slots, size_t max_slots)
{
	if (!eng || !addr || addr->type == MFD_MIDI_NONE) {
		return 0U;
	}
	mfd_engine_lock(eng);
	size_t found = 0U;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (i == exclude_slot) {
			continue;
		}
		if (midi_addr_exact_equal(&eng->fader_slots[i].addr, addr)) {
			if (out_slots && found < max_slots) {
				out_slots[found] = i;
			}
			++found;
		}
	}
	mfd_engine_unlock(eng);
	return found;
}

bool mfd_engine_validate_setup_candidate(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					 char *out_reason, size_t reason_size)
{
	if (out_reason && reason_size > 0U) {
		out_reason[0] = '\0';
	}
	if (!eng || !cand || cand->slot_index < 0 || cand->slot_index >= MFD_MAX_FADER_SLOTS) {
		if (out_reason && reason_size > 0U) {
			snprintf(out_reason, reason_size, "Invalid strip index.");
		}
		return false;
	}
	if (!cand->enabled || cand->target_type == MFD_TARGET_UNASSIGNED) {
		return true;
	}

	if (cand->target_type == MFD_TARGET_TWO_SOURCE_MIX) {
		if (cand->scene_name[0] == '\0') {
			if (out_reason && reason_size > 0U) {
				snprintf(out_reason, reason_size, "Choose a scene with at least two layers.");
			}
			return false;
		}
		if (cand->source_a_name[0] == '\0' || cand->source_b_name[0] == '\0') {
			if (out_reason && reason_size > 0U) {
				snprintf(out_reason, reason_size, "Not used. Click to set up.");
			}
			return false;
		}
		if (strcmp(cand->source_a_name, cand->source_b_name) == 0) {
			if (out_reason && reason_size > 0U) {
				snprintf(out_reason, reason_size, "Left and Right cannot be the same layer.");
			}
			return false;
		}
	} else if (cand->target_type == MFD_TARGET_SINGLE_LAYER) {
		if (cand->scene_name[0] == '\0') {
			if (out_reason && reason_size > 0U) {
				snprintf(out_reason, reason_size, "Choose a scene with at least one layer.");
			}
			return false;
		}
	} else if (cand->target_type == MFD_TARGET_FILTER_PARAM) {
		double fval = cand->filter_from_val;
		double tval = cand->filter_to_val;
		return mfd_engine_validate_filter_param_binding_locked(eng, cand->slot_index, cand->enabled,
								       cand->source_a_name, cand->filter_name,
								       cand->filter_prop, true, &fval, &tval, NULL,
								       out_reason, reason_size);
	}

	struct mfd_fader_slot tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.enabled = true;
	tmp.target_type = cand->target_type;
	snprintf(tmp.scene_name, sizeof(tmp.scene_name), "%s", cand->scene_name);
	snprintf(tmp.source_a_name, sizeof(tmp.source_a_name), "%s", cand->source_a_name);
	snprintf(tmp.source_b_name, sizeof(tmp.source_b_name), "%s", cand->source_b_name);
	tmp.curve = cand->curve;
	tmp.audio_follows_video = cand->audio_follows_video;

	bool avail = false;
	char rsn[160] = {0};
	evaluate_slot_preconditions_const(eng, &tmp, NULL, 0U, &avail, rsn, sizeof(rsn), NULL, 0U, NULL);
	if (!avail && out_reason && reason_size > 0U) {
		snprintf(out_reason, reason_size, "%s", rsn);
	}
	return avail;
}

bool mfd_engine_commit_setup_session(struct mfd_engine *eng, const struct mfd_setup_candidate *cand, char *out_err,
				     size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	if (!eng || !cand || cand->slot_index < 0 || cand->slot_index >= MFD_MAX_FADER_SLOTS) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Invalid strip index.");
		}
		return false;
	}

	mfd_engine_lock(eng);
	if (!eng->setup_session.active || eng->setup_session.slot_index != cand->slot_index) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Setup session is no longer active for this strip.");
		}
		mfd_engine_unlock(eng);
		return false;
	}

	if (eng->slot_config_rev[cand->slot_index] != eng->setup_session.base_slot_rev) {
		eng->setup_session.base_slot_rev = eng->slot_config_rev[cand->slot_index];
		if (out_err && err_size > 0U) {
			snprintf(
				out_err, err_size,
				"This strip was changed externally while Set up was open. Review and press Done again.");
		}
		mfd_engine_mark_ui_dirty(eng, true);
		mfd_engine_unlock(eng);
		return false;
	}

	char rsn[160] = {0};
	if (!mfd_engine_validate_setup_candidate(eng, cand, rsn, sizeof(rsn))) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "%s", rsn);
		}
		mfd_engine_unlock(eng);
		return false;
	}

	if (eng->setup_session.learning && eng->learning_slot_index < 0) {
		mfd_midi_parser_cancel_learn(&eng->midi_parser);
	}

	struct mfd_midi_addr chosen_addr = eng->setup_session.candidate_addr;
	struct mfd_control_calibration chosen_cal = eng->setup_session.candidate_cal;
	bool chosen_invert = eng->setup_session.candidate_invert;
	bool had_cand_input = eng->setup_session.has_candidate_input;
	float cand_raw = eng->setup_session.candidate_raw_norm;

	if (chosen_addr.type != MFD_MIDI_NONE) {
		for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
			if (j != cand->slot_index && midi_addr_exact_equal(&eng->fader_slots[j].addr, &chosen_addr)) {
				eng->fader_slots[j].addr.type = MFD_MIDI_NONE;
				mfd_engine_bump_slot_rev(eng, j);
			}
		}
	}

	if (cand->apply_tbar_bidir) {
		mfd_engine_set_tbar_bidirectional_flip(eng, cand->tbar_bidirectional_flip);
	}

	mfd_engine_detach_filter_binding_locked(eng, cand->slot_index);

	struct mfd_fader_slot *s = &eng->fader_slots[cand->slot_index];
	s->enabled = cand->enabled;
	s->addr = chosen_addr;
	s->cal = chosen_cal;
	store_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
	s->invert = chosen_invert;
	snprintf(s->scene_name, sizeof(s->scene_name), "%s", cand->scene_name);
	if (cand->scene_name[0] == '\0') {
		s->resolved_scene_name[0] = '\0';
	}
	snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", cand->source_a_name);
	snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", cand->source_b_name);
	if (cand->target_type == MFD_TARGET_FILTER_PARAM) {
		snprintf(s->filter_name, sizeof(s->filter_name), "%s", cand->filter_name);
		snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", cand->filter_prop);
		s->filter_from_val = cand->filter_from_val;
		s->filter_to_val = cand->filter_to_val;
		s->filter_bypass_at_zero = cand->filter_bypass_at_zero;
		s->restore_persisted_pre_bypass = false;
	} else {
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
	}
	s->curve = cand->curve;
	s->audio_follows_video = cand->audio_follows_video;
	if (had_cand_input) {
		s->last_raw_norm = cand_raw;
		s->takeover.has_last_fader = true;
	}

	/* Ensure mode state resets cleanly even when re-committing the same target_type with new layers */
	enum mfd_target_type force_prev = (cand->target_type == MFD_TARGET_UNASSIGNED) ? MFD_TARGET_TBAR
										       : MFD_TARGET_UNASSIGNED;
	s->target_type = force_prev;
	mfd_engine_reset_slot_mode_state(eng, s, cand->target_type);

	memset(&eng->setup_session, 0, sizeof(eng->setup_session));
	mfd_engine_bump_slot_rev(eng, cand->slot_index);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

void mfd_engine_accept_first_run_usual_layout(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	mfd_engine_cancel_setup_session(eng);
	mfd_engine_load_apc40_defaults(eng);

	struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
	if (cat) {
		mfd_engine_get_ui_catalog(eng, cat);
		for (size_t i = 0U; i < cat->audio_count; ++i) {
			if (ascii_stricmp(cat->audio_sources[i].name, "Desktop Audio") == 0) {
				struct mfd_fader_slot *master = &eng->fader_slots[9];
				master->enabled = true;
				snprintf(master->source_a_name, sizeof(master->source_a_name), "%s",
					 cat->audio_sources[i].name);
				master->source_b_name[0] = '\0';
				master->scene_name[0] = '\0';
				mfd_engine_reset_slot_mode_state(eng, master, MFD_TARGET_AUDIO_VOLUME);
				break;
			}
		}
		mfd_ui_catalog_destroy(cat);
	}

	eng->first_run_offer_dismissed = true;
	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_decline_first_run_usual_layout(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	mfd_engine_cancel_setup_session(eng);
	mfd_engine_load_apc40_defaults(eng);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		mfd_engine_detach_filter_binding_locked(eng, i);
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		s->scene_name[0] = '\0';
		s->resolved_scene_name[0] = '\0';
		s->source_a_name[0] = '\0';
		s->source_b_name[0] = '\0';
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
		mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
	}
	eng->first_run_offer_dismissed = true;
	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

/* --- Typed UI & Advanced-Tab Operations --- */

bool mfd_engine_update_slot_config_ex(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit,
				      char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	if (!eng || !edit || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Invalid strip index.");
		}
		return false;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];

	bool same_filter_binding =
		(s->target_type == MFD_TARGET_FILTER_PARAM && edit->target_type == MFD_TARGET_FILTER_PARAM &&
		 strcmp(s->source_a_name, edit->source_a_name) == 0 && strcmp(s->filter_name, edit->filter_name) == 0 &&
		 strcmp(s->filter_prop, edit->filter_prop) == 0);
	double cand_from = edit->has_filter_range ? edit->filter_from_val : s->filter_from_val;
	double cand_to = edit->has_filter_range ? edit->filter_to_val : s->filter_to_val;

	if (edit->target_type == MFD_TARGET_FILTER_PARAM &&
	    (edit->source_a_name[0] != '\0' || edit->filter_name[0] != '\0' || edit->filter_prop[0] != '\0')) {
		bool full_binding_fields = (edit->source_a_name[0] != '\0' && edit->filter_name[0] != '\0' &&
					    edit->filter_prop[0] != '\0');
		if (edit->enabled && full_binding_fields) {
			bool live_exists = mfd_engine_source_exists(eng, edit->source_a_name) &&
					   sg_filter_exists(eng, edit->source_a_name, edit->filter_name, NULL);
			if (!same_filter_binding || edit->has_filter_range || live_exists) {
				bool has_ft = edit->has_filter_range || same_filter_binding;
				if (!mfd_engine_validate_filter_param_binding_locked(
					    eng, slot_index, true, edit->source_a_name, edit->filter_name,
					    edit->filter_prop, has_ft, &cand_from, &cand_to, NULL, out_err, err_size)) {
					mfd_engine_unlock(eng);
					return false;
				}
			} else {
				/* Offline bound target: still enforce reserved filter & single-writer uniqueness */
				if (mfd_is_internal_filter(edit->filter_name, NULL)) {
					if (out_err && err_size > 0U) {
						snprintf(
							out_err, err_size,
							"Filter '%s' is reserved by Midi Fadester and cannot be bound directly.",
							edit->filter_name);
					}
					mfd_engine_unlock(eng);
					return false;
				}
				for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
					if (j == slot_index) {
						continue;
					}
					const struct mfd_fader_slot *other = &eng->fader_slots[j];
					if (other->enabled && other->target_type == MFD_TARGET_FILTER_PARAM &&
					    strcmp(other->source_a_name, edit->source_a_name) == 0 &&
					    strcmp(other->filter_name, edit->filter_name) == 0 &&
					    strcmp(other->filter_prop, edit->filter_prop) == 0) {
						if (out_err && err_size > 0U) {
							snprintf(out_err, err_size,
								 "Setting '%s' on '%s' is already controlled by %s.",
								 edit->filter_prop, edit->filter_name,
								 (other->label[0] != '\0') ? other->label : other->id);
						}
						mfd_engine_unlock(eng);
						return false;
					}
				}
			}
		} else if (edit->enabled && (!full_binding_fields && edit->has_filter_range)) {
			if (!mfd_engine_validate_filter_param_binding_locked(
				    eng, slot_index, true, edit->source_a_name, edit->filter_name, edit->filter_prop,
				    true, &cand_from, &cand_to, NULL, out_err, err_size)) {
				mfd_engine_unlock(eng);
				return false;
			}
		}
	}

	mfd_engine_detach_filter_binding_locked(eng, slot_index);

	s->enabled = edit->enabled;
	bool invert_changed = (s->invert != edit->invert);
	s->invert = edit->invert;
	if (invert_changed && s->takeover.has_last_fader) {
		s->last_raw_norm = 1.0f - s->last_raw_norm;
	}

	if (!edit->preserve_scene) {
		snprintf(s->scene_name, sizeof(s->scene_name), "%s", edit->scene_name);
		if (edit->scene_name[0] == '\0') {
			s->resolved_scene_name[0] = '\0';
		}
	} else if (edit->target_type == MFD_TARGET_TBAR || edit->target_type == MFD_TARGET_AUDIO_VOLUME ||
		   edit->target_type == MFD_TARGET_FILTER_PARAM || edit->target_type == MFD_TARGET_UNASSIGNED) {
		s->scene_name[0] = '\0';
		s->resolved_scene_name[0] = '\0';
	} else if (s->scene_name[0] != '\0') {
		/* If a source changed and is not in the previously pinned scene, clear pin so auto-resolve works */
		if (edit->source_a_name[0] != '\0' &&
		    sg_scene_item_stack_index(eng, s->scene_name, edit->source_a_name) < 0) {
			s->scene_name[0] = '\0';
			s->resolved_scene_name[0] = '\0';
		} else if (edit->target_type == MFD_TARGET_TWO_SOURCE_MIX && edit->source_b_name[0] != '\0' &&
			   sg_scene_item_stack_index(eng, s->scene_name, edit->source_b_name) < 0) {
			s->scene_name[0] = '\0';
			s->resolved_scene_name[0] = '\0';
		}
	}

	bool filter_range_changed = false;
	snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", edit->source_a_name);
	snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", edit->source_b_name);
	if (edit->target_type == MFD_TARGET_FILTER_PARAM) {
		filter_range_changed = !same_filter_binding || (fabs(s->filter_from_val - cand_from) > 1e-9) ||
				       (fabs(s->filter_to_val - cand_to) > 1e-9);
		snprintf(s->filter_name, sizeof(s->filter_name), "%s", edit->filter_name);
		snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", edit->filter_prop);
		s->filter_from_val = cand_from;
		s->filter_to_val = cand_to;
		s->filter_bypass_at_zero = edit->filter_bypass_at_zero;
		s->restore_persisted_pre_bypass = false;
	} else {
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
	}
	s->curve = edit->curve;
	s->audio_follows_video = edit->audio_follows_video;
	if (edit->target_type == MFD_TARGET_FILTER_PARAM && filter_range_changed) {
		s->target_type = MFD_TARGET_UNASSIGNED;
	}
	mfd_engine_reset_slot_mode_state(eng, s, edit->target_type);
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

bool mfd_engine_update_slot_config(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit)
{
	return mfd_engine_update_slot_config_ex(eng, slot_index, edit, NULL, 0U);
}

void mfd_engine_set_slot_label(struct mfd_engine *eng, int slot_index, const char *label)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (label && label[0] != '\0') {
		snprintf(s->label, sizeof(s->label), "%s", label);
	} else {
		const struct mfd_controller_profile_def *def = mfd_profile_get_def(mfd_engine_get_active_profile(eng));
		if (def && (size_t)slot_index < MFD_APC40_DEFAULT_SLOT_COUNT && def->slots[slot_index].label &&
		    def->slots[slot_index].label[0] != '\0') {
			snprintf(s->label, sizeof(s->label), "%s", def->slots[slot_index].label);
		} else if (mfd_slot_is_fx_bank(slot_index)) {
			snprintf(s->label, sizeof(s->label), "FX %d", slot_index - 9);
		} else {
			snprintf(s->label, sizeof(s->label), "Fader %d", slot_index + 1);
		}
	}
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_clear_slot_binding(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->learning_slot_index == slot_index) {
		mfd_engine_cancel_learn(eng);
	}
	eng->fader_slots[slot_index].addr.type = MFD_MIDI_NONE;
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

bool mfd_engine_set_slot_value(struct mfd_engine *eng, int slot_index, float value_01, bool release, const char *origin,
			       char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Invalid strip index.");
		}
		return false;
	}

	mfd_engine_lock(eng);
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	if (eng->setup_session.active && eng->setup_session.slot_index == slot_index) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Strip is currently paused while Set up is open.");
		}
		mfd_engine_unlock(eng);
		return false;
	}
	if (!mfd_engine_is_slot_writable(eng, slot)) {
		bool avail = false;
		char rsn[160] = {0};
		mfd_engine_get_slot_status(eng, slot, NULL, 0U, &avail, rsn, sizeof(rsn));
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "%s", rsn);
		}
		mfd_engine_unlock(eng);
		return false;
	}

	float val = clamp01(value_01);
	const char *orig = (origin && *origin) ? origin : "api";
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
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		} else if (mfd_tbar_is_in_origin_rail_zone(val, false)) {
			if (slot->tbar_state == MFD_TBAR_THROWING) {
				mfd_engine_apply_tbar_abort(eng, slot);
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				mfd_engine_notify_state_changed(eng);
			} else if (was_parked) {
				mfd_engine_unlatch_tbar(eng);
				slot->tbar_state = MFD_TBAR_IDLE;
				mfd_takeover_unpark_from_api(&slot->takeover, 0.0f);
				mfd_smoother_reset(&slot->smoother, 0.0f);
				slot->last_applied_val = 0.0f;
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
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
			mfd_engine_notify_fader_changed(eng, slot_index, orig);
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
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				mfd_engine_notify_state_changed(eng);
			}
		} else {
			mfd_takeover_unpark_from_api(&slot->takeover, val);
			mfd_smoother_reset(&slot->smoother, val);
			mfd_engine_apply_slot_output_ex(eng, slot, val, false);
			mfd_engine_notify_fader_changed(eng, slot_index, orig);
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	}
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
	return true;
}

bool mfd_engine_finish_tbar_throw(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return false;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (!s->enabled || s->target_type != MFD_TARGET_TBAR || s->tbar_state != MFD_TBAR_THROWING ||
	    s->takeover.state == MFD_TAKEOVER_PARKED || !mfd_engine_is_slot_writable(eng, s)) {
		mfd_engine_unlock(eng);
		return false;
	}
	s->auto_fade_active = false;
	s->tbar_midi_active = false;
	s->tbar_from_idle_band = false;
	mfd_smoother_reset(&s->smoother, 1.0f);
	mfd_engine_apply_slot_output_ex(eng, s, 1.0f, true);
	mfd_engine_notify_fader_changed(eng, slot_index, "ui");
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
	return true;
}

static void apply_source_opacity_and_afv(const struct mfd_engine *eng, const char *source_name, float alpha_01,
					 bool afv)
{
	if (!source_name || source_name[0] == '\0') {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->apply_opacity) {
		eng->scene_graph_ops->apply_opacity(source_name, alpha_01, afv, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return;
	}

	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return;
	}

	obs_source_t *filter = mfd_ensure_layer_opacity_filter(src);
	if (filter) {
		mfd_layer_filter_set_opacity(filter, alpha_01);
		obs_source_release(filter);
	}

	if (afv) {
		uint32_t flags = obs_source_get_output_flags(src);
		if ((flags & OBS_SOURCE_AUDIO) != 0U) {
			obs_source_set_volume(src, mfd_deflection_to_mul(alpha_01));
		}
	}

	obs_source_release(src);
}

static void apply_tbar_slot_step(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val,
				 bool force_complete, bool force_abort)
{
	if (!eng || !slot || !slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
		return;
	}
	if (slot->tbar_state == MFD_TBAR_PARKED || slot->takeover.state == MFD_TAKEOVER_PARKED) {
		return;
	}

	float driver_val = force_complete ? 1.0f : (force_abort ? 0.0f : clamp01(norm_val));
	bool in_target = slot->tbar_midi_active ? mfd_tbar_is_in_target_rail_zone(slot->last_raw_norm,
										  slot->takeover.polarity_reversed)
						: mfd_tbar_is_in_target_rail_zone(driver_val, false);
	bool in_origin = slot->tbar_midi_active ? mfd_tbar_is_in_origin_rail_zone(slot->last_raw_norm,
										  slot->takeover.polarity_reversed)
						: mfd_tbar_is_in_origin_rail_zone(driver_val, false);
	bool in_start_deadband =
		(slot->tbar_midi_active && slot->tbar_state == MFD_TBAR_IDLE)
			? !mfd_tbar_is_past_start_zone(slot->last_raw_norm, slot->takeover.polarity_reversed)
			: false;

	enum mfd_tbar_state next_state = slot->tbar_state;
	bool completed = false;
	bool aborted = false;

	if (eng->tbar_driver) {
		char err_buf[256] = {0};
		bool ok = eng->tbar_driver(slot->tbar_state, driver_val, force_complete, force_abort, in_target,
					   in_origin, in_start_deadband, &next_state, &completed, &aborted, err_buf,
					   sizeof(err_buf));
		if (!ok && err_buf[0] != '\0') {
			snprintf(eng->tbar_error, sizeof(eng->tbar_error), "%s", err_buf);
		} else if (ok) {
			eng->tbar_error[0] = '\0';
		}
	} else {
		struct mfd_tbar_release_input in;
		memset(&in, 0, sizeof(in));
		in.studio_mode_active = true;
		in.slider_tbar_found = true;
		in.state = slot->tbar_state;
		in.release_latched = false;
		in.force_complete = force_complete;
		in.force_abort = force_abort;
		in.in_target_rail_zone = in_target;
		in.in_origin_rail_zone = in_origin;
		in.in_start_deadband = in_start_deadband;
		in.t_norm = driver_val;
		in.last_pos_1024 = -1;
		struct mfd_tbar_action act;
		mfd_tbar_compute_action_full(&in, &act);
		next_state = act.next_state;
		completed = act.completed_throw;
		aborted = act.aborted_throw;
	}

	if (completed || force_complete) {
		++slot->output_write_count;
		slot->tbar_state = MFD_TBAR_PARKED;
		mfd_takeover_notify_tbar_completed(&slot->takeover);
		mfd_smoother_reset(&slot->smoother, 0.0f);
		slot->last_applied_val = 0.0f;
		slot->tbar_midi_active = false;
		slot->tbar_from_idle_band = false;
	} else if (aborted || force_abort) {
		++slot->output_write_count;
		slot->tbar_state = MFD_TBAR_IDLE;
		mfd_smoother_reset(&slot->smoother, 0.0f);
		slot->last_applied_val = 0.0f;
		slot->tbar_midi_active = false;
		slot->tbar_from_idle_band = false;
		slot->takeover.target_obs = 0.0f;
		slot->takeover.last_fader = 0.0f;
		slot->takeover.rescale_origin = slot->takeover.polarity_reversed ? 1.0f : 0.0f;
		slot->takeover.has_throw_extreme = false;
	} else if (next_state == MFD_TBAR_IDLE) {
		slot->tbar_state = MFD_TBAR_IDLE;
		slot->tbar_from_idle_band = false;
		slot->last_applied_val = 0.0f;
	} else {
		++slot->output_write_count;
		slot->tbar_state = MFD_TBAR_THROWING;
		slot->last_applied_val = driver_val;
	}
}

void mfd_engine_apply_tbar_abort(struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	if (!eng || !slot || !slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
		return;
	}
	mfd_engine_lock(eng);
	if (slot->tbar_state != MFD_TBAR_THROWING) {
		slot->tbar_state = MFD_TBAR_IDLE;
		slot->tbar_from_idle_band = false;
		mfd_smoother_reset(&slot->smoother, 0.0f);
		slot->last_applied_val = 0.0f;
		mfd_engine_unlock(eng);
		return;
	}
	apply_tbar_slot_step(eng, slot, 0.0f, false, true);
	mfd_engine_unlock(eng);
}

static void apply_filter_param_output_locked(struct mfd_engine *eng, struct mfd_fader_slot *slot, float val)
{
	if (!eng || !slot || !slot->cached_available || !slot->filter_prop_valid) {
		return;
	}
	slot->last_applied_val = val;

	double mapped = mfd_filter_map_value(val, slot->filter_from_val, slot->filter_to_val, slot->filter_prop_min,
					     slot->filter_prop_max, slot->filter_prop_is_int);
	int64_t q_int = slot->filter_prop_is_int ? (int64_t)llround(mapped) : 0;
	double q_flt = mapped;
	bool should_write = mfd_filter_should_write_mapped(slot->has_last_written_mapped, slot->filter_prop_is_int,
							   slot->last_written_int, slot->last_written_float, mapped,
							   slot->filter_from_val, slot->filter_to_val, val);
	if (should_write) {
		obs_data_t *settings = sg_get_filter_settings(eng, slot->source_a_name, slot->filter_name);
		if (settings) {
			if (slot->filter_prop_is_int) {
				obs_data_set_int(settings, slot->filter_prop, (long long)q_int);
			} else {
				obs_data_set_double(settings, slot->filter_prop, q_flt);
			}
			slot->writing_filter_settings = true;
			sg_update_filter_settings(eng, slot->source_a_name, slot->filter_name, settings);
			slot->writing_filter_settings = false;
			obs_data_release(settings);
			slot->has_last_written_mapped = true;
			slot->last_written_int = q_int;
			slot->last_written_float = q_flt;
			++slot->output_write_count;
		}
	}

	sync_filter_bypass_lease_ex_locked(eng, slot->source_a_name, slot->filter_name, -1, true);
}

void mfd_engine_apply_slot_output_ex(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val,
				     bool force_release)
{
	if (!eng || !slot || !slot->enabled) {
		return;
	}
	mfd_engine_lock(eng);
	if (!mfd_engine_is_slot_writable(eng, slot)) {
		mfd_engine_unlock(eng);
		return;
	}

	float val = clamp01(norm_val);

	switch (slot->target_type) {
	case MFD_TARGET_UNASSIGNED:
		slot->last_applied_val = val;
		break;

	case MFD_TARGET_TBAR:
		apply_tbar_slot_step(eng, slot, val, force_release, false);
		break;

	case MFD_TARGET_SINGLE_LAYER:
		slot->last_applied_val = val;
		slot->last_alpha_a = val;
		slot->last_alpha_b = 0.0f;
		++slot->output_write_count;
		apply_source_opacity_and_afv(eng, slot->source_a_name, val, slot->audio_follows_video);
		break;

	case MFD_TARGET_TWO_SOURCE_MIX: {
		float alpha_a = 1.0f;
		float alpha_b = 0.0f;
		mfd_crossfade_eval_stacked(slot->curve, val, slot->a_is_above_b, &alpha_a, &alpha_b);
		slot->last_applied_val = val;
		slot->last_alpha_a = alpha_a;
		slot->last_alpha_b = alpha_b;
		++slot->output_write_count;
		apply_source_opacity_and_afv(eng, slot->source_a_name, alpha_a, slot->audio_follows_video);
		apply_source_opacity_and_afv(eng, slot->source_b_name, alpha_b, slot->audio_follows_video);
		break;
	}

	case MFD_TARGET_AUDIO_VOLUME:
		slot->last_applied_val = val;
		slot->last_audio_mul = mfd_deflection_to_mul(val);
		slot->last_audio_db = mfd_deflection_to_db(val);
		++slot->output_write_count;
		if (eng->scene_graph_ops && eng->scene_graph_ops->apply_volume && slot->source_a_name[0] != '\0') {
			eng->scene_graph_ops->apply_volume(slot->source_a_name, slot->last_audio_mul,
							   eng->scene_graph_user_data);
		}
		if (slot->source_a_name[0] != '\0' && obs_initialized()) {
			obs_source_t *src = obs_get_source_by_name(slot->source_a_name);
			if (src) {
				uint32_t flags = obs_source_get_output_flags(src);
				if ((flags & OBS_SOURCE_AUDIO) != 0U) {
					obs_source_set_volume(src, slot->last_audio_mul);
				}
				obs_source_release(src);
			}
		}
		break;

	case MFD_TARGET_FILTER_PARAM:
		apply_filter_param_output_locked(eng, slot, val);
		break;
	}
	mfd_engine_unlock(eng);
}

bool mfd_engine_reset_fx(struct mfd_engine *eng, int ms, int *out_eligible_count, int *out_reset_count,
			 int *out_queued_unavailable_count, char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	if (out_eligible_count) {
		*out_eligible_count = 0;
	}
	if (out_reset_count) {
		*out_reset_count = 0;
	}
	if (out_queued_unavailable_count) {
		*out_queued_unavailable_count = 0;
	}
	if (!eng) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Engine is not initialized.");
		}
		return false;
	}
	if (ms != 0 && (ms < 50 || ms > 20000)) {
		if (out_err && err_size > 0U) {
			snprintf(out_err, err_size, "Duration 'ms' must be 0 (instant) or between 50 and 20000 ms.");
		}
		return false;
	}

	mfd_engine_lock(eng);
	int eligible = 0;
	int reset_cnt = 0;
	int queued_cnt = 0;

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled || slot->target_type == MFD_TARGET_UNASSIGNED ||
		    slot->target_type == MFD_TARGET_TBAR) {
			continue;
		}
		if (!mfd_slot_is_fx_bank(i) && slot->target_type != MFD_TARGET_FILTER_PARAM) {
			continue;
		}
		++eligible;

		bool paused_in_setup = (eng->setup_session.active && eng->setup_session.slot_index == i);
		if (paused_in_setup || !mfd_engine_is_slot_writable(eng, slot)) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->has_last_written_mapped = false;
			slot->pending_clean_apply_on_avail = true;
			slot->last_applied_val = 0.0f;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			mfd_engine_notify_fader_changed(eng, i, "api");
			++queued_cnt;
			continue;
		}

		slot->pending_clean_apply_on_avail = false;
		if (ms == 0 || slot->last_applied_val <= 0.0005f) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->has_last_written_mapped = false;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_engine_apply_slot_output_ex(eng, slot, 0.0f, false);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			mfd_engine_notify_fader_changed(eng, i, "api");
			++reset_cnt;
		} else {
			slot->auto_fade_from = slot->last_applied_val;
			slot->auto_fade_to = 0.0f;
			slot->auto_fade_duration_sec = (float)ms / 1000.0f;
			slot->auto_fade_elapsed_sec = 0.0f;
			slot->auto_fade_active = true;
			slot->auto_fade_is_reset_fx = true;
			if (slot->takeover.state == MFD_TAKEOVER_PARKED) {
				mfd_takeover_unpark_from_api(&slot->takeover, slot->auto_fade_from);
			}
			++reset_cnt;
		}
	}

	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_notify_state_changed(eng);
	if (out_eligible_count) {
		*out_eligible_count = eligible;
	}
	if (out_reset_count) {
		*out_reset_count = reset_cnt;
	}
	if (out_queued_unavailable_count) {
		*out_queued_unavailable_count = queued_cnt;
	}
	mfd_engine_unlock(eng);
	return true;
}

void mfd_engine_apply_slot_output(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val)
{
	mfd_engine_apply_slot_output_ex(eng, slot, norm_val, false);
}

void mfd_engine_on_external_tbar_reset(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	bool changed = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
			continue;
		}
		if (slot->tbar_state == MFD_TBAR_THROWING) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->pending_clean_apply_on_avail = false;
			slot->tbar_state = MFD_TBAR_IDLE;
			slot->tbar_midi_active = false;
			slot->tbar_from_idle_band = false;
			slot->last_applied_val = 0.0f;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			changed = true;
		}
	}
	if (changed) {
		mfd_engine_unlatch_tbar(eng);
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
}

static float normalize_decoded_sample(struct mfd_engine *eng, struct mfd_midi_addr *slot_addr,
				      struct mfd_control_calibration *cal, bool invert,
				      const struct mfd_midi_decoded *dec)
{
	if (!eng || !slot_addr || !cal || !dec) {
		return 0.0f;
	}
	if (dec->addr.type == MFD_MIDI_CC || dec->addr.type == MFD_MIDI_PITCH_BEND) {
		if (dec->addr.type == MFD_MIDI_CC && dec->addr.is_14bit && !slot_addr->is_14bit) {
			int prev_min = cal->cal_min;
			int prev_max = cal->cal_max;
			slot_addr->is_14bit = true;
			if (!mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), slot_addr,
								     cal)) {
				if (prev_min == MFD_CAL_DEFAULT_MIN && prev_max == MFD_CAL_DEFAULT_MAX_7BIT) {
					mfd_calibration_init(cal, true);
				} else {
					mfd_calibration_set(cal, true, prev_min * 128, (prev_max * 128) + 127);
				}
			}
		}
		if (mfd_calibration_observe_and_widen(cal, (int)dec->raw_value)) {
			store_persisted_calibration(eng, effective_profile_name(eng), slot_addr, cal);
			if (obs_initialized()) {
				eng->pending_config_save = true;
			}
			mfd_engine_notify_state_changed(eng);
		}
		return mfd_calibration_normalize(cal, (int)dec->raw_value, invert);
	}
	return invert ? (1.0f - dec->normalized) : dec->normalized;
}

void mfd_engine_tick(struct mfd_engine *eng, float seconds)
{
	if (!eng) {
		return;
	}

	mfd_engine_lock(eng);

	if (os_atomic_load_bool(&eng->midi_dev.device_closed_unexpectedly)) {
		mfd_engine_mark_ui_dirty(eng, true);
	}

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		eng->fader_slots[i].sec_since_last_fader_event += seconds;
	}

	struct mfd_midi_raw_event raw;
	while (mfd_midi_queue_pop(&eng->midi_queue, &raw)) {
		struct mfd_midi_decoded dec;
		if (!mfd_midi_parser_feed(&eng->midi_parser, raw.raw_msg, &dec)) {
			continue;
		}
		eng->last_midi_decoded = dec;
		eng->has_last_midi = true;
		mfd_engine_mark_ui_dirty(eng, false);

		struct mfd_midi_addr learned;
		if (mfd_midi_parser_pop_learned(&eng->midi_parser, &learned)) {
			if (eng->setup_session.active && eng->setup_session.learning) {
				eng->setup_session.candidate_addr = learned;
				mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), &learned,
									&eng->setup_session.candidate_cal);
				eng->setup_session.learning = false;
				mfd_engine_mark_ui_dirty(eng, true);
			} else if (eng->learning_slot_index >= 0 && eng->learning_slot_index < MFD_MAX_FADER_SLOTS) {
				int lidx = eng->learning_slot_index;
				struct mfd_fader_slot *ls = &eng->fader_slots[lidx];
				ls->addr = learned;
				mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), &ls->addr,
									&ls->cal);
				ls->enabled = true;
				eng->learning_slot_index = -1;
				mfd_engine_bump_slot_rev(eng, lidx);
				if (obs_initialized()) {
					eng->pending_config_save = true;
				}
				obs_log(LOG_INFO, "MIDI Learn bound slot '%s' to Ch %u Type %d Num %u", ls->label,
					(unsigned)(learned.channel + 1U), (int)learned.type, (unsigned)learned.number);
				mfd_engine_notify_state_changed(eng);
			}
			eng->learning_slot_index = -1;
		}

		if (dec.addr.type == MFD_MIDI_NOTE_OFF) {
			continue;
		}

		/* Track last-moved strip for Zone 3 status line */
		int matched_slot = -1;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			if (eng->fader_slots[i].enabled &&
			    mfd_midi_addr_matches(&eng->fader_slots[i].addr, &dec.addr)) {
				matched_slot = i;
				break;
			}
		}
		eng->last_moved_slot_index = matched_slot;

		/* Update Step 3 candidate meter if setup session is active */
		if (eng->setup_session.active && mfd_midi_addr_matches(&eng->setup_session.candidate_addr, &dec.addr)) {
			eng->setup_session.candidate_raw_norm = normalize_decoded_sample(
				eng, &eng->setup_session.candidate_addr, &eng->setup_session.candidate_cal,
				eng->setup_session.candidate_invert, &dec);
			eng->setup_session.has_candidate_input = true;
			mfd_engine_mark_ui_dirty(eng, false);
		}

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *slot = &eng->fader_slots[i];
			if (!slot->enabled) {
				continue;
			}
			if (!mfd_midi_addr_matches(&slot->addr, &dec.addr)) {
				continue;
			}

			float norm = normalize_decoded_sample(eng, &slot->addr, &slot->cal, slot->invert, &dec);
			slot->last_raw_norm = norm;
			mfd_engine_mark_ui_dirty(eng, false);

			if (eng->setup_session.active && eng->setup_session.slot_index == i) {
				/* Broadcast-safe preview: live OBS writes paused while Set up is editing this strip */
				continue;
			}
			if (!mfd_engine_is_slot_writable(eng, slot)) {
				continue;
			}

			float effective = slot->last_applied_val;
			bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
					  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);
			if (!mfd_takeover_step(&slot->takeover, norm, &effective)) {
				if (slot->target_type == MFD_TARGET_TBAR && was_parked &&
				    slot->takeover.state == MFD_TAKEOVER_WAITING) {
					slot->tbar_state = MFD_TBAR_IDLE;
					slot->tbar_midi_active = false;
					slot->tbar_from_idle_band = false;
					slot->last_applied_val = 0.0f;
					mfd_smoother_reset(&slot->smoother, 0.0f);
					mfd_engine_unlatch_tbar(eng);
					mfd_engine_notify_state_changed(eng);
				}
				continue;
			}

			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->pending_clean_apply_on_avail = false;

			if (slot->target_type == MFD_TARGET_TBAR) {
				slot->tbar_midi_active = true;
				if (was_parked && slot->takeover.state != MFD_TAKEOVER_PARKED) {
					mfd_engine_unlatch_tbar(eng);
					slot->tbar_state = MFD_TBAR_THROWING;
					slot->tbar_from_idle_band = false;
				}

				bool pol_rev = slot->takeover.polarity_reversed;
				bool in_target = mfd_tbar_is_in_target_rail_zone(norm, pol_rev);
				bool in_origin = mfd_tbar_is_in_origin_rail_zone(norm, pol_rev);
				bool past_start = mfd_tbar_is_past_start_zone(norm, pol_rev);

				if (in_target) {
					if (slot->tbar_state == MFD_TBAR_IDLE) {
						mfd_engine_unlatch_tbar(eng);
					}
					apply_tbar_slot_step(eng, slot, 1.0f, false, false);
					mfd_engine_notify_fader_changed(eng, i, "midi");
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				} else if (in_origin) {
					if (slot->tbar_state == MFD_TBAR_THROWING) {
						apply_tbar_slot_step(eng, slot, 0.0f, false, false);
						mfd_engine_notify_fader_changed(eng, i, "midi");
					} else {
						slot->tbar_state = MFD_TBAR_IDLE;
						slot->tbar_from_idle_band = false;
						mfd_smoother_reset(&slot->smoother, 0.0f);
						slot->last_applied_val = 0.0f;
					}
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				} else if (slot->tbar_state == MFD_TBAR_IDLE && !past_start) {
					/*
					 * 6% origin-rail start band (R-TBAR-SM / D-8):
					 * In Idle, positions inside (0.03, 0.06] normal (CC 4..7) or [0.94, 0.97)
					 * reversed remain Idle with zero OBS writes.
					 */
					slot->tbar_from_idle_band = false;
					mfd_smoother_reset(&slot->smoother, 0.0f);
					slot->last_applied_val = 0.0f;
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				} else {
					bool started_from_idle = (slot->tbar_state == MFD_TBAR_IDLE);
					if (started_from_idle) {
						mfd_engine_unlatch_tbar(eng);
						slot->tbar_from_idle_band = true;
					}
					float t_target = slot->tbar_from_idle_band
								 ? mfd_tbar_rescale_from_start_zone(norm, pol_rev)
								 : effective;
					mfd_smoother_set_target(&slot->smoother, t_target);
					if (started_from_idle || !mfd_smoother_is_active(&slot->smoother)) {
						apply_tbar_slot_step(eng, slot, slot->smoother.current, false, false);
						mfd_engine_notify_fader_changed(eng, i, "midi");
					}
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				}
			} else {
				mfd_smoother_set_target(&slot->smoother, effective);
				if (!mfd_smoother_is_active(&slot->smoother)) {
					mfd_engine_apply_slot_output(eng, slot, slot->smoother.current);
					mfd_engine_notify_fader_changed(eng, i, "midi");
				}
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		}
	}

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled) {
			continue;
		}
		if (eng->setup_session.active && eng->setup_session.slot_index == i) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			mfd_smoother_reset(&slot->smoother, slot->last_applied_val);
			continue;
		}
		if (!mfd_engine_is_slot_writable(eng, slot)) {
			if (slot->auto_fade_active && slot->auto_fade_is_reset_fx) {
				slot->pending_clean_apply_on_avail = true;
				slot->last_applied_val = 0.0f;
				mfd_takeover_sync_external(&slot->takeover, 0.0f);
			}
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			mfd_smoother_reset(&slot->smoother, slot->last_applied_val);
		} else if (slot->auto_fade_active) {
			slot->auto_fade_elapsed_sec += seconds;
			float progress = (slot->auto_fade_duration_sec > 0.0f)
						 ? clamp01(slot->auto_fade_elapsed_sec / slot->auto_fade_duration_sec)
						 : 1.0f;
			float v = slot->auto_fade_from + (slot->auto_fade_to - slot->auto_fade_from) * progress;
			mfd_smoother_reset(&slot->smoother, v);

			if (slot->target_type == MFD_TARGET_TBAR) {
				slot->tbar_midi_active = false;
				slot->tbar_from_idle_band = false;
				if (slot->auto_fade_to <= MFD_TBAR_RAIL_ZONE &&
				    (progress >= 1.0f || v <= MFD_TBAR_RAIL_ZONE)) {
					slot->auto_fade_active = false;
					slot->auto_fade_is_reset_fx = false;
					if (slot->tbar_state == MFD_TBAR_THROWING) {
						mfd_engine_apply_tbar_abort(eng, slot);
					} else {
						slot->tbar_state = MFD_TBAR_IDLE;
						slot->last_applied_val = 0.0f;
					}
					mfd_engine_notify_state_changed(eng);
				} else if (v >= (1.0f - MFD_TBAR_RAIL_ZONE - 1e-6f) ||
					   (progress >= 1.0f &&
					    slot->auto_fade_to >= (1.0f - MFD_TBAR_RAIL_ZONE - 1e-6f))) {
					if (slot->tbar_state == MFD_TBAR_IDLE) {
						mfd_engine_unlatch_tbar(eng);
					}
					mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
					slot->auto_fade_active = false;
					slot->auto_fade_is_reset_fx = false;
					mfd_engine_notify_state_changed(eng);
				} else {
					if (slot->tbar_state == MFD_TBAR_IDLE) {
						mfd_engine_unlatch_tbar(eng);
						slot->tbar_state = MFD_TBAR_THROWING;
					}
					mfd_engine_apply_slot_output(eng, slot, v);
					if (progress >= 1.0f) {
						slot->auto_fade_active = false;
						slot->auto_fade_is_reset_fx = false;
						mfd_takeover_unpark_from_api(&slot->takeover, v);
					}
				}
			} else {
				if (progress >= 1.0f) {
					bool was_reset_fx = slot->auto_fade_is_reset_fx;
					slot->auto_fade_active = false;
					slot->auto_fade_is_reset_fx = false;
					if (was_reset_fx) {
						slot->has_last_written_mapped = false;
						mfd_engine_apply_slot_output(eng, slot, 0.0f);
						mfd_smoother_reset(&slot->smoother, 0.0f);
						mfd_takeover_sync_external(&slot->takeover, 0.0f);
						mfd_engine_notify_state_changed(eng);
					} else if (slot->auto_fade_to >= MFD_TWO_RAIL_UPPER_PARK_THRESHOLD) {
						mfd_engine_apply_slot_output(eng, slot, slot->auto_fade_to);
						mfd_takeover_notify_rail_completed(&slot->takeover, true,
										   slot->auto_fade_to);
						mfd_smoother_reset(&slot->smoother, slot->auto_fade_to);
						mfd_engine_notify_state_changed(eng);
					} else if (slot->auto_fade_to <= MFD_TWO_RAIL_LOWER_PARK_THRESHOLD) {
						mfd_engine_apply_slot_output(eng, slot, slot->auto_fade_to);
						mfd_takeover_notify_rail_completed(&slot->takeover, false,
										   slot->auto_fade_to);
						mfd_smoother_reset(&slot->smoother, slot->auto_fade_to);
						mfd_engine_notify_state_changed(eng);
					} else {
						mfd_engine_apply_slot_output(eng, slot, v);
						mfd_takeover_unpark_from_api(&slot->takeover, v);
					}
				} else {
					mfd_engine_apply_slot_output(eng, slot, v);
				}
			}
			mfd_engine_notify_fader_changed(eng, i, "auto_fade");
		} else if (mfd_smoother_is_active(&slot->smoother)) {
			bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED);
			float smoothed = mfd_smoother_tick(&slot->smoother, seconds);
			mfd_engine_apply_slot_output(eng, slot, smoothed);
			if (!was_parked && slot->takeover.state == MFD_TAKEOVER_PARKED) {
				mfd_engine_notify_state_changed(eng);
			}
			mfd_engine_notify_fader_changed(eng, i, "midi");
		}

		if (slot->fader_event_pending && slot->sec_since_last_fader_event >= MFD_FADER_EVENT_MIN_INTERVAL_SEC) {
			slot->sec_since_last_fader_event = 0.0f;
			slot->fader_event_pending = false;
			eng->pending_fader_emits[i].pending = true;
			snprintf(eng->pending_fader_emits[i].slot_id, sizeof(eng->pending_fader_emits[i].slot_id), "%s",
				 slot->id);
			eng->pending_fader_emits[i].value = slot->last_applied_val;
			mfd_engine_format_slot_display_value(slot, slot->last_applied_val,
							     eng->pending_fader_emits[i].display_value,
							     sizeof(eng->pending_fader_emits[i].display_value));
			snprintf(eng->pending_fader_emits[i].origin, sizeof(eng->pending_fader_emits[i].origin), "%s",
				 slot->pending_origin);
		}
	}

	mfd_engine_unlock(eng);
}

static void mfd_engine_video_tick(void *param, float seconds)
{
	struct mfd_engine *eng = param;
	if (!eng || !eng->initialized) {
		return;
	}
	mfd_engine_tick(eng, seconds);
}

struct mfd_tracked_signal_entry {
	struct mfd_engine *eng;
	obs_source_t *key_ptr;
	obs_weak_source_t *weak_src;
	obs_weak_source_t *weak_parent;
	bool is_filter;
	bool is_scene;
	struct mfd_tracked_signal_entry *next;
};

static void on_obs_scene_signal(void *data, calldata_t *cd);
static void on_obs_host_filter_add(void *data, calldata_t *cd);
static void on_obs_host_filter_remove(void *data, calldata_t *cd);
static void on_obs_filter_rename(void *data, calldata_t *cd);
static void on_obs_filter_update_properties(void *data, calldata_t *cd);
static void on_obs_filter_enable(void *data, calldata_t *cd);
static void on_obs_filter_update(void *data, calldata_t *cd);

static struct mfd_tracked_signal_entry *find_tracked_signal_entry_locked(const struct mfd_engine *eng,
									 const obs_source_t *src)
{
	if (!eng || !src) {
		return NULL;
	}
	for (struct mfd_tracked_signal_entry *e = eng->tracked_signals_head; e != NULL; e = e->next) {
		if (e->key_ptr == src) {
			return e;
		}
	}
	return NULL;
}

static void disconnect_tracked_entry_signals(struct mfd_tracked_signal_entry *entry, signal_handler_t *sh)
{
	if (!entry || !sh) {
		return;
	}
	if (entry->is_filter) {
		signal_handler_disconnect(sh, "rename", on_obs_filter_rename, entry);
		signal_handler_disconnect(sh, "update_properties", on_obs_filter_update_properties, entry);
		signal_handler_disconnect(sh, "enable", on_obs_filter_enable, entry);
		signal_handler_disconnect(sh, "update", on_obs_filter_update, entry);
	} else {
		signal_handler_disconnect(sh, "filter_add", on_obs_host_filter_add, entry);
		signal_handler_disconnect(sh, "filter_remove", on_obs_host_filter_remove, entry);
		if (entry->is_scene) {
			signal_handler_disconnect(sh, "item_add", on_obs_scene_signal, entry);
			signal_handler_disconnect(sh, "item_remove", on_obs_scene_signal, entry);
			signal_handler_disconnect(sh, "reorder", on_obs_scene_signal, entry);
		}
	}
}

static void free_tracked_signal_entry(struct mfd_tracked_signal_entry *entry)
{
	if (!entry) {
		return;
	}
	if (entry->weak_src) {
		obs_weak_source_release(entry->weak_src);
	}
	if (entry->weak_parent) {
		obs_weak_source_release(entry->weak_parent);
	}
	free(entry);
}

static void remove_tracked_signal_by_ptr_locked(struct mfd_engine *eng, obs_source_t *src)
{
	if (!eng || !src) {
		return;
	}
	struct mfd_tracked_signal_entry **pp = &eng->tracked_signals_head;
	while (*pp != NULL) {
		struct mfd_tracked_signal_entry *cur = *pp;
		if (cur->key_ptr == src) {
			*pp = cur->next;
			disconnect_tracked_entry_signals(cur, obs_source_get_signal_handler(src));
			free_tracked_signal_entry(cur);
			continue;
		}
		if (cur->is_filter && cur->weak_parent && obs_weak_source_references_source(cur->weak_parent, src)) {
			*pp = cur->next;
			obs_source_t *flt = cur->weak_src ? obs_weak_source_get_source(cur->weak_src) : NULL;
			if (flt) {
				disconnect_tracked_entry_signals(cur, obs_source_get_signal_handler(flt));
				obs_source_release(flt);
			}
			free_tracked_signal_entry(cur);
			continue;
		}
		pp = &cur->next;
	}
}

static void disconnect_all_tracked_signals_locked(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	while (eng->tracked_signals_head != NULL) {
		struct mfd_tracked_signal_entry *cur = eng->tracked_signals_head;
		eng->tracked_signals_head = cur->next;
		if (obs_initialized() && cur->weak_src) {
			obs_source_t *live = obs_weak_source_get_source(cur->weak_src);
			if (live) {
				disconnect_tracked_entry_signals(cur, obs_source_get_signal_handler(live));
				obs_source_release(live);
			}
		}
		free_tracked_signal_entry(cur);
	}
}

static void connect_filter_source_signals_locked(struct mfd_engine *eng, obs_source_t *host_src, obs_source_t *flt)
{
	if (!eng || !host_src || !flt || obs_source_removed(host_src) || obs_source_removed(flt)) {
		return;
	}
	if (obs_source_get_type(flt) != OBS_SOURCE_TYPE_FILTER) {
		return;
	}
	const char *fname = obs_source_get_name(flt);
	const char *fid = obs_source_get_id(flt);
	if (mfd_is_internal_filter(fname, fid)) {
		return;
	}
	if (find_tracked_signal_entry_locked(eng, flt) != NULL) {
		return;
	}
	signal_handler_t *sh = obs_source_get_signal_handler(flt);
	if (!sh) {
		return;
	}
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)calloc(1U, sizeof(*entry));
	if (!entry) {
		return;
	}
	entry->eng = eng;
	entry->key_ptr = flt;
	entry->weak_src = obs_source_get_weak_source(flt);
	entry->weak_parent = obs_source_get_weak_source(host_src);
	entry->is_filter = true;
	entry->is_scene = false;
	entry->next = eng->tracked_signals_head;
	eng->tracked_signals_head = entry;

	signal_handler_connect(sh, "rename", on_obs_filter_rename, entry);
	signal_handler_connect(sh, "update_properties", on_obs_filter_update_properties, entry);
	signal_handler_connect(sh, "enable", on_obs_filter_enable, entry);
	signal_handler_connect(sh, "update", on_obs_filter_update, entry);
}

static void enum_connect_host_filters_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	connect_filter_source_signals_locked((struct mfd_engine *)param, parent, child);
}

static void connect_host_source_signals_locked(struct mfd_engine *eng, obs_source_t *src)
{
	if (!eng || !src || obs_source_removed(src)) {
		return;
	}
	enum obs_source_type stype = obs_source_get_type(src);
	if (stype != OBS_SOURCE_TYPE_INPUT && stype != OBS_SOURCE_TYPE_SCENE) {
		return;
	}
	if (is_internal_source_name(obs_source_get_name(src))) {
		return;
	}
	if (find_tracked_signal_entry_locked(eng, src) == NULL) {
		signal_handler_t *sh = obs_source_get_signal_handler(src);
		if (sh) {
			struct mfd_tracked_signal_entry *entry =
				(struct mfd_tracked_signal_entry *)calloc(1U, sizeof(*entry));
			if (entry) {
				entry->eng = eng;
				entry->key_ptr = src;
				entry->weak_src = obs_source_get_weak_source(src);
				entry->weak_parent = NULL;
				entry->is_filter = false;
				entry->is_scene = (stype == OBS_SOURCE_TYPE_SCENE) || obs_source_is_scene(src);
				entry->next = eng->tracked_signals_head;
				eng->tracked_signals_head = entry;

				signal_handler_connect(sh, "filter_add", on_obs_host_filter_add, entry);
				signal_handler_connect(sh, "filter_remove", on_obs_host_filter_remove, entry);
				if (entry->is_scene) {
					signal_handler_connect(sh, "item_add", on_obs_scene_signal, entry);
					signal_handler_connect(sh, "item_remove", on_obs_scene_signal, entry);
					signal_handler_connect(sh, "reorder", on_obs_scene_signal, entry);
				}
			}
		}
	}
	obs_source_enum_filters(src, enum_connect_host_filters_cb, eng);
}

static bool enum_connect_sources_cb(void *param, obs_source_t *src)
{
	connect_host_source_signals_locked((struct mfd_engine *)param, src);
	return true;
}

static void connect_all_live_source_and_filter_signals(struct mfd_engine *eng)
{
	if (!eng || !obs_initialized()) {
		return;
	}
	mfd_engine_lock(eng);
	obs_enum_sources(enum_connect_sources_cb, eng);
	obs_enum_scenes(enum_connect_sources_cb, eng);
	mfd_engine_unlock(eng);
}

static void on_obs_scene_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized) {
		return;
	}
	mfd_engine_reevaluate_slots(entry->eng, true);
}

static void on_obs_host_filter_add(void *data, calldata_t *cd)
{
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized || !cd) {
		return;
	}
	struct mfd_engine *eng = entry->eng;
	obs_source_t *host = (obs_source_t *)calldata_ptr(cd, "source");
	obs_source_t *flt = (obs_source_t *)calldata_ptr(cd, "filter");
	mfd_engine_lock(eng);
	if (host && flt) {
		if (mfd_is_internal_filter(obs_source_get_name(flt), obs_source_get_id(flt))) {
			mfd_engine_unlock(eng);
			return;
		}
		connect_filter_source_signals_locked(eng, host, flt);
	}
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

static void on_obs_host_filter_remove(void *data, calldata_t *cd)
{
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized || !cd) {
		return;
	}
	struct mfd_engine *eng = entry->eng;
	obs_source_t *host = (obs_source_t *)calldata_ptr(cd, "source");
	obs_source_t *flt = (obs_source_t *)calldata_ptr(cd, "filter");
	mfd_engine_lock(eng);
	if (flt) {
		remove_tracked_signal_by_ptr_locked(eng, flt);
	}
	const char *hname = host ? obs_source_get_name(host) : NULL;
	const char *fname = flt ? obs_source_get_name(flt) : NULL;
	if (flt && mfd_is_internal_filter(fname, obs_source_get_id(flt))) {
		mfd_engine_unlock(eng);
		return;
	}
	if (hname && hname[0] != '\0') {
		mfd_engine_handle_filter_removed(eng, hname, fname);
	} else {
		mfd_engine_reevaluate_slots(eng, true);
	}
	mfd_engine_unlock(eng);
}

static bool resolve_filter_entry_names(struct mfd_tracked_signal_entry *entry, char *out_parent, size_t parent_size,
				       char *out_filter, size_t filter_size)
{
	if (!entry || !entry->weak_src || !entry->weak_parent) {
		return false;
	}
	obs_source_t *flt = obs_weak_source_get_source(entry->weak_src);
	obs_source_t *parent = obs_weak_source_get_source(entry->weak_parent);
	bool ok = false;
	if (flt && parent && !obs_source_removed(flt) && !obs_source_removed(parent)) {
		const char *pname = obs_source_get_name(parent);
		const char *fname = obs_source_get_name(flt);
		if (pname && pname[0] != '\0' && fname && fname[0] != '\0') {
			snprintf(out_parent, parent_size, "%s", pname);
			snprintf(out_filter, filter_size, "%s", fname);
			ok = true;
		}
	}
	if (flt) {
		obs_source_release(flt);
	}
	if (parent) {
		obs_source_release(parent);
	}
	return ok;
}

static void on_obs_filter_rename(void *data, calldata_t *cd)
{
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized || !cd || !entry->weak_parent) {
		return;
	}
	const char *prev_name = calldata_string(cd, "prev_name");
	const char *new_name = calldata_string(cd, "new_name");
	obs_source_t *parent = obs_weak_source_get_source(entry->weak_parent);
	if (!parent) {
		return;
	}
	char pname[MFD_SOURCE_NAME_MAX] = {0};
	const char *raw_p = obs_source_get_name(parent);
	if (raw_p) {
		snprintf(pname, sizeof(pname), "%s", raw_p);
	}
	obs_source_release(parent);
	if (pname[0] != '\0') {
		mfd_engine_handle_filter_rename(entry->eng, pname, prev_name, new_name);
	}
}

static void on_obs_filter_update_properties(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized) {
		return;
	}
	char pname[MFD_SOURCE_NAME_MAX] = {0};
	char fname[MFD_SOURCE_NAME_MAX] = {0};
	if (resolve_filter_entry_names(entry, pname, sizeof(pname), fname, sizeof(fname))) {
		mfd_engine_handle_filter_properties_updated(entry->eng, pname, fname);
	} else {
		mfd_engine_reevaluate_slots(entry->eng, true);
	}
}

static void on_obs_filter_enable(void *data, calldata_t *cd)
{
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized || !cd) {
		return;
	}
	bool enabled = calldata_bool(cd, "enabled");
	char pname[MFD_SOURCE_NAME_MAX] = {0};
	char fname[MFD_SOURCE_NAME_MAX] = {0};
	if (resolve_filter_entry_names(entry, pname, sizeof(pname), fname, sizeof(fname))) {
		mfd_engine_handle_filter_enable(entry->eng, pname, fname, enabled);
	}
}

static void on_obs_filter_update(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)data;
	if (!entry || !entry->eng || !entry->eng->initialized) {
		return;
	}
	char pname[MFD_SOURCE_NAME_MAX] = {0};
	char fname[MFD_SOURCE_NAME_MAX] = {0};
	if (resolve_filter_entry_names(entry, pname, sizeof(pname), fname, sizeof(fname))) {
		mfd_engine_handle_filter_settings_updated(entry->eng, pname, fname);
	}
}

static void on_obs_source_create(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = (struct mfd_engine *)data;
	if (!eng || !eng->initialized) {
		return;
	}
	obs_source_t *src = cd ? (obs_source_t *)calldata_ptr(cd, "source") : NULL;
	if (!src) {
		return;
	}
	enum obs_source_type stype = obs_source_get_type(src);
	if (stype != OBS_SOURCE_TYPE_INPUT && stype != OBS_SOURCE_TYPE_SCENE) {
		return;
	}
	mfd_engine_lock(eng);
	connect_host_source_signals_locked(eng, src);
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

static void on_obs_source_remove_or_destroy(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = (struct mfd_engine *)data;
	if (!eng || !eng->initialized) {
		return;
	}
	obs_source_t *src = cd ? (obs_source_t *)calldata_ptr(cd, "source") : NULL;
	mfd_engine_lock(eng);
	bool should_reeval = true;
	if (src) {
		enum obs_source_type stype = obs_source_get_type(src);
		remove_tracked_signal_by_ptr_locked(eng, src);
		if (stype == OBS_SOURCE_TYPE_FILTER || stype == OBS_SOURCE_TYPE_TRANSITION) {
			should_reeval = false;
		} else {
			const char *sname = obs_source_get_name(src);
			if (sname && sname[0] != '\0') {
				invalidate_filter_leases_for_removed_locked(eng, sname, NULL);
			}
		}
	}
	if (should_reeval) {
		mfd_engine_reevaluate_slots(eng, true);
	}
	mfd_engine_unlock(eng);
}

static void on_obs_source_rename(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = (struct mfd_engine *)data;
	if (!eng || !eng->initialized || !cd) {
		return;
	}
	const char *prev_name = calldata_string(cd, "prev_name");
	const char *new_name = calldata_string(cd, "new_name");
	mfd_engine_handle_source_rename(eng, prev_name, new_name);
}

static void connect_obs_signals(struct mfd_engine *eng)
{
	if (!eng || eng->signals_connected || !obs_initialized()) {
		return;
	}
	mfd_engine_lock(eng);
	signal_handler_t *sh = obs_get_signal_handler();
	if (sh) {
		signal_handler_connect(sh, "source_create", on_obs_source_create, eng);
		signal_handler_connect(sh, "source_remove", on_obs_source_remove_or_destroy, eng);
		signal_handler_connect(sh, "source_destroy", on_obs_source_remove_or_destroy, eng);
		signal_handler_connect(sh, "source_rename", on_obs_source_rename, eng);
	}
	connect_all_live_source_and_filter_signals(eng);
	eng->signals_connected = true;
	mfd_engine_unlock(eng);
}

static void disconnect_obs_signals(struct mfd_engine *eng)
{
	if (!eng || !eng->signals_connected) {
		return;
	}
	mfd_engine_lock(eng);
	eng->signals_connected = false;
	if (obs_initialized()) {
		signal_handler_t *sh = obs_get_signal_handler();
		if (sh) {
			signal_handler_disconnect(sh, "source_create", on_obs_source_create, eng);
			signal_handler_disconnect(sh, "source_remove", on_obs_source_remove_or_destroy, eng);
			signal_handler_disconnect(sh, "source_destroy", on_obs_source_remove_or_destroy, eng);
			signal_handler_disconnect(sh, "source_rename", on_obs_source_rename, eng);
		}
	}
	disconnect_all_tracked_signals_locked(eng);
	mfd_engine_unlock(eng);
}

static void sync_slots_on_scene_change(struct mfd_engine *eng, bool reset_tbar)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	refresh_frontend_cache_on_ui_thread_locked(eng);
	if (reset_tbar) {
		mfd_engine_unlatch_tbar(eng);
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled) {
			continue;
		}
		if (slot->target_type == MFD_TARGET_TBAR) {
			if (reset_tbar) {
				slot->auto_fade_active = false;
				slot->auto_fade_is_reset_fx = false;
				slot->pending_clean_apply_on_avail = false;
				slot->tbar_state = MFD_TBAR_IDLE;
				slot->tbar_midi_active = false;
				slot->tbar_from_idle_band = false;
				slot->last_applied_val = 0.0f;
				mfd_smoother_reset(&slot->smoother, 0.0f);
				mfd_takeover_sync_external(&slot->takeover, 0.0f);
			}
		} else if (slot->target_type == MFD_TARGET_AUDIO_VOLUME && slot->source_a_name[0] != '\0' &&
			   obs_initialized()) {
			obs_source_t *src = obs_get_source_by_name(slot->source_a_name);
			if (src) {
				float mul = obs_source_get_volume(src);
				mfd_takeover_sync_external(&slot->takeover, mfd_mul_to_deflection(mul));
				obs_source_release(src);
			}
		}
	}
	mfd_engine_reevaluate_slots(eng, false);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM && slot->cached_available &&
		    !slot->pending_clean_apply_on_avail && !slot->auto_fade_active) {
			float t_live = query_live_filter_unmapped_deflection_locked(eng, slot);
			slot->last_applied_val = t_live;
			slot->has_last_written_mapped = false;
			mfd_smoother_reset(&slot->smoother, t_live);
			mfd_takeover_sync_external(&slot->takeover, t_live);
		}
	}
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	struct mfd_engine *eng = private_data;
	if (!eng) {
		return;
	}

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		mfd_engine_lock(eng);
		eng->frontend_finished_loading = true;
		refresh_apc40_detected_locked(eng);
		connect_all_live_source_and_filter_signals(eng);
		sync_slots_on_scene_change(eng, false);
		mfd_engine_unlock(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		mfd_engine_lock(eng);
		memset(eng->filter_leases, 0, sizeof(eng->filter_leases));
		disconnect_all_tracked_signals_locked(eng);
		connect_all_live_source_and_filter_signals(eng);
		sync_slots_on_scene_change(eng, false);
		mfd_engine_unlock(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		mfd_engine_lock(eng);
		connect_all_live_source_and_filter_signals(eng);
		sync_slots_on_scene_change(eng, false);
		mfd_engine_unlock(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		sync_slots_on_scene_change(eng, false);
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		sync_slots_on_scene_change(eng, true);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		mfd_engine_lock(eng);
		mfd_engine_restore_all_bypass_leases(eng);
		disconnect_obs_signals(eng);
		mfd_engine_unlock(eng);
		break;
	default:
		break;
	}
}

static void serialize_calibrations(const struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_CAL_ENTRIES; ++i) {
		const struct mfd_persisted_cal_entry *e = &eng->cal_entries[i];
		if (!e->in_use) {
			continue;
		}
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "profile", e->profile_name);
		obs_data_set_int(item, "midi_type", (int)e->addr.type);
		obs_data_set_int(item, "midi_channel", (int)e->addr.channel);
		obs_data_set_int(item, "midi_number", (int)e->addr.number);
		obs_data_set_bool(item, "midi_14bit", e->addr.is_14bit);
		obs_data_set_int(item, "cal_min", e->cal.cal_min);
		obs_data_set_int(item, "cal_max", e->cal.cal_max);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "control_calibrations", arr);
	obs_data_array_release(arr);
}

static void deserialize_calibrations(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_get_array(root, "control_calibrations");
	if (!arr) {
		return;
	}
	size_t count = obs_data_array_count(arr);
	for (size_t i = 0U; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		if (!item) {
			continue;
		}
		const char *prof = obs_data_get_string(item, "profile");
		struct mfd_midi_addr addr;
		memset(&addr, 0, sizeof(addr));
		addr.type = (enum mfd_midi_msg_type)obs_data_get_int(item, "midi_type");
		addr.channel = (uint8_t)(obs_data_get_int(item, "midi_channel") & 0x0F);
		addr.number = (uint8_t)(obs_data_get_int(item, "midi_number") & 0x7F);
		addr.is_14bit = obs_data_get_bool(item, "midi_14bit");
		struct mfd_control_calibration cal;
		mfd_calibration_set(&cal, slot_addr_is_14bit(&addr), (int)obs_data_get_int(item, "cal_min"),
				    (int)obs_data_get_int(item, "cal_max"));
		store_persisted_calibration(eng, prof, &addr, &cal);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void serialize_slots(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", s->id);
		obs_data_set_string(item, "kind", s->kind);
		obs_data_set_bool(item, "enabled", s->enabled);
		obs_data_set_string(item, "label", s->label);
		obs_data_set_int(item, "midi_type", (int)s->addr.type);
		obs_data_set_int(item, "midi_channel", (int)s->addr.channel);
		obs_data_set_int(item, "midi_number", (int)s->addr.number);
		obs_data_set_bool(item, "midi_14bit", s->addr.is_14bit);
		obs_data_set_int(item, "cal_min", s->cal.cal_min);
		obs_data_set_int(item, "cal_max", s->cal.cal_max);
		obs_data_set_int(item, "target_type", (int)s->target_type);
		obs_data_set_string(item, "scene", s->scene_name);
		obs_data_set_string(item, "source_a", s->source_a_name);
		obs_data_set_string(item, "source_b", s->source_b_name);
		obs_data_set_string(item, "filter_name", s->filter_name);
		obs_data_set_string(item, "filter_prop", s->filter_prop);
		obs_data_set_double(item, "from_val", s->filter_from_val);
		obs_data_set_double(item, "to_val", s->filter_to_val);
		obs_data_set_bool(item, "bypass_at_zero", s->filter_bypass_at_zero);
		if (s->target_type == MFD_TARGET_FILTER_PARAM && s->filter_bypass_at_zero &&
		    (find_filter_lease_locked(eng, s->source_a_name, s->filter_name) != NULL ||
		     s->restore_persisted_pre_bypass)) {
			obs_data_set_bool(item, "filter_pre_bypass_enabled", s->filter_pre_bypass_enabled);
		}
		obs_data_set_int(item, "curve", (int)s->curve);
		obs_data_set_bool(item, "afv", s->audio_follows_video);
		obs_data_set_bool(item, "invert", s->invert);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "slots", arr);
	obs_data_array_release(arr);
}

static void clear_slot_filter_fields(struct mfd_fader_slot *s)
{
	if (!s) {
		return;
	}
	s->filter_name[0] = '\0';
	s->filter_prop[0] = '\0';
	s->filter_from_val = 0.0;
	s->filter_to_val = 0.0;
	s->filter_bypass_at_zero = false;
	s->filter_pre_bypass_enabled = true;
	s->restore_persisted_pre_bypass = false;
	s->filter_prop_valid = false;
}

static void deserialize_slots(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_get_array(root, "slots");
	if (!arr) {
		return;
	}

	/* Pre-seed 0..17 from active profile defaults (detaching any active filter bypass leases first) */
	mfd_engine_load_profile_defaults(eng, mfd_engine_get_active_profile(eng));

	bool seen_slot[MFD_MAX_FADER_SLOTS];
	memset(seen_slot, 0, sizeof(seen_slot));

	size_t count = obs_data_array_count(arr);
	if (count > 256U) {
		count = 256U;
	}
	for (size_t i = 0U; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		if (!item) {
			continue;
		}
		int slot_idx = -1;
		const char *item_id = obs_data_get_string(item, "id");
		if (item_id && item_id[0] != '\0') {
			slot_idx = mfd_slot_index_for_id(item_id);
			if (slot_idx < 0 || slot_idx >= MFD_MAX_FADER_SLOTS) {
				obs_log(LOG_WARNING, "Ignoring unknown persisted slot id '%s' at index %u", item_id,
					(unsigned)i);
				obs_data_release(item);
				continue;
			}
		} else if (i < (size_t)MFD_MAX_FADER_SLOTS) {
			slot_idx = (int)i;
		} else {
			obs_data_release(item);
			continue;
		}

		if (seen_slot[slot_idx]) {
			obs_log(LOG_WARNING, "Ignoring duplicate persisted slot entry for '%s'",
				mfd_slot_id_for_index(slot_idx));
			obs_data_release(item);
			continue;
		}
		seen_slot[slot_idx] = true;

		struct mfd_fader_slot *s = &eng->fader_slots[slot_idx];
		snprintf(s->id, sizeof(s->id), "%s", mfd_slot_id_for_index(slot_idx));
		snprintf(s->kind, sizeof(s->kind), "%s", mfd_slot_kind_for_index(slot_idx));
		s->enabled = obs_data_get_bool(item, "enabled");
		const char *lbl = obs_data_get_string(item, "label");
		if (lbl && *lbl) {
			snprintf(s->label, sizeof(s->label), "%s", lbl);
		}

		if (obs_data_has_user_value(item, "midi_type")) {
			long long raw_mtype = obs_data_get_int(item, "midi_type");
			if (raw_mtype < (long long)MFD_MIDI_NONE || raw_mtype > (long long)MFD_MIDI_PITCH_BEND) {
				obs_log(LOG_WARNING, "Slot '%s' had invalid midi_type %lld; resetting MIDI binding",
					s->id, raw_mtype);
				s->addr.type = MFD_MIDI_NONE;
			} else {
				s->addr.type = (enum mfd_midi_msg_type)raw_mtype;
			}
			s->addr.channel = (uint8_t)(obs_data_get_int(item, "midi_channel") & 0x0F);
			s->addr.number = (uint8_t)(obs_data_get_int(item, "midi_number") & 0x7F);
			s->addr.is_14bit = obs_data_get_bool(item, "midi_14bit");
		}
		if (obs_data_has_user_value(item, "cal_min") && obs_data_has_user_value(item, "cal_max")) {
			mfd_calibration_set(&s->cal, slot_addr_is_14bit(&s->addr),
					    (int)obs_data_get_int(item, "cal_min"),
					    (int)obs_data_get_int(item, "cal_max"));
			store_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
		} else {
			mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
		}

		if (obs_data_has_user_value(item, "target_type")) {
			long long raw_ttype = obs_data_get_int(item, "target_type");
			if (raw_ttype < (long long)MFD_TARGET_UNASSIGNED ||
			    raw_ttype > (long long)MFD_TARGET_FILTER_PARAM) {
				obs_log(LOG_WARNING,
					"Slot '%s' had invalid target_type %lld; downgrading to unassigned", s->id,
					raw_ttype);
				s->target_type = MFD_TARGET_UNASSIGNED;
			} else {
				s->target_type = (enum mfd_target_type)raw_ttype;
			}
		} else if (obs_data_has_user_value(item, "target_mode")) {
			enum mfd_target_type parsed_tm = MFD_TARGET_UNASSIGNED;
			const char *tm_str = obs_data_get_string(item, "target_mode");
			if (mfd_target_mode_from_string(tm_str, &parsed_tm)) {
				s->target_type = parsed_tm;
			} else {
				obs_log(LOG_WARNING,
					"Slot '%s' had invalid target_mode '%s'; downgrading to unassigned", s->id,
					tm_str ? tm_str : "");
				s->target_type = MFD_TARGET_UNASSIGNED;
			}
		} else {
			s->target_type = MFD_TARGET_UNASSIGNED;
		}

		const char *sc = obs_data_get_string(item, "scene");
		const char *sa = obs_data_get_string(item, "source_a");
		const char *sb = obs_data_get_string(item, "source_b");
		const char *fn = obs_data_get_string(item, "filter_name");
		const char *fp = obs_data_get_string(item, "filter_prop");
		snprintf(s->scene_name, sizeof(s->scene_name), "%s", sc ? sc : "");
		snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", sa ? sa : "");
		snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", sb ? sb : "");
		snprintf(s->filter_name, sizeof(s->filter_name), "%s", fn ? fn : "");
		snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", fp ? fp : "");
		s->filter_from_val = obs_data_get_double(item, "from_val");
		s->filter_to_val = obs_data_get_double(item, "to_val");
		s->filter_bypass_at_zero = obs_data_get_bool(item, "bypass_at_zero");
		if (obs_data_has_user_value(item, "filter_pre_bypass_enabled")) {
			s->filter_pre_bypass_enabled = obs_data_get_bool(item, "filter_pre_bypass_enabled");
			s->restore_persisted_pre_bypass = true;
		} else {
			s->filter_pre_bypass_enabled = true;
			s->restore_persisted_pre_bypass = false;
		}

		if (s->target_type == MFD_TARGET_FILTER_PARAM) {
			if (s->source_a_name[0] == '\0' || s->filter_name[0] == '\0' || s->filter_prop[0] == '\0' ||
			    mfd_is_internal_filter(s->filter_name, NULL) || !isfinite(s->filter_from_val) ||
			    !isfinite(s->filter_to_val) ||
			    fabs(s->filter_to_val - s->filter_from_val) < MFD_FILTER_MIN_RANGE_SPAN) {
				obs_log(LOG_WARNING,
					"Slot '%s' had malformed filter_param fields; downgrading to unassigned",
					s->id);
				clear_slot_filter_fields(s);
				s->target_type = MFD_TARGET_UNASSIGNED;
			}
		} else {
			clear_slot_filter_fields(s);
			if (s->target_type != MFD_TARGET_TBAR && s->source_a_name[0] == '\0' &&
			    s->source_b_name[0] == '\0') {
				s->target_type = MFD_TARGET_UNASSIGNED;
			}
		}

		long long raw_curve = obs_data_get_int(item, "curve");
		if (raw_curve < (long long)MFD_CURVE_DIP_FREE || raw_curve > (long long)MFD_CURVE_CUT) {
			s->curve = MFD_CURVE_DIP_FREE;
		} else {
			s->curve = (enum mfd_crossfade_curve)raw_curve;
		}
		s->audio_follows_video = obs_data_get_bool(item, "afv");
		s->invert = obs_data_get_bool(item, "invert");
		obs_data_release(item);
	}
	obs_data_array_release(arr);

	/* Deterministic 0..17 single-writer resolution (first valid owner wins) + runtime init */
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->enabled && s->target_type == MFD_TARGET_FILTER_PARAM) {
			for (int j = 0; j < i; ++j) {
				const struct mfd_fader_slot *earlier = &eng->fader_slots[j];
				if (earlier->enabled && earlier->target_type == MFD_TARGET_FILTER_PARAM &&
				    strcmp(earlier->source_a_name, s->source_a_name) == 0 &&
				    strcmp(earlier->filter_name, s->filter_name) == 0 &&
				    strcmp(earlier->filter_prop, s->filter_prop) == 0) {
					obs_log(LOG_WARNING,
						"Persisted slot '%s' conflicted with earlier owner '%s' on (%s, %s, %s); downgrading '%s' to unassigned",
						s->id, earlier->id, s->source_a_name, s->filter_name, s->filter_prop,
						s->id);
					clear_slot_filter_fields(s);
					s->target_type = MFD_TARGET_UNASSIGNED;
					break;
				}
			}
		}

		float init_val = 0.0f;
		if (s->target_type == MFD_TARGET_SINGLE_LAYER || s->target_type == MFD_TARGET_AUDIO_VOLUME) {
			init_val = 1.0f;
		} else if (s->target_type == MFD_TARGET_FILTER_PARAM) {
			init_val = query_live_filter_unmapped_deflection_locked(eng, s);
		}
		init_slot_runtime(eng, s, init_val);
	}

	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
}

static void on_save_callback(obs_data_t *save_data, bool saving, void *private_data)
{
	struct mfd_engine *eng = private_data;
	if (!eng || !save_data) {
		return;
	}

	mfd_engine_lock(eng);
	if (saving) {
		const char *self_test_env = getenv("MFD_SELF_TEST");
		if (self_test_env && strcmp(self_test_env, "1") == 0) {
			mfd_engine_unlock(eng);
			return;
		}
		obs_data_t *obj = obs_data_create();
		serialize_slots(eng, obj);
		obs_data_set_obj(save_data, MFD_SAVE_DATA_KEY, obj);
		obs_data_release(obj);
	} else {
		obs_data_t *obj = obs_data_get_obj(save_data, MFD_SAVE_DATA_KEY);
		if (obj) {
			deserialize_slots(eng, obj);
			if (slots_differ_from_apc40_defaults(eng)) {
				eng->first_run_offer_dismissed = true;
			}
			obs_data_release(obj);
			mfd_engine_notify_state_changed(eng);
		}
	}
	mfd_engine_unlock(eng);
}

bool mfd_engine_export_config_file(struct mfd_engine *eng, const char *filepath)
{
	if (!eng || !filepath || filepath[0] == '\0') {
		return false;
	}
	mfd_engine_lock(eng);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "device_name", eng->configured_device_name);
	obs_data_set_string(data, "active_profile", effective_profile_name(eng));
	obs_data_set_double(data, "smooth_tau_ms", (double)eng->smooth_tau_ms);
	obs_data_set_bool(data, "tbar_bidirectional_flip", eng->tbar_bidirectional_flip);
	obs_data_set_bool(data, "first_run_offer_dismissed", eng->first_run_offer_dismissed);
	serialize_calibrations(eng, data);
	serialize_slots(eng, data);
	mfd_engine_unlock(eng);

	bool ok = obs_data_save_json_safe(data, filepath, "tmp", "bak");
	obs_data_release(data);
	return ok;
}

bool mfd_engine_import_config_file(struct mfd_engine *eng, const char *filepath)
{
	if (!eng || !filepath || filepath[0] == '\0') {
		return false;
	}
	obs_data_t *data = obs_data_create_from_json_file_safe(filepath, "bak");
	if (!data) {
		return false;
	}
	mfd_engine_lock(eng);
	const char *dev = obs_data_get_string(data, "device_name");
	if (dev) {
		snprintf(eng->configured_device_name, sizeof(eng->configured_device_name), "%s", dev);
	}
	const char *prof = obs_data_get_string(data, "active_profile");
	if (prof && prof[0] != '\0') {
		snprintf(eng->active_profile_name, sizeof(eng->active_profile_name), "%s", prof);
	}
	if (obs_data_has_user_value(data, "smooth_tau_ms")) {
		eng->smooth_tau_ms = (float)obs_data_get_double(data, "smooth_tau_ms");
	}
	if (obs_data_has_user_value(data, "tbar_bidirectional_flip")) {
		eng->tbar_bidirectional_flip = obs_data_get_bool(data, "tbar_bidirectional_flip");
	}
	if (obs_data_has_user_value(data, "first_run_offer_dismissed")) {
		eng->first_run_offer_dismissed = obs_data_get_bool(data, "first_run_offer_dismissed");
	} else {
		/* Upgrading from 0.1.x global.json without first_run_offer_dismissed: never prompt to reset */
		eng->first_run_offer_dismissed = true;
	}
	deserialize_calibrations(eng, data);
	deserialize_slots(eng, data);
	refresh_apc40_detected_locked(eng);
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
	obs_data_release(data);
	return true;
}

bool mfd_engine_save_global_config(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	if (!obs_initialized()) {
		return true;
	}
	char *dir_path = obs_module_config_path("");
	if (dir_path) {
		os_mkdirs(dir_path);
		bfree(dir_path);
	}
	char *cfg_path = obs_module_config_path("global.json");
	if (!cfg_path) {
		return false;
	}
	bool ok = mfd_engine_export_config_file(eng, cfg_path);
	bfree(cfg_path);
	return ok;
}

static bool load_global_config_if_present(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	char *cfg_path = obs_module_config_path("global.json");
	if (!cfg_path) {
		return false;
	}
	bool loaded = false;
	if (os_file_exists(cfg_path)) {
		loaded = mfd_engine_import_config_file(eng, cfg_path);
	}
	bfree(cfg_path);
	return loaded;
}

void mfd_engine_load_global_config(struct mfd_engine *eng)
{
	(void)load_global_config_if_present(eng);
}

bool mfd_engine_reconnect_midi(struct mfd_engine *eng, const char *device_name)
{
	if (!eng) {
		return false;
	}
	mfd_engine_lock(eng);
	if (device_name) {
		snprintf(eng->configured_device_name, sizeof(eng->configured_device_name), "%s", device_name);
	}
	bool ok = mfd_winmm_open_by_name(&eng->midi_dev, eng->configured_device_name);
	refresh_apc40_detected_locked(eng);
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
	return ok;
}

void mfd_engine_set_smooth_tau_ms(struct mfd_engine *eng, float tau_ms)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->smooth_tau_ms = (isfinite(tau_ms) && tau_ms >= 0.0f) ? tau_ms : 0.0f;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		mfd_smoother_set_tau_ms(&eng->fader_slots[i].smoother, eng->smooth_tau_ms);
	}
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_set_tbar_bidirectional_flip(struct mfd_engine *eng, bool flip)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->tbar_bidirectional_flip = flip;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (eng->fader_slots[i].target_type == MFD_TARGET_TBAR) {
			eng->fader_slots[i].takeover.bidirectional_flip = flip;
		}
	}
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_start_learn(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	eng->setup_session.learning = false;
	eng->learning_slot_index = slot_index;
	mfd_midi_parser_start_learn(&eng->midi_parser);
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_cancel_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->learning_slot_index = -1;
	if (!eng->setup_session.learning) {
		mfd_midi_parser_cancel_learn(&eng->midi_parser);
	}
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_poll_midi_health_on_ui_thread(struct mfd_engine *eng)
{
	if (!eng || !eng->initialized) {
		return;
	}

	mfd_engine_lock(eng);
	if (eng->midi_dev.is_open) {
		if (!mfd_winmm_check_health(&eng->midi_dev)) {
			refresh_apc40_detected_locked(eng);
			mfd_engine_notify_state_changed(eng);
		}
		mfd_engine_unlock(eng);
		return;
	}

	/* Event-driven hot-plug rescan on WM_DEVICECHANGE / Reconnect (R-MIDI-2) */
	struct mfd_midi_device_info list[MFD_MAX_MIDI_DEVICES];
	size_t count = mfd_winmm_enumerate(list, MFD_MAX_MIDI_DEVICES);
	if (count == 0U) {
		refresh_apc40_detected_locked(eng);
		mfd_engine_unlock(eng);
		return;
	}

	bool candidate_present = false;
	if (eng->configured_device_name[0] != '\0') {
		for (size_t i = 0U; i < count; ++i) {
			if (strcmp(list[i].name, eng->configured_device_name) == 0) {
				candidate_present = true;
				break;
			}
		}
	} else {
		for (size_t i = 0U; i < count; ++i) {
			if (list[i].is_apc40_mk2) {
				candidate_present = true;
				break;
			}
		}
		if (!candidate_present && eng->midi_dev.error_kind != MFD_WINMM_ERR_BUSY_ALLOCATED) {
			candidate_present = true;
		}
	}

	if (candidate_present) {
		if (mfd_winmm_open_by_name(&eng->midi_dev, eng->configured_device_name)) {
			refresh_apc40_detected_locked(eng);
			mfd_engine_notify_state_changed(eng);
		}
	} else {
		refresh_apc40_detected_locked(eng);
	}
	mfd_engine_unlock(eng);
}

bool mfd_engine_init(void)
{
	mfd_tbar_apply_fn saved_tbar_driver = g_engine.tbar_driver;
	mfd_tbar_unlatch_fn saved_tbar_unlatch = g_engine.tbar_unlatch;
	memset(&g_engine, 0, sizeof(g_engine));
	if (!mfd_engine_init_mutex(&g_engine)) {
		return false;
	}
	g_engine.tbar_driver = saved_tbar_driver;
	g_engine.tbar_unlatch = saved_tbar_unlatch;
	mfd_midi_queue_init(&g_engine.midi_queue);
	mfd_midi_parser_init(&g_engine.midi_parser);
	mfd_winmm_init(&g_engine.midi_dev, &g_engine.midi_queue);

	snprintf(g_engine.active_profile_name, sizeof(g_engine.active_profile_name), "%s", MFD_DEFAULT_PROFILE_NAME);
	g_engine.smooth_tau_ms = MFD_DEFAULT_SMOOTH_TAU_MS;
	g_engine.tbar_bidirectional_flip = true;
	g_engine.learning_slot_index = -1;
	g_engine.last_moved_slot_index = -1;
	g_engine.first_run_offer_dismissed = false;

	mfd_engine_load_apc40_defaults(&g_engine);
	bool had_global_cfg = load_global_config_if_present(&g_engine);

	mfd_winmm_open_by_name(&g_engine.midi_dev, g_engine.configured_device_name);
	if (!had_global_cfg && g_engine.midi_dev.is_open) {
		enum mfd_controller_profile detected = mfd_profile_match_device_name(g_engine.midi_dev.opened_name);
		if (detected != MFD_PROFILE_GENERIC && detected != MFD_PROFILE_APC40_MK2) {
			mfd_engine_load_profile_defaults(&g_engine, detected);
		}
	}
	refresh_apc40_detected_locked(&g_engine);
	refresh_frontend_cache_on_ui_thread_locked(&g_engine);

	connect_obs_signals(&g_engine);
	mfd_engine_reevaluate_slots(&g_engine, false);
	obs_frontend_add_event_callback(on_frontend_event, &g_engine);
	obs_frontend_add_save_callback(on_save_callback, &g_engine);
	obs_add_tick_callback(mfd_engine_video_tick, &g_engine);

	g_engine.initialized = true;
	return true;
}

void mfd_engine_shutdown(void)
{
	if (!g_engine.initialized) {
		return;
	}
	mfd_engine_lock(&g_engine);
	g_engine.initialized = false;
	g_engine.ui_notify_fn = NULL;
	g_engine.ui_notify_user_data = NULL;
	mfd_engine_cancel_setup_session(&g_engine);
	mfd_engine_restore_all_bypass_leases(&g_engine);
	disconnect_obs_signals(&g_engine);
	mfd_engine_unlock(&g_engine);

	obs_remove_tick_callback(mfd_engine_video_tick, &g_engine);
	obs_frontend_remove_save_callback(on_save_callback, &g_engine);
	obs_frontend_remove_event_callback(on_frontend_event, &g_engine);

	mfd_engine_save_global_config(&g_engine);
	mfd_winmm_close(&g_engine.midi_dev);
	mfd_engine_destroy_mutex(&g_engine);
}
