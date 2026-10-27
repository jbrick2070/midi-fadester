/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-layer-filter.h"
#include "plugin-support.h"
#include <util/threading.h>
#include <math.h>
#include <string.h>

#define PROP_OPACITY "opacity"
#define MFD_BYPASS_THRESHOLD 0.999f

struct mfd_layer_filter_data {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *param_opacity;
	volatile float opacity_01;
};

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

static const char *mfd_layer_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LayerOpacityFilterName");
}

static void mfd_layer_filter_update(void *data, obs_data_t *settings)
{
	struct mfd_layer_filter_data *f = data;
	if (!f || !settings) {
		return;
	}
	double pct = obs_data_get_double(settings, PROP_OPACITY);
	f->opacity_01 = clamp01((float)(pct / 100.0));
}

static void *mfd_layer_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct mfd_layer_filter_data *f = bzalloc(sizeof(*f));
	f->context = source;
	f->opacity_01 = 1.0f;

	char *effect_path = obs_module_file("effects/mfd_layer_mixer.effect");
	if (!effect_path) {
		obs_log(LOG_ERROR, "Failed to locate effects/mfd_layer_mixer.effect in module data directory");
		bfree(f);
		return NULL;
	}

	obs_enter_graphics();
	char *error_string = NULL;
	f->effect = gs_effect_create_from_file(effect_path, &error_string);
	if (f->effect) {
		f->param_opacity = gs_effect_get_param_by_name(f->effect, "opacity");
	}
	obs_leave_graphics();

	if (!f->effect) {
		obs_log(LOG_ERROR, "Failed to compile shader '%s': %s", effect_path,
			error_string ? error_string : "unknown error");
		bfree(error_string);
		bfree(effect_path);
		bfree(f);
		return NULL;
	}

	bfree(error_string);
	bfree(effect_path);

	mfd_layer_filter_update(f, settings);
	return f;
}

static void mfd_layer_filter_destroy(void *data)
{
	struct mfd_layer_filter_data *f = data;
	if (!f) {
		return;
	}

	obs_enter_graphics();
	if (f->effect) {
		gs_effect_destroy(f->effect);
		f->effect = NULL;
	}
	obs_leave_graphics();

	bfree(f);
}

static void mfd_layer_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, PROP_OPACITY, 100.0);
}

static obs_properties_t *mfd_layer_filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_float_slider(props, PROP_OPACITY, obs_module_text("Opacity"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, " %");
	return props;
}

static enum gs_color_space mfd_layer_filter_get_color_space(void *data, size_t count,
							    const enum gs_color_space *preferred_spaces)
{
	struct mfd_layer_filter_data *f = data;
	obs_source_t *target = f ? obs_filter_get_target(f->context) : NULL;
	if (!target) {
		return (count > 0) ? preferred_spaces[0] : GS_CS_SRGB;
	}
	return obs_source_get_color_space(target, count, preferred_spaces);
}

static void mfd_layer_filter_render(void *data, gs_effect_t *unused_effect)
{
	UNUSED_PARAMETER(unused_effect);
	struct mfd_layer_filter_data *f = data;
	if (!f || !f->context) {
		return;
	}

	float alpha = clamp01(f->opacity_01);
	if (alpha >= MFD_BYPASS_THRESHOLD || !f->effect) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space = mfd_layer_filter_get_color_space(
		f, sizeof(preferred_spaces) / sizeof(preferred_spaces[0]), preferred_spaces);

	if (obs_source_process_filter_begin_with_color_space(f->context, GS_RGBA, source_space,
							     OBS_ALLOW_DIRECT_RENDERING)) {
		if (f->param_opacity) {
			gs_effect_set_float(f->param_opacity, alpha);
		}
		obs_source_process_filter_tech_end(f->context, f->effect, 0, 0, "DrawAlpha");
	}
}

static struct obs_source_info mfd_layer_filter_info = {
	.id = MFD_LAYER_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = mfd_layer_filter_get_name,
	.create = mfd_layer_filter_create,
	.destroy = mfd_layer_filter_destroy,
	.update = mfd_layer_filter_update,
	.get_defaults = mfd_layer_filter_defaults,
	.get_properties = mfd_layer_filter_properties,
	.video_render = mfd_layer_filter_render,
	.video_get_color_space = mfd_layer_filter_get_color_space,
};

void mfd_register_layer_filter(void)
{
	obs_register_source(&mfd_layer_filter_info);
}

void mfd_layer_filter_set_opacity(obs_source_t *filter, float opacity_01)
{
	if (!filter) {
		return;
	}
	const char *id = obs_source_get_unversioned_id(filter);
	if (!id || strcmp(id, MFD_LAYER_FILTER_ID) != 0) {
		return;
	}
	struct mfd_layer_filter_data *f = obs_obj_get_data(filter);
	if (!f) {
		return;
	}
	f->opacity_01 = clamp01(opacity_01);
}

float mfd_layer_filter_get_opacity(obs_source_t *filter)
{
	if (!filter) {
		return 1.0f;
	}
	const char *id = obs_source_get_unversioned_id(filter);
	if (!id || strcmp(id, MFD_LAYER_FILTER_ID) != 0) {
		return 1.0f;
	}
	struct mfd_layer_filter_data *f = obs_obj_get_data(filter);
	return f ? clamp01(f->opacity_01) : 1.0f;
}

obs_source_t *mfd_ensure_layer_opacity_filter(obs_source_t *target_source)
{
	if (!target_source) {
		return NULL;
	}

	uint32_t flags = obs_source_get_output_flags(target_source);
	if ((flags & OBS_SOURCE_VIDEO) == 0) {
		return NULL;
	}

	obs_source_t *existing = obs_source_get_filter_by_name(target_source, MFD_INTERNAL_OPACITY_FILTER_NAME);
	if (existing) {
		return existing;
	}

	obs_data_t *settings = obs_data_create();
	obs_data_set_double(settings, PROP_OPACITY, 100.0);
	obs_source_t *created =
		obs_source_create_private(MFD_LAYER_FILTER_ID, MFD_INTERNAL_OPACITY_FILTER_NAME, settings);
	obs_data_release(settings);

	if (!created) {
		obs_log(LOG_ERROR, "Failed to create internal layer opacity filter on source '%s'",
			obs_source_get_name(target_source));
		return NULL;
	}

	obs_source_filter_add(target_source, created);
	return created;
}
