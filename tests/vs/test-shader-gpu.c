/*
 * visual-sidechain - every built-in shader compiled by a real OBS renderer (REQUIREMENTS R-PLAT-9, R-TST-1)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The shaders are written in OBS's HLSL dialect. On macOS, libobs-opengl turns each pass into GLSL and the driver
 * compiles it; libobs-metal (OBS 33's Metal renderer) turns it into Metal Shading Language. Both translators accept
 * less than HLSL does, so a shader that runs on Windows can fail to build on a Mac. This test loads each renderer
 * module built from the same OBS sources the plugin is built against, builds the exact effect text the Shader filter
 * hands to gs_effect_create for every file in data/shaders, and fails on any translation or compile error. A
 * deliberately broken shader must fail, which proves the GPU compiler really ran. A renderer whose device cannot be
 * created on this machine (no GPU, no window server) is reported as skipped; when every renderer is skipped the test
 * exits with 77, which CTest reports as skipped rather than passed.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/shader-parse.h"

#include <graphics/graphics.h>
#include <util/base.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VS_GPU_MODULES
#define VS_GPU_MODULES ""
#endif

#define SKIP_EXIT_CODE 77

/* Everything libobs logs at warning or worse while one effect builds: the translator's and the driver's messages */
static struct dstr g_errors;

static void log_handler(int level, const char *format, va_list args, void *param)
{
	(void)param;
	char line[8192];
	vsnprintf(line, sizeof(line), format, args);
	if (level <= LOG_WARNING) {
		dstr_cat(&g_errors, line);
		dstr_cat(&g_errors, "\n");
	}
	if (level <= LOG_INFO)
		printf("    [libobs] %s\n", line);
}

/* Builds the effect exactly as the Shader filter does. True when OBS's renderer compiled it. */
static bool build_effect(const char *name, const char *source, const char *base_dir, bool expect_failure)
{
	struct vs_shader_parse_result res;
	enum vs_shader_error perr = vs_shader_ExpandAndParse(source, base_dir, false, NULL, NULL, &res);
	if (perr != VS_SHADER_OK) {
		printf("  FAIL %s: the plugin's own parser refused it: %s\n", name, res.error_msg);
		vs_shader_parse_result_free(&res);
		return false;
	}
	dstr_free(&g_errors);
	char *effect_err = NULL;
	gs_effect_t *effect = gs_effect_create(res.full_effect_source, name, &effect_err);
	bool ok = effect != NULL && gs_effect_get_technique(effect, "Draw") != NULL;
	if (!ok && !expect_failure) {
		printf("  FAIL %s\n", name);
		if (effect_err)
			printf("    effect error: %s\n", effect_err);
		if (g_errors.len)
			printf("    renderer log:\n%s", g_errors.array);
	} else if (ok && expect_failure) {
		printf("  FAIL %s: compiled, but it must not\n", name);
	} else {
		printf("  ok   %s%s\n", name, expect_failure ? " (refused, as it must be)" : "");
	}
	if (effect)
		gs_effect_destroy(effect);
	bfree(effect_err);
	vs_shader_parse_result_free(&res);
	return ok;
}

static bool ends_with(const char *s, const char *suffix)
{
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* Returns the number of failures for one renderer, or -1 when its device cannot be created here */
static int check_renderer(const char *module)
{
	printf("renderer %s\n", module);
	graphics_t *graphics = NULL;
	int rc = gs_create(&graphics, module, 0);
	if (rc != GS_SUCCESS) {
		printf("  SKIP: the device could not be created on this machine (gs_create returned %d)\n", rc);
		if (g_errors.len)
			printf("    renderer log:\n%s", g_errors.array);
		if (graphics)
			gs_destroy(graphics);
		return -1;
	}
	gs_enter_context(graphics);

	int failures = 0, built = 0, bad = 0;
	os_dir_t *dir = os_opendir(VS_SHADERS_DIR);
	assert(dir);
	struct os_dirent *ent;
	while ((ent = os_readdir(dir)) != NULL) {
		if (ent->directory || !ends_with(ent->d_name, ".shader"))
			continue;
		struct dstr path = {0};
		dstr_printf(&path, "%s/%s", VS_SHADERS_DIR, ent->d_name);
		char *text = os_quick_read_utf8_file(path.array);
		assert(text);
		if (!build_effect(ent->d_name, text, VS_SHADERS_DIR, false))
			bad++;
		built++;
		bfree(text);
		dstr_free(&path);
	}
	os_closedir(dir);
	failures += bad;
	/* all eleven built-in shaders, so a renamed or missing data folder cannot pass by building nothing */
	if (built < 11) {
		printf("  FAIL: only %d shaders found in %s\n", built, VS_SHADERS_DIR);
		failures++;
	}

	/* Negative control: a call to a function that does not exist passes OBS's effect parser and must be refused by the
	 * GPU compiler. If it builds, nothing above was really compiled. */
	const char *broken = "float4 mainImage(VertData v_in) : TARGET\n"
			     "{\n"
			     "\treturn vs_no_such_function(v_in.uv) * audio_level;\n"
			     "}\n";
	if (build_effect("negative-control (undefined function)", broken, "", true))
		failures++;

	gs_leave_context();
	gs_destroy(graphics);
	printf("  %d of %d built-in shaders built\n", built - bad, built);
	return failures;
}

int main(void)
{
	base_set_log_handler(log_handler, NULL);
	printf("test-shader-gpu: shaders in %s\n", VS_SHADERS_DIR);

	char modules[4096];
	snprintf(modules, sizeof(modules), "%s", VS_GPU_MODULES);
	int failures = 0, tested = 0;
	for (char *module = strtok(modules, "|"); module; module = strtok(NULL, "|")) {
		if (!*module)
			continue;
		int r = check_renderer(module);
		if (r < 0)
			continue;
		tested++;
		failures += r;
	}
	dstr_free(&g_errors);

	if (failures) {
		printf("test-shader-gpu: %d failure(s)\n", failures);
		return 1;
	}
	if (!tested) {
		printf("test-shader-gpu: SKIPPED, no renderer could create a device here\n");
		return SKIP_EXIT_CODE;
	}
	printf("test-shader-gpu: ALL PASS on %d renderer(s)\n", tested);
	return 0;
}
