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

#define PRESET_CUSTOM_FILE "__custom_file__"
#define PRESET_CUSTOM_TEXT "__custom_text__"

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

	/* Fader slot uniform handles (R-MOD-7) */
	gs_eparam_t *p_fader[VS_FADER_SLOT_COUNT];

	/* Ping-pong render targets for feedback shaders (R-UP-5, fft-feedback-trails.shader) */
	gs_texrender_t *input_texrender[2];
	gs_texrender_t *output_texrender[2];
	int ping_pong_idx;
	bool feedback_seeded;

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
static void destroy_shader_effect_locked(struct vs_shader_filter *sf)
{
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
		char *slash = (slash1 > slash2) ? slash1 : slash2;
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
			"fader_crossfader", "fader_1", "fader_2", "fader_3", "fader_4",
			"fader_5",          "fader_6", "fader_7", "fader_8", "fader_master",
		};
		for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
			sf->p_fader[i] = gs_effect_get_param_by_name(sf->effect, k_fader_uniform_names[i]);
		}

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

	sf->override_entire_effect = obs_data_get_bool(settings, S_OVERRIDE_EFFECT);

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

	vs_audio_tap_init(&sf->tap, true);

	obs_enter_graphics();
	for (int i = 0; i < 2; i++) {
		sf->input_texrender[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		sf->output_texrender[i] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}
	obs_leave_graphics();

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

	obs_enter_graphics();
	for (int i = 0; i < 2; i++) {
		if (sf->input_texrender[i])
			gs_texrender_destroy(sf->input_texrender[i]);
		if (sf->output_texrender[i])
			gs_texrender_destroy(sf->output_texrender[i]);
	}
	obs_leave_graphics();

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

static void bind_all_shader_uniforms(struct vs_shader_filter *sf, uint32_t cx, uint32_t cy)
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

	const struct vs_audio_frame *f = &sf->tap.current_frame;
	if (sf->p_audio_level)
		gs_effect_set_float(sf->p_audio_level, f->level);
	if (sf->p_audio_sub_bass)
		gs_effect_set_float(sf->p_audio_sub_bass, f->sub_bass);
	if (sf->p_audio_bass)
		gs_effect_set_float(sf->p_audio_bass, f->bass);
	if (sf->p_audio_mid)
		gs_effect_set_float(sf->p_audio_mid, f->mid);
	if (sf->p_audio_high)
		gs_effect_set_float(sf->p_audio_high, f->high);
	if (sf->p_audio_kick)
		gs_effect_set_float(sf->p_audio_kick, f->kick);
	if (sf->p_audio_snare)
		gs_effect_set_float(sf->p_audio_snare, f->snare);
	if (sf->p_audio_beat)
		gs_effect_set_float(sf->p_audio_beat, f->beat);
	if (sf->p_audio_spectral_centroid)
		gs_effect_set_float(sf->p_audio_spectral_centroid, f->spectral_centroid);
	if (sf->p_audio_spectral_flux)
		gs_effect_set_float(sf->p_audio_spectral_flux, f->spectral_flux);
	if (sf->p_audio_custom_band)
		gs_effect_set_float(sf->p_audio_custom_band, sf->current_custom_band);

	/* Legacy obs-shaderfilter compatibility uniforms (R-SHD-1, R-UP-6) */
	if (sf->p_audio_peak)
		gs_effect_set_float(sf->p_audio_peak, fmaxf(f->kick, f->level));
	if (sf->p_audio_magnitude)
		gs_effect_set_float(sf->p_audio_magnitude, f->level);

	/* Dynamic FFT & Spectrum GPU textures (R-SHD-2) */
	if (sf->p_audio_spectrum && sf->tap.tex_spectrum)
		gs_effect_set_texture(sf->p_audio_spectrum, sf->tap.tex_spectrum);
	if (sf->p_audio_fft && sf->tap.tex_fft)
		gs_effect_set_texture(sf->p_audio_fft, sf->tap.tex_fft);

	/* Fader slot float uniforms (R-MOD-7) */
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		if (sf->p_fader[i])
			gs_effect_set_float(sf->p_fader[i], vs_fader_bridge_get_slot(i));
	}

	/* User-annotated uniforms (R-UP-4) */
	const bool panic_active = vs_fader_bridge_is_panic_active();
	for (size_t i = 0; i < sf->parsed.param_count; i++) {
		const struct vs_user_uniform_binding *b = &sf->user_bindings[i];
		if (!b->eparam)
			continue;
		if (b->type == VS_SHADER_PARAM_FLOAT) {
			const float val =
				(panic_active && vs_fader_bridge_is_strobe_or_flash_param(sf->parsed.params[i].name))
					? 0.0f
					: b->f_val;
			gs_effect_set_float(b->eparam, val);
		} else if (b->type == VS_SHADER_PARAM_INT) {
			gs_effect_set_int(b->eparam, b->i_val);
		} else if (b->type == VS_SHADER_PARAM_BOOL) {
			gs_effect_set_bool(b->eparam, b->b_val);
		} else if (b->type == VS_SHADER_PARAM_COLOR) {
			gs_effect_set_vec4(b->eparam, &b->c_val);
		} else if (b->type == VS_SHADER_PARAM_TEXTURE && b->image_loaded && b->image_file.texture) {
			gs_effect_set_texture_srgb(b->eparam, b->image_file.texture);
		}
	}
}

static void render_feedback_pipeline(struct vs_shader_filter *sf, obs_source_t *target, uint32_t cx, uint32_t cy)
{
	const int cur = sf->ping_pong_idx;
	const int prev = 1 - cur;

	gs_texrender_reset(sf->input_texrender[cur]);
	if (!gs_texrender_begin(sf->input_texrender[cur], cx, cy)) {
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
	if (!gs_texrender_begin(sf->output_texrender[cur], cx, cy)) {
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

	bind_all_shader_uniforms(sf, cx, cy);

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

	if (sf->uses_feedback) {
		render_feedback_pipeline(sf, target, cx, cy);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space source_space = obs_source_get_color_space(
		target, sizeof(preferred_spaces) / sizeof(preferred_spaces[0]), preferred_spaces);

	if (!obs_source_process_filter_begin_with_color_space(sf->context, GS_RGBA, source_space,
							      OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	bind_all_shader_uniforms(sf, cx, cy);

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

	/* 1. Audio Sidechain & Analysis group (defaulting to Track 1) */
	vs_audio_tap_add_properties(props, sf ? sf->context : NULL);

	/* Snapshot shader compiler state under graphics lock without holding the lock across OBS property creation */
	char compile_err_snap[1024] = {0};
	char preset_snap[128] = {0};
	bool has_effect = false;
	bool uses_custom_band = true;
	size_t param_count = 0u;
	struct vs_shader_param_info *params_snap = NULL;

	if (sf) {
		obs_enter_graphics();
		snprintf(compile_err_snap, sizeof(compile_err_snap), "%s", sf->compile_error);
		snprintf(preset_snap, sizeof(preset_snap), "%s", sf->preset_shader);
		has_effect = (sf->effect != NULL);
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
