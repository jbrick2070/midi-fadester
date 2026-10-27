/*
 * Visual Sidechain - OBS 33 Audio Tap Manager & Shared GPU FFT Textures
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "audio-tap.h"
#include "plugin-support.h"

#include <media-io/audio-io.h>
#include <string.h>

static void raw_audio_callback(void *param, size_t mix_idx, struct audio_data *frames)
{
	UNUSED_PARAMETER(mix_idx);
	struct vs_audio_tap *tap = (struct vs_audio_tap *)param;
	if (!tap || !frames || frames->frames == 0)
		return;

	vs_analyzer_push_planar(&tap->analyzer, (const float *const *)frames->data, tap->channels, frames->frames,
				false, true, frames->timestamp);
}

static void source_audio_capture_callback(void *param, obs_source_t *source, const struct audio_data *audio_data,
					  bool muted)
{
	UNUSED_PARAMETER(source);
	struct vs_audio_tap *tap = (struct vs_audio_tap *)param;
	if (!tap || !audio_data || audio_data->frames == 0)
		return;

	vs_analyzer_push_planar(&tap->analyzer, (const float *const *)audio_data->data, tap->channels,
				audio_data->frames, muted, tap->react_when_muted, audio_data->timestamp);
}

static void detach_tap_callbacks(struct vs_audio_tap *tap)
{
	if (!tap)
		return;

	if (tap->raw_cb_active) {
		obs_remove_raw_audio_callback(tap->active_mix_idx, raw_audio_callback, tap);
		tap->raw_cb_active = false;
	}

	if (tap->weak_capture_source) {
		if (tap->source_cb_active) {
			obs_source_t *src = obs_weak_source_get_source(tap->weak_capture_source);
			if (src) {
				obs_source_remove_audio_capture_callback(src, source_audio_capture_callback, tap);
				obs_source_release(src);
			}
			tap->source_cb_active = false;
		}
		obs_weak_source_release(tap->weak_capture_source);
		tap->weak_capture_source = NULL;
	}
}

static bool try_attach_tap_callbacks(struct vs_audio_tap *tap, obs_source_t *filter_source)
{
	if (!tap)
		return false;

	if (tap->mode >= VS_TAP_TRACK_1 && tap->mode <= VS_TAP_TRACK_6) {
		const size_t mix_idx = (size_t)(tap->mode - VS_TAP_TRACK_1);
		obs_add_raw_audio_callback(mix_idx, NULL, raw_audio_callback, tap);
		tap->active_mix_idx = mix_idx;
		tap->raw_cb_active = true;
		obs_log(LOG_INFO, "Attached audio tap to Track %u (mix_idx=%zu)", (unsigned)(mix_idx + 1u), mix_idx);
		return true;
	}

	if (tap->mode == VS_TAP_PARENT_SOURCE) {
		if (!filter_source)
			return false;
		obs_source_t *parent = obs_filter_get_parent(filter_source);
		if (!parent)
			return false;

		const uint32_t flags = obs_source_get_output_flags(parent);
		if ((flags & OBS_SOURCE_AUDIO) == 0) {
			obs_log(LOG_WARNING, "Parent source '%s' has no audio output capability",
				obs_source_get_name(parent));
			return false;
		}

		tap->weak_capture_source = obs_source_get_weak_source(parent);
		obs_source_add_audio_capture_callback(parent, source_audio_capture_callback, tap);
		tap->source_cb_active = true;
		obs_log(LOG_INFO, "Attached audio tap to parent source '%s'", obs_source_get_name(parent));
		return true;
	}

	if (tap->mode == VS_TAP_NAMED_SOURCE) {
		if (tap->source_name[0] == '\0')
			return false;
		obs_source_t *src = obs_get_source_by_name(tap->source_name);
		if (!src)
			return false;

		const uint32_t flags = obs_source_get_output_flags(src);
		if ((flags & OBS_SOURCE_AUDIO) == 0) {
			obs_log(LOG_WARNING, "Named source '%s' has no audio output capability", tap->source_name);
			obs_source_release(src);
			return false;
		}

		tap->weak_capture_source = obs_source_get_weak_source(src);
		obs_source_add_audio_capture_callback(src, source_audio_capture_callback, tap);
		tap->source_cb_active = true;
		obs_log(LOG_INFO, "Attached audio tap to named source '%s'", tap->source_name);
		obs_source_release(src);
		return true;
	}

	return false;
}

bool vs_audio_tap_init(struct vs_audio_tap *tap, bool upload_gpu_textures)
{
	if (!tap)
		return false;

	memset(tap, 0, sizeof(*tap));
	tap->mode = VS_TAP_TRACK_1;
	tap->upload_gpu_textures = upload_gpu_textures;
	tap->sample_rate = 48000u;
	tap->channels = 2u;

	audio_t *obs_audio = obs_get_audio();
	if (obs_audio) {
		const uint32_t sr = audio_output_get_sample_rate(obs_audio);
		const size_t ch = audio_output_get_channels(obs_audio);
		if (sr > 0)
			tap->sample_rate = sr;
		if (ch > 0)
			tap->channels = ch;
	}

	struct vs_analyzer_config cfg;
	vs_analyzer_default_config(&cfg);
	cfg.sample_rate = tap->sample_rate;
	if (!vs_analyzer_init(&tap->analyzer, &cfg))
		return false;

	for (uint32_t i = 0; i < VS_FFT_TEX_BINS; i++)
		tap->current_frame.waveform[i] = 0.5f;

	return true;
}

void vs_audio_tap_destroy(struct vs_audio_tap *tap)
{
	if (!tap)
		return;

	detach_tap_callbacks(tap);

	if (tap->tex_spectrum || tap->tex_fft) {
		obs_enter_graphics();
		if (tap->tex_spectrum) {
			gs_texture_destroy(tap->tex_spectrum);
			tap->tex_spectrum = NULL;
		}
		if (tap->tex_fft) {
			gs_texture_destroy(tap->tex_fft);
			tap->tex_fft = NULL;
		}
		obs_leave_graphics();
	}

	vs_analyzer_free(&tap->analyzer);
}

void vs_audio_tap_configure(struct vs_audio_tap *tap, obs_source_t *filter_source, enum vs_tap_mode mode,
			    const char *source_name, bool react_when_muted, const struct vs_analyzer_config *ana_cfg)
{
	if (!tap)
		return;

	const char *safe_name = source_name ? source_name : "";
	const bool target_changed = (tap->mode != mode) ||
				    (strncmp(tap->source_name, safe_name, sizeof(tap->source_name) - 1u) != 0) ||
				    (!tap->raw_cb_active && !tap->source_cb_active);

	tap->react_when_muted = react_when_muted;
	if (ana_cfg)
		vs_analyzer_update_config(&tap->analyzer, ana_cfg);

	if (target_changed) {
		detach_tap_callbacks(tap);
		tap->mode = mode;
		strncpy(tap->source_name, safe_name, sizeof(tap->source_name) - 1u);
		tap->source_name[sizeof(tap->source_name) - 1u] = '\0';
		tap->retry_accum_sec = 0.0f;
		vs_analyzer_reset(&tap->analyzer);
		try_attach_tap_callbacks(tap, filter_source);
	}
}

static void upload_tap_textures(struct vs_audio_tap *tap)
{
	if (!tap || !tap->upload_gpu_textures)
		return;

	obs_enter_graphics();
	if (!tap->tex_spectrum) {
		tap->tex_spectrum = gs_texture_create(VS_SPECTRUM_BANDS, 1u, GS_R32F, 1u, NULL, GS_DYNAMIC);
	}
	if (!tap->tex_fft) {
		tap->tex_fft = gs_texture_create(VS_FFT_TEX_BINS, 2u, GS_R32F, 1u, NULL, GS_DYNAMIC);
	}

	if (tap->tex_spectrum) {
		gs_texture_set_image(tap->tex_spectrum, (const uint8_t *)tap->current_frame.spectrum,
				     VS_SPECTRUM_BANDS * sizeof(float), false);
	}
	if (tap->tex_fft) {
		memcpy(&tap->fft_tex_staging[0], tap->current_frame.fft_bins, VS_FFT_TEX_BINS * sizeof(float));
		memcpy(&tap->fft_tex_staging[VS_FFT_TEX_BINS], tap->current_frame.waveform,
		       VS_FFT_TEX_BINS * sizeof(float));
		gs_texture_set_image(tap->tex_fft, (const uint8_t *)tap->fft_tex_staging,
				     VS_FFT_TEX_BINS * sizeof(float), false);
	}
	obs_leave_graphics();
}

void vs_audio_tap_tick(struct vs_audio_tap *tap, obs_source_t *filter_source, float seconds)
{
	if (!tap)
		return;

	/* Retry attachment once per second if a named or parent source was not ready at load time. */
	if (!tap->raw_cb_active && !tap->source_cb_active) {
		tap->retry_accum_sec += seconds;
		if (tap->retry_accum_sec >= 1.0f) {
			tap->retry_accum_sec = 0.0f;
			try_attach_tap_callbacks(tap, filter_source);
		}
	}

	const uint64_t video_ts = obs_get_video_frame_time();
	if (video_ts != 0 && video_ts == tap->last_video_frame_ts)
		return;
	tap->last_video_frame_ts = video_ts;

	vs_analyzer_step(&tap->analyzer, seconds, &tap->current_frame);
	upload_tap_textures(tap);
}

void vs_audio_tap_get_defaults(obs_data_t *settings)
{
	if (!settings)
		return;
	obs_data_set_default_int(settings, "tap_mode", (int)VS_TAP_TRACK_1);
	obs_data_set_default_string(settings, "tap_source_name", "");
	obs_data_set_default_bool(settings, "react_when_muted", false);
	obs_data_set_default_double(settings, "sync_offset_ms", 0.0);
	obs_data_set_default_bool(settings, "auto_gain", true);
	obs_data_set_default_double(settings, "gate_db", (double)VS_ENV_DEFAULT_GATE_DB);
	obs_data_set_default_double(settings, "floor_db", (double)VS_ENV_DEFAULT_FLOOR_DB);
	obs_data_set_default_double(settings, "ceiling_db", (double)VS_ENV_DEFAULT_CEILING_DB);
	obs_data_set_default_double(settings, "attack_ms", (double)VS_ENV_DEFAULT_ATTACK_MS);
	obs_data_set_default_double(settings, "release_ms", (double)VS_ENV_DEFAULT_RELEASE_MS);
	obs_data_set_default_double(settings, "kick_sensitivity", 0.5);
	obs_data_set_default_double(settings, "snare_sensitivity", 0.5);
}

void vs_audio_tap_update_from_settings(struct vs_audio_tap *tap, obs_source_t *filter_source, obs_data_t *settings)
{
	if (!tap || !settings)
		return;

	int mode_int = (int)obs_data_get_int(settings, "tap_mode");
	if (mode_int < (int)VS_TAP_TRACK_1 || mode_int > (int)VS_TAP_NAMED_SOURCE)
		mode_int = (int)VS_TAP_TRACK_1;

	const char *source_name = obs_data_get_string(settings, "tap_source_name");
	const bool react_when_muted = obs_data_get_bool(settings, "react_when_muted");

	struct vs_analyzer_config cfg;
	vs_analyzer_default_config(&cfg);
	cfg.sample_rate = tap->sample_rate;
	cfg.sync_offset_ms = (float)obs_data_get_double(settings, "sync_offset_ms");
	cfg.auto_gain = obs_data_get_bool(settings, "auto_gain");
	cfg.gate_db = (float)obs_data_get_double(settings, "gate_db");
	cfg.floor_db = (float)obs_data_get_double(settings, "floor_db");
	cfg.ceiling_db = (float)obs_data_get_double(settings, "ceiling_db");
	cfg.attack_ms = (float)obs_data_get_double(settings, "attack_ms");
	cfg.release_ms = (float)obs_data_get_double(settings, "release_ms");
	cfg.kick_sensitivity = (float)obs_data_get_double(settings, "kick_sensitivity");
	cfg.snare_sensitivity = (float)obs_data_get_double(settings, "snare_sensitivity");

	vs_audio_tap_configure(tap, filter_source, (enum vs_tap_mode)mode_int, source_name, react_when_muted, &cfg);
}

static bool enum_audio_sources_cb(void *data, obs_source_t *source)
{
	obs_property_t *list = (obs_property_t *)data;
	if (!list || !source)
		return true;

	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_AUDIO) != 0) {
		const char *name = obs_source_get_name(source);
		if (name && name[0] != '\0')
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

static bool tap_mode_modified_cb(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	if (!props || !settings)
		return false;

	const int mode = (int)obs_data_get_int(settings, "tap_mode");
	const bool is_named = (mode == (int)VS_TAP_NAMED_SOURCE);
	const bool is_source = (mode == (int)VS_TAP_NAMED_SOURCE || mode == (int)VS_TAP_PARENT_SOURCE);
	const bool auto_gain = obs_data_get_bool(settings, "auto_gain");

	obs_property_t *p_src = obs_properties_get(props, "tap_source_name");
	obs_property_t *p_mute = obs_properties_get(props, "react_when_muted");
	obs_property_t *p_floor = obs_properties_get(props, "floor_db");
	obs_property_t *p_ceil = obs_properties_get(props, "ceiling_db");

	if (p_src)
		obs_property_set_visible(p_src, is_named);
	if (p_mute)
		obs_property_set_visible(p_mute, is_source);
	if (p_floor)
		obs_property_set_visible(p_floor, !auto_gain);
	if (p_ceil)
		obs_property_set_visible(p_ceil, !auto_gain);

	return true;
}

void vs_audio_tap_add_properties(obs_properties_t *props)
{
	if (!props)
		return;

	obs_properties_t *grp = obs_properties_create();

	obs_property_t *mode_list = obs_properties_add_list(grp, "tap_mode", obs_module_text("AudioTap.Mode"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.Track1"), (int)VS_TAP_TRACK_1);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.Track2"), (int)VS_TAP_TRACK_2);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.Track3"), (int)VS_TAP_TRACK_3);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.Track4"), (int)VS_TAP_TRACK_4);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.Track5"), (int)VS_TAP_TRACK_5);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.Track6"), (int)VS_TAP_TRACK_6);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.ParentSource"), (int)VS_TAP_PARENT_SOURCE);
	obs_property_list_add_int(mode_list, obs_module_text("AudioTap.Mode.NamedSource"), (int)VS_TAP_NAMED_SOURCE);
	obs_property_set_modified_callback(mode_list, tap_mode_modified_cb);

	obs_property_t *src_list = obs_properties_add_list(grp, "tap_source_name", obs_module_text("AudioTap.Source"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(src_list, obs_module_text("AudioTap.Source.None"), "");
	obs_enum_sources(enum_audio_sources_cb, src_list);

	obs_properties_add_bool(grp, "react_when_muted", obs_module_text("AudioTap.ReactWhenMuted"));

	obs_property_t *p_sync = obs_properties_add_float_slider(
		grp, "sync_offset_ms", obs_module_text("AudioTap.SyncOffsetMs"), -200.0, 500.0, 1.0);
	obs_property_float_set_suffix(p_sync, " ms");

	obs_property_t *p_ag = obs_properties_add_bool(grp, "auto_gain", obs_module_text("AudioTap.AutoGain"));
	obs_property_set_modified_callback(p_ag, tap_mode_modified_cb);

	obs_property_t *p_gate =
		obs_properties_add_float_slider(grp, "gate_db", obs_module_text("AudioTap.GateDb"), -80.0, -20.0, 1.0);
	obs_property_float_set_suffix(p_gate, " dBFS");

	obs_property_t *p_floor = obs_properties_add_float_slider(grp, "floor_db", obs_module_text("AudioTap.FloorDb"),
								  -80.0, -12.0, 1.0);
	obs_property_float_set_suffix(p_floor, " dBFS");

	obs_property_t *p_ceil = obs_properties_add_float_slider(
		grp, "ceiling_db", obs_module_text("AudioTap.CeilingDb"), -36.0, 0.0, 1.0);
	obs_property_float_set_suffix(p_ceil, " dBFS");

	obs_property_t *p_att = obs_properties_add_float_slider(grp, "attack_ms", obs_module_text("AudioTap.AttackMs"),
								0.0, 500.0, 1.0);
	obs_property_float_set_suffix(p_att, " ms");

	obs_property_t *p_rel = obs_properties_add_float_slider(
		grp, "release_ms", obs_module_text("AudioTap.ReleaseMs"), 0.0, 2000.0, 5.0);
	obs_property_float_set_suffix(p_rel, " ms");

	obs_properties_add_float_slider(grp, "kick_sensitivity", obs_module_text("AudioTap.KickSensitivity"), 0.0, 1.0,
					0.01);
	obs_properties_add_float_slider(grp, "snare_sensitivity", obs_module_text("AudioTap.SnareSensitivity"), 0.0,
					1.0, 0.01);

	obs_properties_add_group(props, "audio_tap_group", obs_module_text("AudioTap.Group"), OBS_GROUP_NORMAL, grp);
}
