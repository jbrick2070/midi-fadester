/*
 * Visual Sidechain - OBS 33 FFT-Reactive GPU Shader Filter
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Clean-room OBS 33 shader filter host supporting `.shader` uniform annotation
 * conventions compatible with obs-shaderfilter (GPL-2.0-only, Copyright
 * 2016-2017 nleseul, 2017-2022 Charles Fettinger, 2022-2026 Exeldro).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "shader-filter.h"

#include "audio-tap.h"
#include "property-status.h"
#include "core/fader-bridge.h"
#include "core/shader-parse.h"

#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S_PRESET_SHADER "preset_shader"
#define S_SHADER_FILE_PATH "shader_file_path"
#define S_OVERRIDE_EFFECT "override_entire_effect"
#define S_SHADER_TEXT "shader_text"
#define S_RELOAD_SHADER "reload_shader"
#define S_COMPILE_STATUS "compile_status"
#define S_CUSTOM_LOW_HZ "custom_low_hz"
#define S_CUSTOM_HIGH_HZ "custom_high_hz"
#define S_CUSTOM_MODE "custom_mode"
#define S_MIX_FADER_SLOT "mix_fader_slot"
#define S_REACTIVITY_FADER_SLOT "reactivity_fader_slot"
#define S_MIX_FADER_STATUS "vs_mix_fader_status"
#define S_REACTIVITY_FADER_STATUS "vs_reactivity_fader_status"
#define S_MIX_FADER_RESET "vs_mix_fader_reset"
#define S_REACTIVITY_FADER_RESET "vs_reactivity_fader_reset"
#define S_EFFECT_MIX "vs_effect_mix_pct"
#define S_AUDIO_REACTIVITY "vs_audio_reactivity_pct"
#define S_FADER_NOT_LOADED "vs_shader_fader_not_loaded"

#define PRESET_CUSTOM_FILE "__custom_file__"
#define PRESET_CUSTOM_TEXT "__custom_text__"

struct vs_shader_param_fader_cfg {
	char target_param[64];
	int fader_slot;
	double min_val;
	double max_val;
};

struct vs_user_uniform_binding {
	gs_eparam_t *eparam;
	enum vs_shader_param_type type;
	float f_val;
	int i_val;
	bool b_val;
	struct vec4 c_val;
	gs_image_file_ex_t image_file;
	char image_path[512];
	bool image_loaded;
};

struct vs_shader_filter {
	obs_source_t *context;
	struct vs_audio_tap tap;

	char preset_shader[128];
	char shader_file_path[512];
	char *shader_text;
	bool override_entire_effect;
	char last_compiled_signature[1024];
	bool in_create;
	bool rebuilding_props;

	float custom_low_hz;
	float custom_high_hz;
	enum vs_hz_band_mode custom_mode;
	float current_custom_band;

	gs_effect_t *effect;
	bool uses_feedback;
	char compile_error[1024];

	struct vs_shader_parse_result parsed;
	struct vs_user_uniform_binding user_bindings[VS_SHADER_MAX_UNIFORMS];
	bool panic_passthrough;
	struct vs_panic_param_slot panic_slots[VS_SHADER_MAX_UNIFORMS];

	/* Built-in effect parameter handles */
	gs_eparam_t *p_image;
	gs_eparam_t *p_previous_image;
	gs_eparam_t *p_previous_output;
	gs_eparam_t *p_uv_offset;
	gs_eparam_t *p_uv_scale;
	gs_eparam_t *p_uv_pixel_interval;
	gs_eparam_t *p_uv_size;
	gs_eparam_t *p_elapsed_time;
	gs_eparam_t *p_loops;
	gs_eparam_t *p_loop_second;
	gs_eparam_t *p_rand_f;
	gs_eparam_t *p_rand_instance_f;
	gs_eparam_t *p_rand_activation_f;

	/* Visual Sidechain audio uniform handles (R-SHD-1, R-SHD-2) */
	gs_eparam_t *p_audio_level;
	gs_eparam_t *p_audio_sub_bass;
	gs_eparam_t *p_audio_bass;
	gs_eparam_t *p_audio_mid;
	gs_eparam_t *p_audio_high;
	gs_eparam_t *p_audio_kick;
	gs_eparam_t *p_audio_snare;
	gs_eparam_t *p_audio_beat;
	gs_eparam_t *p_audio_spectral_centroid;
	gs_eparam_t *p_audio_spectral_flux;
	gs_eparam_t *p_audio_custom_band;
	gs_eparam_t *p_audio_peak;
	gs_eparam_t *p_audio_magnitude;
	gs_eparam_t *p_audio_spectrum;
	gs_eparam_t *p_audio_fft;

	/* Fader slot uniform handles & live Fadester control bindings (R-MOD-7) */
	gs_eparam_t *p_fader[VS_FADER_SLOT_COUNT];
	gs_eparam_t *p_vs_effect_mix;
	gs_eparam_t *p_vs_reactivity_scale;
	float effect_mix;       /* 0..1 slider; a bound Fadester slot scales it */
	float audio_reactivity; /* 0..1 slider; a bound Fadester slot scales it */
	int mix_fader_slot;
	int reactivity_fader_slot;
	struct vs_shader_param_fader_cfg param_faders[VS_SHADER_PARAM_FADER_SLOTS];

	/* Ping-pong render targets for feedback shaders (R-UP-5, fft-feedback-trails.shader) */
	gs_texrender_t *input_texrender[2];
	gs_texrender_t *output_texrender[2];
	int ping_pong_idx;
	bool feedback_seeded;
	uint32_t feedback_width;
	uint32_t feedback_height;
	enum gs_color_space feedback_space;

	float elapsed_time;
	float rand_f;
	float rand_instance_f;
	float rand_activation_f;
};

static float next_unit_rand(void)
{
	return (float)rand() / (float)RAND_MAX;
}

static uint64_t fnv1a_64(const char *s, size_t *out_len)
{
	uint64_t hash = 14695981039346656037ULL;
	size_t len = 0u;
	if (s) {
		for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
			hash ^= (uint64_t)(*p);
			hash *= 1099511628211ULL;
			len++;
		}
	}
	if (out_len)
		*out_len = len;
	return hash;
}

static uint32_t vec4_to_abgr_u32(const double rgba[4])
{
	const uint32_t r = (uint32_t)(fmin(fmax(rgba[0], 0.0), 1.0) * 255.0 + 0.5);
	const uint32_t g = (uint32_t)(fmin(fmax(rgba[1], 0.0), 1.0) * 255.0 + 0.5);
	const uint32_t b = (uint32_t)(fmin(fmax(rgba[2], 0.0), 1.0) * 255.0 + 0.5);
	const uint32_t a = (uint32_t)(fmin(fmax(rgba[3], 0.0), 1.0) * 255.0 + 0.5);
	return (a << 24) | (b << 16) | (g << 8) | r;
}

/* Caller must hold obs_enter_graphics(). */
static void destroy_feedback_targets_locked(struct vs_shader_filter *sf)
{
	for (int i = 0; i < 2; i++) {
		gs_texrender_destroy(sf->input_texrender[i]);
		gs_texrender_destroy(sf->output_texrender[i]);
		sf->input_texrender[i] = NULL;
		sf->output_texrender[i] = NULL;
	}
	sf->feedback_seeded = false;
	sf->ping_pong_idx = 0;
	sf->feedback_width = 0;
	sf->feedback_height = 0;
}

/* Caller must hold obs_enter_graphics(). */
static void destroy_shader_effect_locked(struct vs_shader_filter *sf)
{
	destroy_feedback_targets_locked(sf);
	for (size_t i = 0; i < VS_SHADER_MAX_UNIFORMS; i++) {
		if (sf->user_bindings[i].image_loaded) {
			gs_image_file_ex_free(&sf->user_bindings[i].image_file);
			sf->user_bindings[i].image_loaded = false;
			sf->user_bindings[i].image_path[0] = '\0';
		}
	}
	if (sf->effect) {
		gs_effect_destroy(sf->effect);
		sf->effect = NULL;
	}
	vs_shader_parse_result_free(&sf->parsed);
	memset(sf->user_bindings, 0, sizeof(sf->user_bindings));
	memset(sf->panic_slots, 0, sizeof(sf->panic_slots));
	sf->p_audio_custom_band = NULL;
	sf->p_vs_effect_mix = NULL;
	sf->p_vs_reactivity_scale = NULL;
}

static void destroy_shader_effect(struct vs_shader_filter *sf)
{
	obs_enter_graphics();
	destroy_shader_effect_locked(sf);
	obs_leave_graphics();
}

static char *read_text_file_utf8(const char *path)
{
	if (!path || path[0] == '\0')
		return NULL;
	return os_quick_read_utf8_file(path);
}

static void resolve_shader_path_and_base_dir(const struct vs_shader_filter *sf, char *out_file_path, size_t file_cap,
					     char *out_base_dir, size_t dir_cap)
{
	out_file_path[0] = '\0';
	out_base_dir[0] = '\0';

	if (strcmp(sf->preset_shader, PRESET_CUSTOM_FILE) == 0) {
		snprintf(out_file_path, file_cap, "%s", sf->shader_file_path);
	} else if (strcmp(sf->preset_shader, PRESET_CUSTOM_TEXT) != 0) {
		char rel[256];
		snprintf(rel, sizeof(rel), "shaders/%s", sf->preset_shader);
		char *mod_file = obs_module_file(rel);
		if (mod_file) {
			snprintf(out_file_path, file_cap, "%s", mod_file);
			bfree(mod_file);
		}
	}

	if (out_file_path[0] != '\0') {
		snprintf(out_base_dir, dir_cap, "%s", out_file_path);
		char *slash1 = strrchr(out_base_dir, '/');
		char *slash2 = strrchr(out_base_dir, '\\');
		char *slash = slash1;
		if (!slash || (slash2 && slash2 > slash))
			slash = slash2;
		if (slash)
			*slash = '\0';
		else
			out_base_dir[0] = '\0';
	} else {
		char *mod_dir = obs_module_file("shaders");
		if (mod_dir) {
			snprintf(out_base_dir, dir_cap, "%s", mod_dir);
			bfree(mod_dir);
		}
	}
}

/* Caller must hold obs_enter_graphics(). */
static void update_user_uniform_values_locked(struct vs_shader_filter *sf, obs_data_t *settings)
{
	if (!settings)
		return;

	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		const struct vs_shader_param_info *info = &sf->parsed.params[i];
		struct vs_user_uniform_binding *b = &sf->user_bindings[i];

		if (info->type == VS_SHADER_PARAM_FLOAT) {
			if (info->has_default)
				obs_data_set_default_double(settings, info->name, info->default_val[0]);
			b->f_val = (float)obs_data_get_double(settings, info->name);
		} else if (info->type == VS_SHADER_PARAM_INT) {
			if (info->has_default)
				obs_data_set_default_int(settings, info->name,
							 (long long)llround(info->default_val[0]));
			b->i_val = (int)obs_data_get_int(settings, info->name);
		} else if (info->type == VS_SHADER_PARAM_BOOL) {
			if (info->has_default)
				obs_data_set_default_bool(settings, info->name, info->default_val[0] != 0.0);
			b->b_val = obs_data_get_bool(settings, info->name);
		} else if (info->type == VS_SHADER_PARAM_COLOR) {
			if (info->has_default)
				obs_data_set_default_int(settings, info->name,
							 (long long)vec4_to_abgr_u32(info->default_val));
			const uint32_t abgr = (uint32_t)obs_data_get_int(settings, info->name);
			vec4_from_rgba(&b->c_val, abgr);
		} else if (info->type == VS_SHADER_PARAM_TEXTURE) {
			const char *path = obs_data_get_string(settings, info->name);
			if (!path)
				path = "";
			if (strcmp(b->image_path, path) != 0) {
				if (b->image_loaded) {
					gs_image_file_ex_free(&b->image_file);
					b->image_loaded = false;
				}
				snprintf(b->image_path, sizeof(b->image_path), "%s", path);
				if (path[0] != '\0') {
					gs_image_file_ex_init(&b->image_file, path, GS_IMAGE_ALPHA_PREMULTIPLY);
					gs_image_file_ex_init_texture(&b->image_file);
					b->image_loaded = true;
				}
			}
		}
	}
}

/* Caller holds the graphics lock. The old percent keys can also be ordinary shader uniforms.
 * Resolve their ownership only after successful compilation, and initialize the new keys once so switching away
 * from a shader that owns a legacy name cannot later reinterpret its uniform as a host control. */
static void sync_static_controls_from_settings_locked(struct vs_shader_filter *sf, obs_data_t *settings)
{
	if (!sf || !settings)
		return;
	const char *const current_keys[] = {S_EFFECT_MIX, S_AUDIO_REACTIVITY};
	const char *const legacy_keys[] = {"effect_mix", "audio_reactivity"};
	if (sf->effect && sf->parsed.full_effect_source) {
		for (size_t k = 0; k < 2u; k++) {
			if (obs_data_has_user_value(settings, current_keys[k]))
				continue;
			bool shader_owned = false;
			for (size_t i = 0; i < sf->parsed.param_count; i++) {
				if (strcmp(sf->parsed.params[i].name, legacy_keys[k]) == 0) {
					shader_owned = true;
					break;
				}
			}
			const bool migrate_legacy = !shader_owned && obs_data_has_user_value(settings, legacy_keys[k]);
			const double value = migrate_legacy ? obs_data_get_double(settings, legacy_keys[k])
							    : obs_data_get_double(settings, current_keys[k]);
			obs_data_set_double(settings, current_keys[k], value);
			/* A proven host percentage must not become a future shader's same-named parameter. */
			if (migrate_legacy)
				obs_data_unset_user_value(settings, legacy_keys[k]);
		}
	}
	sf->effect_mix = (float)fmin(fmax(obs_data_get_double(settings, S_EFFECT_MIX) * 0.01, 0.0), 1.0);
	sf->audio_reactivity = (float)fmin(fmax(obs_data_get_double(settings, S_AUDIO_REACTIVITY) * 0.01, 0.0), 1.0);
}

static void compile_shader(struct vs_shader_filter *sf, obs_data_t *settings)
{
	char file_path[512];
	char base_dir[512];
	resolve_shader_path_and_base_dir(sf, file_path, sizeof(file_path), base_dir, sizeof(base_dir));

	char *raw_source = NULL;
	const char *shader_label = sf->preset_shader;

	if (strcmp(sf->preset_shader, PRESET_CUSTOM_TEXT) == 0) {
		raw_source = bstrdup(sf->shader_text ? sf->shader_text : "");
		shader_label = "inline shader";
	} else {
		if (file_path[0] == '\0') {
			obs_enter_graphics();
			destroy_shader_effect_locked(sf);
			sf->uses_feedback = false;
			sf->feedback_seeded = false;
			snprintf(sf->compile_error, sizeof(sf->compile_error), "No shader file path specified.");
			obs_leave_graphics();
			blog(LOG_WARNING, "[visual-sidechain] No shader file path specified.");
			return;
		}
		raw_source = read_text_file_utf8(file_path);
		shader_label = file_path;
		if (!raw_source) {
			obs_enter_graphics();
			destroy_shader_effect_locked(sf);
			sf->uses_feedback = false;
			sf->feedback_seeded = false;
			snprintf(sf->compile_error, sizeof(sf->compile_error), "Shader file not found: %s", file_path);
			obs_leave_graphics();
			blog(LOG_WARNING, "[visual-sidechain] Shader file not found: %s", file_path);
			return;
		}
	}

	const bool is_full_effect = (strcmp(sf->preset_shader, PRESET_CUSTOM_FILE) == 0 ||
				     strcmp(sf->preset_shader, PRESET_CUSTOM_TEXT) == 0)
					    ? sf->override_entire_effect
					    : false;

	struct vs_shader_parse_result local_parsed;
	const enum vs_shader_error perr =
		vs_shader_ExpandAndParse(raw_source, base_dir, is_full_effect, NULL, NULL, &local_parsed);
	bfree(raw_source);

	if (perr != VS_SHADER_OK) {
		obs_enter_graphics();
		destroy_shader_effect_locked(sf);
		sf->uses_feedback = false;
		sf->feedback_seeded = false;
		snprintf(sf->compile_error, sizeof(sf->compile_error), "Shader parse error (%s): %s", shader_label,
			 local_parsed.error_msg);
		obs_leave_graphics();
		blog(LOG_WARNING, "[visual-sidechain] Shader parse error (%s): %s", shader_label,
		     local_parsed.error_msg);
		vs_shader_parse_result_free(&local_parsed);
		return;
	}

	const bool uses_fb =
		(local_parsed.expanded_source && (strstr(local_parsed.expanded_source, "previous_output") != NULL ||
						  strstr(local_parsed.expanded_source, "previous_image") != NULL));

	char *gs_err = NULL;
	obs_enter_graphics();
	destroy_shader_effect_locked(sf);
	sf->compile_error[0] = '\0';
	sf->uses_feedback = uses_fb;
	sf->feedback_seeded = false;
	sf->parsed = local_parsed;

	sf->effect = gs_effect_create(sf->parsed.full_effect_source, shader_label, &gs_err);
	if (sf->effect) {
		/* Manual Reload also enters here without the normal update's later fader synchronization. */
		sync_static_controls_from_settings_locked(sf, settings);
		sf->p_image = gs_effect_get_param_by_name(sf->effect, "image");
		sf->p_previous_image = gs_effect_get_param_by_name(sf->effect, "previous_image");
		sf->p_previous_output = gs_effect_get_param_by_name(sf->effect, "previous_output");
		sf->p_uv_offset = gs_effect_get_param_by_name(sf->effect, "uv_offset");
		sf->p_uv_scale = gs_effect_get_param_by_name(sf->effect, "uv_scale");
		sf->p_uv_pixel_interval = gs_effect_get_param_by_name(sf->effect, "uv_pixel_interval");
		sf->p_uv_size = gs_effect_get_param_by_name(sf->effect, "uv_size");
		sf->p_elapsed_time = gs_effect_get_param_by_name(sf->effect, "elapsed_time");
		sf->p_loops = gs_effect_get_param_by_name(sf->effect, "loops");
		sf->p_loop_second = gs_effect_get_param_by_name(sf->effect, "loop_second");
		sf->p_rand_f = gs_effect_get_param_by_name(sf->effect, "rand_f");
		sf->p_rand_instance_f = gs_effect_get_param_by_name(sf->effect, "rand_instance_f");
		sf->p_rand_activation_f = gs_effect_get_param_by_name(sf->effect, "rand_activation_f");

		sf->p_audio_level = gs_effect_get_param_by_name(sf->effect, "audio_level");
		sf->p_audio_sub_bass = gs_effect_get_param_by_name(sf->effect, "audio_sub_bass");
		sf->p_audio_bass = gs_effect_get_param_by_name(sf->effect, "audio_bass");
		sf->p_audio_mid = gs_effect_get_param_by_name(sf->effect, "audio_mid");
		sf->p_audio_high = gs_effect_get_param_by_name(sf->effect, "audio_high");
		sf->p_audio_kick = gs_effect_get_param_by_name(sf->effect, "audio_kick");
		sf->p_audio_snare = gs_effect_get_param_by_name(sf->effect, "audio_snare");
		sf->p_audio_beat = gs_effect_get_param_by_name(sf->effect, "audio_beat");
		sf->p_audio_spectral_centroid = gs_effect_get_param_by_name(sf->effect, "audio_spectral_centroid");
		sf->p_audio_spectral_flux = gs_effect_get_param_by_name(sf->effect, "audio_spectral_flux");
		sf->p_audio_custom_band = gs_effect_get_param_by_name(sf->effect, "audio_custom_band");
		sf->p_audio_peak = gs_effect_get_param_by_name(sf->effect, "audio_peak");
		sf->p_audio_magnitude = gs_effect_get_param_by_name(sf->effect, "audio_magnitude");
		sf->p_audio_spectrum = gs_effect_get_param_by_name(sf->effect, "audio_spectrum");
		sf->p_audio_fft = gs_effect_get_param_by_name(sf->effect, "audio_fft");

		static const char *const k_fader_uniform_names[VS_FADER_SLOT_COUNT] = {
			"fader_crossfader", "fader_1",    "fader_2",    "fader_3",      "fader_4",    "fader_5",
			"fader_6",          "fader_7",    "fader_8",    "fader_master", "fader_fx_1", "fader_fx_2",
			"fader_fx_3",       "fader_fx_4", "fader_fx_5", "fader_fx_6",   "fader_fx_7", "fader_fx_8",
		};
		for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
			sf->p_fader[i] = gs_effect_get_param_by_name(sf->effect, k_fader_uniform_names[i]);
		}
		sf->p_vs_effect_mix = gs_effect_get_param_by_name(sf->effect, "vs_effect_mix");
		sf->p_vs_reactivity_scale = gs_effect_get_param_by_name(sf->effect, "vs_reactivity_scale");

		for (size_t i = 0; i < sf->parsed.param_count; i++) {
			sf->user_bindings[i].eparam =
				gs_effect_get_param_by_name(sf->effect, sf->parsed.params[i].name);
			sf->user_bindings[i].type = sf->parsed.params[i].type;
			snprintf(sf->panic_slots[i].name, sizeof(sf->panic_slots[i].name), "%s",
				 sf->parsed.params[i].name);
		}

		if (settings)
			update_user_uniform_values_locked(sf, settings);

		if (vs_fader_bridge_is_panic_active()) {
			for (size_t i = 0; i < sf->parsed.param_count; i++)
				sf->panic_slots[i].current_value = (double)sf->user_bindings[i].f_val;
			vs_fader_bridge_apply_panic_to_shader(&sf->panic_passthrough, sf->panic_slots,
							      sf->parsed.param_count);
		}
	} else {
		snprintf(sf->compile_error, sizeof(sf->compile_error), "HLSL compiler error (%s): %s", shader_label,
			 gs_err ? gs_err : "unknown error");
	}

	const bool ok = (sf->effect != NULL);
	const size_t compiled_params = sf->parsed.param_count;
	obs_leave_graphics();

	if (!ok) {
		blog(LOG_WARNING, "[visual-sidechain] HLSL compiler error (%s): %s", shader_label,
		     gs_err ? gs_err : "unknown error");
		bfree(gs_err);
		return;
	}
	bfree(gs_err);

	blog(LOG_INFO, "[visual-sidechain] Compiled shader '%s' (%zu user uniforms, feedback=%d)", sf->preset_shader,
	     compiled_params, uses_fb ? 1 : 0);
}

static const char *shader_filter_get_name(void *unused)
{
	(void)unused;
	return obs_module_text("VisualSidechain.ShaderFilter");
}

static void param_fader_key(char *buf, size_t cap, int idx, const char *suffix)
{
	snprintf(buf, cap, "param_fader_%d_%s", idx + 1, suffix);
}

static bool is_fader_bindable_shader_param(const struct vs_shader_param_info *info)
{
	if (!info || info->name[0] == '\0')
		return false;
	if (info->type == VS_SHADER_PARAM_FLOAT)
		return true;
	if (info->type == VS_SHADER_PARAM_INT) {
		const bool is_select = (info->option_count > 0u) && (strcmp(info->widget_type, "select") == 0 ||
								     strcmp(info->widget_type, "list") == 0);
		return !is_select;
	}
	return false;
}

/* Caller must hold obs_enter_graphics(). */
static bool find_bindable_shader_param_locked(const struct vs_shader_filter *sf, const char *param_name,
					      struct vs_shader_param_info *out_info)
{
	if (!sf || !param_name || param_name[0] == '\0')
		return false;
	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		const struct vs_shader_param_info *info = &sf->parsed.params[i];
		if (is_fader_bindable_shader_param(info) && strcmp(info->name, param_name) == 0) {
			if (out_info)
				*out_info = *info;
			return true;
		}
	}
	return false;
}

/* Caller must hold obs_enter_graphics(). */
static void sync_fader_controls_from_settings_locked(struct vs_shader_filter *sf, obs_data_t *settings)
{
	if (!sf || !settings)
		return;
	sync_static_controls_from_settings_locked(sf, settings);

	const int mix_slot = (int)obs_data_get_int(settings, S_MIX_FADER_SLOT);
	sf->mix_fader_slot = (mix_slot >= 0 && mix_slot < VS_FADER_SLOT_COUNT) ? mix_slot : VS_FADER_SLOT_NONE;

	const int react_slot = (int)obs_data_get_int(settings, S_REACTIVITY_FADER_SLOT);
	sf->reactivity_fader_slot = (react_slot >= 0 && react_slot < VS_FADER_SLOT_COUNT) ? react_slot
											  : VS_FADER_SLOT_NONE;

	char k_target[64];
	char k_slot[64];
	char k_min[64];
	char k_max[64];
	char k_seed[64];

	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		struct vs_shader_param_fader_cfg prev = sf->param_faders[k];
		param_fader_key(k_target, sizeof(k_target), k, "target");
		param_fader_key(k_slot, sizeof(k_slot), k, "slot");
		param_fader_key(k_min, sizeof(k_min), k, "min");
		param_fader_key(k_max, sizeof(k_max), k, "max");
		param_fader_key(k_seed, sizeof(k_seed), k, "seeded");

		const char *target = obs_data_get_string(settings, k_target);
		if (!target)
			target = "";
		for (int prev_k = 0; prev_k < k; prev_k++) {
			if (target[0] != '\0' && strcmp(target, sf->param_faders[prev_k].target_param) == 0) {
				/* A uniform has one value per frame; keep the first binding and clear duplicates. */
				obs_data_set_string(settings, k_target, "");
				target = "";
				break;
			}
		}

		if (target[0] != '\0') {
			char seed_id[220];
			snprintf(seed_id, sizeof(seed_id), "%s::%s", sf->preset_shader, target);
			const char *prev_seed = obs_data_get_string(settings, k_seed);
			const bool same_as_prev = (strcmp(prev.target_param, target) == 0);
			const bool has_explicit_bounds = (obs_data_has_user_value(settings, k_min) ||
							  obs_data_has_user_value(settings, k_max)) &&
							 same_as_prev;
			if ((!same_as_prev || !prev_seed || strcmp(prev_seed, seed_id) != 0) && !has_explicit_bounds &&
			    (!same_as_prev || (!obs_data_has_user_value(settings, k_min) &&
					       !obs_data_has_user_value(settings, k_max)))) {
				struct vs_shader_param_info pinfo;
				if (find_bindable_shader_param_locked(sf, target, &pinfo)) {
					obs_data_set_default_double(settings, k_min, pinfo.min_val);
					obs_data_set_default_double(settings, k_max, pinfo.max_val);
					obs_data_set_string(settings, k_seed, seed_id);
				}
			}
		} else {
			obs_data_set_string(settings, k_seed, "");
		}

		snprintf(sf->param_faders[k].target_param, sizeof(sf->param_faders[k].target_param), "%s", target);
		const int fslot = (int)obs_data_get_int(settings, k_slot);
		sf->param_faders[k].fader_slot = (fslot >= 0 && fslot < VS_FADER_SLOT_COUNT) ? fslot
											     : VS_FADER_SLOT_NONE;
		sf->param_faders[k].min_val = obs_data_get_double(settings, k_min);
		sf->param_faders[k].max_val = obs_data_get_double(settings, k_max);
	}
}

static void shader_filter_get_defaults(obs_data_t *settings)
{
	vs_audio_tap_get_defaults(settings);
	obs_data_set_default_string(settings, S_PRESET_SHADER, "fft-spectral-aura.shader");
	obs_data_set_default_string(settings, S_SHADER_FILE_PATH, "");
	obs_data_set_default_bool(settings, S_OVERRIDE_EFFECT, false);
	obs_data_set_default_string(
		settings, S_SHADER_TEXT,
		"float4 mainImage(VertData v_in) : TARGET\n{\n\tfloat4 c = image.Sample(texture_sampler, v_in.uv);\n\treturn float4(c.rgb * (1.0 + audio_kick * 0.5), c.a);\n}\n");
	obs_data_set_default_double(settings, S_CUSTOM_LOW_HZ, 60.0);
	obs_data_set_default_double(settings, S_CUSTOM_HIGH_HZ, 150.0);
	obs_data_set_default_int(settings, S_CUSTOM_MODE, (long long)VS_HZ_MODE_AVERAGE);
	obs_data_set_default_double(settings, S_EFFECT_MIX, 100.0);
	obs_data_set_default_double(settings, S_AUDIO_REACTIVITY, 100.0);
	obs_data_set_default_int(settings, S_MIX_FADER_SLOT, VS_FADER_SLOT_NONE);
	obs_data_set_default_int(settings, S_REACTIVITY_FADER_SLOT, VS_FADER_SLOT_NONE);

	char key[64];
	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		param_fader_key(key, sizeof(key), k, "target");
		obs_data_set_default_string(settings, key, "");
		param_fader_key(key, sizeof(key), k, "slot");
		obs_data_set_default_int(settings, key, VS_FADER_SLOT_NONE);
		param_fader_key(key, sizeof(key), k, "min");
		obs_data_set_default_double(settings, key, 0.0);
		param_fader_key(key, sizeof(key), k, "max");
		obs_data_set_default_double(settings, key, 1.0);
		param_fader_key(key, sizeof(key), k, "seeded");
		obs_data_set_default_string(settings, key, "");
	}
}

static void shader_filter_update(void *data, obs_data_t *settings)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	vs_audio_tap_update_from_settings(&sf->tap, sf->context, settings);

	sf->custom_low_hz = (float)obs_data_get_double(settings, S_CUSTOM_LOW_HZ);
	sf->custom_high_hz = (float)obs_data_get_double(settings, S_CUSTOM_HIGH_HZ);
	sf->custom_mode = (enum vs_hz_band_mode)obs_data_get_int(settings, S_CUSTOM_MODE);

	char prev_preset[128];
	char prev_file_path[512];
	snprintf(prev_preset, sizeof(prev_preset), "%s", sf->preset_shader);
	snprintf(prev_file_path, sizeof(prev_file_path), "%s", sf->shader_file_path);
	const bool prev_override = sf->override_entire_effect;

	const char *preset = obs_data_get_string(settings, S_PRESET_SHADER);
	if (!preset || preset[0] == '\0')
		preset = "fft-spectral-aura.shader";
	snprintf(sf->preset_shader, sizeof(sf->preset_shader), "%s", preset);

	const char *fpath = obs_data_get_string(settings, S_SHADER_FILE_PATH);
	snprintf(sf->shader_file_path, sizeof(sf->shader_file_path), "%s", fpath ? fpath : "");

	const bool is_custom = strcmp(sf->preset_shader, PRESET_CUSTOM_FILE) == 0 ||
			       strcmp(sf->preset_shader, PRESET_CUSTOM_TEXT) == 0;
	sf->override_entire_effect = is_custom && obs_data_get_bool(settings, S_OVERRIDE_EFFECT);

	const char *stext = obs_data_get_string(settings, S_SHADER_TEXT);
	bfree(sf->shader_text);
	sf->shader_text = bstrdup(stext ? stext : "");

	size_t text_len = 0u;
	const uint64_t text_hash =
		(strcmp(sf->preset_shader, PRESET_CUSTOM_TEXT) == 0) ? fnv1a_64(sf->shader_text, &text_len) : 0ULL;

	char sig[1024];
	snprintf(sig, sizeof(sig), "%s|%s|%d|%zu|%016llx", sf->preset_shader, sf->shader_file_path,
		 sf->override_entire_effect ? 1 : 0, text_len, (unsigned long long)text_hash);

	obs_enter_graphics();
	const bool needs_compile = (strcmp(sig, sf->last_compiled_signature) != 0) ||
				   (!sf->effect && sf->compile_error[0] == '\0');
	obs_leave_graphics();

	if (needs_compile) {
		snprintf(sf->last_compiled_signature, sizeof(sf->last_compiled_signature), "%s", sig);
		compile_shader(sf, settings);

		obs_enter_graphics();
		sync_fader_controls_from_settings_locked(sf, settings);
		obs_leave_graphics();

		/* Rebuild Properties immediately when switching presets, selecting a file, toggling full-effect mode,
		 * or clicking Defaults. Avoid rebuilding on every keystroke inside the inline HLSL multiline editor
		 * so Qt text cursor focus is preserved while typing (Reload & Recompile rebuilds inline uniforms). */
		const bool ui_source_switched = (strcmp(prev_preset, sf->preset_shader) != 0) ||
						(strcmp(prev_file_path, sf->shader_file_path) != 0) ||
						(prev_override != sf->override_entire_effect);
		if (!sf->in_create && !sf->rebuilding_props && ui_source_switched && sf->context) {
			sf->rebuilding_props = true;
			obs_source_update_properties(sf->context);
			sf->rebuilding_props = false;
		}
	} else {
		obs_enter_graphics();
		update_user_uniform_values_locked(sf, settings);
		sync_fader_controls_from_settings_locked(sf, settings);
		obs_leave_graphics();
	}
}

static void shader_filter_save(void *data, obs_data_t *settings)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	if (!sf || !settings)
		return;

	/* If OBS saves the scene collection while Panic is active, write pre-panic uniform values into settings
	 * for serialization while keeping runtime panic_passthrough active on live video output. */
	obs_enter_graphics();
	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		if (sf->panic_slots[i].saved_valid && sf->parsed.params[i].type == VS_SHADER_PARAM_FLOAT) {
			obs_data_set_double(settings, sf->parsed.params[i].name, sf->panic_slots[i].saved_value);
		}
	}
	obs_leave_graphics();
}

static void *shader_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)bzalloc(sizeof(*sf));
	sf->context = source;
	sf->rand_instance_f = next_unit_rand();
	sf->rand_activation_f = next_unit_rand();
	sf->panic_passthrough = vs_fader_bridge_is_panic_active();
	sf->effect_mix = 1.0f;
	sf->audio_reactivity = 1.0f;
	sf->mix_fader_slot = VS_FADER_SLOT_NONE;
	sf->reactivity_fader_slot = VS_FADER_SLOT_NONE;
	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++)
		sf->param_faders[k].fader_slot = VS_FADER_SLOT_NONE;

	vs_audio_tap_init(&sf->tap, true);

	sf->in_create = true;
	shader_filter_update(sf, settings);
	sf->in_create = false;
	return sf;
}

static void shader_filter_destroy(void *data)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	if (!sf)
		return;

	vs_audio_tap_destroy(&sf->tap);
	destroy_shader_effect(sf);

	bfree(sf->shader_text);
	bfree(sf);
}

static void shader_filter_activate(void *data)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	sf->rand_activation_f = next_unit_rand();
}

static void shader_filter_add(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	if (!sf || !sf->context)
		return;

	obs_data_t *s = obs_source_get_settings(sf->context);
	if (s) {
		vs_audio_tap_update_from_settings(&sf->tap, sf->context, s);
		obs_data_release(s);
	}
}

static void shader_filter_video_tick(void *data, float seconds)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	vs_audio_tap_tick(&sf->tap, sf->context, seconds);

	sf->current_custom_band = vs_analyzer_get_signal(&sf->tap.analyzer, &sf->tap.current_frame, VS_SIG_CUSTOM_HZ,
							 sf->custom_low_hz, sf->custom_high_hz, sf->custom_mode);

	sf->elapsed_time += seconds;
	sf->rand_f = next_unit_rand();

	const uint64_t elapsed_ns = (uint64_t)(seconds * 1e9f);
	obs_enter_graphics();
	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		struct vs_user_uniform_binding *b = &sf->user_bindings[i];
		if (b->image_loaded && b->image_file.is_animated_gif) {
			if (gs_image_file_ex_tick(&b->image_file, elapsed_ns)) {
				gs_image_file_ex_update_texture(&b->image_file);
			}
		}
	}
	obs_leave_graphics();
}

/* A bound slot scales the static slider; with no slot, or when Midi Fadester is not loaded, the slider
 * value stands alone so a missing fader plugin never silences or dries the effect. */
static float fader_snapshot_scale(const float faders[VS_FADER_SLOT_COUNT], int slot, bool fadester_loaded)
{
	if (slot < 0 || !fadester_loaded)
		return 1.0f;
	return slot < VS_FADER_SLOT_COUNT ? faders[slot] : 0.0f;
}

static float effective_mix_scale(const struct vs_shader_filter *sf, const float faders[VS_FADER_SLOT_COUNT],
				 bool fadester_loaded)
{
	return sf->effect_mix * fader_snapshot_scale(faders, sf->mix_fader_slot, fadester_loaded);
}

static void bind_all_shader_uniforms(struct vs_shader_filter *sf, uint32_t cx, uint32_t cy,
				     const float faders[VS_FADER_SLOT_COUNT], bool fadester_loaded)
{
	struct vec2 uv_offset = {0.0f, 0.0f};
	struct vec2 uv_scale = {1.0f, 1.0f};
	struct vec2 uv_size = {(float)cx, (float)cy};
	struct vec2 uv_pixel_interval = {cx > 0 ? (1.0f / (float)cx) : 0.0f, cy > 0 ? (1.0f / (float)cy) : 0.0f};

	if (sf->p_uv_offset)
		gs_effect_set_vec2(sf->p_uv_offset, &uv_offset);
	if (sf->p_uv_scale)
		gs_effect_set_vec2(sf->p_uv_scale, &uv_scale);
	if (sf->p_uv_size)
		gs_effect_set_vec2(sf->p_uv_size, &uv_size);
	if (sf->p_uv_pixel_interval)
		gs_effect_set_vec2(sf->p_uv_pixel_interval, &uv_pixel_interval);

	if (sf->p_elapsed_time)
		gs_effect_set_float(sf->p_elapsed_time, sf->elapsed_time);
	if (sf->p_loops)
		gs_effect_set_float(sf->p_loops, floorf(sf->elapsed_time));
	if (sf->p_loop_second)
		gs_effect_set_float(sf->p_loop_second, fmodf(sf->elapsed_time, 1.0f));
	if (sf->p_rand_f)
		gs_effect_set_float(sf->p_rand_f, sf->rand_f);
	if (sf->p_rand_instance_f)
		gs_effect_set_float(sf->p_rand_instance_f, sf->rand_instance_f);
	if (sf->p_rand_activation_f)
		gs_effect_set_float(sf->p_rand_activation_f, sf->rand_activation_f);

	const float mix_scale = effective_mix_scale(sf, faders, fadester_loaded);
	const float react_scale =
		sf->audio_reactivity * fader_snapshot_scale(faders, sf->reactivity_fader_slot, fadester_loaded);

	if (sf->p_vs_effect_mix)
		gs_effect_set_float(sf->p_vs_effect_mix, mix_scale);
	if (sf->p_vs_reactivity_scale)
		gs_effect_set_float(sf->p_vs_reactivity_scale, react_scale);

	/* GPU helpers scale the audio textures; only scalar uniforms need CPU scaling here. */
	const struct vs_audio_frame *f = &sf->tap.current_frame;
	if (sf->p_audio_level)
		gs_effect_set_float(sf->p_audio_level, f->level * react_scale);
	if (sf->p_audio_sub_bass)
		gs_effect_set_float(sf->p_audio_sub_bass, f->sub_bass * react_scale);
	if (sf->p_audio_bass)
		gs_effect_set_float(sf->p_audio_bass, f->bass * react_scale);
	if (sf->p_audio_mid)
		gs_effect_set_float(sf->p_audio_mid, f->mid * react_scale);
	if (sf->p_audio_high)
		gs_effect_set_float(sf->p_audio_high, f->high * react_scale);
	if (sf->p_audio_kick)
		gs_effect_set_float(sf->p_audio_kick, f->kick * react_scale);
	if (sf->p_audio_snare)
		gs_effect_set_float(sf->p_audio_snare, f->snare * react_scale);
	if (sf->p_audio_beat)
		gs_effect_set_float(sf->p_audio_beat, f->beat * react_scale);
	if (sf->p_audio_spectral_centroid)
		gs_effect_set_float(sf->p_audio_spectral_centroid, f->spectral_centroid);
	if (sf->p_audio_spectral_flux)
		gs_effect_set_float(sf->p_audio_spectral_flux, f->spectral_flux * react_scale);
	if (sf->p_audio_custom_band)
		gs_effect_set_float(sf->p_audio_custom_band, sf->current_custom_band * react_scale);

	/* Legacy obs-shaderfilter compatibility uniforms (R-SHD-1, R-UP-6) */
	if (sf->p_audio_peak)
		gs_effect_set_float(sf->p_audio_peak, fmaxf(f->kick, f->level) * react_scale);
	if (sf->p_audio_magnitude)
		gs_effect_set_float(sf->p_audio_magnitude, f->level * react_scale);

	/* Dynamic FFT & Spectrum GPU textures (R-SHD-2) */
	if (sf->p_audio_spectrum && sf->tap.tex_spectrum)
		gs_effect_set_texture(sf->p_audio_spectrum, sf->tap.tex_spectrum);
	if (sf->p_audio_fft && sf->tap.tex_fft)
		gs_effect_set_texture(sf->p_audio_fft, sf->tap.tex_fft);

	/* Fader slot float uniforms (R-MOD-7) */
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		if (sf->p_fader[i])
			gs_effect_set_float(sf->p_fader[i], faders[i]);
	}

	/* User-annotated uniforms with optional Fadester parameter overrides (R-UP-4, R-MOD-7) */
	const bool panic_active = vs_fader_bridge_is_panic_active();
	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		const struct vs_shader_param_info *info = &sf->parsed.params[i];
		const struct vs_user_uniform_binding *b = &sf->user_bindings[i];
		if (!b->eparam)
			continue;

		const struct vs_shader_param_fader_cfg *pf = NULL;
		for (int k = 0; fadester_loaded && k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
			if (sf->param_faders[k].fader_slot >= 0 && sf->param_faders[k].target_param[0] != '\0' &&
			    strcmp(sf->param_faders[k].target_param, info->name) == 0) {
				pf = &sf->param_faders[k];
			}
		}

		if (b->type == VS_SHADER_PARAM_FLOAT) {
			float val = b->f_val;
			if (pf) {
				val = vs_fader_bridge_map_float_param(faders[pf->fader_slot], pf->min_val, pf->max_val,
								      info->has_min, info->min_val, info->has_max,
								      info->max_val);
			}
			if (panic_active && vs_fader_bridge_is_strobe_or_flash_param(info->name))
				val = 0.0f;
			gs_effect_set_float(b->eparam, val);
		} else if (b->type == VS_SHADER_PARAM_INT) {
			int val = b->i_val;
			if (pf && is_fader_bindable_shader_param(info)) {
				const int step = isfinite(info->step_val) && info->step_val >= 1.0
							 ? (int)fmin(info->step_val, INT_MAX)
							 : 1;
				val = vs_fader_bridge_map_int_param(faders[pf->fader_slot], pf->min_val, pf->max_val,
								    step, info->has_min, info->min_val, info->has_max,
								    info->max_val);
			}
			gs_effect_set_int(b->eparam, val);
		} else if (b->type == VS_SHADER_PARAM_BOOL) {
			gs_effect_set_bool(b->eparam, b->b_val);
		} else if (b->type == VS_SHADER_PARAM_COLOR) {
			gs_effect_set_vec4(b->eparam, &b->c_val);
		} else if (b->type == VS_SHADER_PARAM_TEXTURE && b->image_loaded && b->image_file.texture) {
			gs_effect_set_texture_srgb(b->eparam, b->image_file.texture);
		}
	}
}

static void render_feedback_pipeline(struct vs_shader_filter *sf, obs_source_t *target, uint32_t cx, uint32_t cy,
				     enum gs_color_space source_space, const float faders[VS_FADER_SLOT_COUNT],
				     bool fadester_loaded)
{
	const enum gs_color_format format = gs_get_format_from_space(source_space);
	if (sf->feedback_width != cx || sf->feedback_height != cy || sf->feedback_space != source_space) {
		/* Drop history on size/space changes and avoid retaining four full-size targets after preset switches. */
		destroy_feedback_targets_locked(sf);
		for (int i = 0; i < 2; i++) {
			sf->input_texrender[i] = gs_texrender_create(format, GS_ZS_NONE);
			sf->output_texrender[i] = gs_texrender_create(format, GS_ZS_NONE);
			if (!sf->input_texrender[i] || !sf->output_texrender[i]) {
				destroy_feedback_targets_locked(sf);
				obs_source_skip_video_filter(sf->context);
				return;
			}
		}
		sf->feedback_width = cx;
		sf->feedback_height = cy;
		sf->feedback_space = source_space;
	}
	const int cur = sf->ping_pong_idx;
	const int prev = 1 - cur;

	gs_texrender_reset(sf->input_texrender[cur]);
	if (!gs_texrender_begin_with_color_space(sf->input_texrender[cur], cx, cy, source_space)) {
		/* OBS retains dimensions after a failed GPU allocation; discard it to retry next frame. */
		destroy_feedback_targets_locked(sf);
		obs_source_skip_video_filter(sf->context);
		return;
	}
	struct vec4 clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	obs_source_video_render(target);
	gs_texrender_end(sf->input_texrender[cur]);

	gs_texture_t *cur_in_tex = gs_texrender_get_texture(sf->input_texrender[cur]);
	gs_texture_t *prev_in_tex = sf->feedback_seeded ? gs_texrender_get_texture(sf->input_texrender[prev])
							: cur_in_tex;
	gs_texture_t *prev_out_tex = sf->feedback_seeded ? gs_texrender_get_texture(sf->output_texrender[prev])
							 : cur_in_tex;

	gs_texrender_reset(sf->output_texrender[cur]);
	if (!gs_texrender_begin_with_color_space(sf->output_texrender[cur], cx, cy, source_space)) {
		destroy_feedback_targets_locked(sf);
		obs_source_skip_video_filter(sf->context);
		return;
	}
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (sf->p_image)
		gs_effect_set_texture_srgb(sf->p_image, cur_in_tex);
	if (sf->p_previous_image)
		gs_effect_set_texture_srgb(sf->p_previous_image, prev_in_tex ? prev_in_tex : cur_in_tex);
	if (sf->p_previous_output)
		gs_effect_set_texture_srgb(sf->p_previous_output, prev_out_tex ? prev_out_tex : cur_in_tex);

	bind_all_shader_uniforms(sf, cx, cy, faders, fadester_loaded);

	while (gs_effect_loop(sf->effect, "Draw")) {
		gs_draw_sprite(cur_in_tex, 0, cx, cy);
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prev_srgb);
	gs_texrender_end(sf->output_texrender[cur]);

	/* Present output_texrender[cur] to the filter chain (premultiplied alpha, R-SHD-5) */
	gs_texture_t *out_tex = gs_texrender_get_texture(sf->output_texrender[cur]);
	if (out_tex) {
		const bool srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img = gs_effect_get_param_by_name(default_effect, "image");
		gs_effect_set_texture_srgb(img, out_tex);
		while (gs_effect_loop(default_effect, "Draw")) {
			gs_draw_sprite(out_tex, 0, cx, cy);
		}
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(srgb);
	}

	sf->ping_pong_idx = prev;
	sf->feedback_seeded = true;
}

static void shader_filter_video_render(void *data, gs_effect_t *effect)
{
	(void)effect;
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;

	if (vs_fader_bridge_should_shader_passthrough(sf->panic_passthrough, sf->effect != NULL)) {
		obs_source_skip_video_filter(sf->context);
		return;
	}

	float faders[VS_FADER_SLOT_COUNT];
	vs_fader_bridge_get_slots(faders);
	const bool fadester_loaded = vs_fader_bridge_is_midi_fadester_loaded();
	if (!sf->override_entire_effect && effective_mix_scale(sf, faders, fadester_loaded) <= 1e-4f) {
		sf->feedback_seeded = false;
		obs_source_skip_video_filter(sf->context);
		return;
	}

	obs_source_t *target = obs_filter_get_target(sf->context);
	if (!target) {
		obs_source_skip_video_filter(sf->context);
		return;
	}

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);
	if (cx == 0 || cy == 0) {
		obs_source_skip_video_filter(sf->context);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space source_space = obs_source_get_color_space(
		target, sizeof(preferred_spaces) / sizeof(preferred_spaces[0]), preferred_spaces);

	if (sf->uses_feedback) {
		render_feedback_pipeline(sf, target, cx, cy, source_space, faders, fadester_loaded);
		return;
	}

	const enum gs_color_format format = gs_get_format_from_space(source_space);
	if (!obs_source_process_filter_begin_with_color_space(sf->context, format, source_space,
							      OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	bind_all_shader_uniforms(sf, cx, cy, faders, fadester_loaded);

	/* Premultiplied alpha blend around obs_source_process_filter_tech_end (R-SHD-5):
	 * obs_source_process_filter_begin_with_color_space renders the upstream source into its internal
	 * texrender with premultiplied alpha. Under OBS's default GS_BLEND_SRCALPHA blend state, a shader
	 * outputting premultiplied RGBA (e.g. 50% opacity) would be multiplied by alpha a second time (25%). */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(sf->context, sf->effect, cx, cy, "Draw");
	gs_blend_state_pop();
}

static enum gs_color_space shader_filter_get_color_space(void *data, size_t count,
							 const enum gs_color_space *preferred_spaces)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	obs_source_t *target = obs_filter_get_target(sf->context);
	if (!target)
		return GS_CS_SRGB;

	const enum gs_color_space filter_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space source_space =
		obs_source_get_color_space(target, sizeof(filter_spaces) / sizeof(filter_spaces[0]), filter_spaces);

	for (size_t i = 0; i < count; i++) {
		if (preferred_spaces[i] == source_space)
			return source_space;
	}
	return GS_CS_SRGB;
}

static bool on_preset_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)property;
	const char *preset = obs_data_get_string(settings, S_PRESET_SHADER);
	const bool is_file = (preset && strcmp(preset, PRESET_CUSTOM_FILE) == 0);
	const bool is_text = (preset && strcmp(preset, PRESET_CUSTOM_TEXT) == 0);

	obs_property_set_visible(obs_properties_get(props, S_SHADER_FILE_PATH), is_file);
	obs_property_set_visible(obs_properties_get(props, S_SHADER_TEXT), is_text);
	obs_property_set_visible(obs_properties_get(props, S_OVERRIDE_EFFECT), is_file || is_text);
	return true;
}

static void populate_shader_fader_slot_list(obs_property_t *list, bool fadester_loaded, const char *none_label)
{
	if (!list)
		return;
	obs_property_list_add_int(list, none_label, VS_FADER_SLOT_NONE);

	const char *not_loaded_text = obs_module_text("Modulator.MidiFadesterNotLoaded");
	static const char *const k_fader_locale_keys[VS_FADER_SLOT_COUNT] = {
		"Signal.FaderCrossfader", "Signal.Fader1",   "Signal.Fader2",   "Signal.Fader3",   "Signal.Fader4",
		"Signal.Fader5",          "Signal.Fader6",   "Signal.Fader7",   "Signal.Fader8",   "Signal.FaderMaster",
		"Signal.FaderFx1",        "Signal.FaderFx2", "Signal.FaderFx3", "Signal.FaderFx4", "Signal.FaderFx5",
		"Signal.FaderFx6",        "Signal.FaderFx7", "Signal.FaderFx8",
	};
	char label[160];
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		const char *base = obs_module_text(k_fader_locale_keys[i]);
		if (fadester_loaded) {
			obs_property_list_add_int(list, base, i);
		} else {
			snprintf(label, sizeof(label), "%s - %s", base, not_loaded_text);
			obs_property_list_add_int(list, label, i);
		}
	}
}

/* True when any Fadester binding is stored, so the Fader Control group must stay visible even when
 * Midi Fadester is not loaded (the user may need to inspect or clear it). */
static bool shader_fader_bindings_stored(obs_data_t *settings)
{
	if (obs_data_get_int(settings, S_MIX_FADER_SLOT) != VS_FADER_SLOT_NONE ||
	    obs_data_get_int(settings, S_REACTIVITY_FADER_SLOT) != VS_FADER_SLOT_NONE)
		return true;
	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		char key[64];
		param_fader_key(key, sizeof(key), k, "target");
		if (obs_data_get_string(settings, key)[0] != '\0')
			return true;
		param_fader_key(key, sizeof(key), k, "slot");
		if (obs_data_get_int(settings, key) != VS_FADER_SLOT_NONE)
			return true;
	}
	return false;
}

static void refresh_shader_fader_props_visibility(obs_properties_t *props, obs_data_t *settings, bool has_bindable)
{
	if (!props || !settings)
		return;

	const char *preset = obs_data_get_string(settings, S_PRESET_SHADER);
	const bool is_custom = strcmp(preset, PRESET_CUSTOM_FILE) == 0 || strcmp(preset, PRESET_CUSTOM_TEXT) == 0;
	const bool show_mix_react = !(is_custom && obs_data_get_bool(settings, S_OVERRIDE_EFFECT));
	const bool fadester_loaded = vs_fader_bridge_is_midi_fadester_loaded();
	/* Without Midi Fadester and without stored bindings, the group shrinks to one explanatory row. */
	const bool collapsed = !fadester_loaded && !shader_fader_bindings_stored(settings);
	obs_property_set_visible(obs_properties_get(props, S_FADER_NOT_LOADED), collapsed);
	obs_property_set_visible(obs_properties_get(props, S_EFFECT_MIX), show_mix_react);
	obs_property_set_visible(obs_properties_get(props, S_AUDIO_REACTIVITY), show_mix_react);

	const char *slot_keys[] = {S_MIX_FADER_SLOT, S_REACTIVITY_FADER_SLOT};
	const char *slider_keys[] = {S_EFFECT_MIX, S_AUDIO_REACTIVITY};
	const char *status_keys[] = {S_MIX_FADER_STATUS, S_REACTIVITY_FADER_STATUS};
	const char *reset_keys[] = {S_MIX_FADER_RESET, S_REACTIVITY_FADER_RESET};
	const char *fixed_text[] = {"Shader.MixFader.StatusFixed", "Shader.ReactivityFader.StatusFixed"};
	const char *mapped_text[] = {"Shader.MixFader.StatusMapped", "Shader.ReactivityFader.StatusMapped"};
	const char *unloaded_text[] = {"Shader.MixFader.StatusNotLoaded", "Shader.ReactivityFader.StatusNotLoaded"};
	char summary[768];
	for (size_t i = 0; i < 2; i++) {
		obs_property_t *list = obs_properties_get(props, slot_keys[i]);
		const long long slot = obs_data_get_int(settings, slot_keys[i]);
		const char *label = vs_property_int_choice_name(list, slot);
		const double slider_pct = obs_data_get_double(settings, slider_keys[i]);
		if (slot >= 0 && slot < VS_FADER_SLOT_COUNT && label && fadester_loaded)
			snprintf(summary, sizeof(summary), obs_module_text(mapped_text[i]), label, slider_pct);
		else if (slot >= 0 && slot < VS_FADER_SLOT_COUNT && label)
			snprintf(summary, sizeof(summary), obs_module_text(unloaded_text[i]), label, slider_pct);
		else
			snprintf(summary, sizeof(summary), obs_module_text(fixed_text[i]), slider_pct);
		obs_property_t *status = obs_properties_get(props, status_keys[i]);
		vs_property_info_set_text(status, summary);
		obs_property_set_visible(list, show_mix_react && !collapsed);
		obs_property_set_visible(status, show_mix_react && !collapsed);
		obs_property_t *reset = obs_properties_get(props, reset_keys[i]);
		obs_property_set_visible(reset, show_mix_react && !collapsed);
		obs_property_set_enabled(reset, slot != VS_FADER_SLOT_NONE);
	}

	bool earlier_selected = false;
	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		char kt[64], ks[64], kmin[64], kmax[64], kstatus[64], kreset[64];
		param_fader_key(kt, sizeof(kt), k, "target");
		param_fader_key(ks, sizeof(ks), k, "slot");
		param_fader_key(kmin, sizeof(kmin), k, "min");
		param_fader_key(kmax, sizeof(kmax), k, "max");
		param_fader_key(kstatus, sizeof(kstatus), k, "status");
		param_fader_key(kreset, sizeof(kreset), k, "reset");
		const char *target = obs_data_get_string(settings, kt);
		const bool selected = target[0] != '\0';
		const bool visible = !collapsed && (has_bindable || selected) &&
				     (k == 0 || earlier_selected || selected);
		obs_property_t *target_list = obs_properties_get(props, kt);
		const char *target_label = vs_property_string_choice_name(target_list, target);
		for (size_t opt = 0; opt < obs_property_list_item_count(target_list); opt++) {
			const char *candidate = obs_property_list_item_string(target_list, opt);
			bool duplicate = false;
			for (int prev = 0; prev < k && candidate && candidate[0]; prev++) {
				char prev_key[64];
				param_fader_key(prev_key, sizeof(prev_key), prev, "target");
				if (strcmp(candidate, obs_data_get_string(settings, prev_key)) == 0)
					duplicate = true;
			}
			obs_property_list_item_disable(target_list, opt, duplicate);
		}
		const long long slot = obs_data_get_int(settings, ks);
		const char *slot_label = vs_property_int_choice_name(obs_properties_get(props, ks), slot);
		if (!selected)
			snprintf(summary, sizeof(summary), "%s", obs_module_text("Shader.ParamFader.StatusEmpty"));
		else if (!target_label)
			snprintf(summary, sizeof(summary), obs_module_text("Shader.ParamFader.StatusMissing"), target);
		else if (slot < 0 || slot >= VS_FADER_SLOT_COUNT || !slot_label)
			snprintf(summary, sizeof(summary), obs_module_text("Shader.ParamFader.StatusSlider"),
				 target_label);
		else if (!fadester_loaded)
			snprintf(summary, sizeof(summary), obs_module_text("Shader.ParamFader.StatusNotLoaded"),
				 slot_label, target_label);
		else
			snprintf(summary, sizeof(summary), obs_module_text("Shader.ParamFader.StatusMapped"),
				 slot_label, target_label, obs_data_get_double(settings, kmin),
				 obs_data_get_double(settings, kmax));
		vs_property_info_set_text(obs_properties_get(props, kstatus), summary);
		obs_property_set_visible(target_list, visible);
		obs_property_set_visible(obs_properties_get(props, ks), visible && selected);
		obs_property_set_visible(obs_properties_get(props, kmin), visible && selected);
		obs_property_set_visible(obs_properties_get(props, kmax), visible && selected);
		obs_property_set_visible(obs_properties_get(props, kstatus), visible);
		obs_property_set_visible(obs_properties_get(props, kreset), visible);
		obs_property_set_enabled(obs_properties_get(props, kreset),
					 selected || slot != VS_FADER_SLOT_NONE ||
						 obs_data_has_user_value(settings, kmin) ||
						 obs_data_has_user_value(settings, kmax));
		earlier_selected = earlier_selected || selected;
	}
	obs_property_set_visible(obs_properties_get(props, "vs_shader_fader_ownership"),
				 !collapsed && (has_bindable || earlier_selected));
}

static void normalize_shader_fader_targets_for_ui(obs_data_t *settings)
{
	/* VIDEO source updates run on the next tick. Mirror the existing first-target
	 * ownership rule now so this UI refresh cannot show a soon-to-be-cleared binding. */
	char earlier_targets[VS_SHADER_PARAM_FADER_SLOTS][64] = {{0}};
	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		char key[64];
		param_fader_key(key, sizeof(key), k, "target");
		const char *target = obs_data_get_string(settings, key);
		for (int prev = 0; target[0] && prev < k; prev++) {
			if (strcmp(target, earlier_targets[prev]) != 0)
				continue;
			obs_data_set_string(settings, key, "");
			param_fader_key(key, sizeof(key), k, "seeded");
			obs_data_set_string(settings, key, "");
			target = "";
			break;
		}
		snprintf(earlier_targets[k], sizeof(earlier_targets[k]), "%s", target);
	}
}

static bool on_shader_param_fader_changed(void *priv, obs_properties_t *props, obs_property_t *property,
					  obs_data_t *settings)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)priv;
	if (!props || !settings)
		return false;

	bool has_bindable = false;
	const char *prop_name = property ? obs_property_name(property) : NULL;
	for (int k = 0; prop_name && k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		char key[64];
		param_fader_key(key, sizeof(key), k, "target");
		if (strcmp(prop_name, key) == 0) {
			normalize_shader_fader_targets_for_ui(settings);
			break;
		}
	}

	if (sf) {
		obs_enter_graphics();
		for (size_t i = 0; i < sf->parsed.param_count; i++) {
			if (is_fader_bindable_shader_param(&sf->parsed.params[i])) {
				has_bindable = true;
				break;
			}
		}
		for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
			char k_target[64];
			param_fader_key(k_target, sizeof(k_target), k, "target");
			if (!prop_name || strcmp(prop_name, k_target) != 0)
				continue;
			char k_min[64], k_max[64], k_seed[64];
			param_fader_key(k_min, sizeof(k_min), k, "min");
			param_fader_key(k_max, sizeof(k_max), k, "max");
			param_fader_key(k_seed, sizeof(k_seed), k, "seeded");
			const char *target = obs_data_get_string(settings, prop_name);
			if (target && target[0] != '\0') {
				char seed_id[220];
				snprintf(seed_id, sizeof(seed_id), "%s::%s", sf->preset_shader, target);
				const char *prev_seed = obs_data_get_string(settings, k_seed);
				if (!prev_seed || strcmp(prev_seed, seed_id) != 0) {
					struct vs_shader_param_info pinfo;
					if (find_bindable_shader_param_locked(sf, target, &pinfo)) {
						obs_data_set_default_double(settings, k_min, pinfo.min_val);
						obs_data_set_default_double(settings, k_max, pinfo.max_val);
						obs_data_set_string(settings, k_seed, seed_id);
					}
				}
			} else {
				obs_data_set_string(settings, k_seed, "");
			}
		}
		obs_leave_graphics();
	}

	refresh_shader_fader_props_visibility(props, settings, has_bindable);
	return true;
}

static bool on_shader_fader_reset_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	if (!sf || !sf->context || !property)
		return false;
	const char *name = obs_property_name(property);
	obs_data_t *settings = obs_source_get_settings(sf->context);
	if (!settings)
		return false;
	bool reset = false;
	if (strcmp(name, S_MIX_FADER_RESET) == 0) {
		obs_data_set_int(settings, S_MIX_FADER_SLOT, VS_FADER_SLOT_NONE);
		reset = true;
	} else if (strcmp(name, S_REACTIVITY_FADER_RESET) == 0) {
		obs_data_set_int(settings, S_REACTIVITY_FADER_SLOT, VS_FADER_SLOT_NONE);
		reset = true;
	} else {
		for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
			char key[64];
			param_fader_key(key, sizeof(key), k, "reset");
			if (strcmp(name, key) != 0)
				continue;
			param_fader_key(key, sizeof(key), k, "target");
			obs_data_set_string(settings, key, "");
			param_fader_key(key, sizeof(key), k, "slot");
			obs_data_set_int(settings, key, VS_FADER_SLOT_NONE);
			param_fader_key(key, sizeof(key), k, "min");
			obs_data_unset_user_value(settings, key);
			param_fader_key(key, sizeof(key), k, "max");
			obs_data_unset_user_value(settings, key);
			param_fader_key(key, sizeof(key), k, "seeded");
			obs_data_set_string(settings, key, "");
			reset = true;
			break;
		}
	}
	if (reset) {
		obs_source_update(sf->context, settings);
		on_shader_param_fader_changed(sf, props, NULL, settings);
	}
	obs_data_release(settings);
	return reset;
}

static bool on_reload_shader_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	(void)property;
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	if (!sf)
		return false;

	obs_data_t *settings = obs_source_get_settings(sf->context);
	sf->last_compiled_signature[0] = '\0';
	compile_shader(sf, settings);
	obs_data_release(settings);
	sf->rebuilding_props = true;
	obs_source_update_properties(sf->context);
	sf->rebuilding_props = false;
	return true;
}

static obs_properties_t *shader_filter_get_properties(void *data)
{
	struct vs_shader_filter *sf = (struct vs_shader_filter *)data;
	obs_properties_t *props = obs_properties_create();

	/* Panel order: tap status row, Shader Source, Shader Parameters, Fader Control, then the audio
	 * analysis group last so a new user meets the effect before the DSP plumbing. */
	vs_audio_tap_add_status_property(props);

	/* Snapshot shader compiler state under graphics lock without holding the lock across OBS property creation */
	char compile_err_snap[1024] = {0};
	char preset_snap[128] = {0};
	bool has_effect = false;
	bool override_effect = false;
	bool uses_custom_band = true;
	size_t param_count = 0u;
	struct vs_shader_param_info *params_snap = NULL;

	if (sf) {
		obs_enter_graphics();
		snprintf(compile_err_snap, sizeof(compile_err_snap), "%s", sf->compile_error);
		snprintf(preset_snap, sizeof(preset_snap), "%s", sf->preset_shader);
		has_effect = (sf->effect != NULL);
		override_effect = sf->override_entire_effect;
		uses_custom_band = (sf->p_audio_custom_band != NULL);
		param_count = sf->parsed.param_count;
		if (param_count > 0u) {
			params_snap = (struct vs_shader_param_info *)bzalloc(sizeof(*params_snap) * param_count);
			if (params_snap)
				memcpy(params_snap, sf->parsed.params, sizeof(*params_snap) * param_count);
			else
				param_count = 0u;
		}
		obs_leave_graphics();
	}

	/* 2. Shader selection & Custom Hz Window group */
	obs_properties_t *sh_grp = obs_properties_create();
	obs_property_t *p_preset = obs_properties_add_list(sh_grp, S_PRESET_SHADER, obs_module_text("Shader.Preset"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.SpectralAura"),
				     "fft-spectral-aura.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.WaveWarpGlitch"),
				     "fft-wave-warp-glitch.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.ChromaticPrismBloom"),
				     "fft-chromatic-prism-bloom.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.VisualizerOverlay"),
				     "fft-visualizer-overlay.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.ZoomPulse"), "fft-zoom-pulse.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.Kaleidoscope"),
				     "fft-kaleidoscope.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.FeedbackTrails"),
				     "fft-feedback-trails.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.StrobeFlash"), "fft-strobe-flash.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.MirrorSplit"), "fft-mirror-split.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.HexPulseLattice"),
				     "fft-hex-pulse-lattice.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.PolygonTunnel"),
				     "fft-polygon-tunnel.shader");
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.CustomFile"), PRESET_CUSTOM_FILE);
	obs_property_list_add_string(p_preset, obs_module_text("Shader.Preset.CustomText"), PRESET_CUSTOM_TEXT);
	obs_property_set_modified_callback(p_preset, on_preset_changed);

	const bool is_file = (strcmp(preset_snap, PRESET_CUSTOM_FILE) == 0);
	const bool is_text = (strcmp(preset_snap, PRESET_CUSTOM_TEXT) == 0);

	obs_property_t *p_path =
		obs_properties_add_path(sh_grp, S_SHADER_FILE_PATH, obs_module_text("Shader.FilePath"), OBS_PATH_FILE,
					"Shader Files (*.shader *.effect *.hlsl);;All Files (*.*)", NULL);
	obs_property_set_visible(p_path, is_file);

	obs_property_t *p_text =
		obs_properties_add_text(sh_grp, S_SHADER_TEXT, obs_module_text("Shader.Text"), OBS_TEXT_MULTILINE);
	obs_property_set_visible(p_text, is_text);

	obs_property_t *p_ov =
		obs_properties_add_bool(sh_grp, S_OVERRIDE_EFFECT, obs_module_text("Shader.OverrideEffect"));
	obs_property_set_visible(p_ov, is_file || is_text);

	obs_properties_add_button2(sh_grp, S_RELOAD_SHADER, obs_module_text("Shader.Reload"), on_reload_shader_clicked,
				   sf);

	if (sf && compile_err_snap[0] != '\0') {
		obs_property_t *err_prop = obs_properties_add_text(
			sh_grp, S_COMPILE_STATUS, obs_module_text("Shader.CompileStatus"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(err_prop, OBS_TEXT_INFO_ERROR);
		obs_data_t *settings = obs_source_get_settings(sf->context);
		obs_data_set_string(settings, S_COMPILE_STATUS, compile_err_snap);
		obs_data_release(settings);
	}

	obs_property_t *p_emix = obs_properties_add_float_slider(sh_grp, S_EFFECT_MIX,
								 obs_module_text("Shader.EffectMix"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p_emix, "%");
	obs_property_set_long_description(p_emix, obs_module_text("Shader.EffectMix.Tooltip"));
	obs_property_set_visible(p_emix, !override_effect);
	obs_property_set_modified_callback2(p_emix, on_shader_param_fader_changed, sf);

	obs_property_t *p_areact = obs_properties_add_float_slider(
		sh_grp, S_AUDIO_REACTIVITY, obs_module_text("Shader.AudioReactivity"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p_areact, "%");
	obs_property_set_long_description(p_areact, obs_module_text("Shader.AudioReactivity.Tooltip"));
	obs_property_set_visible(p_areact, !override_effect);
	obs_property_set_modified_callback2(p_areact, on_shader_param_fader_changed, sf);

	const bool show_custom_band = (!has_effect || uses_custom_band);

	obs_property_t *p_low = obs_properties_add_float_slider(
		sh_grp, S_CUSTOM_LOW_HZ, obs_module_text("Shader.CustomLowHz"), 20.0, 20000.0, 5.0);
	obs_property_float_set_suffix(p_low, " Hz");
	obs_property_set_visible(p_low, show_custom_band);

	obs_property_t *p_high = obs_properties_add_float_slider(
		sh_grp, S_CUSTOM_HIGH_HZ, obs_module_text("Shader.CustomHighHz"), 20.0, 20000.0, 5.0);
	obs_property_float_set_suffix(p_high, " Hz");
	obs_property_set_visible(p_high, show_custom_band);

	obs_property_t *p_cmode = obs_properties_add_list(sh_grp, S_CUSTOM_MODE, obs_module_text("Shader.CustomMode"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_cmode, obs_module_text("Modulator.CustomMode.Average"),
				  (long long)VS_HZ_MODE_AVERAGE);
	obs_property_list_add_int(p_cmode, obs_module_text("Modulator.CustomMode.Peak"), (long long)VS_HZ_MODE_PEAK);
	obs_property_list_add_int(p_cmode, obs_module_text("Modulator.CustomMode.Flux"), (long long)VS_HZ_MODE_FLUX);
	obs_property_set_visible(p_cmode, show_custom_band);

	obs_properties_add_group(props, "vs_shader_source_group", obs_module_text("Shader.Group"), OBS_GROUP_NORMAL,
				 sh_grp);

	/* 3. Dynamic User Uniform Parameters (R-UP-4) */
	if (param_count > 0u && params_snap) {
		obs_properties_t *u_grp = obs_properties_create();
		for (size_t i = 0; i < param_count; i++) {
			const struct vs_shader_param_info *info = &params_snap[i];
			const bool is_scroller = (strcmp(info->widget_type, "scroller") == 0 ||
						  strcmp(info->widget_type, "spinbox") == 0);
			const bool is_select = (info->option_count > 0u) && (strcmp(info->widget_type, "select") == 0 ||
									     strcmp(info->widget_type, "list") == 0);
			obs_property_t *up = NULL;
			if (info->type == VS_SHADER_PARAM_FLOAT) {
				if (is_scroller) {
					up = obs_properties_add_float(u_grp, info->name, info->label, info->min_val,
								      info->max_val, info->step_val);
				} else {
					up = obs_properties_add_float_slider(u_grp, info->name, info->label,
									     info->min_val, info->max_val,
									     info->step_val);
				}
			} else if (info->type == VS_SHADER_PARAM_INT) {
				const int step = info->step_val >= 1.0 ? (int)info->step_val : 1;
				if (is_select) {
					up = obs_properties_add_list(u_grp, info->name, info->label,
								     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
					for (size_t opt_i = 0; opt_i < info->option_count; opt_i++) {
						const char *lbl = info->option_labels[opt_i][0] != '\0'
									  ? info->option_labels[opt_i]
									  : info->name;
						obs_property_list_add_int(up, lbl,
									  (long long)info->option_values[opt_i]);
					}
				} else if (is_scroller) {
					up = obs_properties_add_int(u_grp, info->name, info->label, (int)info->min_val,
								    (int)info->max_val, step);
				} else {
					up = obs_properties_add_int_slider(u_grp, info->name, info->label,
									   (int)info->min_val, (int)info->max_val,
									   step);
				}
			} else if (info->type == VS_SHADER_PARAM_BOOL) {
				up = obs_properties_add_bool(u_grp, info->name, info->label);
			} else if (info->type == VS_SHADER_PARAM_COLOR) {
				up = obs_properties_add_color_alpha(u_grp, info->name, info->label);
			} else if (info->type == VS_SHADER_PARAM_TEXTURE) {
				up = obs_properties_add_path(u_grp, info->name, info->label, OBS_PATH_FILE,
							     "Images (*.png *.jpg *.jpeg *.gif *.bmp *.tga *.webp)",
							     NULL);
			}
			if (up && info->tooltip[0] != '\0') {
				obs_property_set_long_description(up, info->tooltip);
			}
		}
		obs_properties_add_group(props, "vs_shader_params_group", obs_module_text("Shader.ParamsGroup"),
					 OBS_GROUP_NORMAL, u_grp);
	}

	/* 4. Fader Control (Midi & Stream Deck Fadester) */
	vs_fader_bridge_poll_once();
	const bool fadester_loaded = vs_fader_bridge_is_midi_fadester_loaded();

	obs_properties_t *f_grp = obs_properties_create();
	vs_properties_add_info(f_grp, S_FADER_NOT_LOADED, obs_module_text("Shader.FaderGroup.NotLoaded"));
	obs_property_t *p_mix = obs_properties_add_list(f_grp, S_MIX_FADER_SLOT, obs_module_text("Shader.MixFader"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	populate_shader_fader_slot_list(p_mix, fadester_loaded, obs_module_text("Shader.MixFader.None"));
	obs_property_set_long_description(p_mix, obs_module_text("Shader.MixFader.Tooltip"));
	obs_property_set_visible(p_mix, !override_effect);
	obs_property_set_modified_callback2(p_mix, on_shader_param_fader_changed, sf);
	vs_properties_add_info(f_grp, S_MIX_FADER_STATUS, "");
	obs_properties_add_button2(f_grp, S_MIX_FADER_RESET, obs_module_text("Shader.MixFader.Reset"),
				   on_shader_fader_reset_clicked, sf);

	obs_property_t *p_react = obs_properties_add_list(f_grp, S_REACTIVITY_FADER_SLOT,
							  obs_module_text("Shader.ReactivityFader"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	populate_shader_fader_slot_list(p_react, fadester_loaded, obs_module_text("Shader.ReactivityFader.None"));
	obs_property_set_long_description(p_react, obs_module_text("Shader.ReactivityFader.Tooltip"));
	obs_property_set_visible(p_react, !override_effect);
	obs_property_set_modified_callback2(p_react, on_shader_param_fader_changed, sf);
	vs_properties_add_info(f_grp, S_REACTIVITY_FADER_STATUS, "");
	obs_properties_add_button2(f_grp, S_REACTIVITY_FADER_RESET, obs_module_text("Shader.ReactivityFader.Reset"),
				   on_shader_fader_reset_clicked, sf);
	vs_properties_add_info(f_grp, "vs_shader_fader_ownership", obs_module_text("Shader.ParamFader.Ownership"));

	bool has_bindable_params = false;
	for (size_t i = 0; i < param_count; i++) {
		if (is_fader_bindable_shader_param(&params_snap[i])) {
			has_bindable_params = true;
			break;
		}
	}

	static const char *const k_pf_target_keys[VS_SHADER_PARAM_FADER_SLOTS] = {
		"Shader.ParamFader1.Target",
		"Shader.ParamFader2.Target",
	};
	static const char *const k_pf_slot_keys[VS_SHADER_PARAM_FADER_SLOTS] = {
		"Shader.ParamFader1.Slot",
		"Shader.ParamFader2.Slot",
	};
	static const char *const k_pf_min_keys[VS_SHADER_PARAM_FADER_SLOTS] = {
		"Shader.ParamFader1.Min",
		"Shader.ParamFader2.Min",
	};
	static const char *const k_pf_max_keys[VS_SHADER_PARAM_FADER_SLOTS] = {
		"Shader.ParamFader1.Max",
		"Shader.ParamFader2.Max",
	};

	char key[64];
	char opt_label[220];
	for (int k = 0; k < VS_SHADER_PARAM_FADER_SLOTS; k++) {
		param_fader_key(key, sizeof(key), k, "target");
		obs_property_t *p_pt = obs_properties_add_list(f_grp, key, obs_module_text(k_pf_target_keys[k]),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p_pt, obs_module_text("Shader.ParamFader.TargetNone"), "");
		for (size_t i = 0; i < param_count; i++) {
			const struct vs_shader_param_info *info = &params_snap[i];
			if (!is_fader_bindable_shader_param(info))
				continue;
			if (info->label[0] != '\0' && strcmp(info->label, info->name) != 0)
				snprintf(opt_label, sizeof(opt_label), "%s (%s)", info->label, info->name);
			else
				snprintf(opt_label, sizeof(opt_label), "%s", info->name);
			obs_property_list_add_string(p_pt, opt_label, info->name);
		}
		obs_property_set_modified_callback2(p_pt, on_shader_param_fader_changed, sf);

		param_fader_key(key, sizeof(key), k, "slot");
		obs_property_t *p_ps = obs_properties_add_list(f_grp, key, obs_module_text(k_pf_slot_keys[k]),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		populate_shader_fader_slot_list(p_ps, fadester_loaded, obs_module_text("Shader.ParamFader.SlotNone"));
		obs_property_set_modified_callback2(p_ps, on_shader_param_fader_changed, sf);

		param_fader_key(key, sizeof(key), k, "min");
		obs_property_t *p_min = obs_properties_add_float(f_grp, key, obs_module_text(k_pf_min_keys[k]),
								 -100000.0, 100000.0, 0.01);
		obs_property_set_modified_callback2(p_min, on_shader_param_fader_changed, sf);

		param_fader_key(key, sizeof(key), k, "max");
		obs_property_t *p_max = obs_properties_add_float(f_grp, key, obs_module_text(k_pf_max_keys[k]),
								 -100000.0, 100000.0, 0.01);
		obs_property_set_modified_callback2(p_max, on_shader_param_fader_changed, sf);
		param_fader_key(key, sizeof(key), k, "status");
		vs_properties_add_info(f_grp, key, "");
		param_fader_key(key, sizeof(key), k, "reset");
		obs_properties_add_button2(f_grp, key, obs_module_text("Shader.ParamFader.Reset"),
					   on_shader_fader_reset_clicked, sf);
	}

	obs_properties_add_group(props, "vs_shader_fader_group", obs_module_text("Shader.FaderGroup"), OBS_GROUP_NORMAL,
				 f_grp);

	/* 5. Audio Sidechain & Analysis group (defaulting to Track 1); advanced tuning hidden by default */
	vs_audio_tap_add_properties(props, sf ? sf->context : NULL);

	if (sf && sf->context) {
		obs_data_t *cur_s = obs_source_get_settings(sf->context);
		if (cur_s) {
			refresh_shader_fader_props_visibility(props, cur_s, has_bindable_params);
			obs_data_release(cur_s);
		}
	} else {
		obs_data_t *tmp_s = obs_data_create();
		shader_filter_get_defaults(tmp_s);
		refresh_shader_fader_props_visibility(props, tmp_s, has_bindable_params);
		obs_data_release(tmp_s);
	}

	bfree(params_snap);

	return props;
}

void vs_register_shader_filter(void)
{
	struct obs_source_info info = {0};
	info.id = "visual_sidechain_shader_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = shader_filter_get_name;
	info.create = shader_filter_create;
	info.destroy = shader_filter_destroy;
	info.get_defaults = shader_filter_get_defaults;
	info.update = shader_filter_update;
	info.save = shader_filter_save;
	info.activate = shader_filter_activate;
	info.video_tick = shader_filter_video_tick;
	info.video_render = shader_filter_video_render;
	info.video_get_color_space = shader_filter_get_color_space;
	info.get_properties = shader_filter_get_properties;
	info.filter_add = shader_filter_add;

	obs_register_source(&info);
}

void vs_shader_filter_apply_panic(obs_source_t *source)
{
	if (!source)
		return;
	struct vs_shader_filter *sf = (struct vs_shader_filter *)obs_obj_get_data(source);
	if (!sf)
		return;

	obs_data_t *settings = obs_source_get_settings(source);

	obs_enter_graphics();
	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		sf->panic_slots[i].current_value = (double)sf->user_bindings[i].f_val;
	}
	vs_fader_bridge_apply_panic_to_shader(&sf->panic_passthrough, sf->panic_slots, sf->parsed.param_count);
	sf->feedback_seeded = false;

	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		const struct vs_shader_param_info *info = &sf->parsed.params[i];
		if (info->type == VS_SHADER_PARAM_FLOAT) {
			sf->user_bindings[i].f_val = (float)sf->panic_slots[i].current_value;
			if (settings)
				obs_data_set_double(settings, info->name, sf->panic_slots[i].current_value);
		}
	}
	obs_leave_graphics();

	if (settings)
		obs_data_release(settings);
	obs_source_update_properties(source);
}

void vs_shader_filter_apply_restore(obs_source_t *source)
{
	if (!source)
		return;
	struct vs_shader_filter *sf = (struct vs_shader_filter *)obs_obj_get_data(source);
	if (!sf)
		return;

	obs_data_t *settings = obs_source_get_settings(source);

	obs_enter_graphics();
	vs_fader_bridge_apply_restore_to_shader(&sf->panic_passthrough, sf->panic_slots, sf->parsed.param_count);

	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		const struct vs_shader_param_info *info = &sf->parsed.params[i];
		if (info->type == VS_SHADER_PARAM_FLOAT) {
			sf->user_bindings[i].f_val = (float)sf->panic_slots[i].current_value;
			if (settings)
				obs_data_set_double(settings, info->name, sf->panic_slots[i].current_value);
		}
	}
	obs_leave_graphics();

	if (settings)
		obs_data_release(settings);
	obs_source_update_properties(source);
}
