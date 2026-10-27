/*
 * Visual Sidechain - Audio Modulator Filter (Layer 3b)
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "modulator-filter.h"
#include "audio-tap.h"
#include "core/baseline.h"
#include "core/modulation.h"
#include "plugin-support.h"

#include <stdio.h>
#include <string.h>

struct vs_modulator_filter {
	obs_source_t *context;
	struct vs_audio_tap tap;
	struct vs_mod_slot_config slot_cfg[VS_MOD_MAX_SLOTS];
	struct vs_mod_slot_state slot_state[VS_MOD_MAX_SLOTS];
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

static enum vs_prop_type resolve_target_prop_type(struct vs_modulator_filter *f, const char *filter_name,
						  const char *setting_name, int64_t *out_step)
{
	if (out_step)
		*out_step = 1;

	obs_source_t *target = acquire_sibling_filter(f, filter_name);
	if (!target)
		return VS_PROP_FLOAT;

	struct prop_query_ctx q;
	memset(&q, 0, sizeof(q));
	q.setting_name = setting_name;
	q.prop_type = VS_PROP_FLOAT;
	q.int_step = 1;

	obs_properties_t *props = obs_source_properties(target);
	if (props) {
		inspect_properties_for_setting(props, &q);
		obs_properties_destroy(props);
	}
	obs_source_release(target);

	if (out_step)
		*out_step = q.int_step;
	return q.found ? q.prop_type : VS_PROP_FLOAT;
}

static void on_modulator_enable_signal(void *data, calldata_t *cd)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f)
		return;
	const bool enabled = calldata_bool(cd, "enabled");
	if (!enabled) {
		vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
		for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++)
			vs_mod_slot_reset_state(&f->slot_state[i]);
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

	vs_audio_tap_update_from_settings(&f->tap, f->context, settings);

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

		slot_key(key, sizeof(key), i, "signal");
		next.signal_id = (enum vs_signal_id)obs_data_get_int(settings, key);

		slot_key(key, sizeof(key), i, "custom_low_hz");
		next.custom_low_hz = (float)obs_data_get_double(settings, key);

		slot_key(key, sizeof(key), i, "custom_high_hz");
		next.custom_high_hz = (float)obs_data_get_double(settings, key);

		slot_key(key, sizeof(key), i, "custom_mode");
		next.custom_mode = (enum vs_hz_band_mode)obs_data_get_int(settings, key);

		slot_key(key, sizeof(key), i, "amount_pct");
		next.amount = obs_data_get_double(settings, key) * 0.01;

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

	modulator_update(f, settings);
	f->pending_load_restore = true;
}

static void modulator_save(void *data, obs_data_t *settings)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !settings)
		return;

	/* 1. Restore all live target filter settings to their baseline values right now so OBS
	 * serializes the unmodulated baseline values into the scene collection JSON (R-MOD-6). */
	vs_baseline_on_pre_save(&f->baselines, apply_baseline_to_obs_filter, f);

	/* 2. Also persist the baseline table in the modulator's own settings for crash-recovery reload. */
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
}

static void *modulator_create(obs_data_t *settings, obs_source_t *source)
{
	struct vs_modulator_filter *f = bzalloc(sizeof(*f));
	if (!f)
		return NULL;

	f->context = source;
	vs_baseline_init(&f->baselines);
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		vs_mod_slot_default_config(&f->slot_cfg[i]);
		vs_mod_slot_reset_state(&f->slot_state[i]);
	}

	if (!vs_audio_tap_init(&f->tap, false)) {
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

static void modulator_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f)
		return;
	vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
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

	vs_baseline_restore_all(&f->baselines, apply_baseline_to_obs_filter, f);
	vs_audio_tap_destroy(&f->tap);
	bfree(f);
}

static void modulator_video_tick(void *data, float seconds)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	if (!f || !f->context)
		return;

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

	if (!obs_source_enabled(f->context))
		return;

	vs_audio_tap_tick(&f->tap, f->context, seconds);

	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		struct vs_mod_slot_config *cfg = &f->slot_cfg[i];
		if (!cfg->enabled || cfg->target_filter[0] == '\0' || cfg->target_setting[0] == '\0')
			continue;

		const float sig = vs_analyzer_get_signal(&f->tap.analyzer, &f->tap.current_frame, cfg->signal_id,
							 cfg->custom_low_hz, cfg->custom_high_hz, cfg->custom_mode);

		struct vs_mod_eval_result eval;
		vs_mod_slot_eval(cfg, &f->slot_state[i], sig, &eval);
		if (!eval.should_write)
			continue;

		obs_source_t *target = acquire_sibling_filter(f, cfg->target_filter);
		if (!target)
			continue;

		obs_data_t *ts = obs_source_get_settings(target);
		if (ts) {
			if (cfg->prop_type == VS_PROP_INT) {
				const int64_t cur_i = (int64_t)obs_data_get_int(ts, cfg->target_setting);
				vs_baseline_record_before_write_int(&f->baselines, cfg->target_filter,
								    cfg->target_setting, cur_i, eval.int_val);
				obs_data_set_int(ts, cfg->target_setting, (long long)eval.int_val);
			} else {
				const double cur_f = obs_data_get_double(ts, cfg->target_setting);
				vs_baseline_record_before_write_float(&f->baselines, cfg->target_filter,
								      cfg->target_setting, cur_f, eval.float_val);
				obs_data_set_double(ts, cfg->target_setting, eval.float_val);
			}
			obs_source_update(target, ts);
			obs_data_release(ts);
		}
		obs_source_release(target);
	}
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

static bool slot_modified_cb(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)priv;
	if (!props || !settings)
		return false;

	char key[64];
	for (uint32_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		slot_key(key, sizeof(key), i, "enabled");
		const bool enabled = obs_data_get_bool(settings, key);

		slot_key(key, sizeof(key), i, "target_filter");
		const char *filter_name = obs_data_get_string(settings, key);

		slot_key(key, sizeof(key), i, "target_setting");
		obs_property_t *p_setting = obs_properties_get(props, key);
		if (p_setting)
			populate_target_setting_list(f, p_setting, filter_name);

		slot_key(key, sizeof(key), i, "signal");
		const int sig = (int)obs_data_get_int(settings, key);
		const bool is_custom = enabled && (sig == (int)VS_SIG_CUSTOM_HZ);

		slot_key(key, sizeof(key), i, "custom_low_hz");
		obs_property_t *p_lo = obs_properties_get(props, key);
		if (p_lo)
			obs_property_set_visible(p_lo, is_custom);

		slot_key(key, sizeof(key), i, "custom_high_hz");
		obs_property_t *p_hi = obs_properties_get(props, key);
		if (p_hi)
			obs_property_set_visible(p_hi, is_custom);

		slot_key(key, sizeof(key), i, "custom_mode");
		obs_property_t *p_mode = obs_properties_get(props, key);
		if (p_mode)
			obs_property_set_visible(p_mode, is_custom);
	}
	return true;
}

static void add_signal_choices(obs_property_t *list)
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

	char label[64];
	for (uint32_t b = 0; b < VS_SPECTRUM_BANDS; b++) {
		snprintf(label, sizeof(label), "%s %u", obs_module_text("Signal.SpectrumBandPrefix"), b + 1u);
		obs_property_list_add_int(list, label, (int)(VS_SIG_SPECTRUM_BAND_0 + b));
	}

	obs_property_list_add_int(list, obs_module_text("Signal.FaderCrossfader"), (int)VS_SIG_FADER_CROSSFADER);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader1"), (int)VS_SIG_FADER_1);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader2"), (int)VS_SIG_FADER_2);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader3"), (int)VS_SIG_FADER_3);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader4"), (int)VS_SIG_FADER_4);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader5"), (int)VS_SIG_FADER_5);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader6"), (int)VS_SIG_FADER_6);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader7"), (int)VS_SIG_FADER_7);
	obs_property_list_add_int(list, obs_module_text("Signal.Fader8"), (int)VS_SIG_FADER_8);
	obs_property_list_add_int(list, obs_module_text("Signal.FaderMaster"), (int)VS_SIG_FADER_MASTER);
}

static obs_properties_t *modulator_get_properties(void *data)
{
	struct vs_modulator_filter *f = (struct vs_modulator_filter *)data;
	obs_properties_t *props = obs_properties_create();

	vs_audio_tap_add_properties(props);

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

		slot_key(key, sizeof(key), i, "target_setting");
		obs_property_t *p_ts = obs_properties_add_list(grp, key, obs_module_text("Modulator.TargetSetting"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p_ts, obs_module_text("Modulator.TargetSetting.None"), "");

		slot_key(key, sizeof(key), i, "signal");
		obs_property_t *p_sig = obs_properties_add_list(grp, key, obs_module_text("Modulator.Signal"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		add_signal_choices(p_sig);
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

		snprintf(grp_key, sizeof(grp_key), "mod_slot_group_%u", i + 1u);
		snprintf(grp_title, sizeof(grp_title), "%s %u", obs_module_text("Modulator.SlotGroupPrefix"), i + 1u);
		obs_properties_add_group(props, grp_key, grp_title, OBS_GROUP_NORMAL, grp);
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
	.filter_remove = modulator_filter_remove,
};

void vs_register_modulator_filter(void)
{
	obs_register_source(&vs_modulator_filter_info);
}
