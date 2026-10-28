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
	assert(strstr(res.full_effect_source, "technique Draw") != NULL);

	vs_shader_parse_result_free(&res);
}

static void test_builtin_shaders_parse_and_provenance(void)
{
	static const char *const k_builtin_shaders[9] = {
		"fft-spectral-aura.shader",      "fft-wave-warp-glitch.shader", "fft-chromatic-prism-bloom.shader",
		"fft-visualizer-overlay.shader", "fft-zoom-pulse.shader",       "fft-kaleidoscope.shader",
		"fft-feedback-trails.shader",    "fft-strobe-flash.shader",     "fft-mirror-split.shader",
	};

	for (size_t i = 0; i < 9u; i++) {
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

int main(void)
{
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	test_shader_include_cycle_guard();
	test_shader_malformed_include_quotes();
	test_shader_uniform_annotations();
	test_builtin_shaders_parse_and_provenance();
	printf("test-shader-parser: all 9 built-in shaders and parser assertions passed\n");
	return 0;
}
