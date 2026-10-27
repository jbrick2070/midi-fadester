/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-engine.h"
#include "obs/mfd-layer-filter.h"
#include "core/mixer-math.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define MFD_SAVE_DATA_KEY "midi_fadester_scene_collection"
#define MFD_HEALTH_CHECK_INTERVAL_SEC 1.0f

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

struct mfd_engine *mfd_engine_get(void)
{
	return &g_engine;
}

void mfd_engine_set_tbar_driver(mfd_tbar_apply_fn driver_fn)
{
	g_engine.tbar_driver = driver_fn;
}

static void init_slot_runtime(struct mfd_engine *eng, struct mfd_fader_slot *slot, float initial_val)
{
	mfd_smoother_init(&slot->smoother, eng->smooth_tau_ms, initial_val);
	mfd_takeover_init(&slot->takeover, initial_val, MFD_DEFAULT_PICKUP_THRESHOLD,
			  (slot->target_type == MFD_TARGET_TBAR) ? eng->tbar_bidirectional_flip : false);
	slot->last_raw_norm = initial_val;
	slot->last_applied_val = initial_val;
	slot->last_alpha_a = 1.0f;
	slot->last_alpha_b = 0.0f;
}

void mfd_engine_load_apc40_defaults(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}

	struct mfd_apc40_default_slot defs[MFD_APC40_DEFAULT_SLOT_COUNT];
	size_t count = mfd_apc40_get_default_slots(defs, MFD_APC40_DEFAULT_SLOT_COUNT);

	for (size_t i = 0U; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		memset(s, 0, sizeof(*s));
		if (i < count) {
			s->enabled = true;
			snprintf(s->label, sizeof(s->label), "%s", defs[i].label);
			s->addr = defs[i].addr;
			s->target_type = defs[i].default_target;
			s->curve = defs[i].default_curve;
			s->audio_follows_video = false;
			s->invert = false;
		} else {
			s->enabled = false;
			snprintf(s->label, sizeof(s->label), "Fader %u", (unsigned)(i + 1U));
			s->addr.type = MFD_MIDI_NONE;
			s->target_type = MFD_TARGET_UNASSIGNED;
			s->curve = MFD_CURVE_DIP_FREE;
		}
		float init_val = (s->target_type == MFD_TARGET_TBAR || s->target_type == MFD_TARGET_TWO_SOURCE_MIX ||
				  s->target_type == MFD_TARGET_UNASSIGNED)
					 ? 0.0f
					 : 1.0f;
		init_slot_runtime(eng, s, init_val);
	}
}

static void apply_source_opacity_and_afv(const char *source_name, float alpha_01, bool afv)
{
	if (!source_name || source_name[0] == '\0') {
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

void mfd_engine_apply_slot_output(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val)
{
	if (!eng || !slot || !slot->enabled) {
		return;
	}

	float val = clamp01(norm_val);
	slot->last_applied_val = val;

	switch (slot->target_type) {
	case MFD_TARGET_UNASSIGNED:
		break;

	case MFD_TARGET_TBAR:
		if (eng->tbar_driver) {
			bool completed = false;
			char err_buf[256] = {0};
			bool ok = eng->tbar_driver(val, &completed, err_buf, sizeof(err_buf));
			if (!ok && err_buf[0] != '\0') {
				snprintf(eng->tbar_error, sizeof(eng->tbar_error), "%s", err_buf);
			} else if (ok) {
				eng->tbar_error[0] = '\0';
			}
			if (completed) {
				mfd_takeover_notify_tbar_completed(&slot->takeover);
				mfd_smoother_reset(&slot->smoother, 0.0f);
				slot->last_applied_val = 0.0f;
			}
		}
		break;

	case MFD_TARGET_SINGLE_LAYER:
		slot->last_alpha_a = val;
		slot->last_alpha_b = 0.0f;
		apply_source_opacity_and_afv(slot->source_a_name, val, slot->audio_follows_video);
		break;

	case MFD_TARGET_TWO_SOURCE_MIX: {
		float alpha_a = 1.0f;
		float alpha_b = 0.0f;
		mfd_crossfade_eval(slot->curve, val, &alpha_a, &alpha_b);
		slot->last_alpha_a = alpha_a;
		slot->last_alpha_b = alpha_b;
		apply_source_opacity_and_afv(slot->source_a_name, alpha_a, slot->audio_follows_video);
		apply_source_opacity_and_afv(slot->source_b_name, alpha_b, slot->audio_follows_video);
		break;
	}

	case MFD_TARGET_AUDIO_VOLUME:
		if (slot->source_a_name[0] != '\0') {
			obs_source_t *src = obs_get_source_by_name(slot->source_a_name);
			if (src) {
				uint32_t flags = obs_source_get_output_flags(src);
				if ((flags & OBS_SOURCE_AUDIO) != 0U) {
					obs_source_set_volume(src, mfd_deflection_to_mul(val));
				}
				obs_source_release(src);
			}
		}
		break;
	}
}

static void mfd_engine_video_tick(void *param, float seconds)
{
	struct mfd_engine *eng = param;
	if (!eng || !eng->initialized) {
		return;
	}

	eng->health_check_accum_sec += seconds;
	if (eng->health_check_accum_sec >= MFD_HEALTH_CHECK_INTERVAL_SEC) {
		eng->health_check_accum_sec = 0.0f;
		if (eng->midi_dev.is_open) {
			mfd_winmm_check_health(&eng->midi_dev);
		}
	}

	struct mfd_midi_raw_event raw;
	while (mfd_midi_queue_pop(&eng->midi_queue, &raw)) {
		struct mfd_midi_decoded dec;
		if (!mfd_midi_parser_feed(&eng->midi_parser, raw.raw_msg, &dec)) {
			continue;
		}
		eng->last_midi_decoded = dec;
		eng->has_last_midi = true;

		struct mfd_midi_addr learned;
		if (mfd_midi_parser_pop_learned(&eng->midi_parser, &learned)) {
			if (eng->learning_slot_index >= 0 && eng->learning_slot_index < MFD_MAX_FADER_SLOTS) {
				struct mfd_fader_slot *ls = &eng->fader_slots[eng->learning_slot_index];
				ls->addr = learned;
				ls->enabled = true;
				obs_log(LOG_INFO, "MIDI Learn bound slot '%s' to Ch %u Type %d Num %u", ls->label,
					(unsigned)(learned.channel + 1U), (int)learned.type, (unsigned)learned.number);
			}
			eng->learning_slot_index = -1;
		}

		if (dec.addr.type == MFD_MIDI_NOTE_OFF) {
			continue;
		}

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *slot = &eng->fader_slots[i];
			if (!slot->enabled) {
				continue;
			}
			if (!mfd_midi_addr_matches(&slot->addr, &dec.addr)) {
				continue;
			}

			float norm = slot->invert ? (1.0f - dec.normalized) : dec.normalized;
			slot->last_raw_norm = norm;

			float effective = slot->last_applied_val;
			if (mfd_takeover_step(&slot->takeover, norm, &effective)) {
				mfd_smoother_set_target(&slot->smoother, effective);
				if (!mfd_smoother_is_active(&slot->smoother)) {
					mfd_engine_apply_slot_output(eng, slot, slot->smoother.current);
				}
			}
		}
	}

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled) {
			continue;
		}
		if (mfd_smoother_is_active(&slot->smoother)) {
			float smoothed = mfd_smoother_tick(&slot->smoother, seconds);
			mfd_engine_apply_slot_output(eng, slot, smoothed);
		}
	}
}

static void sync_slots_on_scene_change(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled) {
			continue;
		}
		if (slot->target_type == MFD_TARGET_TBAR) {
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
		} else if (slot->target_type == MFD_TARGET_AUDIO_VOLUME && slot->source_a_name[0] != '\0') {
			obs_source_t *src = obs_get_source_by_name(slot->source_a_name);
			if (src) {
				float mul = obs_source_get_volume(src);
				mfd_takeover_sync_external(&slot->takeover, mfd_mul_to_deflection(mul));
				obs_source_release(src);
			}
		}
	}
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	struct mfd_engine *eng = private_data;
	if (!eng) {
		return;
	}

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		sync_slots_on_scene_change(eng);
		break;
	default:
		break;
	}
}

static void serialize_slots(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		obs_data_t *item = obs_data_create();
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
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "slots", arr);
	obs_data_array_release(arr);
}

static void deserialize_slots(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_get_array(root, "slots");
	if (!arr) {
		return;
	}
	size_t count = obs_data_array_count(arr);
	if (count > MFD_MAX_FADER_SLOTS) {
		count = MFD_MAX_FADER_SLOTS;
	}
	for (size_t i = 0U; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		if (!item) {
			continue;
		}
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		s->enabled = obs_data_get_bool(item, "enabled");
		const char *lbl = obs_data_get_string(item, "label");
		if (lbl && *lbl) {
			snprintf(s->label, sizeof(s->label), "%s", lbl);
		}
		s->addr.type = (enum mfd_midi_msg_type)obs_data_get_int(item, "midi_type");
		s->addr.channel = (uint8_t)(obs_data_get_int(item, "midi_channel") & 0x0F);
		s->addr.number = (uint8_t)(obs_data_get_int(item, "midi_number") & 0x7F);
		s->addr.is_14bit = obs_data_get_bool(item, "midi_14bit");
		s->target_type = (enum mfd_target_type)obs_data_get_int(item, "target_type");
		const char *sa = obs_data_get_string(item, "source_a");
		const char *sb = obs_data_get_string(item, "source_b");
		snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", sa ? sa : "");
		snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", sb ? sb : "");
		if (s->target_type != MFD_TARGET_TBAR && s->source_a_name[0] == '\0' && s->source_b_name[0] == '\0') {
			s->target_type = MFD_TARGET_UNASSIGNED;
		}
		s->curve = (enum mfd_crossfade_curve)obs_data_get_int(item, "curve");
		s->audio_follows_video = obs_data_get_bool(item, "afv");
		s->invert = obs_data_get_bool(item, "invert");

		float init_val = (s->target_type == MFD_TARGET_TBAR || s->target_type == MFD_TARGET_TWO_SOURCE_MIX ||
				  s->target_type == MFD_TARGET_UNASSIGNED)
					 ? 0.0f
					 : 1.0f;
		init_slot_runtime(eng, s, init_val);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void on_save_callback(obs_data_t *save_data, bool saving, void *private_data)
{
	struct mfd_engine *eng = private_data;
	if (!eng || !save_data) {
		return;
	}

	if (saving) {
		obs_data_t *obj = obs_data_create();
		serialize_slots(eng, obj);
		obs_data_set_obj(save_data, MFD_SAVE_DATA_KEY, obj);
		obs_data_release(obj);
	} else {
		obs_data_t *obj = obs_data_get_obj(save_data, MFD_SAVE_DATA_KEY);
		if (obj) {
			deserialize_slots(eng, obj);
			obs_data_release(obj);
		}
	}
}

void mfd_engine_save_global_config(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	char *dir_path = obs_module_config_path("");
	if (dir_path) {
		os_mkdirs(dir_path);
		bfree(dir_path);
	}
	char *cfg_path = obs_module_config_path("global.json");
	if (!cfg_path) {
		return;
	}
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "device_name", eng->configured_device_name);
	obs_data_set_double(data, "smooth_tau_ms", (double)eng->smooth_tau_ms);
	obs_data_set_bool(data, "tbar_bidirectional_flip", eng->tbar_bidirectional_flip);
	serialize_slots(eng, data);
	obs_data_save_json_safe(data, cfg_path, "tmp", "bak");
	obs_data_release(data);
	bfree(cfg_path);
}

void mfd_engine_load_global_config(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	char *cfg_path = obs_module_config_path("global.json");
	if (!cfg_path) {
		return;
	}
	obs_data_t *data = obs_data_create_from_json_file_safe(cfg_path, "bak");
	bfree(cfg_path);
	if (!data) {
		return;
	}
	const char *dev = obs_data_get_string(data, "device_name");
	if (dev) {
		snprintf(eng->configured_device_name, sizeof(eng->configured_device_name), "%s", dev);
	}
	if (obs_data_has_user_value(data, "smooth_tau_ms")) {
		eng->smooth_tau_ms = (float)obs_data_get_double(data, "smooth_tau_ms");
	}
	if (obs_data_has_user_value(data, "tbar_bidirectional_flip")) {
		eng->tbar_bidirectional_flip = obs_data_get_bool(data, "tbar_bidirectional_flip");
	}
	deserialize_slots(eng, data);
	obs_data_release(data);
}

bool mfd_engine_reconnect_midi(struct mfd_engine *eng, const char *device_name)
{
	if (!eng) {
		return false;
	}
	if (device_name) {
		snprintf(eng->configured_device_name, sizeof(eng->configured_device_name), "%s", device_name);
	}
	return mfd_winmm_open_by_name(&eng->midi_dev, eng->configured_device_name);
}

void mfd_engine_set_smooth_tau_ms(struct mfd_engine *eng, float tau_ms)
{
	if (!eng) {
		return;
	}
	eng->smooth_tau_ms = (isfinite(tau_ms) && tau_ms >= 0.0f) ? tau_ms : 0.0f;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		mfd_smoother_set_tau_ms(&eng->fader_slots[i].smoother, eng->smooth_tau_ms);
	}
}

void mfd_engine_set_tbar_bidirectional_flip(struct mfd_engine *eng, bool flip)
{
	if (!eng) {
		return;
	}
	eng->tbar_bidirectional_flip = flip;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (eng->fader_slots[i].target_type == MFD_TARGET_TBAR) {
			eng->fader_slots[i].takeover.bidirectional_flip = flip;
		}
	}
}

void mfd_engine_start_learn(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	eng->learning_slot_index = slot_index;
	mfd_midi_parser_start_learn(&eng->midi_parser);
}

void mfd_engine_cancel_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	eng->learning_slot_index = -1;
	mfd_midi_parser_cancel_learn(&eng->midi_parser);
}

bool mfd_engine_init(void)
{
	memset(&g_engine, 0, sizeof(g_engine));
	mfd_midi_queue_init(&g_engine.midi_queue);
	mfd_midi_parser_init(&g_engine.midi_parser);
	mfd_winmm_init(&g_engine.midi_dev, &g_engine.midi_queue);

	g_engine.smooth_tau_ms = MFD_DEFAULT_SMOOTH_TAU_MS;
	g_engine.tbar_bidirectional_flip = true;
	g_engine.learning_slot_index = -1;

	mfd_engine_load_apc40_defaults(&g_engine);
	mfd_engine_load_global_config(&g_engine);

	mfd_winmm_open_by_name(&g_engine.midi_dev, g_engine.configured_device_name);

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
	g_engine.initialized = false;

	obs_remove_tick_callback(mfd_engine_video_tick, &g_engine);
	obs_frontend_remove_save_callback(on_save_callback, &g_engine);
	obs_frontend_remove_event_callback(on_frontend_event, &g_engine);

	mfd_engine_save_global_config(&g_engine);
	mfd_winmm_close(&g_engine.midi_dev);
}
