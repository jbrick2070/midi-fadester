/*
 * Visual Sidechain - Shader Include Expander & Uniform Annotation Parser Tests
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/shader-parse.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool double_near(double a, double b, double eps)
{
	return fabs(a - b) <= eps;
}

struct cycle_loader_ctx {
	int call_count;
};

static char *cycle_load_cb(const char *path, void *user_data)
{
	struct cycle_loader_ctx *ctx = (struct cycle_loader_ctx *)user_data;
	ctx->call_count++;
	/* Guard against infinite recursion if the depth limit in shader-parse.c is mutated out. */
	assert(ctx->call_count <= 32);

	if (strstr(path, "a.shader")) {
		const char *s = "// file A\n#include \"b.shader\"\n";
		char *copy = (char *)malloc(strlen(s) + 1u);
		strcpy(copy, s);
		return copy;
	}
	if (strstr(path, "b.shader")) {
		const char *s = "// file B\n#include \"a.shader\"\n";
		char *copy = (char *)malloc(strlen(s) + 1u);
		strcpy(copy, s);
		return copy;
	}
	return NULL;
}

static void test_shader_include_cycle_guard(void)
{
	struct cycle_loader_ctx ctx = {0};
	struct vs_shader_parse_result res;
	const char *root_src = "#include \"a.shader\"\nfloat4 mainImage(VertData v_in) : TARGET { return 0; }\n";

	const enum vs_shader_error err = vs_shader_ExpandAndParse(root_src, "", false, cycle_load_cb, &ctx, &res);
	assert(err == VS_SHADER_ERR_INCLUDE_DEPTH);
	assert(res.error == VS_SHADER_ERR_INCLUDE_DEPTH);
	assert(ctx.call_count == (int)VS_SHADER_MAX_INCLUDE_DEPTH + 1);
	assert(strstr(res.error_msg, "recursion depth") != NULL);
	vs_shader_parse_result_free(&res);
}

static void test_shader_malformed_include_quotes(void)
{
	struct vs_shader_parse_result res;

	/* Missing quotes altogether */
	const char *unquoted =
		"#include unquoted_file.shader\nfloat4 mainImage(VertData v_in) : TARGET { return 0; }\n";
	enum vs_shader_error err = vs_shader_ExpandAndParse(unquoted, "", false, NULL, NULL, &res);
	assert(err == VS_SHADER_ERR_MALFORMED_INCLUDE);
	vs_shader_parse_result_free(&res);

	/* Single opening quote without closing quote */
	const char *single_quote =
		"#include \"unclosed_file.shader\nfloat4 mainImage(VertData v_in) : TARGET { return 0; }\n";
	err = vs_shader_ExpandAndParse(single_quote, "", false, NULL, NULL, &res);
	assert(err == VS_SHADER_ERR_MALFORMED_INCLUDE);
	vs_shader_parse_result_free(&res);

	/* Empty quotes */
	const char *empty_quotes = "#include \"\"\nfloat4 mainImage(VertData v_in) : TARGET { return 0; }\n";
	err = vs_shader_ExpandAndParse(empty_quotes, "", false, NULL, NULL, &res);
	assert(err == VS_SHADER_ERR_MALFORMED_INCLUDE);
	vs_shader_parse_result_free(&res);
}

static void test_shader_uniform_annotations(void)
{
	const char *shader_src =
		"uniform float amount <\n"
		"\tstring label = \"Amount\";\n"
		"\tstring widget_type = \"slider\";\n"
		"\tstring group = \"Main Controls\";\n"
		"\tstring tooltip = \"Controls overall effect intensity\";\n"
		"\tfloat minimum = 0.0;\n"
		"\tfloat maximum = 2.0;\n"
		"\tfloat step = 0.01;\n"
		"> = 1.0;\n"
		"uniform int segments < string label = \"Segments\"; int minimum = 2; int maximum = 16; int step = 1; > = 6;\n"
		"uniform bool invert_flash < string label = \"Invert Flash\"; > = true;\n"
		"uniform float4 aura_color < string label = \"Aura Color\"; > = float4(0.1, 0.8, 1.0, 1.0);\n"
		"uniform int mode_sel <\n"
		"\tstring label = \"Mode Select\";\n"
		"\tstring widget_type = \"select\";\n"
		"\tint option_0_value = 10;\n"
		"\tstring option_0_label = \"Alpha\";\n"
		"\tint option_1_value = 20;\n"
		"\tstring option_1_label = \"Beta\";\n"
		"> = 10;\n"
		"\n"
		"float4 mainImage(VertData v_in) : TARGET\n"
		"{\n"
		"\tfloat4 c = image.Sample(texture_sampler, v_in.uv);\n"
		"\treturn c * (amount + audio_kick + vs_sample_fft(v_in.uv.x) + (float)mode_sel);\n"
		"}\n";

	struct vs_shader_parse_result res;
	const enum vs_shader_error err = vs_shader_ExpandAndParse(shader_src, "", false, NULL, NULL, &res);
	assert(err == VS_SHADER_OK);
	assert(res.param_count == 5u);

	/* 1. float amount */
	assert(strcmp(res.params[0].name, "amount") == 0);
	assert(strcmp(res.params[0].label, "Amount") == 0);
	assert(strcmp(res.params[0].widget_type, "slider") == 0);
	assert(strcmp(res.params[0].group, "Main Controls") == 0);
	assert(strcmp(res.params[0].tooltip, "Controls overall effect intensity") == 0);
	assert(res.params[0].type == VS_SHADER_PARAM_FLOAT);
	assert(res.params[0].has_min && double_near(res.params[0].min_val, 0.0, 1e-6));
	assert(res.params[0].has_max && double_near(res.params[0].max_val, 2.0, 1e-6));
	assert(res.params[0].has_step && double_near(res.params[0].step_val, 0.01, 1e-6));
	assert(res.params[0].has_default && double_near(res.params[0].default_val[0], 1.0, 1e-6));

	/* 2. int segments */
	assert(strcmp(res.params[1].name, "segments") == 0);
	assert(strcmp(res.params[1].label, "Segments") == 0);
	assert(res.params[1].type == VS_SHADER_PARAM_INT);
	assert(double_near(res.params[1].min_val, 2.0, 1e-6));
	assert(double_near(res.params[1].max_val, 16.0, 1e-6));
	assert(double_near(res.params[1].default_val[0], 6.0, 1e-6));

	/* 3. bool invert_flash */
	assert(strcmp(res.params[2].name, "invert_flash") == 0);
	assert(res.params[2].type == VS_SHADER_PARAM_BOOL);
	assert(res.params[2].has_default && double_near(res.params[2].default_val[0], 1.0, 1e-6));

	/* 4. float4 aura_color */
	assert(strcmp(res.params[3].name, "aura_color") == 0);
	assert(res.params[3].type == VS_SHADER_PARAM_COLOR);
	assert(res.params[3].has_default);
	assert(double_near(res.params[3].default_val[0], 0.1, 1e-6));
	assert(double_near(res.params[3].default_val[1], 0.8, 1e-6));
	assert(double_near(res.params[3].default_val[2], 1.0, 1e-6));
	assert(double_near(res.params[3].default_val[3], 1.0, 1e-6));

	/* 5. int mode_sel with select option annotations */
	assert(strcmp(res.params[4].name, "mode_sel") == 0);
	assert(strcmp(res.params[4].widget_type, "select") == 0);
	assert(res.params[4].option_count == 2u);
	assert(strcmp(res.params[4].option_labels[0], "Alpha") == 0);
	assert(res.params[4].option_values[0] == 10);
	assert(strcmp(res.params[4].option_labels[1], "Beta") == 0);
	assert(res.params[4].option_values[1] == 20);

	/* Verify full_effect_source stripped the < ... > annotation blocks and wrapped the effect */
	assert(res.full_effect_source != NULL);
	assert(strstr(res.full_effect_source, "uniform float amount = 1.0;") != NULL);
	assert(strstr(res.full_effect_source, "uniform texture2d audio_spectrum;") != NULL);
	assert(strstr(res.full_effect_source, "uniform texture2d audio_fft;") != NULL);
	assert(strstr(res.full_effect_source, "uniform float vs_effect_mix;") != NULL);
	assert(strstr(res.full_effect_source, "uniform float vs_reactivity_scale;") != NULL);
	assert(strstr(res.full_effect_source, "pixel_shader  = vs_mainImage_entry(v_in);") != NULL);
	assert(strstr(res.full_effect_source, "technique Draw") != NULL);

	vs_shader_parse_result_free(&res);
}

static void test_fader_control_uniform_ownership(void)
{
	static const char *const host_uniforms[] = {"vs_effect_mix", "vs_reactivity_scale"};
	for (size_t i = 0; i < sizeof(host_uniforms) / sizeof(host_uniforms[0]); i++) {
		char source[128];
		snprintf(source, sizeof(source), "uniform float %s = 1.0;\n", host_uniforms[i]);
		struct vs_shader_parse_result res;
		assert(vs_shader_ExpandAndParse(source, "", false, NULL, NULL, &res) == VS_SHADER_ERR_RESERVED_UNIFORM);
		assert(strstr(res.error_msg, host_uniforms[i]) != NULL);
		vs_shader_parse_result_free(&res);
	}

	/* Full effects declare host uniforms, but those declarations must not create editable user
	 * parameters whose saved/default values would overwrite the host's live values at render time. */
	const char *shader_src =
		"uniform float vs_effect_mix = 0.1;\n"
		"uniform float vs_reactivity_scale = 0.2;\n"
		"uniform float amount = 0.75;\n"
		"float4 mainImage(VertData v_in) : TARGET { return image.Sample(texture_sampler, v_in.uv); }\n";
	struct vs_shader_parse_result res;
	assert(vs_shader_ExpandAndParse(shader_src, "", true, NULL, NULL, &res) == VS_SHADER_OK);
	assert(res.param_count == 1u);
	assert(strcmp(res.params[0].name, "amount") == 0);
	assert(res.params[0].default_val[0] == 0.75);
	assert(strstr(res.full_effect_source, "uniform float vs_effect_mix = 0.1;") != NULL);
	assert(strstr(res.full_effect_source, "uniform float vs_reactivity_scale = 0.2;") != NULL);
	vs_shader_parse_result_free(&res);
}

static void test_host_control_setting_names_are_reserved(void)
{
	/* This public naming contract covers source selection, audio settings, group/button/status
	 * property IDs, and both parameter fader assignments, not just numeric host sliders. */
	static const char *const reserved[] = {
		"preset_shader",
		"shader_file_path",
		"override_entire_effect",
		"shader_text",
		"reload_shader",
		"compile_status",
		"custom_low_hz",
		"custom_high_hz",
		"custom_mode",
		"mix_fader_slot",
		"reactivity_fader_slot",
		"vs_mix_fader_status",
		"vs_reactivity_fader_status",
		"vs_mix_fader_reset",
		"vs_reactivity_fader_reset",
		"vs_effect_mix_pct",
		"vs_audio_reactivity_pct",
		"vs_shader_fader_not_loaded",
		"vs_shader_source_group",
		"vs_shader_params_group",
		"vs_shader_fader_group",
		"vs_shader_fader_ownership",
		"audio_tap_group",
		"tap_status_warning",
		"tap_mode",
		"tap_source_name",
		"react_when_muted",
		"sync_offset_ms",
		"auto_gain",
		"gate_db",
		"floor_db",
		"ceiling_db",
		"attack_ms",
		"release_ms",
		"kick_sensitivity",
		"snare_sensitivity",
		"show_advanced_analysis",
		"param_fader_1_target",
		"param_fader_1_slot",
		"param_fader_1_min",
		"param_fader_1_max",
		"param_fader_1_seeded",
		"param_fader_1_status",
		"param_fader_1_reset",
		"param_fader_2_target",
		"param_fader_2_slot",
		"param_fader_2_min",
		"param_fader_2_max",
		"param_fader_2_seeded",
		"param_fader_2_status",
		"param_fader_2_reset",
	};
	for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
		char source[256];
		snprintf(
			source, sizeof(source),
			"uniform float %s = 50.0;\n"
			"float4 mainImage(VertData v_in) : TARGET { return image.Sample(texture_sampler, v_in.uv); }\n",
			reserved[i]);
		for (int full_effect = 0; full_effect <= 1; full_effect++) {
			struct vs_shader_parse_result res;
			assert(vs_shader_ExpandAndParse(source, "", full_effect != 0, NULL, NULL, &res) ==
			       VS_SHADER_ERR_RESERVED_UNIFORM);
			assert(strstr(res.error_msg, reserved[i]) != NULL);
			vs_shader_parse_result_free(&res);
		}
	}

	/* Common shader parameter names remain legal; reservation is exact, not a broad prefix ban. */
	const char *source =
		"// uniform float shader_text = 1.0;\n"
		"/* uniform float custom_low_hz = 1.0; */\n"
		"uniform float effect_mix = 0.25;\n"
		"uniform float audio_reactivity = 0.75;\n"
		"uniform float custom_low_hz_extra = 0.5;\n"
		"uniform float vs_effect_mix_pct_extra = 0.5;\n"
		"uniform float param_fader_1_target_extra = 0.5;\n"
		"uniform float param_fader_01_target = 0.5;\n"
		"uniform float param_fader_3_target = 0.5;\n"
		"uniform float my_shader_text = 0.5;\n"
		"float4 mainImage(VertData v_in) : TARGET { return float4(effect_mix, audio_reactivity, 0.0, 1.0); }\n";
	for (int full_effect = 0; full_effect <= 1; full_effect++) {
		struct vs_shader_parse_result res;
		assert(vs_shader_ExpandAndParse(source, "", full_effect != 0, NULL, NULL, &res) == VS_SHADER_OK);
		assert(res.param_count == 8u);
		assert(strcmp(res.params[0].name, "effect_mix") == 0);
		assert(res.params[0].default_val[0] == 0.25);
		assert(strcmp(res.params[1].name, "audio_reactivity") == 0);
		assert(res.params[1].default_val[0] == 0.75);
		vs_shader_parse_result_free(&res);
	}
}

static void test_reserved_names_after_parameter_limit(void)
{
	/* Reaching the UI parameter limit must not bypass reserved declarations later in the file. */
	char source[8192] = {0};
	size_t len = 0;
	for (size_t i = 0; i < VS_SHADER_MAX_UNIFORMS; i++) {
		int added = snprintf(source + len, sizeof(source) - len, "uniform float amount_%zu = 0.5;\n", i);
		assert(added > 0 && (size_t)added < sizeof(source) - len);
		len += (size_t)added;
	}
	strcpy(source + len, "uniform float shader_text = 0.5;\n");
	for (int full_effect = 0; full_effect <= 1; full_effect++) {
		struct vs_shader_parse_result res;
		assert(vs_shader_ExpandAndParse(source, "", full_effect != 0, NULL, NULL, &res) ==
		       VS_SHADER_ERR_RESERVED_UNIFORM);
		assert(strstr(res.error_msg, "shader_text") != NULL);
		vs_shader_parse_result_free(&res);
	}
}

static void test_invalid_numeric_annotations_are_rejected(void)
{
	static const char *const invalid[] = {
		"uniform int amount < int maximum = 1e300; > = 1;\n",
		"uniform int amount < int step = nan; > = 1;\n",
		"uniform int amount < int option_0_value = 1e300; > = 1;\n",
		"uniform int amount = -1e300;\n",
		"uniform float amount < float minimum = nan; > = 1;\n",
		"uniform float amount < float step = 0; > = 1;\n",
		"uniform float amount < float minimum = 3; float maximum = 2; > = 1;\n",
		"uniform float amount = 1e300;\n",
	};
	for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		struct vs_shader_parse_result res;
		assert(vs_shader_ExpandAndParse(invalid[i], "", false, NULL, NULL, &res) ==
		       VS_SHADER_ERR_INVALID_PARAM);
		assert(strstr(res.error_msg, "amount") != NULL);
		vs_shader_parse_result_free(&res);
	}
}

static void test_builtin_shaders_parse_and_provenance(void)
{
	static const char *const k_builtin_shaders[] = {
		"fft-spectral-aura.shader",      "fft-wave-warp-glitch.shader", "fft-chromatic-prism-bloom.shader",
		"fft-visualizer-overlay.shader", "fft-zoom-pulse.shader",       "fft-kaleidoscope.shader",
		"fft-feedback-trails.shader",    "fft-strobe-flash.shader",     "fft-mirror-split.shader",
		"fft-hex-pulse-lattice.shader",  "fft-polygon-tunnel.shader",
	};

	for (size_t i = 0; i < sizeof(k_builtin_shaders) / sizeof(k_builtin_shaders[0]); i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", VS_SHADERS_DIR, k_builtin_shaders[i]);

		char err_buf[256] = {0};
		const bool valid = vs_shader_validate_builtin_file(path, err_buf, sizeof(err_buf));
		if (!valid) {
			fprintf(stderr, "Built-in shader validation failed for %s: %s\n", path, err_buf);
		}
		assert(valid);
	}

	/* Verify fft-strobe-flash.shader has Maximum Flash Rate <= 3.0 Hz and photosensitivity guideline tooltip */
	char strobe_path[512];
	snprintf(strobe_path, sizeof(strobe_path), "%s/fft-strobe-flash.shader", VS_SHADERS_DIR);
	FILE *fp = fopen(strobe_path, "rb");
	assert(fp != NULL);
	fseek(fp, 0, SEEK_END);
	const long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	char *buf = (char *)malloc((size_t)sz + 1u);
	assert(buf != NULL);
	fread(buf, 1u, (size_t)sz, fp);
	buf[sz] = '\0';
	fclose(fp);

	struct vs_shader_parse_result res;
	const enum vs_shader_error perr = vs_shader_ExpandAndParse(buf, VS_SHADERS_DIR, false, NULL, NULL, &res);
	free(buf);
	assert(perr == VS_SHADER_OK);

	bool found_rate = false;
	for (size_t i = 0; i < res.param_count; i++) {
		if (strcmp(res.params[i].name, "max_flash_rate_hz") == 0) {
			found_rate = true;
			assert(res.params[i].has_default);
			assert(res.params[i].default_val[0] <= 3.0);
			assert(strstr(res.params[i].tooltip, "Photosensitivity guideline") != NULL);
		}
	}
	assert(found_rate);
	vs_shader_parse_result_free(&res);

	/* Verify fft-visualizer-overlay.shader and fft-mirror-split.shader expose named select options (R-UP-4) */
	char vis_path[512];
	snprintf(vis_path, sizeof(vis_path), "%s/fft-visualizer-overlay.shader", VS_SHADERS_DIR);
	fp = fopen(vis_path, "rb");
	assert(fp != NULL);
	fseek(fp, 0, SEEK_END);
	const long vis_sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = (char *)malloc((size_t)vis_sz + 1u);
	assert(buf != NULL);
	fread(buf, 1u, (size_t)vis_sz, fp);
	buf[vis_sz] = '\0';
	fclose(fp);
	assert(vs_shader_ExpandAndParse(buf, VS_SHADERS_DIR, false, NULL, NULL, &res) == VS_SHADER_OK);
	free(buf);
	assert(res.param_count >= 1u);
	assert(strcmp(res.params[0].name, "overlay_mode") == 0);
	assert(strcmp(res.params[0].widget_type, "select") == 0);
	assert(res.params[0].option_count == 3u);
	assert(res.params[0].option_values[2] == 2);
	vs_shader_parse_result_free(&res);

	char mir_path[512];
	snprintf(mir_path, sizeof(mir_path), "%s/fft-mirror-split.shader", VS_SHADERS_DIR);
	fp = fopen(mir_path, "rb");
	assert(fp != NULL);
	fseek(fp, 0, SEEK_END);
	const long mir_sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = (char *)malloc((size_t)mir_sz + 1u);
	assert(buf != NULL);
	fread(buf, 1u, (size_t)mir_sz, fp);
	buf[mir_sz] = '\0';
	fclose(fp);
	assert(vs_shader_ExpandAndParse(buf, VS_SHADERS_DIR, false, NULL, NULL, &res) == VS_SHADER_OK);
	free(buf);
	assert(res.param_count >= 1u);
	assert(strcmp(res.params[0].name, "mirror_mode") == 0);
	assert(strcmp(res.params[0].widget_type, "select") == 0);
	assert(res.params[0].option_count == 4u);
	assert(res.params[0].option_values[3] == 3);
	vs_shader_parse_result_free(&res);
}

struct geometry_control_contract {
	const char *name;
	enum vs_shader_param_type type;
	double minimum;
	double maximum;
	double default_value;
};

static void check_geometry_shader_controls(const char *filename, const struct geometry_control_contract *controls,
					   size_t control_count)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", VS_SHADERS_DIR, filename);
	FILE *fp = fopen(path, "rb");
	assert(fp != NULL);
	assert(fseek(fp, 0, SEEK_END) == 0);
	const long size = ftell(fp);
	assert(size > 0);
	assert(fseek(fp, 0, SEEK_SET) == 0);
	char *source = (char *)malloc((size_t)size + 1u);
	assert(source != NULL);
	assert(fread(source, 1u, (size_t)size, fp) == (size_t)size);
	source[size] = '\0';
	fclose(fp);

	struct vs_shader_parse_result res;
	assert(vs_shader_ExpandAndParse(source, VS_SHADERS_DIR, false, NULL, NULL, &res) == VS_SHADER_OK);
	free(source);
	/* These overlays must not allocate the four feedback render targets. */
	assert(strstr(res.expanded_source, "previous_image") == NULL);
	assert(strstr(res.expanded_source, "previous_output") == NULL);
	assert(res.param_count == control_count + 2u); /* Numeric controls plus two colors. */
	for (size_t i = 0; i < control_count; i++) {
		const struct vs_shader_param_info *param = NULL;
		for (size_t j = 0; j < res.param_count; j++) {
			if (strcmp(res.params[j].name, controls[i].name) == 0) {
				param = &res.params[j];
				break;
			}
		}
		/* Stable names and bounded defaults form the parameter-fader/settings contract. */
		assert(param != NULL);
		assert(param->type == controls[i].type);
		assert(strcmp(param->widget_type, "slider") == 0);
		assert(param->has_min && param->has_max && param->has_default && param->has_step);
		assert(double_near(param->min_val, controls[i].minimum, 1e-9));
		assert(double_near(param->max_val, controls[i].maximum, 1e-9));
		assert(double_near(param->default_val[0], controls[i].default_value, 1e-9));
		assert(param->step_val > 0.0);
		assert(param->default_val[0] >= param->min_val && param->default_val[0] <= param->max_val);
		assert(param->option_count == 0u); /* Integer sides/density stay contiguous for faders. */
	}
	for (size_t j = control_count; j < res.param_count; j++) {
		assert(res.params[j].type == VS_SHADER_PARAM_COLOR);
		assert(res.params[j].has_default);
		for (size_t k = 0; k < 4u; k++) {
			assert(isfinite(res.params[j].default_val[k]));
			assert(res.params[j].default_val[k] >= 0.0 && res.params[j].default_val[k] <= 1.0);
		}
	}
	vs_shader_parse_result_free(&res);
}

static void test_geometry_shader_control_contract(void)
{
	static const struct geometry_control_contract hex_controls[] = {
		{"hex_cells", VS_SHADER_PARAM_INT, 4.0, 32.0, 12.0},
		{"hex_line_width", VS_SHADER_PARAM_FLOAT, 0.005, 0.08, 0.025},
		{"hex_ripple_speed", VS_SHADER_PARAM_FLOAT, 0.0, 0.5, 0.15},
		{"hex_audio_amount", VS_SHADER_PARAM_FLOAT, 0.0, 1.0, 0.6},
		{"hex_opacity", VS_SHADER_PARAM_FLOAT, 0.0, 1.0, 0.6},
	};
	static const struct geometry_control_contract tunnel_controls[] = {
		{"tunnel_sides", VS_SHADER_PARAM_INT, 3.0, 8.0, 6.0},
		{"tunnel_depth", VS_SHADER_PARAM_FLOAT, 2.0, 8.0, 3.0},
		{"tunnel_speed", VS_SHADER_PARAM_FLOAT, -0.3, 0.3, 0.15},
		{"tunnel_rotation_speed", VS_SHADER_PARAM_FLOAT, -0.3, 0.3, 0.08},
		{"tunnel_line_width", VS_SHADER_PARAM_FLOAT, 0.005, 0.12, 0.035},
		{"tunnel_audio_amount", VS_SHADER_PARAM_FLOAT, 0.0, 1.0, 0.6},
		{"tunnel_opacity", VS_SHADER_PARAM_FLOAT, 0.0, 1.0, 0.65},
	};
	check_geometry_shader_controls("fft-hex-pulse-lattice.shader", hex_controls,
				       sizeof(hex_controls) / sizeof(hex_controls[0]));
	check_geometry_shader_controls("fft-polygon-tunnel.shader", tunnel_controls,
				       sizeof(tunnel_controls) / sizeof(tunnel_controls[0]));
}

int main(void)
{
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	test_shader_include_cycle_guard();
	test_shader_malformed_include_quotes();
	test_shader_uniform_annotations();
	test_fader_control_uniform_ownership();
	test_host_control_setting_names_are_reserved();
	test_reserved_names_after_parameter_limit();
	test_invalid_numeric_annotations_are_rejected();
	test_builtin_shaders_parse_and_provenance();
	test_geometry_shader_control_contract();
	printf("test-shader-parser: all 11 built-in shaders and parser assertions passed\n");
	return 0;
}
