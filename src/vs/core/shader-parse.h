/*
 * Visual Sidechain - Pure-C Shader Include Expander, Annotation Parser & Effect Builder
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_SHADER_MAX_INCLUDE_DEPTH 16u
#define VS_SHADER_MAX_UNIFORMS 64u
#define VS_SHADER_MAX_OPTIONS 8u

enum vs_shader_error {
	VS_SHADER_OK = 0,
	VS_SHADER_ERR_INVALID_ARG = 1,
	VS_SHADER_ERR_FILE_NOT_FOUND = 2,
	VS_SHADER_ERR_INCLUDE_DEPTH = 3,
	VS_SHADER_ERR_MALFORMED_INCLUDE = 4,
	VS_SHADER_ERR_UNBALANCED_BRACES = 5,
	VS_SHADER_ERR_OUT_OF_MEMORY = 6,
	VS_SHADER_ERR_RESERVED_UNIFORM = 7,
	VS_SHADER_ERR_INVALID_PARAM = 8,
};

enum vs_shader_param_type {
	VS_SHADER_PARAM_UNKNOWN = 0,
	VS_SHADER_PARAM_FLOAT = 1,
	VS_SHADER_PARAM_INT = 2,
	VS_SHADER_PARAM_BOOL = 3,
	VS_SHADER_PARAM_COLOR = 4, /* float4 with color/slider widget */
	VS_SHADER_PARAM_TEXTURE = 5,
};

struct vs_shader_param_info {
	char name[64];
	char label[128];
	char widget_type[32];
	char group[64];
	char tooltip[256];
	enum vs_shader_param_type type;
	bool has_min;
	bool has_max;
	bool has_step;
	bool has_default;
	double min_val;
	double max_val;
	double step_val;
	double default_val[4];
	char option_labels[VS_SHADER_MAX_OPTIONS][64];
	int option_values[VS_SHADER_MAX_OPTIONS];
	size_t option_count;
};

struct vs_shader_parse_result {
	enum vs_shader_error error;
	char error_msg[256];
	char *expanded_source;
	char *full_effect_source;
	struct vs_shader_param_info params[VS_SHADER_MAX_UNIFORMS];
	size_t param_count;
};

typedef char *(*vs_shader_load_file_fn)(const char *path, void *user_data);

void vs_shader_parse_result_free(struct vs_shader_parse_result *res);

/*
 * Expands #include "..." directives with a hard recursion depth cap (16) and
 * NULL quote guards (fixing R-UP-3), parses user uniform annotations, and builds
 * the complete OBS .effect source with all Visual Sidechain audio_* uniforms and
 * audio_spectrum (32x1) / audio_fft (512x2) textures.
 */
enum vs_shader_error vs_shader_ExpandAndParse(const char *source_text, const char *base_dir,
					      bool override_entire_effect, vs_shader_load_file_fn load_cb,
					      void *user_data, struct vs_shader_parse_result *out_res);

/*
 * Validates a built-in .shader file for GPL-2.0-or-later header, balanced braces,
 * audio_* reactivity, zero UTF-8 BOM, and zero banned provenance strings.
 */
bool vs_shader_validate_builtin_file(const char *file_path, char *err_buf, size_t err_cap);

#ifdef __cplusplus
}
#endif
