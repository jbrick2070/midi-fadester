/*
 * Visual Sidechain - Pure-C Shader Include Expander, Annotation Parser & Effect Builder
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "shader-parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_effect_template_begin =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform texture2d previous_image;\n"
	"uniform texture2d previous_output;\n"
	"uniform float2 uv_offset;\n"
	"uniform float2 uv_scale;\n"
	"uniform float2 uv_pixel_interval;\n"
	"uniform float2 uv_size;\n"
	"uniform float elapsed_time;\n"
	"uniform float loops;\n"
	"uniform float loop_second;\n"
	"uniform float rand_f;\n"
	"uniform float rand_instance_f;\n"
	"uniform float rand_activation_f;\n"
	"uniform float audio_level;\n"
	"uniform float audio_sub_bass;\n"
	"uniform float audio_bass;\n"
	"uniform float audio_mid;\n"
	"uniform float audio_high;\n"
	"uniform float audio_kick;\n"
	"uniform float audio_snare;\n"
	"uniform float audio_beat;\n"
	"uniform float audio_spectral_centroid;\n"
	"uniform float audio_spectral_flux;\n"
	"uniform float audio_custom_band;\n"
	"uniform float audio_peak;\n"
	"uniform float audio_magnitude;\n"
	"uniform texture2d audio_spectrum;\n"
	"uniform texture2d audio_fft;\n"
	"\n"
	"sampler_state texture_sampler {\n"
	"\tFilter   = Linear;\n"
	"\tAddressU = Clamp;\n"
	"\tAddressV = Clamp;\n"
	"};\n"
	"\n"
	"sampler_state audio_sampler {\n"
	"\tFilter   = Linear;\n"
	"\tAddressU = Clamp;\n"
	"\tAddressV = Clamp;\n"
	"};\n"
	"\n"
	"struct VertData {\n"
	"\tfloat4 pos : POSITION;\n"
	"\tfloat2 uv  : TEXCOORD0;\n"
	"};\n"
	"\n"
	"VertData mainTransform(VertData v_in)\n"
	"{\n"
	"\tVertData vert_out;\n"
	"\tvert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n"
	"\tvert_out.uv  = v_in.uv * uv_scale + uv_offset;\n"
	"\treturn vert_out;\n"
	"}\n"
	"\n"
	"float vs_sample_spectrum(float u)\n"
	"{\n"
	"\treturn audio_spectrum.Sample(audio_sampler, float2(clamp(u, 0.0, 1.0), 0.5)).r;\n"
	"}\n"
	"\n"
	"float vs_sample_fft(float u)\n"
	"{\n"
	"\treturn audio_fft.Sample(audio_sampler, float2(clamp(u, 0.0, 1.0), 0.25)).r;\n"
	"}\n"
	"\n"
	"float vs_sample_waveform(float u)\n"
	"{\n"
	"\treturn audio_fft.Sample(audio_sampler, float2(clamp(u, 0.0, 1.0), 0.75)).r;\n"
	"}\n\n";

static const char *const k_effect_template_end = "\n\ntechnique Draw\n"
						 "{\n"
						 "\tpass\n"
						 "\t{\n"
						 "\t\tvertex_shader = mainTransform(v_in);\n"
						 "\t\tpixel_shader  = mainImage(v_in);\n"
						 "\t}\n"
						 "}\n";

struct str_builder {
	char *data;
	size_t len;
	size_t cap;
};

static bool sb_init(struct str_builder *sb, size_t initial_cap)
{
	sb->data = (char *)malloc(initial_cap);
	if (!sb->data)
		return false;
	sb->data[0] = '\0';
	sb->len = 0;
	sb->cap = initial_cap;
	return true;
}

static bool sb_append_n(struct str_builder *sb, const char *s, size_t n)
{
	if (n == 0)
		return true;
	if (sb->len + n + 1u > sb->cap) {
		size_t new_cap = sb->cap * 2u;
		while (new_cap < sb->len + n + 1u)
			new_cap *= 2u;
		char *next = (char *)realloc(sb->data, new_cap);
		if (!next)
			return false;
		sb->data = next;
		sb->cap = new_cap;
	}
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	sb->data[sb->len] = '\0';
	return true;
}

static bool sb_append(struct str_builder *sb, const char *s)
{
	return s ? sb_append_n(sb, s, strlen(s)) : true;
}

static char *default_load_file(const char *path, void *user_data)
{
	(void)user_data;
	if (!path)
		return NULL;
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	const long sz = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (sz < 0) {
		fclose(fp);
		return NULL;
	}
	char *buf = (char *)malloc((size_t)sz + 1u);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	const size_t rd = fread(buf, 1, (size_t)sz, fp);
	buf[rd] = '\0';
	fclose(fp);
	return buf;
}

static enum vs_shader_error expand_recursive(const char *src, const char *base_dir, uint32_t depth,
					     vs_shader_load_file_fn load_cb, void *user_data, struct str_builder *out,
					     char *err_msg, size_t err_cap)
{
	if (depth > VS_SHADER_MAX_INCLUDE_DEPTH) {
		snprintf(err_msg, err_cap, "Maximum #include recursion depth (%u) exceeded",
			 VS_SHADER_MAX_INCLUDE_DEPTH);
		return VS_SHADER_ERR_INCLUDE_DEPTH;
	}

	const char *p = src;
	while (*p != '\0') {
		const char *line_end = strchr(p, '\n');
		const size_t line_len = line_end ? (size_t)(line_end - p + 1) : strlen(p);

		const char *trim = p;
		while (trim < p + line_len && (*trim == ' ' || *trim == '\t'))
			trim++;

		if ((size_t)(p + line_len - trim) >= 8u && strncmp(trim, "#include", 8) == 0 &&
		    (trim[8] == ' ' || trim[8] == '\t' || trim[8] == '"')) {
			const char *q1 = strchr(trim + 8, '"');
			if (!q1 || q1 >= p + line_len) {
				snprintf(err_msg, err_cap, "Malformed #include directive: missing opening quote");
				return VS_SHADER_ERR_MALFORMED_INCLUDE;
			}
			const char *q2 = strchr(q1 + 1, '"');
			if (!q2 || q2 >= p + line_len || q2 == q1 + 1) {
				snprintf(err_msg, err_cap,
					 "Malformed #include directive: missing closing quote or empty path");
				return VS_SHADER_ERR_MALFORMED_INCLUDE;
			}

			char inc_rel[256];
			const size_t rel_len = (size_t)(q2 - (q1 + 1));
			if (rel_len >= sizeof(inc_rel)) {
				snprintf(err_msg, err_cap, "Include path too long");
				return VS_SHADER_ERR_MALFORMED_INCLUDE;
			}
			memcpy(inc_rel, q1 + 1, rel_len);
			inc_rel[rel_len] = '\0';

			char full_path[512];
			if (base_dir && base_dir[0] != '\0')
				snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, inc_rel);
			else
				snprintf(full_path, sizeof(full_path), "%s", inc_rel);

			vs_shader_load_file_fn loader = load_cb ? load_cb : default_load_file;
			char *inc_text = loader(full_path, user_data);
			if (!inc_text && base_dir && base_dir[0] != '\0') {
				inc_text = loader(inc_rel, user_data);
			}
			if (!inc_text) {
				snprintf(err_msg, err_cap, "Included file not found: %s", inc_rel);
				return VS_SHADER_ERR_FILE_NOT_FOUND;
			}

			enum vs_shader_error sub_err = expand_recursive(inc_text, base_dir, depth + 1u, load_cb,
									user_data, out, err_msg, err_cap);
			free(inc_text);
			if (sub_err != VS_SHADER_OK)
				return sub_err;
			if (!sb_append(out, "\n"))
				return VS_SHADER_ERR_OUT_OF_MEMORY;
		} else {
			if (!sb_append_n(out, p, line_len))
				return VS_SHADER_ERR_OUT_OF_MEMORY;
		}

		p += line_len;
	}

	return VS_SHADER_OK;
}

static bool is_builtin_uniform_name(const char *name)
{
	static const char *const builtins[] = {
		"ViewProj",
		"image",
		"previous_image",
		"previous_output",
		"uv_offset",
		"uv_scale",
		"uv_pixel_interval",
		"uv_size",
		"elapsed_time",
		"loops",
		"loop_second",
		"rand_f",
		"rand_instance_f",
		"rand_activation_f",
		"audio_level",
		"audio_sub_bass",
		"audio_bass",
		"audio_mid",
		"audio_high",
		"audio_kick",
		"audio_snare",
		"audio_beat",
		"audio_spectral_centroid",
		"audio_spectral_flux",
		"audio_custom_band",
		"audio_peak",
		"audio_magnitude",
		"audio_spectrum",
		"audio_fft",
	};
	for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
		if (strcmp(name, builtins[i]) == 0)
			return true;
	}
	return false;
}

static void parse_annotation_block(const char *ann_start, const char *ann_end, struct vs_shader_param_info *param)
{
	const char *p = ann_start;
	while (p < ann_end) {
		const char *semi = memchr(p, ';', (size_t)(ann_end - p));
		const char *stmt_end = semi ? semi : ann_end;

		const char *eq = memchr(p, '=', (size_t)(stmt_end - p));
		if (eq) {
			/* Extract key name right before '='. */
			const char *k_end = eq;
			while (k_end > p && isspace((unsigned char)k_end[-1]))
				k_end--;
			const char *k_start = k_end;
			while (k_start > p && (isalnum((unsigned char)k_start[-1]) || k_start[-1] == '_'))
				k_start--;

			char key[64];
			const size_t k_len = (size_t)(k_end - k_start);
			if (k_len > 0 && k_len < sizeof(key)) {
				memcpy(key, k_start, k_len);
				key[k_len] = '\0';

				const char *v_start = eq + 1;
				while (v_start < stmt_end && isspace((unsigned char)*v_start))
					v_start++;

				if (*v_start == '"') {
					const char *q2 = memchr(v_start + 1, '"', (size_t)(stmt_end - (v_start + 1)));
					if (q2) {
						const size_t v_len = (size_t)(q2 - (v_start + 1));
						if (strcmp(key, "label") == 0 || strcmp(key, "name") == 0) {
							const size_t cp = v_len < sizeof(param->label) - 1u
										  ? v_len
										  : sizeof(param->label) - 1u;
							memcpy(param->label, v_start + 1, cp);
							param->label[cp] = '\0';
						} else if (strcmp(key, "widget_type") == 0) {
							const size_t cp = v_len < sizeof(param->widget_type) - 1u
										  ? v_len
										  : sizeof(param->widget_type) - 1u;
							memcpy(param->widget_type, v_start + 1, cp);
							param->widget_type[cp] = '\0';
						} else if (strcmp(key, "group") == 0) {
							const size_t cp = v_len < sizeof(param->group) - 1u
										  ? v_len
										  : sizeof(param->group) - 1u;
							memcpy(param->group, v_start + 1, cp);
							param->group[cp] = '\0';
						} else if (strcmp(key, "tooltip") == 0) {
							const size_t cp = v_len < sizeof(param->tooltip) - 1u
										  ? v_len
										  : sizeof(param->tooltip) - 1u;
							memcpy(param->tooltip, v_start + 1, cp);
							param->tooltip[cp] = '\0';
						}
					}
				} else {
					const double num = strtod(v_start, NULL);
					if (strcmp(key, "minimum") == 0) {
						param->min_val = num;
						param->has_min = true;
					} else if (strcmp(key, "maximum") == 0) {
						param->max_val = num;
						param->has_max = true;
					} else if (strcmp(key, "step") == 0) {
						param->step_val = num;
						param->has_step = true;
					}
				}
			}
		}

		p = semi ? (semi + 1) : ann_end;
	}
}

/*
 * Strips HLSL < ... > uniform annotations from full_effect_source before passing
 * to OBS's gs_effect_create when needed, while extracting all uniform metadata
 * into out_res->params[].
 */
static void parse_uniforms_from_source(const char *src, struct vs_shader_parse_result *out_res)
{
	const char *p = src;
	while (*p != '\0' && out_res->param_count < VS_SHADER_MAX_UNIFORMS) {
		/* Skip single-line and block comments. */
		if (p[0] == '/' && p[1] == '/') {
			const char *nl = strchr(p + 2, '\n');
			p = nl ? (nl + 1) : (p + strlen(p));
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			const char *end_c = strstr(p + 2, "*/");
			p = end_c ? (end_c + 2) : (p + strlen(p));
			continue;
		}

		if (strncmp(p, "uniform", 7) == 0 && (p == src || !isalnum((unsigned char)p[-1])) &&
		    isspace((unsigned char)p[7])) {
			const char *t_start = p + 7;
			while (*t_start && isspace((unsigned char)*t_start))
				t_start++;
			const char *t_end = t_start;
			while (*t_end && (isalnum((unsigned char)*t_end) || *t_end == '_'))
				t_end++;

			const char *n_start = t_end;
			while (*n_start && isspace((unsigned char)*n_start))
				n_start++;
			const char *n_end = n_start;
			while (*n_end && (isalnum((unsigned char)*n_end) || *n_end == '_'))
				n_end++;

			char type_str[32];
			char name_str[64];
			const size_t t_len = (size_t)(t_end - t_start);
			const size_t n_len = (size_t)(n_end - n_start);

			if (t_len > 0 && t_len < sizeof(type_str) && n_len > 0 && n_len < sizeof(name_str)) {
				memcpy(type_str, t_start, t_len);
				type_str[t_len] = '\0';
				memcpy(name_str, n_start, n_len);
				name_str[n_len] = '\0';

				const char *rest = n_end;
				while (*rest && isspace((unsigned char)*rest))
					rest++;

				const char *ann_open = NULL;
				const char *ann_close = NULL;
				if (*rest == '<') {
					ann_open = rest + 1;
					ann_close = strchr(ann_open, '>');
					if (ann_close)
						rest = ann_close + 1;
				}

				const char *stmt_semi = strchr(rest, ';');
				if (!is_builtin_uniform_name(name_str)) {
					enum vs_shader_param_type ptype = VS_SHADER_PARAM_UNKNOWN;
					if (strcmp(type_str, "float") == 0)
						ptype = VS_SHADER_PARAM_FLOAT;
					else if (strcmp(type_str, "int") == 0)
						ptype = VS_SHADER_PARAM_INT;
					else if (strcmp(type_str, "bool") == 0)
						ptype = VS_SHADER_PARAM_BOOL;
					else if (strcmp(type_str, "float4") == 0)
						ptype = VS_SHADER_PARAM_COLOR;
					else if (strcmp(type_str, "texture2d") == 0)
						ptype = VS_SHADER_PARAM_TEXTURE;

					if (ptype != VS_SHADER_PARAM_UNKNOWN) {
						struct vs_shader_param_info *info =
							&out_res->params[out_res->param_count++];
						memset(info, 0, sizeof(*info));
						strncpy(info->name, name_str, sizeof(info->name) - 1u);
						strncpy(info->label, name_str, sizeof(info->label) - 1u);
						strncpy(info->widget_type, "slider", sizeof(info->widget_type) - 1u);
						info->type = ptype;
						info->min_val = 0.0;
						info->max_val = 1.0;
						info->step_val = (ptype == VS_SHADER_PARAM_INT) ? 1.0 : 0.01;

						if (ann_open && ann_close) {
							parse_annotation_block(ann_open, ann_close, info);
						}

						if (stmt_semi) {
							const char *eq = memchr(rest, '=', (size_t)(stmt_semi - rest));
							if (eq) {
								const char *v = eq + 1;
								while (v < stmt_semi && isspace((unsigned char)*v))
									v++;
								if (strncmp(v, "true", 4) == 0) {
									info->default_val[0] = 1.0;
									info->has_default = true;
								} else if (strncmp(v, "false", 5) == 0) {
									info->default_val[0] = 0.0;
									info->has_default = true;
								} else if (strncmp(v, "float4", 6) == 0) {
									const char *lp = strchr(v, '(');
									if (lp && lp < stmt_semi) {
										sscanf(lp + 1, "%lf , %lf , %lf , %lf",
										       &info->default_val[0],
										       &info->default_val[1],
										       &info->default_val[2],
										       &info->default_val[3]);
										info->has_default = true;
									}
								} else {
									info->default_val[0] = strtod(v, NULL);
									info->has_default = true;
								}
							}
						}
					}
				}

				if (stmt_semi) {
					p = stmt_semi + 1;
					continue;
				}
			}
		}
		p++;
	}
}

static bool strip_annotations_and_check_braces(const char *src, struct str_builder *clean_out, char *err_msg,
					       size_t err_cap)
{
	int brace_depth = 0;
	const char *p = src;

	while (*p != '\0') {
		if (p[0] == '/' && p[1] == '/') {
			const char *nl = strchr(p, '\n');
			const size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
			if (!sb_append_n(clean_out, p, len))
				return false;
			p += len;
			continue;
		}
		if (p[0] == '/' && p[1] == '*') {
			const char *end_c = strstr(p + 2, "*/");
			if (!end_c) {
				snprintf(err_msg, err_cap, "Unclosed block comment /*");
				return false;
			}
			const size_t len = (size_t)(end_c + 2 - p);
			if (!sb_append_n(clean_out, p, len))
				return false;
			p += len;
			continue;
		}

		/* Strip uniform < ... > annotation blocks so OBS's HLSL effect parser receives clean declarations. */
		if (strncmp(p, "uniform", 7) == 0 && (p == src || !isalnum((unsigned char)p[-1])) &&
		    isspace((unsigned char)p[7])) {
			const char *scan = p + 7;
			while (*scan && isspace((unsigned char)*scan))
				scan++;
			while (*scan && (isalnum((unsigned char)*scan) || *scan == '_'))
				scan++;
			while (*scan && isspace((unsigned char)*scan))
				scan++;
			while (*scan && (isalnum((unsigned char)*scan) || *scan == '_'))
				scan++;
			while (*scan && isspace((unsigned char)*scan))
				scan++;

			if (*scan == '<') {
				const char *lt = scan;
				const char *gt = strchr(lt + 1, '>');
				if (!gt) {
					snprintf(err_msg, err_cap, "Unclosed uniform annotation '<'");
					return false;
				}
				const char *semi = strchr(gt + 1, ';');
				if (!semi) {
					snprintf(err_msg, err_cap, "Missing ';' after uniform declaration");
					return false;
				}
				/* Trim trailing space before '<' so 'uniform float x <...> = 1;' becomes 'uniform float x = 1;' */
				const char *pre_end = lt;
				while (pre_end > p && (pre_end[-1] == ' ' || pre_end[-1] == '\t'))
					pre_end--;
				if (!sb_append_n(clean_out, p, (size_t)(pre_end - p)))
					return false;
				if (!sb_append_n(clean_out, gt + 1, (size_t)(semi + 1 - (gt + 1))))
					return false;
				p = semi + 1;
				continue;
			}
		}

		if (*p == '{') {
			brace_depth++;
		} else if (*p == '}') {
			brace_depth--;
			if (brace_depth < 0) {
				snprintf(err_msg, err_cap, "Unexpected closing brace '}'");
				return false;
			}
		}

		if (!sb_append_n(clean_out, p, 1))
			return false;
		p++;
	}

	if (brace_depth != 0) {
		snprintf(err_msg, err_cap, "Unbalanced braces in shader (depth=%d)", brace_depth);
		return false;
	}
	return true;
}

void vs_shader_parse_result_free(struct vs_shader_parse_result *res)
{
	if (!res)
		return;
	free(res->expanded_source);
	free(res->full_effect_source);
	memset(res, 0, sizeof(*res));
}

enum vs_shader_error vs_shader_ExpandAndParse(const char *source_text, const char *base_dir,
					      bool override_entire_effect, vs_shader_load_file_fn load_cb,
					      void *user_data, struct vs_shader_parse_result *out_res)
{
	if (!out_res)
		return VS_SHADER_ERR_INVALID_ARG;
	memset(out_res, 0, sizeof(*out_res));

	if (!source_text) {
		out_res->error = VS_SHADER_ERR_INVALID_ARG;
		snprintf(out_res->error_msg, sizeof(out_res->error_msg), "NULL shader source");
		return out_res->error;
	}

	struct str_builder expanded;
	if (!sb_init(&expanded, 4096u)) {
		out_res->error = VS_SHADER_ERR_OUT_OF_MEMORY;
		return out_res->error;
	}

	enum vs_shader_error err = expand_recursive(source_text, base_dir ? base_dir : "", 0u, load_cb, user_data,
						    &expanded, out_res->error_msg, sizeof(out_res->error_msg));
	if (err != VS_SHADER_OK) {
		free(expanded.data);
		out_res->error = err;
		return err;
	}

	out_res->expanded_source = expanded.data;
	parse_uniforms_from_source(out_res->expanded_source, out_res);

	struct str_builder clean_body;
	if (!sb_init(&clean_body, expanded.len + 256u)) {
		out_res->error = VS_SHADER_ERR_OUT_OF_MEMORY;
		return out_res->error;
	}

	if (!strip_annotations_and_check_braces(out_res->expanded_source, &clean_body, out_res->error_msg,
						sizeof(out_res->error_msg))) {
		free(clean_body.data);
		out_res->error = VS_SHADER_ERR_UNBALANCED_BRACES;
		return out_res->error;
	}

	struct str_builder full_effect;
	if (!sb_init(&full_effect, clean_body.len + 2048u)) {
		free(clean_body.data);
		out_res->error = VS_SHADER_ERR_OUT_OF_MEMORY;
		return out_res->error;
	}

	if (!override_entire_effect) {
		sb_append(&full_effect, k_effect_template_begin);
		sb_append(&full_effect, clean_body.data);
		sb_append(&full_effect, k_effect_template_end);
	} else {
		sb_append(&full_effect, clean_body.data);
	}

	free(clean_body.data);
	out_res->full_effect_source = full_effect.data;
	out_res->error = VS_SHADER_OK;
	return VS_SHADER_OK;
}

bool vs_shader_validate_builtin_file(const char *file_path, char *err_buf, size_t err_cap)
{
	if (!file_path)
		return false;

	char *raw = default_load_file(file_path, NULL);
	if (!raw) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "Cannot open shader file: %s", file_path);
		return false;
	}

	/* 1. Reject UTF-8 BOM (0xEF 0xBB 0xBF). */
	const unsigned char *bytes = (const unsigned char *)raw;
	if (bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "UTF-8 BOM detected in %s", file_path);
		free(raw);
		return false;
	}

	/* 2. Require GPL-2.0-or-later header and forbid CC BY-NC-SA / Shadertoy provenance. */
	if (!strstr(raw, "GPL-2.0-or-later")) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "Missing GPL-2.0-or-later header in %s", file_path);
		free(raw);
		return false;
	}
	if (strstr(raw, "CC BY-NC-SA") || strstr(raw, "shadertoy.com")) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "Forbidden provenance string in %s", file_path);
		free(raw);
		return false;
	}

	/* 3. Require audio_* uniform or texture reactivity and mainImage entry point. */
	if (!strstr(raw, "audio_") && !strstr(raw, "vs_sample_")) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "Shader %s does not reference any audio_* uniforms or FFT samplers",
				 file_path);
		free(raw);
		return false;
	}
	if (!strstr(raw, "mainImage")) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "Shader %s is missing mainImage entry point", file_path);
		free(raw);
		return false;
	}

	/* 4. Expand, parse uniform annotations, and verify balanced braces. */
	struct vs_shader_parse_result res;
	const enum vs_shader_error err = vs_shader_ExpandAndParse(raw, "", false, NULL, NULL, &res);
	free(raw);
	if (err != VS_SHADER_OK) {
		if (err_buf && err_cap > 0)
			snprintf(err_buf, err_cap, "Shader %s failed parse: %s", file_path, res.error_msg);
		vs_shader_parse_result_free(&res);
		return false;
	}

	vs_shader_parse_result_free(&res);
	return true;
}
