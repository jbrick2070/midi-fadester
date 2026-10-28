/*
 * Visual Sidechain - Audio Modulator Filter (Layer 3b)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "modulator-filter.h"
#include "audio-tap.h"
#include "core/baseline.h"
#include "core/fader-bridge.h"
#include "core/modulation.h"
#include "plugin-support.h"

#include <util/threading.h>

#include <stdio.h>
#include <string.h>

struct vs_modulator_filter {
	obs_source_t *context;
	pthread_mutex_t mutex;
	struct vs_audio_tap tap;
	struct vs_mod_slot_config slot_cfg[VS_MOD_MAX_SLOTS];
	struct vs_mod_slot_state slot_state[VS_MOD_MAX_SLOTS];
	double saved_pre_panic_amount_pct[VS_MOD_MAX_SLOTS];
	bool has_saved_pre_panic[VS_MOD_MAX_SLOTS];
	struct vs_baseline_table baselines;
	bool pending_load_restore;
};

static const char *modulator_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("VisualSidechain.AudioModulator");
}

static obs_source_t *acquire_sibling_filter(struct vs_modulator_filter *f, const char *filter_name)
{
	if (!f || !f->context || !filter_name || filter_name[0] == '\0')
		return NULL;

	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!parent)
		return NULL;

	obs_source_t *target = obs_source_get_filter_by_name(parent, filter_name);
	if (target == f->context) {
		obs_source_release(target);
		return NULL;
	}
	return target;
}

static void apply_baseline_to_obs_filter(void *user_data, const char *filter_name, const char *setting_name,
					 enum vs_prop_type prop_type, double float_val, int64_t int_val)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)user_data;
	obs_source_t *target = acquire_sibling_filter(f, filter_name);
	if (!target)
		return;

	obs_data_t *s = obs_source_get_settings(target);
	if (s) {
		if (prop_type == VS_PROP_INT) {
			obs_data_set_int(s, setting_name, (long long)int_val);
		} else {
			obs_data_set_double(s, setting_name, float_val);
		}
		obs_source_update(target, s);
		obs_data_release(s);
	}
	obs_source_release(target);
}

static bool has_any_active_baseline(const struct vs_baseline_table *table)
{
	if (!table)
		return false;
	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		if (table->entries[i].in_use)
			return true;
	}
	return false;
}

struct prop_query_ctx {
	const char *setting_name;
	bool found;
	enum vs_prop_type prop_type;
	double min_val;
	double max_val;
	int64_t int_step;
};

static void inspect_properties_for_setting(obs_properties_t *props, struct prop_query_ctx *q)
{
	if (!props || !q || q->found)
		return;

	obs_property_t *p = obs_properties_first(props);
	while (p) {
		const enum obs_property_type t = obs_property_get_type(p);
		const char *name = obs_property_name(p);
		if (t == OBS_PROPERTY_GROUP) {
			obs_properties_t *grp = obs_property_group_content(p);
			inspect_properties_for_setting(grp, q);
			if (q->found)
				return;
		} else if (name && strcmp(name, q->setting_name) == 0) {
			if (t == OBS_PROPERTY_FLOAT) {
				q->found = true;
				q->prop_type = VS_PROP_FLOAT;
				q->min_val = obs_property_float_min(p);
				q->max_val = obs_property_float_max(p);
				q->int_step = 1;
				return;
			}
			if (t == OBS_PROPERTY_INT) {
				q->found = true;
				q->prop_type = VS_PROP_INT;
				q->min_val = (double)obs_property_int_min(p);
				q->max_val = (double)obs_property_int_max(p);
				q->int_step = (int64_t)obs_property_int_step(p);
				if (q->int_step < 1)
					q->int_step = 1;
				return;
			}
			if (t == OBS_PROPERTY_LIST && obs_property_list_format(p) == OBS_COMBO_FORMAT_INT) {
				const size_t count = obs_property_list_item_count(p);
				q->found = true;
				q->prop_type = VS_PROP_INT;
				if (count > 0) {
					long long lo = obs_property_list_item_int(p, 0);
					long long hi = lo;
					for (size_t i = 1; i < count; i++) {
						const long long v = obs_property_list_item_int(p, i);
						if (v < lo)
							lo = v;
						if (v > hi)
							hi = v;
					}
					q->min_val = (double)lo;
					q->max_val = (double)hi;
				} else {
					q->min_val = 0.0;
					q->max_val = 3.0;
				}
				q->int_step = 1;
				return;
			}
		}
		obs_property_next(&p);
	}
}

static bool query_target_setting_info(struct vs_modulator_filter *f, const char *filter_name, const char *setting_name,
				      struct prop_query_ctx *out_q)
{
	if (!out_q)
		return false;
	memset(out_q, 0, sizeof(*out_q));
	out_q->setting_name = setting_name;
	out_q->prop_type = VS_PROP_FLOAT;
	out_q->int_step = 1;

	if (!f || !filter_name || filter_name[0] == '\0' || !setting_name || setting_name[0] == '\0')
		return false;

	obs_source_t *target = acquire_sibling_filter(f, filter_name);
	if (!target)
		return false;

	obs_properties_t *props = obs_source_properties(target);
	if (props) {
		inspect_properties_for_setting(props, out_q);
		obs_properties_destroy(props);
	}
	obs_source_release(target);
	return out_q->found;
}

static enum vs_prop_type resolve_target_prop_type(struct vs_modulator_filter *f, const char *filter_name,
						  const char *setting_name, int64_t *out_step)
{
	struct prop_query_ctx q;
	const bool found = query_target_setting_info(f, filter_name, setting_name, &q);
	if (out_step)
		*out_step = q.int_step;
	return found ? q.prop_type : VS_PROP_FLOAT;
}

static void on_modulator_enable_signal(void *data, calldata_t *cd)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f)
		return;
	const bool enabled = calldata_bool(cd, "enabled");
	if (!enabled) {
		pthread_mutex_lock(&f->mutex);
		vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
		for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++)
			vs_mod_slot_reset_state(&f->slot_state[i]);
		pthread_mutex_unlock(&f->mutex);
	}
}

static void slot_key(char *buf, size_t cap, uint32_t idx, const char *suffix)
{
	snprintf(buf, cap, "slot_%u_%s", idx + 1u, suffix);
}

static void modulator_get_defaults(obs_data_t *settings)
{
	vs_audio_tap_get_defaults(settings);

	char key[64];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		slot_key(key, sizeof(key), i, "enabled");
		obs_data_set_default_bool(settings, key, (i == 0u));

		slot_key(key, sizeof(key), i, "target_filter");
		obs_data_set_default_string(settings, key, "");

		slot_key(key, sizeof(key), i, "target_setting");
		obs_data_set_default_string(settings, key, "");

		slot_key(key, sizeof(key), i, "seeded_target");
		obs_data_set_default_string(settings, key, "");

		slot_key(key, sizeof(key), i, "signal");
		obs_data_set_default_int(settings, key, (int)VS_SIG_BASS);

		slot_key(key, sizeof(key), i, "custom_low_hz");
		obs_data_set_default_double(settings, key, 60.0);

		slot_key(key, sizeof(key), i, "custom_high_hz");
		obs_data_set_default_double(settings, key, 120.0);

		slot_key(key, sizeof(key), i, "custom_mode");
		obs_data_set_default_int(settings, key, (int)VS_HZ_MODE_PEAK);

		slot_key(key, sizeof(key), i, "amount_pct");
		obs_data_set_default_double(settings, key, 100.0);

		slot_key(key, sizeof(key), i, "min_value");
		obs_data_set_default_double(settings, key, 0.0);

		slot_key(key, sizeof(key), i, "max_value");
		obs_data_set_default_double(settings, key, 1.0);

		slot_key(key, sizeof(key), i, "invert");
		obs_data_set_default_bool(settings, key, false);

		slot_key(key, sizeof(key), i, "rate_divider");
		obs_data_set_default_int(settings, key, 1);
	}
}

static void modulator_update(void *data, obs_data_t *settings)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !settings)
		return;

	pthread_mutex_lock(&f->mutex);
	vs_audio_tap_update_from_settings(&f->tap, f->context, settings);

	const bool panic_active = vs_fader_bridge_is_panic_active();
	char key[64];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		struct vs_mod_slot_config prev = f->slot_cfg[i];
		struct vs_mod_slot_config next;
		vs_mod_slot_default_config(&next);

		slot_key(key, sizeof(key), i, "enabled");
		next.enabled = obs_data_get_bool(settings, key);

		slot_key(key, sizeof(key), i, "target_filter");
		const char *tf = obs_data_get_string(settings, key);
		if (tf) {
			strncpy(next.target_filter, tf, sizeof(next.target_filter) - 1u);
			next.target_filter[sizeof(next.target_filter) - 1u] = '\0';
		}

		slot_key(key, sizeof(key), i, "target_setting");
		const char *ts = obs_data_get_string(settings, key);
		if (ts) {
			strncpy(next.target_setting, ts, sizeof(next.target_setting) - 1u);
			next.target_setting[sizeof(next.target_setting) - 1u] = '\0';
		}

		/* Auto-seed min_value / max_value once when a new (target_filter, target_setting) pair is selected,
		 * including via obs-websocket SetSourceFilterSettings when min/max were not already seeded (R-MOD-2). */
		char seed_key[64];
		slot_key(seed_key, sizeof(seed_key), i, "seeded_target");
		if (next.target_filter[0] != '\0' && next.target_setting[0] != '\0') {
			char seed_id[280];
			char min_k[64];
			char max_k[64];
			snprintf(seed_id, sizeof(seed_id), "%s::%s", next.target_filter, next.target_setting);
			slot_key(min_k, sizeof(min_k), i, "min_value");
			slot_key(max_k, sizeof(max_k), i, "max_value");
			const char *prev_seed = obs_data_get_string(settings, seed_key);
			const bool has_explicit_bounds = (obs_data_has_user_value(settings, min_k) ||
							  obs_data_has_user_value(settings, max_k)) &&
							 strcmp(prev.target_filter, next.target_filter) == 0 &&
							 strcmp(prev.target_setting, next.target_setting) == 0;
			if ((!prev_seed || strcmp(prev_seed, seed_id) != 0) && !has_explicit_bounds &&
			    !obs_data_has_user_value(settings, min_k) && !obs_data_has_user_value(settings, max_k)) {
				struct prop_query_ctx q;
				if (query_target_setting_info(f, next.target_filter, next.target_setting, &q)) {
					obs_data_set_double(settings, min_k, q.min_val);
					obs_data_set_double(settings, max_k, q.max_val);
					obs_data_set_string(settings, seed_key, seed_id);
				}
			}
		}

		slot_key(key, sizeof(key), i, "signal");
		next.signal_id = (enum vs_signal_id)obs_data_get_int(settings, key);

		slot_key(key, sizeof(key), i, "custom_low_hz");
		next.custom_low_hz = (float)obs_data_get_double(settings, key);

		slot_key(key, sizeof(key), i, "custom_high_hz");
		next.custom_high_hz = (float)obs_data_get_double(settings, key);

		slot_key(key, sizeof(key), i, "custom_mode");
		next.custom_mode = (enum vs_hz_band_mode)obs_data_get_int(settings, key);

		slot_key(key, sizeof(key), i, "amount_pct");
		const double raw_amt_pct = obs_data_get_double(settings, key);
		if (panic_active) {
			if (!f->has_saved_pre_panic[i]) {
				f->saved_pre_panic_amount_pct[i] = raw_amt_pct;
				f->has_saved_pre_panic[i] = true;
			}
			next.amount = 0.0;
		} else {
			next.amount = raw_amt_pct * 0.01;
		}

		slot_key(key, sizeof(key), i, "min_value");
		next.min_value = obs_data_get_double(settings, key);

		slot_key(key, sizeof(key), i, "max_value");
		next.max_value = obs_data_get_double(settings, key);

		slot_key(key, sizeof(key), i, "invert");
		next.invert = obs_data_get_bool(settings, key);

		slot_key(key, sizeof(key), i, "rate_divider");
		const int div = (int)obs_data_get_int(settings, key);
		next.rate_divider = (div == 2 || div == 4) ? (uint32_t)div : 1u;

		next.prop_type = resolve_target_prop_type(f, next.target_filter, next.target_setting, &next.int_step);

		/* If the slot was disabled or retargeted, restore the old target's baseline (R-MOD-5). */
		const bool had_prev = (prev.enabled && prev.target_filter[0] != '\0' && prev.target_setting[0] != '\0');
		const bool target_changed = (!next.enabled || strcmp(prev.target_filter, next.target_filter) != 0 ||
					     strcmp(prev.target_setting, next.target_setting) != 0);

		if (had_prev && target_changed) {
			vs_baseline_restore_entry(&f->baselines, prev.target_filter, prev.target_setting,
						  apply_baseline_to_obs_filter, f);
			vs_mod_slot_reset_state(&f->slot_state[i]);
		}

		f->slot_cfg[i] = next;
	}
	pthread_mutex_unlock(&f->mutex);
}

static void restore_persisted_baselines_from_settings(struct vs_modulator_filter *f, obs_data_t *settings)
{
	if (!f || !settings)
		return;

	obs_data_array_t *arr = obs_data_get_array(settings, "saved_baselines");
	if (!arr)
		return;

	const size_t count = obs_data_array_count(arr);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		if (!item)
			continue;
		const char *filter_name = obs_data_get_string(item, "filter");
		const char *setting_name = obs_data_get_string(item, "setting");
		const enum vs_prop_type prop_type = (enum vs_prop_type)obs_data_get_int(item, "type");
		const double orig_f = obs_data_get_double(item, "orig_float");
		const int64_t orig_i = (int64_t)obs_data_get_int(item, "orig_int");

		vs_baseline_on_load_entry(&f->baselines, filter_name, setting_name, prop_type, orig_f, orig_i,
					  apply_baseline_to_obs_filter, f);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void modulator_load(void *data, obs_data_t *settings)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !settings)
		return;

	pthread_mutex_lock(&f->mutex);
	modulator_update(f, settings);
	f->pending_load_restore = true;
	pthread_mutex_unlock(&f->mutex);
}

static void modulator_save(void *data, obs_data_t *settings)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !settings)
		return;

	pthread_mutex_lock(&f->mutex);

	/* 1. Restore all live target filter settings to their baseline values right now so OBS
	 * serializes the unmodulated baseline values into the scene collection JSON (R-MOD-6). */
	vs_baseline_on_pre_save(&f->baselines, apply_baseline_to_obs_filter, f);

	/* 2. If Panic is active during save, preserve pre-panic amounts in settings for serialization
	 * while keeping runtime slot_cfg[].amount at 0.0 until Restore. */
	char key[64];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		if (f->has_saved_pre_panic[i]) {
			slot_key(key, sizeof(key), i, "amount_pct");
			obs_data_set_double(settings, key, f->saved_pre_panic_amount_pct[i]);
		}
	}

	/* 3. Also persist the baseline table in the modulator's own settings for crash-recovery reload. */
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		const struct vs_baseline_entry *e = &f->baselines.entries[i];
		if (!e->in_use)
			continue;
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "filter", e->filter_name);
		obs_data_set_string(item, "setting", e->setting_name);
		obs_data_set_int(item, "type", (int)e->prop_type);
		obs_data_set_double(item, "orig_float", e->original_float);
		obs_data_set_int(item, "orig_int", (long long)e->original_int);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, "saved_baselines", arr);
	obs_data_array_release(arr);

	/* Reset slot epsilon gates so the next video_tick immediately resumes modulation. */
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++)
		f->slot_state[i].has_last_written = false;

	pthread_mutex_unlock(&f->mutex);
}

static void *modulator_create(obs_data_t *settings, obs_source_t *source)
{
	struct vs_modulator_filter *f = bzalloc(sizeof(*f));
	if (!f)
		return NULL;

	if (pthread_mutex_init_recursive(&f->mutex) != 0) {
		bfree(f);
		return NULL;
	}

	f->context = source;
	vs_baseline_init(&f->baselines);
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		vs_mod_slot_default_config(&f->slot_cfg[i]);
		vs_mod_slot_reset_state(&f->slot_state[i]);
	}

	if (!vs_audio_tap_init(&f->tap, false)) {
		pthread_mutex_destroy(&f->mutex);
		bfree(f);
		return NULL;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(source);
	if (sh) {
		signal_handler_connect(sh, "enable", on_modulator_enable_signal, f);
	}

	modulator_update(f, settings);
	return f;
}

static void modulator_filter_add(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !f->context)
		return;

	obs_data_t *s = obs_source_get_settings(f->context);
	if (s) {
		modulator_update(f, s);
		obs_data_release(s);
	}
}

static void modulator_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f)
		return;
	pthread_mutex_lock(&f->mutex);
	vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
	pthread_mutex_unlock(&f->mutex);
}

static void modulator_destroy(void *data)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f)
		return;

	if (f->context) {
		signal_handler_t *sh = obs_source_get_signal_handler(f->context);
		if (sh) {
			signal_handler_disconnect(sh, "enable", on_modulator_enable_signal, f);
		}
	}

	pthread_mutex_lock(&f->mutex);
	vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
	pthread_mutex_unlock(&f->mutex);

	vs_audio_tap_destroy(&f->tap);
	pthread_mutex_destroy(&f->mutex);
	bfree(f);
}

static void modulator_video_tick(void *data, float seconds)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !f->context)
		return;

	pthread_mutex_lock(&f->mutex);

	if (f->pending_load_restore) {
		obs_source_t *parent = obs_filter_get_parent(f->context);
		if (parent) {
			obs_data_t *s = obs_source_get_settings(f->context);
			if (s) {
				restore_persisted_baselines_from_settings(f, s);
				modulator_update(f, s);
				obs_data_release(s);
			}
			f->pending_load_restore = false;
		}
	}

	/* Idempotent baseline restore if the filter is disabled via any path (R-MOD-5(2)(c)). */
	if (!obs_source_enabled(f->context)) {
		if (has_any_active_baseline(&f->baselines)) {
			vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
			for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++)
				vs_mod_slot_reset_state(&f->slot_state[i]);
		}
		pthread_mutex_unlock(&f->mutex);
		return;
	}

	vs_audio_tap_tick(&f->tap, f->context, seconds);

	if (vs_fader_bridge_is_panic_active()) {
		if (has_any_active_baseline(&f->baselines)) {
			vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
			for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++)
				f->slot_state[i].has_last_written = false;
		}
		pthread_mutex_unlock(&f->mutex);
		return;
	}

	float signals[VS_MOD_MAX_SLOTS];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		const struct vs_mod_slot_config *cfg = &f->slot_cfg[i];
		if (!cfg->enabled || cfg->target_filter[0] == '\0' || cfg->target_setting[0] == '\0') {
			signals[i] = 0.0f;
			continue;
		}
		signals[i] = vs_analyzer_get_signal(&f->tap.analyzer, &f->tap.current_frame, cfg->signal_id,
						    cfg->custom_low_hz, cfg->custom_high_hz, cfg->custom_mode);
	}

	/* Evaluate all 8 slots and batch dirty writes by distinct target_filter so each target filter
	 * receives at most ONE obs_source_update call per video frame (R-MOD-4). */
	struct vs_mod_target_batch batches[VS_MOD_MAX_SLOTS];
	const size_t batch_count =
		vs_mod_eval_and_batch_slots(f->slot_cfg, f->slot_state, signals, VS_MOD_MAX_SLOTS, batches);

	for (size_t b = 0; b < batch_count; b++) {
		obs_source_t *target = acquire_sibling_filter(f, batches[b].target_filter);
		if (!target)
			continue;

		obs_data_t *ts = obs_source_get_settings(target);
		if (ts) {
			for (size_t w = 0; w < batches[b].write_count; w++) {
				const struct vs_mod_target_write *wr = &batches[b].writes[w];
				if (wr->prop_type == VS_PROP_INT) {
					const int64_t cur_i = (int64_t)obs_data_get_int(ts, wr->target_setting);
					vs_baseline_record_before_write_int(&f->baselines, batches[b].target_filter,
									    wr->target_setting, cur_i, wr->int_val);
					obs_data_set_int(ts, wr->target_setting, (long long)wr->int_val);
				} else {
					const double cur_f = obs_data_get_double(ts, wr->target_setting);
					vs_baseline_record_before_write_float(&f->baselines, batches[b].target_filter,
									      wr->target_setting, cur_f, wr->float_val);
					obs_data_set_double(ts, wr->target_setting, wr->float_val);
				}
			}
			obs_source_update(target, ts);
			obs_data_release(ts);
		}
		obs_source_release(target);
	}

	pthread_mutex_unlock(&f->mutex);
}

static void modulator_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !f->context)
		return;
	obs_source_skip_video_filter(f->context);
}

struct enum_sibling_ctx {
	obs_source_t *self;
	obs_property_t *list;
};

static void enum_sibling_filters_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct enum_sibling_ctx *ctx = (struct enum_sibling_ctx *)param;
	if (!ctx || !child || child == ctx->self)
		return;

	const char *name = obs_source_get_name(child);
	if (name && name[0] != '\0')
		obs_property_list_add_string(ctx->list, name, name);
}

static void populate_numeric_settings_recursive(obs_properties_t *props, obs_property_t *list)
{
	if (!props || !list)
		return;

	obs_property_t *p = obs_properties_first(props);
	while (p) {
		const enum obs_property_type t = obs_property_get_type(p);
		const char *name = obs_property_name(p);
		const char *desc = obs_property_description(p);
		if (t == OBS_PROPERTY_GROUP) {
			populate_numeric_settings_recursive(obs_property_group_content(p), list);
		} else if (name && name[0] != '\0') {
			const bool is_num =
				(t == OBS_PROPERTY_FLOAT || t == OBS_PROPERTY_INT ||
				 (t == OBS_PROPERTY_LIST && obs_property_list_format(p) == OBS_COMBO_FORMAT_INT));
			if (is_num) {
				char label[256];
				if (desc && desc[0] != '\0')
					snprintf(label, sizeof(label), "%s (%s)", desc, name);
				else
					snprintf(label, sizeof(label), "%s", name);
				obs_property_list_add_string(list, label, name);
			}
		}
		obs_property_next(&p);
	}
}

static void populate_target_setting_list(struct vs_modulator_filter *f, obs_property_t *setting_list,
					 const char *filter_name)
{
	if (!setting_list)
		return;
	obs_property_list_clear(setting_list);
	obs_property_list_add_string(setting_list, obs_module_text("Modulator.TargetSetting.None"), "");

	if (!f || !filter_name || filter_name[0] == '\0')
		return;

	obs_source_t *target = acquire_sibling_filter(f, filter_name);
	if (!target)
		return;

	obs_properties_t *target_props = obs_source_properties(target);
	if (target_props) {
		populate_numeric_settings_recursive(target_props, setting_list);
		obs_properties_destroy(target_props);
	}
	obs_source_release(target);
}

static void set_slot_inner_controls_visible(obs_properties_t *props, uint32_t slot_idx, bool enabled, bool is_custom_hz)
{
	static const char *const k_always_when_enabled[] = {
		"target_filter", "target_setting", "signal", "amount_pct",
		"min_value",     "max_value",      "invert", "rate_divider",
	};
	char key[64];
	for (size_t k = 0; k < sizeof(k_always_when_enabled) / sizeof(k_always_when_enabled[0]); k++) {
		slot_key(key, sizeof(key), slot_idx, k_always_when_enabled[k]);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, enabled);
	}

	static const char *const k_custom_only[] = {
		"custom_low_hz",
		"custom_high_hz",
		"custom_mode",
	};
	for (size_t k = 0; k < sizeof(k_custom_only) / sizeof(k_custom_only[0]); k++) {
		slot_key(key, sizeof(key), slot_idx, k_custom_only[k]);
		obs_property_t *p = obs_properties_get(props, key);
		if (p)
			obs_property_set_visible(p, enabled && is_custom_hz);
	}
}

static void maybe_autoseed_slot_bounds(struct vs_modulator_filter *f, obs_data_t *settings, uint32_t slot_idx)
{
	if (!f || !settings)
		return;

	char tf_key[64];
	char ts_key[64];
	char seed_key[64];
	slot_key(tf_key, sizeof(tf_key), slot_idx, "target_filter");
	slot_key(ts_key, sizeof(ts_key), slot_idx, "target_setting");
	slot_key(seed_key, sizeof(seed_key), slot_idx, "seeded_target");

	const char *filter_name = obs_data_get_string(settings, tf_key);
	const char *setting_name = obs_data_get_string(settings, ts_key);
	if (!filter_name || filter_name[0] == '\0' || !setting_name || setting_name[0] == '\0') {
		obs_data_set_string(settings, seed_key, "");
		return;
	}

	char seed_id[280];
	snprintf(seed_id, sizeof(seed_id), "%s::%s", filter_name, setting_name);
	const char *prev_seed = obs_data_get_string(settings, seed_key);
	if (prev_seed && strcmp(prev_seed, seed_id) == 0)
		return;

	struct prop_query_ctx q;
	if (query_target_setting_info(f, filter_name, setting_name, &q)) {
		char min_key[64];
		char max_key[64];
		slot_key(min_key, sizeof(min_key), slot_idx, "min_value");
		slot_key(max_key, sizeof(max_key), slot_idx, "max_value");
		obs_data_set_double(settings, min_key, q.min_val);
		obs_data_set_double(settings, max_key, q.max_val);
		obs_data_set_string(settings, seed_key, seed_id);
	}
}

static void refresh_modulator_properties_visibility(struct vs_modulator_filter *f, obs_properties_t *props,
						    obs_data_t *settings, int repopulate_slot_idx)
{
	if (!props || !settings)
		return;

	char key[64];
	char grp_key[64];
	int highest_active = -1;

	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		slot_key(key, sizeof(key), i, "enabled");
		const bool enabled = obs_data_get_bool(settings, key);
		slot_key(key, sizeof(key), i, "target_filter");
		const char *tf = obs_data_get_string(settings, key);
		if (enabled || (tf && tf[0] != '\0'))
			highest_active = (int)i;
	}

	const int max_visible_slot = (highest_active + 1 < (int)VS_MOD_MAX_SLOTS) ? (highest_active + 1)
										  : ((int)VS_MOD_MAX_SLOTS - 1);

	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		snprintf(grp_key, sizeof(grp_key), "mod_slot_group_%u", i + 1u);
		obs_property_t *p_grp = obs_properties_get(props, grp_key);
		const bool slot_visible = ((int)i <= max_visible_slot);
		if (p_grp)
			obs_property_set_visible(p_grp, slot_visible);

		slot_key(key, sizeof(key), i, "enabled");
		const bool enabled = obs_data_get_bool(settings, key);

		if (repopulate_slot_idx == (int)i) {
			slot_key(key, sizeof(key), i, "target_filter");
			const char *filter_name = obs_data_get_string(settings, key);
			slot_key(key, sizeof(key), i, "target_setting");
			obs_property_t *p_setting = obs_properties_get(props, key);
			if (p_setting)
				populate_target_setting_list(f, p_setting, filter_name);
		}

		slot_key(key, sizeof(key), i, "signal");
		const int sig = (int)obs_data_get_int(settings, key);
		const bool is_custom = (sig == (int)VS_SIG_CUSTOM_HZ);

		set_slot_inner_controls_visible(props, i, enabled, is_custom);
	}
}

static bool slot_modified_cb(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)priv;
	if (!props || !settings)
		return false;

	int repopulate_idx = -1;
	const char *prop_name = property ? obs_property_name(property) : NULL;
	unsigned one_based = 0u;
	if (prop_name && sscanf(prop_name, "slot_%u_", &one_based) == 1 && one_based >= 1u &&
	    one_based <= VS_MOD_MAX_SLOTS) {
		const uint32_t slot_idx = one_based - 1u;
		if (strstr(prop_name, "_target_filter") != NULL) {
			repopulate_idx = (int)slot_idx;
			maybe_autoseed_slot_bounds(f, settings, slot_idx);
		} else if (strstr(prop_name, "_target_setting") != NULL) {
			maybe_autoseed_slot_bounds(f, settings, slot_idx);
		}
	}

	refresh_modulator_properties_visibility(f, props, settings, repopulate_idx);
	return true;
}

static void add_signal_choices(obs_property_t *list, bool fadester_loaded)
{
	obs_property_list_add_int(list, obs_module_text("Signal.Level"), (int)VS_SIG_LEVEL);
	obs_property_list_add_int(list, obs_module_text("Signal.SubBass"), (int)VS_SIG_SUB_BASS);
	obs_property_list_add_int(list, obs_module_text("Signal.Bass"), (int)VS_SIG_BASS);
	obs_property_list_add_int(list, obs_module_text("Signal.Mid"), (int)VS_SIG_MID);
	obs_property_list_add_int(list, obs_module_text("Signal.High"), (int)VS_SIG_HIGH);
	obs_property_list_add_int(list, obs_module_text("Signal.Kick"), (int)VS_SIG_KICK);
	obs_property_list_add_int(list, obs_module_text("Signal.Snare"), (int)VS_SIG_SNARE);
	obs_property_list_add_int(list, obs_module_text("Signal.Beat"), (int)VS_SIG_BEAT);
	obs_property_list_add_int(list, obs_module_text("Signal.Centroid"), (int)VS_SIG_CENTROID);
	obs_property_list_add_int(list, obs_module_text("Signal.Flux"), (int)VS_SIG_FLUX);
	obs_property_list_add_int(list, obs_module_text("Signal.CustomHz"), (int)VS_SIG_CUSTOM_HZ);

	char label[160];
	for (uint32_t b = 0; b < VS_SPECTRUM_BANDS; b++) {
		snprintf(label, sizeof(label), "%s %u", obs_module_text("Signal.SpectrumBandPrefix"), b + 1u);
		obs_property_list_add_int(list, label, (int)(VS_SIG_SPECTRUM_BAND_0 + b));
	}

	const char *not_loaded_text = obs_module_text("Modulator.MidiFadesterNotLoaded");

	static const char *const k_fader_locale_keys[VS_FADER_SLOT_COUNT] = {
		"Signal.FaderCrossfader", "Signal.Fader1", "Signal.Fader2", "Signal.Fader3", "Signal.Fader4",
		"Signal.Fader5",          "Signal.Fader6", "Signal.Fader7", "Signal.Fader8", "Signal.FaderMaster",
	};
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		const char *base = obs_module_text(k_fader_locale_keys[i]);
		if (fadester_loaded) {
			obs_property_list_add_int(list, base, (int)VS_SIG_FADER_CROSSFADER + i);
		} else {
			snprintf(label, sizeof(label), "%s - %s", base, not_loaded_text);
			obs_property_list_add_int(list, label, (int)VS_SIG_FADER_CROSSFADER + i);
		}
	}
}

static obs_properties_t *modulator_get_properties(void *data)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	obs_properties_t *props = obs_properties_create();

	vs_audio_tap_add_properties(props, f ? f->context : NULL);

	/* Probe Midi Fadester once for the entire Properties build instead of 8x across slots */
	vs_fader_bridge_poll_once();
	const bool fadester_loaded = vs_fader_bridge_is_midi_fadester_loaded();

	obs_data_t *cur_settings = (f && f->context) ? obs_source_get_settings(f->context) : NULL;

	char key[64];
	char grp_key[64];
	char grp_title[128];

	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		obs_properties_t *grp = obs_properties_create();

		slot_key(key, sizeof(key), i, "enabled");
		obs_property_t *p_en = obs_properties_add_bool(grp, key, obs_module_text("Modulator.Slot.Enabled"));
		obs_property_set_modified_callback2(p_en, slot_modified_cb, f);

		slot_key(key, sizeof(key), i, "target_filter");
		obs_property_t *p_tf = obs_properties_add_list(grp, key, obs_module_text("Modulator.TargetFilter"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p_tf, obs_module_text("Modulator.TargetFilter.None"), "");
		if (f && f->context) {
			obs_source_t *parent = obs_filter_get_parent(f->context);
			if (parent) {
				struct enum_sibling_ctx ectx = {f->context, p_tf};
				obs_source_enum_filters(parent, enum_sibling_filters_cb, &ectx);
			}
		}
		obs_property_set_modified_callback2(p_tf, slot_modified_cb, f);

		const char *cur_tf = NULL;
		if (cur_settings) {
			slot_key(key, sizeof(key), i, "target_filter");
			cur_tf = obs_data_get_string(cur_settings, key);
		}

		slot_key(key, sizeof(key), i, "target_setting");
		obs_property_t *p_ts = obs_properties_add_list(grp, key, obs_module_text("Modulator.TargetSetting"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		populate_target_setting_list(f, p_ts, cur_tf);
		obs_property_set_modified_callback2(p_ts, slot_modified_cb, f);

		slot_key(key, sizeof(key), i, "signal");
		obs_property_t *p_sig = obs_properties_add_list(grp, key, obs_module_text("Modulator.Signal"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		add_signal_choices(p_sig, fadester_loaded);
		obs_property_set_modified_callback2(p_sig, slot_modified_cb, f);

		slot_key(key, sizeof(key), i, "custom_low_hz");
		obs_property_t *p_lo = obs_properties_add_float_slider(
			grp, key, obs_module_text("Modulator.CustomLowHz"), 20.0, 20000.0, 1.0);
		obs_property_float_set_suffix(p_lo, " Hz");

		slot_key(key, sizeof(key), i, "custom_high_hz");
		obs_property_t *p_hi = obs_properties_add_float_slider(
			grp, key, obs_module_text("Modulator.CustomHighHz"), 20.0, 20000.0, 1.0);
		obs_property_float_set_suffix(p_hi, " Hz");

		slot_key(key, sizeof(key), i, "custom_mode");
		obs_property_t *p_cm = obs_properties_add_list(grp, key, obs_module_text("Modulator.CustomMode"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p_cm, obs_module_text("Modulator.CustomMode.Average"),
					  (int)VS_HZ_MODE_AVERAGE);
		obs_property_list_add_int(p_cm, obs_module_text("Modulator.CustomMode.Peak"), (int)VS_HZ_MODE_PEAK);
		obs_property_list_add_int(p_cm, obs_module_text("Modulator.CustomMode.Flux"), (int)VS_HZ_MODE_FLUX);

		slot_key(key, sizeof(key), i, "amount_pct");
		obs_property_t *p_amt = obs_properties_add_float_slider(grp, key, obs_module_text("Modulator.Amount"),
									-100.0, 100.0, 1.0);
		obs_property_float_set_suffix(p_amt, "%");
		obs_property_set_long_description(p_amt, obs_module_text("Modulator.Amount.Tooltip"));

		slot_key(key, sizeof(key), i, "min_value");
		obs_properties_add_float(grp, key, obs_module_text("Modulator.MinValue"), -100000.0, 100000.0, 0.01);

		slot_key(key, sizeof(key), i, "max_value");
		obs_properties_add_float(grp, key, obs_module_text("Modulator.MaxValue"), -100000.0, 100000.0, 0.01);

		slot_key(key, sizeof(key), i, "invert");
		obs_properties_add_bool(grp, key, obs_module_text("Modulator.Invert"));

		slot_key(key, sizeof(key), i, "rate_divider");
		obs_property_t *p_div = obs_properties_add_list(grp, key, obs_module_text("Modulator.RateDivider"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p_div, obs_module_text("Modulator.RateDivider.1"), 1);
		obs_property_list_add_int(p_div, obs_module_text("Modulator.RateDivider.2"), 2);
		obs_property_list_add_int(p_div, obs_module_text("Modulator.RateDivider.4"), 4);
		obs_property_set_long_description(p_div, obs_module_text("Modulator.RateDivider.Tooltip"));

		snprintf(grp_key, sizeof(grp_key), "mod_slot_group_%u", i + 1u);
		snprintf(grp_title, sizeof(grp_title), "%s %u", obs_module_text("Modulator.SlotGroupPrefix"), i + 1u);
		obs_properties_add_group(props, grp_key, grp_title, OBS_GROUP_NORMAL, grp);
	}

	if (cur_settings) {
		refresh_modulator_properties_visibility(f, props, cur_settings, -1);
		obs_data_release(cur_settings);
	}

	return props;
}

struct obs_source_info vs_modulator_filter_info = {
	.id = "visual_sidechain_modulator_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = modulator_get_name,
	.create = modulator_create,
	.destroy = modulator_destroy,
	.get_defaults = modulator_get_defaults,
	.get_properties = modulator_get_properties,
	.update = modulator_update,
	.load = modulator_load,
	.save = modulator_save,
	.video_tick = modulator_video_tick,
	.video_render = modulator_video_render,
	.filter_add = modulator_filter_add,
	.filter_remove = modulator_filter_remove,
};

void vs_register_modulator_filter(void)
{
	obs_register_source(&vs_modulator_filter_info);
}

void vs_modulator_filter_apply_panic(obs_source_t *source)
{
	if (!source)
		return;
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)obs_obj_get_data(source);
	if (!f)
		return;

	obs_data_t *settings = obs_source_get_settings(source);

	pthread_mutex_lock(&f->mutex);
	char key[64];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		slot_key(key, sizeof(key), i, "amount_pct");
		if (!f->has_saved_pre_panic[i]) {
			f->saved_pre_panic_amount_pct[i] = f->slot_cfg[i].amount * 100.0;
			f->has_saved_pre_panic[i] = true;
		}
		f->slot_cfg[i].amount = 0.0;
		if (settings)
			obs_data_set_double(settings, key, 0.0);
	}
	vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
	pthread_mutex_unlock(&f->mutex);

	if (settings)
		obs_data_release(settings);
	obs_source_update_properties(source);
}

void vs_modulator_filter_apply_restore(obs_source_t *source)
{
	if (!source)
		return;
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)obs_obj_get_data(source);
	if (!f)
		return;

	obs_data_t *settings = obs_source_get_settings(source);

	pthread_mutex_lock(&f->mutex);
	char key[64];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		if (!f->has_saved_pre_panic[i])
			continue;
		const double restored_pct = f->saved_pre_panic_amount_pct[i];
		slot_key(key, sizeof(key), i, "amount_pct");
		f->slot_cfg[i].amount = restored_pct * 0.01;
		f->has_saved_pre_panic[i] = false;
		f->slot_state[i].has_last_written = false;
		if (settings)
			obs_data_set_double(settings, key, restored_pct);
	}
	pthread_mutex_unlock(&f->mutex);

	if (settings)
		obs_data_release(settings);
	obs_source_update_properties(source);
}
