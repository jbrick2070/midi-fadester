/*
 * Midi Fadester — test-vendor-api
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include "core/tbar-logic.h"
#include "core/mfd-text.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/text-lookup.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("midi-fadester", "en-US")

/* en-US.ini through OBS's own text lookup, as the plugin loads it, so English assertions pass through the real keys */
static const char *test_locale_text(const char *key)
{
	const char *text = NULL;
	return (obs_module_get_string(key, &text) && text) ? text : NULL;
}

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static int g_mock_tbar_calls = 0;
static int g_mock_tbar_set_writes = 0;
static int g_mock_tbar_completions = 0;
static int g_mock_tbar_aborts = 0;
static int g_mock_tbar_unlatches = 0;
static int g_mock_tbar_last_pos = 0;
static bool g_mock_tbar_latched = false;
static float g_mock_tbar_last_val = -1.0f;

static void mock_tbar_unlatch(void)
{
	++g_mock_tbar_unlatches;
	g_mock_tbar_latched = false;
	g_mock_tbar_last_pos = 0;
}

static bool mock_tbar_driver(enum mfd_tbar_state state, float t_norm, bool force_complete, bool force_abort,
			     bool in_target_rail_zone, bool in_origin_rail_zone, bool in_start_deadband,
			     enum mfd_tbar_state *out_next_state, bool *out_completed_throw, bool *out_aborted_throw,
			     char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	++g_mock_tbar_calls;
	g_mock_tbar_last_val = t_norm;

	struct mfd_tbar_release_input in;
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.state = state;
	in.release_latched = g_mock_tbar_latched;
	in.force_complete = force_complete;
	in.force_abort = force_abort;
	in.in_target_rail_zone = in_target_rail_zone;
	in.in_origin_rail_zone = in_origin_rail_zone;
	in.in_start_deadband = in_start_deadband;
	in.t_norm = t_norm;
	in.last_pos_1024 = g_mock_tbar_last_pos;

	struct mfd_tbar_action act;
	mfd_tbar_compute_action_full(&in, &act);
	if (out_next_state) {
		*out_next_state = act.next_state;
	}
	if (act.call_set_tbar) {
		++g_mock_tbar_set_writes;
	}
	if (act.call_release_tbar) {
		g_mock_tbar_latched = true;
		g_mock_tbar_last_pos = 0;
		if (act.completed_throw) {
			++g_mock_tbar_completions;
		}
		if (act.aborted_throw) {
			++g_mock_tbar_aborts;
		}
		if (out_completed_throw) {
			*out_completed_throw = act.completed_throw;
		}
		if (out_aborted_throw) {
			*out_aborted_throw = act.aborted_throw;
		}
	} else {
		if (!g_mock_tbar_latched) {
			g_mock_tbar_last_pos = act.target_pos_1024;
		}
		if (out_completed_throw) {
			*out_completed_throw = false;
		}
		if (out_aborted_throw) {
			*out_aborted_throw = false;
		}
	}
	return true;
}

static void push_midi_cc(struct mfd_engine *eng, uint8_t ch, uint8_t cc, uint8_t val)
{
	uint32_t raw_msg = (uint32_t)(0xB0U | (ch & 0x0FU)) | ((uint32_t)(cc & 0x7FU) << 8) |
			   ((uint32_t)(val & 0x7FU) << 16);
	mfd_midi_queue_push(&eng->midi_queue, raw_msg, 0U);
}

static bool get_slot_parked_via_get_state(struct mfd_engine *eng, int slot_idx)
{
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, eng);
	obs_data_array_t *arr = obs_data_get_array(res, "slots");
	obs_data_t *item = obs_data_array_item(arr, (size_t)slot_idx);
	bool parked = obs_data_get_bool(item, "parked");
	obs_data_release(item);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);
	return parked;
}

#define FAKE_MAX_SOURCES 16
#define FAKE_MAX_SCENES 8
#define FAKE_MAX_SCENE_ITEMS 8
#define FAKE_MAX_FILTERS 24

enum fake_filter_schema {
	FAKE_SCHEMA_BLUR_GROUPED = 0,
	FAKE_SCHEMA_AUDIO_MOD = 1,
	FAKE_SCHEMA_INTERNAL = 2,
};

struct fake_sg_filter {
	bool in_use;
	char host_name[64];
	char filter_name[64];
	char type_id[64];
	bool enabled;
	enum fake_filter_schema schema_kind;
	double float_max_override;
	bool omit_radius_prop;
	int settings_writes;
	int enable_writes;
	obs_data_t *settings;
};

struct fake_sg_source {
	bool in_use;
	char name[64];
	bool has_audio;
	float last_opacity;
	float last_volume_mul;
	int opacity_writes;
	int volume_writes;
};

struct fake_sg_scene {
	bool in_use;
	char name[64];
	int item_count;
	char items[FAKE_MAX_SCENE_ITEMS][64]; /* index 0 = bottom, index item_count-1 = top */
};

struct fake_scene_graph {
	struct fake_sg_source sources[FAKE_MAX_SOURCES];
	struct fake_sg_scene scenes[FAKE_MAX_SCENES];
	struct fake_sg_filter filters[FAKE_MAX_FILTERS];
	int total_opacity_writes;
	int total_volume_writes;
	int total_filter_settings_writes;
	int total_filter_enable_writes;
	struct mfd_engine *owner_eng;
};

static struct fake_sg_source *fake_sg_find_source(struct fake_scene_graph *sg, const char *name)
{
	if (!sg || !name || name[0] == '\0') {
		return NULL;
	}
	for (int i = 0; i < FAKE_MAX_SOURCES; ++i) {
		if (sg->sources[i].in_use && strcmp(sg->sources[i].name, name) == 0) {
			return &sg->sources[i];
		}
	}
	return NULL;
}

static struct fake_sg_scene *fake_sg_find_scene(struct fake_scene_graph *sg, const char *name)
{
	if (!sg || !name || name[0] == '\0') {
		return NULL;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (sg->scenes[i].in_use && strcmp(sg->scenes[i].name, name) == 0) {
			return &sg->scenes[i];
		}
	}
	return NULL;
}

static void fake_sg_add_source(struct fake_scene_graph *sg, const char *name, bool has_audio)
{
	struct fake_sg_source *existing = fake_sg_find_source(sg, name);
	if (existing) {
		existing->has_audio = has_audio;
		return;
	}
	for (int i = 0; i < FAKE_MAX_SOURCES; ++i) {
		if (!sg->sources[i].in_use) {
			memset(&sg->sources[i], 0, sizeof(sg->sources[i]));
			sg->sources[i].in_use = true;
			snprintf(sg->sources[i].name, sizeof(sg->sources[i].name), "%s", name);
			sg->sources[i].has_audio = has_audio;
			sg->sources[i].last_opacity = 1.0f;
			sg->sources[i].last_volume_mul = 1.0f;
			return;
		}
	}
}

static struct fake_sg_scene *fake_sg_add_scene(struct fake_scene_graph *sg, const char *name)
{
	struct fake_sg_scene *existing = fake_sg_find_scene(sg, name);
	if (existing) {
		return existing;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			memset(&sg->scenes[i], 0, sizeof(sg->scenes[i]));
			sg->scenes[i].in_use = true;
			snprintf(sg->scenes[i].name, sizeof(sg->scenes[i].name), "%s", name);
			return &sg->scenes[i];
		}
	}
	return NULL;
}

static void fake_sg_scene_add_item(struct fake_scene_graph *sg, const char *scene_name, const char *source_name)
{
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (!sc || sc->item_count >= FAKE_MAX_SCENE_ITEMS) {
		return;
	}
	snprintf(sc->items[sc->item_count], sizeof(sc->items[sc->item_count]), "%s", source_name);
	++sc->item_count;
}

static void fake_sg_scene_remove_item(struct fake_scene_graph *sg, const char *scene_name, const char *source_name)
{
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (!sc) {
		return;
	}
	for (int i = 0; i < sc->item_count; ++i) {
		if (strcmp(sc->items[i], source_name) == 0) {
			for (int j = i; j + 1 < sc->item_count; ++j) {
				snprintf(sc->items[j], sizeof(sc->items[j]), "%s", sc->items[j + 1]);
			}
			sc->items[sc->item_count - 1][0] = '\0';
			--sc->item_count;
			return;
		}
	}
}

static struct fake_sg_filter *fake_sg_find_filter(struct fake_scene_graph *sg, const char *host_name,
						  const char *filter_name)
{
	if (!sg || !host_name || !host_name[0] || !filter_name || !filter_name[0]) {
		return NULL;
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (sg->filters[i].in_use && strcmp(sg->filters[i].host_name, host_name) == 0 &&
		    strcmp(sg->filters[i].filter_name, filter_name) == 0) {
			return &sg->filters[i];
		}
	}
	return NULL;
}

static struct fake_sg_filter *fake_sg_add_filter(struct fake_scene_graph *sg, const char *host_name,
						 const char *filter_name, const char *type_id, bool enabled,
						 enum fake_filter_schema schema_kind)
{
	struct fake_sg_filter *existing = fake_sg_find_filter(sg, host_name, filter_name);
	if (existing) {
		existing->enabled = enabled;
		existing->schema_kind = schema_kind;
		return existing;
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (!sg->filters[i].in_use) {
			memset(&sg->filters[i], 0, sizeof(sg->filters[i]));
			sg->filters[i].in_use = true;
			snprintf(sg->filters[i].host_name, sizeof(sg->filters[i].host_name), "%s", host_name);
			snprintf(sg->filters[i].filter_name, sizeof(sg->filters[i].filter_name), "%s", filter_name);
			snprintf(sg->filters[i].type_id, sizeof(sg->filters[i].type_id), "%s",
				 type_id ? type_id : "mock_filter");
			sg->filters[i].enabled = enabled;
			sg->filters[i].schema_kind = schema_kind;
			sg->filters[i].float_max_override = 50.0;
			sg->filters[i].omit_radius_prop = false;
			sg->filters[i].settings = obs_data_create();
			if (schema_kind == FAKE_SCHEMA_BLUR_GROUPED) {
				obs_data_set_default_double(sg->filters[i].settings, "radius", 10.0);
				obs_data_set_default_int(sg->filters[i].settings, "passes", 2);
				obs_data_set_default_int(sg->filters[i].settings, "mode", 1);
				obs_data_set_default_bool(sg->filters[i].settings, "clamp_edges", true);
			} else if (schema_kind == FAKE_SCHEMA_AUDIO_MOD) {
				obs_data_set_default_double(sg->filters[i].settings, "slot_1_amount_pct", 0.0);
				obs_data_set_default_double(sg->filters[i].settings, "slot_2_amount_pct", 25.0);
			}
			return &sg->filters[i];
		}
	}
	return NULL;
}

static void fake_sg_remove_filter(struct fake_scene_graph *sg, const char *host_name, const char *filter_name)
{
	struct fake_sg_filter *f = fake_sg_find_filter(sg, host_name, filter_name);
	if (f) {
		if (f->settings) {
			obs_data_release(f->settings);
			f->settings = NULL;
		}
		memset(f, 0, sizeof(*f));
	}
}

static void fake_sg_rename_filter(struct fake_scene_graph *sg, const char *host_name, const char *prev_name,
				  const char *new_name)
{
	struct fake_sg_filter *f = fake_sg_find_filter(sg, host_name, prev_name);
	if (f) {
		snprintf(f->filter_name, sizeof(f->filter_name), "%s", new_name);
	}
}

static void fake_sg_cleanup_filters(struct fake_scene_graph *sg)
{
	if (!sg) {
		return;
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (sg->filters[i].in_use && sg->filters[i].settings) {
			obs_data_release(sg->filters[i].settings);
			sg->filters[i].settings = NULL;
		}
		sg->filters[i].in_use = false;
	}
}

static void fake_sg_remove_source(struct fake_scene_graph *sg, const char *name)
{
	struct fake_sg_source *src = fake_sg_find_source(sg, name);
	if (src) {
		memset(src, 0, sizeof(*src));
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (sg->filters[i].in_use && strcmp(sg->filters[i].host_name, name) == 0) {
			if (sg->filters[i].settings) {
				obs_data_release(sg->filters[i].settings);
			}
			memset(&sg->filters[i], 0, sizeof(sg->filters[i]));
		}
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (sg->scenes[i].in_use) {
			fake_sg_scene_remove_item(sg, sg->scenes[i].name, name);
		}
	}
}

static void fake_sg_rename(struct fake_scene_graph *sg, const char *prev_name, const char *new_name)
{
	struct fake_sg_source *src = fake_sg_find_source(sg, prev_name);
	if (src) {
		snprintf(src->name, sizeof(src->name), "%s", new_name);
	}
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, prev_name);
	if (sc) {
		snprintf(sc->name, sizeof(sc->name), "%s", new_name);
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (sg->filters[i].in_use && strcmp(sg->filters[i].host_name, prev_name) == 0) {
			snprintf(sg->filters[i].host_name, sizeof(sg->filters[i].host_name), "%s", new_name);
		}
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			continue;
		}
		for (int j = 0; j < sg->scenes[i].item_count; ++j) {
			if (strcmp(sg->scenes[i].items[j], prev_name) == 0) {
				snprintf(sg->scenes[i].items[j], sizeof(sg->scenes[i].items[j]), "%s", new_name);
			}
		}
	}
}

static bool fake_op_source_exists(const char *source_name, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	return fake_sg_find_source(sg, source_name) != NULL || fake_sg_find_scene(sg, source_name) != NULL;
}

static bool fake_op_scene_exists(const char *scene_name, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	return fake_sg_find_scene(sg, scene_name) != NULL;
}

static bool fake_op_source_has_audio(const char *source_name, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_source *src = fake_sg_find_source(sg, source_name);
	return src ? src->has_audio : false;
}

static int fake_op_scene_item_stack_index(const char *scene_name, const char *source_name, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (!sc || !source_name) {
		return -1;
	}
	for (int i = 0; i < sc->item_count; ++i) {
		if (strcmp(sc->items[i], source_name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool fake_op_find_scene_with_layer(const char *source_name, const char *exclude_scene, char *out_scene,
					  size_t out_size, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!sg || !source_name) {
		return false;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			continue;
		}
		if (exclude_scene && exclude_scene[0] != '\0' && strcmp(sg->scenes[i].name, exclude_scene) == 0) {
			continue;
		}
		if (fake_op_scene_item_stack_index(sg->scenes[i].name, source_name, sg) >= 0) {
			if (out_scene && out_size > 0U) {
				snprintf(out_scene, out_size, "%s", sg->scenes[i].name);
			}
			return true;
		}
	}
	return false;
}

static bool fake_op_find_scene_with_pair(const char *source_a, const char *source_b, char *out_scene, size_t out_size,
					 int *out_idx_a, int *out_idx_b, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!sg || !source_a || !source_b) {
		return false;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			continue;
		}
		int ia = fake_op_scene_item_stack_index(sg->scenes[i].name, source_a, sg);
		int ib = fake_op_scene_item_stack_index(sg->scenes[i].name, source_b, sg);
		if (ia >= 0 && ib >= 0) {
			if (out_scene && out_size > 0U) {
				snprintf(out_scene, out_size, "%s", sg->scenes[i].name);
			}
			if (out_idx_a) {
				*out_idx_a = ia;
			}
			if (out_idx_b) {
				*out_idx_b = ib;
			}
			return true;
		}
	}
	return false;
}

static void fake_op_apply_opacity(const char *source_name, float alpha_01, bool afv, void *user_data)
{
	UNUSED_PARAMETER(afv);
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!sg) {
		return;
	}
	++sg->total_opacity_writes;
	struct fake_sg_source *src = fake_sg_find_source(sg, source_name);
	if (src) {
		src->last_opacity = alpha_01;
		++src->opacity_writes;
	}
}

static void fake_op_apply_volume(const char *source_name, float mul, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!sg) {
		return;
	}
	++sg->total_volume_writes;
	struct fake_sg_source *src = fake_sg_find_source(sg, source_name);
	if (src) {
		src->last_volume_mul = mul;
		++src->volume_writes;
	}
}

static bool fake_op_get_volume(const char *source_name, float *out_mul, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_source *src = sg ? fake_sg_find_source(sg, source_name) : NULL;
	if (!src || !out_mul) {
		return false;
	}
	*out_mul = src->last_volume_mul;
	return true;
}

static void fake_op_fill_catalog(struct mfd_ui_catalog *out, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!out || !sg) {
		return;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			continue;
		}
		if (out->scene_count >= MFD_CATALOG_MAX_SCENES) {
			out->scenes_truncated = true;
			break;
		}
		struct mfd_ui_scene_entry *se = &out->scenes[out->scene_count++];
		memset(se, 0, sizeof(*se));
		snprintf(se->name, sizeof(se->name), "%s", sg->scenes[i].name);
		for (int j = 0; j < sg->scenes[i].item_count; ++j) {
			const char *lnm = sg->scenes[i].items[j];
			bool dup = false;
			for (size_t k = 0U; k < se->layer_count; ++k) {
				if (strcmp(se->layers[k], lnm) == 0) {
					dup = true;
					break;
				}
			}
			if (dup) {
				continue;
			}
			if (se->layer_count >= MFD_CATALOG_MAX_SCENE_LAYERS) {
				se->layers_truncated = true;
				break;
			}
			snprintf(se->layers[se->layer_count], sizeof(se->layers[se->layer_count]), "%s", lnm);
			se->layer_has_audio[se->layer_count] = fake_op_source_has_audio(lnm, sg);
			++se->layer_count;
		}
	}
	for (int i = 0; i < FAKE_MAX_SOURCES; ++i) {
		if (!sg->sources[i].in_use || !sg->sources[i].has_audio) {
			continue;
		}
		if (out->audio_count >= MFD_CATALOG_MAX_AUDIO_SOURCES) {
			out->audio_truncated = true;
			break;
		}
		struct mfd_ui_audio_entry *ae = &out->audio_sources[out->audio_count++];
		snprintf(ae->name, sizeof(ae->name), "%s", sg->sources[i].name);
		ae->db = mfd_mul_to_db(sg->sources[i].last_volume_mul);
	}
}

static void fake_op_set_studio_mode(bool enabled, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (sg && sg->owner_eng) {
		sg->owner_eng->studio_mode_override_set = true;
		sg->owner_eng->studio_mode_override = enabled;
	}
}

static void fake_op_enum_filter_hosts(mfd_filter_host_cb cb, void *cb_param, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!sg || !cb) {
		return;
	}
	for (int i = 0; i < FAKE_MAX_SOURCES; ++i) {
		if (sg->sources[i].in_use) {
			cb(sg->sources[i].name, false, cb_param);
		}
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (sg->scenes[i].in_use) {
			cb(sg->scenes[i].name, true, cb_param);
		}
	}
}

static bool fake_op_enum_source_filters(const char *source_name, bool *out_is_scene, mfd_source_filter_cb cb,
					void *cb_param, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	if (!sg || !source_name || !fake_op_source_exists(source_name, sg)) {
		return false;
	}
	if (out_is_scene) {
		*out_is_scene = fake_op_scene_exists(source_name, sg);
	}
	if (cb) {
		for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
			if (sg->filters[i].in_use && strcmp(sg->filters[i].host_name, source_name) == 0) {
				cb(sg->filters[i].filter_name, sg->filters[i].type_id, sg->filters[i].enabled,
				   cb_param);
			}
		}
	}
	return true;
}

static obs_properties_t *fake_op_create_filter_properties(const char *source_name, const char *filter_name,
							  void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_filter *f = fake_sg_find_filter(sg, source_name, filter_name);
	if (!f) {
		return NULL;
	}
	obs_properties_t *props = obs_properties_create();
	if (!props) {
		return NULL;
	}
	if (f->schema_kind == FAKE_SCHEMA_BLUR_GROUPED) {
		/* Non-numeric list & bool must be excluded from numeric property catalog */
		obs_property_t *lst =
			obs_properties_add_list(props, "mode", "Blur Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(lst, "Box", 0);
		obs_property_list_add_int(lst, "Gaussian", 1);
		obs_properties_add_bool(props, "clamp_edges", "Clamp Edges");

		obs_properties_t *grp = obs_properties_create();
		if (!f->omit_radius_prop) {
			obs_property_t *rad = obs_properties_add_float_slider(grp, "radius", "Radius", 0.0,
									      f->float_max_override, 0.5);
			obs_property_float_set_suffix(rad, " px");
		}
		obs_properties_t *inner = obs_properties_create();
		obs_properties_add_int_slider(inner, "passes", "Passes", 1, 8, 1);
		obs_properties_add_group(grp, "inner_group", "Quality", OBS_GROUP_NORMAL, inner);
		obs_properties_add_group(props, "blur_group", "Blur Settings", OBS_GROUP_NORMAL, grp);
	} else if (f->schema_kind == FAKE_SCHEMA_AUDIO_MOD) {
		obs_property_t *s1 = obs_properties_add_float_slider(props, "slot_1_amount_pct", "Slot 1 Amount",
								     -100.0, 100.0, 1.0);
		obs_property_float_set_suffix(s1, "%");
		obs_property_t *s2 =
			obs_properties_add_float_slider(props, "slot_2_amount_pct", "Slot 2 Amount", 0.0, 100.0, 1.0);
		obs_property_float_set_suffix(s2, "%");
	} else {
		obs_properties_add_float_slider(props, "opacity", "Opacity", 0.0, 1.0, 0.01);
	}
	return props;
}

static obs_data_t *fake_op_get_filter_settings(const char *source_name, const char *filter_name, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_filter *f = fake_sg_find_filter(sg, source_name, filter_name);
	if (!f || !f->settings) {
		return NULL;
	}
	obs_data_t *copy = obs_data_create();
	if (copy) {
		obs_data_set_default_double(copy, "radius", obs_data_get_default_double(f->settings, "radius"));
		obs_data_set_default_int(copy, "passes", obs_data_get_default_int(f->settings, "passes"));
		obs_data_set_default_double(copy, "slot_1_amount_pct",
					    obs_data_get_default_double(f->settings, "slot_1_amount_pct"));
		obs_data_set_default_double(copy, "slot_2_amount_pct",
					    obs_data_get_default_double(f->settings, "slot_2_amount_pct"));
		obs_data_apply(copy, f->settings);
	}
	return copy;
}

static void fake_op_update_filter_settings(const char *source_name, const char *filter_name, obs_data_t *settings,
					   void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_filter *f = fake_sg_find_filter(sg, source_name, filter_name);
	if (!f || !f->settings || !settings) {
		return;
	}
	obs_data_apply(f->settings, settings);
	++f->settings_writes;
	++sg->total_filter_settings_writes;
	if (sg->owner_eng) {
		/* Simulate OBS synchronous "update" signal inside obs_source_update */
		mfd_engine_handle_filter_settings_updated(sg->owner_eng, source_name, filter_name);
	}
}

static bool fake_op_get_filter_enabled(const char *source_name, const char *filter_name, bool *out_enabled,
				       void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_filter *f = fake_sg_find_filter(sg, source_name, filter_name);
	if (!f) {
		return false;
	}
	if (out_enabled) {
		*out_enabled = f->enabled;
	}
	return true;
}

static void fake_op_set_filter_enabled(const char *source_name, const char *filter_name, bool enabled, void *user_data)
{
	struct fake_scene_graph *sg = (struct fake_scene_graph *)user_data;
	struct fake_sg_filter *f = fake_sg_find_filter(sg, source_name, filter_name);
	if (!f) {
		return;
	}
	f->enabled = enabled;
	++f->enable_writes;
	++sg->total_filter_enable_writes;
	if (sg->owner_eng) {
		/* Simulate OBS synchronous "enable" signal inside obs_source_set_enabled */
		mfd_engine_handle_filter_enable(sg->owner_eng, source_name, filter_name, enabled);
	}
}

/* OBS output channel 1 is Desktop Audio; the fake names it as a localized OBS might. */
static bool fake_op_get_output_source_name(uint32_t channel, char *out_name, size_t out_size, void *user_data)
{
	(void)user_data;
	if (channel != 1U || !out_name || out_size == 0U) {
		return false;
	}
	snprintf(out_name, out_size, "%s", "desktop audio");
	return true;
}

static const struct mfd_scene_graph_ops g_fake_sg_ops = {
	fake_op_source_exists,
	fake_op_scene_exists,
	fake_op_source_has_audio,
	fake_op_scene_item_stack_index,
	fake_op_find_scene_with_layer,
	fake_op_find_scene_with_pair,
	fake_op_apply_opacity,
	fake_op_apply_volume,
	fake_op_fill_catalog,
	fake_op_set_studio_mode,
	fake_op_enum_filter_hosts,
	fake_op_enum_source_filters,
	fake_op_create_filter_properties,
	fake_op_get_filter_settings,
	fake_op_update_filter_settings,
	fake_op_get_filter_enabled,
	fake_op_set_filter_enabled,
	fake_op_get_output_source_name,
};

struct mt_stress_ctx {
	struct mfd_engine *eng;
	volatile long ui_notify_calls;
	bool bad_snapshot;
};

static void on_mt_ui_notify(void *user_data)
{
	struct mt_stress_ctx *ctx = (struct mt_stress_ctx *)user_data;
	if (!ctx || !ctx->eng) {
		return;
	}
	/* Verify ui_notify_fn runs strictly outside state_mutex (no deadlock, lock_depth == 0 on fresh lock) */
	os_atomic_inc_long(&ctx->ui_notify_calls);
}

static void *mt_tick_worker(void *arg)
{
	struct mt_stress_ctx *ctx = (struct mt_stress_ctx *)arg;
	for (int iter = 0; iter < 400; ++iter) {
		push_midi_cc(ctx->eng, (uint8_t)(iter % 4), 7, (uint8_t)(iter & 0x7F));
		push_midi_cc(ctx->eng, 0, 15, (uint8_t)((iter * 3) & 0x7F));
		push_midi_cc(ctx->eng, 0, 16, (uint8_t)((iter * 5) & 0x7F)); /* FX 1 Device Knob 1 */
		mfd_engine_tick(ctx->eng, 0.008f);
	}
	return NULL;
}

static void *mt_vendor_worker(void *arg)
{
	struct mt_stress_ctx *ctx = (struct mt_stress_ctx *)arg;
	for (int iter = 0; iter < 250; ++iter) {
		obs_data_t *req = obs_data_create();
		obs_data_t *res = obs_data_create();
		mfd_vendor_handle_get_state(req, res, ctx->eng);
		obs_data_release(req);
		obs_data_release(res);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_2");
		obs_data_set_double(req, "value", (double)(iter % 100) / 100.0);
		mfd_vendor_handle_set_fader_value(req, res, ctx->eng);
		obs_data_release(req);
		obs_data_release(res);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_double(req, "value", (double)((iter * 7) % 100) / 100.0);
		mfd_vendor_handle_set_fader_value(req, res, ctx->eng);
		obs_data_release(req);
		obs_data_release(res);

		if ((iter % 25) == 0) {
			req = obs_data_create();
			res = obs_data_create();
			obs_data_set_string(req, "slot", "fader_3");
			obs_data_set_string(req, "target_mode", "audio_volume");
			obs_data_set_string(req, "source_a", "Mic/Aux");
			mfd_vendor_handle_set_slot(req, res, ctx->eng);
			obs_data_release(req);
			obs_data_release(res);

			req = obs_data_create();
			res = obs_data_create();
			mfd_vendor_handle_get_catalog(req, res, ctx->eng);
			obs_data_release(req);
			obs_data_release(res);
		}
		if ((iter % 50) == 0) {
			req = obs_data_create();
			res = obs_data_create();
			obs_data_set_int(req, "ms", 0);
			mfd_vendor_handle_reset_fx(req, res, ctx->eng);
			obs_data_release(req);
			obs_data_release(res);
		}
	}
	return NULL;
}

static void *mt_ui_worker(void *arg)
{
	struct mt_stress_ctx *ctx = (struct mt_stress_ctx *)arg;
	struct mfd_ui_snapshot *snap = (struct mfd_ui_snapshot *)calloc(1U, sizeof(*snap));
	struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
	struct mfd_source_filter_catalog *fcat = mfd_source_filter_catalog_create();
	if (!snap || !cat || !fcat) {
		ctx->bad_snapshot = true;
		free(snap);
		mfd_ui_catalog_destroy(cat);
		mfd_source_filter_catalog_destroy(fcat);
		return NULL;
	}
	for (int iter = 0; iter < 250; ++iter) {
		mfd_engine_get_ui_snapshot(ctx->eng, snap);
		if (snap->slots[0].id[0] == '\0' || snap->slots[9].id[0] == '\0' || snap->slots[17].id[0] == '\0') {
			ctx->bad_snapshot = true;
		}
		mfd_engine_get_ui_catalog(ctx->eng, cat);
		if (cat->scene_count == 0U || cat->audio_count == 0U) {
			ctx->bad_snapshot = true;
		}
		if ((iter % 15) == 0) {
			mfd_engine_get_source_filter_catalog(ctx->eng, "Color", fcat);
		}
		if ((iter % 20) == 0) {
			mfd_engine_begin_setup_session(ctx->eng, 10);
			mfd_engine_set_setup_invert(ctx->eng, (iter % 40) == 0);
			mfd_engine_cancel_setup_session(ctx->eng);
		}
		mfd_engine_flush_pending_ui_work(ctx->eng);
	}
	free(snap);
	mfd_ui_catalog_destroy(cat);
	mfd_source_filter_catalog_destroy(fcat);
	return NULL;
}

struct fake_ws_log {
	int mix_requests;
	char mix_id[40];
	double mix_db;
	int fader_changed_count;
	int state_changed_count;
	char last_fader_slot[32];
	double last_fader_value;
	char last_fader_display_value[64];
	char last_fader_origin[32];
};

static void on_fake_ws_event(const char *event_name, obs_data_t *event_data, void *user_data)
{
	struct fake_ws_log *log = (struct fake_ws_log *)user_data;
	if (!log || !event_name || !event_data) {
		return;
	}
	if (strcmp(event_name, "fader_changed") == 0) {
		++log->fader_changed_count;
		const char *s = obs_data_get_string(event_data, "slot");
		const char *o = obs_data_get_string(event_data, "origin");
		const char *dv = obs_data_get_string(event_data, "display_value");
		snprintf(log->last_fader_slot, sizeof(log->last_fader_slot), "%s", s ? s : "");
		snprintf(log->last_fader_origin, sizeof(log->last_fader_origin), "%s", o ? o : "");
		snprintf(log->last_fader_display_value, sizeof(log->last_fader_display_value), "%s", dv ? dv : "");
		log->last_fader_value = obs_data_get_double(event_data, "value");
	} else if (strcmp(event_name, "state_changed") == 0) {
		++log->state_changed_count;
	} else if (strcmp(event_name, "CustomEvent") == 0 &&
		   strcmp(obs_data_get_string(event_data, "client"), "midi-fadester") == 0 &&
		   strcmp(obs_data_get_string(event_data, "namespace"), MFD_MIX_NAMESPACE) == 0 &&
		   strcmp(obs_data_get_string(event_data, "op"), "mix.set") == 0 &&
		   obs_data_get_int(event_data, "sentAtMs") > 0) {
		++log->mix_requests;
		snprintf(log->mix_id, sizeof(log->mix_id), "%s", obs_data_get_string(event_data, "id"));
		log->mix_db = obs_data_get_double(event_data, "db");
	}
}

struct fake_signal_log {
	int count;
	char slot[32];
	float value;
	char display_value[64];
	char origin[32];
};

static void on_fake_signal(void *data, calldata_t *cd)
{
	struct fake_signal_log *sig = (struct fake_signal_log *)data;
	if (!sig || !cd) {
		return;
	}
	++sig->count;
	const char *s = calldata_string(cd, "slot");
	const char *o = calldata_string(cd, "origin");
	const char *dv = calldata_string(cd, "display_value");
	snprintf(sig->slot, sizeof(sig->slot), "%s", s ? s : "");
	snprintf(sig->origin, sizeof(sig->origin), "%s", o ? o : "");
	snprintf(sig->display_value, sizeof(sig->display_value), "%s", dv ? dv : "");
	sig->value = (float)calldata_float(cd, "value");
}

static bool start_config_test_throw(struct mfd_engine *eng)
{
	mfd_engine_cancel_setup_session(eng);
	eng->studio_mode_override = true;
	eng->tbar_error[0] = '\0';
	mfd_engine_load_apc40_defaults(eng);
	char error[160] = {0};
	return mfd_engine_set_slot_value(eng, 0, 0.5f, false, "api", error, sizeof(error)) &&
	       eng->fader_slots[0].tbar_state == MFD_TBAR_THROWING;
}

static int test_config_abort_and_movement_errors(void)
{
	struct mfd_engine *eng = calloc(1U, sizeof(*eng));
	REQUIRE(eng != NULL);
	REQUIRE(mfd_engine_init_mutex(eng));
	eng->tbar_driver = mock_tbar_driver;
	eng->tbar_unlatch = mock_tbar_unlatch;
	eng->studio_mode_override_set = true;
	eng->studio_mode_override = true;

	/* Valid vendor disables/reassignments/same-mode resets abort before fields change. */
	for (int edit_case = 0; edit_case < 3; ++edit_case) {
		REQUIRE(start_config_test_throw(eng));
		int aborts = g_mock_tbar_aborts;
		int completions = g_mock_tbar_completions;
		obs_data_t *req = obs_data_create();
		obs_data_t *res = obs_data_create();
		obs_data_set_string(req, "slot", "crossfader");
		if (edit_case == 0) {
			obs_data_set_bool(req, "enabled", false);
		} else {
			obs_data_set_string(req, "target_mode", edit_case == 1 ? "unassigned" : "tbar");
		}
		mfd_vendor_handle_set_slot(req, res, eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(g_mock_tbar_aborts == aborts + 1);
		REQUIRE(g_mock_tbar_completions == completions);
		REQUIRE(g_mock_tbar_last_pos == 0);
		REQUIRE(eng->fader_slots[0].tbar_state == MFD_TBAR_IDLE);
		REQUIRE(fabsf(eng->fader_slots[0].last_applied_val) < 1e-6f);
		obs_data_release(req);
		obs_data_release(res);
	}

	/* Invalid vendor and Advanced candidates leave the in-flight throw untouched. */
	REQUIRE(start_config_test_throw(eng));
	int aborts = g_mock_tbar_aborts;
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_bool(req, "enabled", false);
	obs_data_set_string(req, "target_mode", "not_a_target");
	mfd_vendor_handle_set_slot(req, res, eng);
	REQUIRE(obs_data_has_user_value(res, "error"));
	REQUIRE(g_mock_tbar_aborts == aborts);
	REQUIRE(eng->fader_slots[0].tbar_state == MFD_TBAR_THROWING);
	obs_data_release(req);
	obs_data_release(res);
	struct mfd_slot_config_edit edit = {0};
	edit.enabled = true;
	edit.target_type = MFD_TARGET_FILTER_PARAM;
	edit.has_filter_range = true;
	snprintf(edit.source_a_name, sizeof(edit.source_a_name), "Missing source");
	char error[256] = {0};
	REQUIRE(!mfd_engine_update_slot_config_ex(eng, 0, &edit, error, sizeof(error)));
	REQUIRE(g_mock_tbar_aborts == aborts);
	REQUIRE(eng->fader_slots[0].tbar_state == MFD_TBAR_THROWING);

	for (int edit_case = 0; edit_case < 3; ++edit_case) {
		REQUIRE(start_config_test_throw(eng));
		aborts = g_mock_tbar_aborts;
		memset(&edit, 0, sizeof(edit));
		edit.enabled = edit_case != 0;
		edit.target_type = edit_case == 1 ? MFD_TARGET_UNASSIGNED : MFD_TARGET_TBAR;
		REQUIRE(mfd_engine_update_slot_config_ex(eng, 0, &edit, error, sizeof(error)));
		REQUIRE(g_mock_tbar_aborts == aborts + 1);
		REQUIRE(eng->fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	}

	/* Opening/canceling Set up is still a pause; only a valid commit aborts. */
	for (int setup_case = 0; setup_case < 3; ++setup_case) {
		REQUIRE(start_config_test_throw(eng));
		aborts = g_mock_tbar_aborts;
		mfd_engine_begin_setup_session(eng, 0);
		REQUIRE(g_mock_tbar_aborts == aborts);
		REQUIRE(eng->fader_slots[0].tbar_state == MFD_TBAR_THROWING);
		struct mfd_setup_candidate cand = {0};
		cand.slot_index = 0;
		cand.enabled = true;
		cand.target_type = MFD_TARGET_SINGLE_LAYER;
		REQUIRE(!mfd_engine_commit_setup_session(eng, &cand, error, sizeof(error)));
		REQUIRE(g_mock_tbar_aborts == aborts);
		cand.enabled = setup_case != 0;
		cand.target_type = setup_case == 1 ? MFD_TARGET_UNASSIGNED : MFD_TARGET_TBAR;
		REQUIRE(mfd_engine_commit_setup_session(eng, &cand, error, sizeof(error)));
		REQUIRE(g_mock_tbar_aborts == aborts + 1);
		REQUIRE(eng->fader_slots[0].tbar_state == MFD_TBAR_IDLE);
		REQUIRE(!eng->setup_session.active);
	}

	REQUIRE(start_config_test_throw(eng));
	aborts = g_mock_tbar_aborts;
	REQUIRE(mfd_engine_load_profile_defaults(eng, MFD_PROFILE_AKAI_MIDIMIX));
	REQUIRE(g_mock_tbar_aborts == aborts + 1);
	REQUIRE(!eng->fader_slots[0].enabled);
	REQUIRE(start_config_test_throw(eng));
	aborts = g_mock_tbar_aborts;
	REQUIRE(mfd_engine_select_profile(eng, MFD_PROFILE_AKAI_MIDIMIX, false));
	REQUIRE(g_mock_tbar_aborts == aborts + 1);
	REQUIRE(!eng->fader_slots[0].enabled);

	/* Every movement request must report failure before changing paused/unavailable state. */
	void (*handlers[])(obs_data_t *, obs_data_t *, void *) = {
		mfd_vendor_handle_set_fader_value, mfd_vendor_handle_nudge_fader, mfd_vendor_handle_auto_fade};
	for (int blocked_case = 0; blocked_case < 3; ++blocked_case) {
		mfd_engine_cancel_setup_session(eng);
		eng->studio_mode_override = true;
		mfd_engine_load_apc40_defaults(eng);
		const char *expected = "Not used. Click to set up.";
		if (blocked_case == 0) {
			eng->fader_slots[0].enabled = false;
		} else if (blocked_case == 1) {
			eng->studio_mode_override = false;
			expected = "Studio Mode is off.";
		} else {
			mfd_engine_begin_setup_session(eng, 0);
			expected = "Strip is currently paused while Set up is open.";
		}
		mfd_engine_reevaluate_slots(eng, false);
		int driver_calls = g_mock_tbar_calls;
		uint32_t output_writes = eng->fader_slots[0].output_write_count;
		for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
			req = obs_data_create();
			res = obs_data_create();
			obs_data_set_string(req, "slot", "crossfader");
			obs_data_set_double(req, "value", 0.5);
			obs_data_set_double(req, "delta", 0.1);
			obs_data_set_double(req, "to", 1.0);
			obs_data_set_int(req, "ms", 300);
			handlers[i](req, res, eng);
			REQUIRE(strcmp(obs_data_get_string(res, "error"), expected) == 0);
			REQUIRE(strcmp(obs_data_get_string(res, "comment"), expected) == 0);
			REQUIRE(!obs_data_has_user_value(res, "value"));
			REQUIRE(g_mock_tbar_calls == driver_calls);
			REQUIRE(eng->fader_slots[0].output_write_count == output_writes);
			REQUIRE(!eng->fader_slots[0].auto_fade_active);
			obs_data_release(req);
			obs_data_release(res);
		}
	}
	mfd_engine_cancel_setup_session(eng);
	mfd_engine_destroy_mutex(eng);
	free(eng);
	g_mock_tbar_calls = g_mock_tbar_set_writes = g_mock_tbar_completions = g_mock_tbar_aborts = 0;
	g_mock_tbar_unlatches = g_mock_tbar_last_pos = 0;
	g_mock_tbar_latched = false;
	return 0;
}

/*
 * Lock-order model of the OBS 33 shutdown hang with Mix two layers slots loaded. Visual Sidechain polls
 * midi_fadester_get_slot_value on the graphics thread every frame. The status read takes the engine lock; for the
 * layer and volume modes it used to look the scene and its layers up in OBS (obs_get_source_by_name,
 * obs_scene_enum_items), which take libobs's sources, canvas and scene locks. OBS's own threads hold those locks while
 * they call into Midi Fadester, whose callbacks take the engine lock. Here every scene-graph lookup takes
 * libobs_lock, standing in for those locks. The main thread holds libobs_lock as OBS does and then needs the engine
 * lock. A status read must therefore finish without the libobs lock.
 */
/* Generous margins so a slow CI runner cannot fail a correct build: a lookup gives up on the stand-in lock only
 * after LOCK_ORDER_LOOKUP_TIMEOUT_MS (an OBS deadlock), and a step that must not wait gets LOCK_ORDER_WAIT_MS. */
#define LOCK_ORDER_LOOKUP_TIMEOUT_MS 15000
#define LOCK_ORDER_WAIT_MS 5000

struct lock_order_graph {
	struct fake_scene_graph sg; /* first: the base fake ops take this struct's address as their user data */
	pthread_mutex_t libobs_lock;
	volatile long lookups;
	volatile long lookups_timed_out;
};

static bool lock_order_take_libobs_lock(struct lock_order_graph *g)
{
	os_atomic_inc_long(&g->lookups);
	for (int waited_ms = 0; waited_ms < LOCK_ORDER_LOOKUP_TIMEOUT_MS; waited_ms += 2) {
		if (pthread_mutex_trylock(&g->libobs_lock) == 0) {
			return true;
		}
		os_sleep_ms(2);
	}
	/* Would have deadlocked in OBS; go on without it so the test fails instead of hanging. */
	os_atomic_inc_long(&g->lookups_timed_out);
	return false;
}

static bool lock_order_source_exists(const char *source_name, void *user_data)
{
	struct lock_order_graph *g = user_data;
	bool locked = lock_order_take_libobs_lock(g);
	bool r = fake_op_source_exists(source_name, &g->sg);
	if (locked) {
		pthread_mutex_unlock(&g->libobs_lock);
	}
	return r;
}

static bool lock_order_scene_exists(const char *scene_name, void *user_data)
{
	struct lock_order_graph *g = user_data;
	bool locked = lock_order_take_libobs_lock(g);
	bool r = fake_op_scene_exists(scene_name, &g->sg);
	if (locked) {
		pthread_mutex_unlock(&g->libobs_lock);
	}
	return r;
}

static bool lock_order_source_has_audio(const char *source_name, void *user_data)
{
	struct lock_order_graph *g = user_data;
	bool locked = lock_order_take_libobs_lock(g);
	bool r = fake_op_source_has_audio(source_name, &g->sg);
	if (locked) {
		pthread_mutex_unlock(&g->libobs_lock);
	}
	return r;
}

static int lock_order_scene_item_stack_index(const char *scene_name, const char *source_name, void *user_data)
{
	struct lock_order_graph *g = user_data;
	bool locked = lock_order_take_libobs_lock(g);
	int r = fake_op_scene_item_stack_index(scene_name, source_name, &g->sg);
	if (locked) {
		pthread_mutex_unlock(&g->libobs_lock);
	}
	return r;
}

static bool lock_order_find_scene_with_layer(const char *source_name, const char *exclude_scene, char *out_scene,
					     size_t out_size, void *user_data)
{
	struct lock_order_graph *g = user_data;
	bool locked = lock_order_take_libobs_lock(g);
	bool r = fake_op_find_scene_with_layer(source_name, exclude_scene, out_scene, out_size, &g->sg);
	if (locked) {
		pthread_mutex_unlock(&g->libobs_lock);
	}
	return r;
}

static bool lock_order_find_scene_with_pair(const char *source_a, const char *source_b, char *out_scene,
					    size_t out_size, int *out_idx_a, int *out_idx_b, void *user_data)
{
	struct lock_order_graph *g = user_data;
	bool locked = lock_order_take_libobs_lock(g);
	bool r = fake_op_find_scene_with_pair(source_a, source_b, out_scene, out_size, out_idx_a, out_idx_b, &g->sg);
	if (locked) {
		pthread_mutex_unlock(&g->libobs_lock);
	}
	return r;
}

struct lock_order_poll {
	proc_handler_t *ph;
	volatile bool done;
	bool all_ok;
	bool all_available;
};

static void *lock_order_poll_worker(void *arg)
{
	struct lock_order_poll *p = arg;
	static const char *const slots[] = {"fader_2", "fader_3", "fader_4"};
	p->all_ok = true;
	p->all_available = true;
	for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "slot", slots[i]);
		if (!proc_handler_call(p->ph, "midi_fadester_get_slot_value", &cd) || !calldata_bool(&cd, "ok")) {
			p->all_ok = false;
		}
		const char *reason = calldata_string(&cd, "reason");
		if (reason && reason[0] != '\0') {
			p->all_available = false;
		}
		calldata_free(&cd);
	}
	os_atomic_set_bool(&p->done, true);
	return NULL;
}

static int test_status_read_takes_no_libobs_lock(void)
{
	struct lock_order_graph g;
	memset(&g, 0, sizeof(g));
	REQUIRE(pthread_mutex_init(&g.libobs_lock, NULL) == 0);
	fake_sg_add_source(&g.sg, "A", true);
	fake_sg_add_source(&g.sg, "B", true);
	fake_sg_add_scene(&g.sg, "Cold");
	fake_sg_scene_add_item(&g.sg, "Cold", "A");
	fake_sg_scene_add_item(&g.sg, "Cold", "B");

	struct mfd_scene_graph_ops ops = g_fake_sg_ops;
	ops.source_exists = lock_order_source_exists;
	ops.scene_exists = lock_order_scene_exists;
	ops.source_has_audio = lock_order_source_has_audio;
	ops.scene_item_stack_index = lock_order_scene_item_stack_index;
	ops.find_scene_with_layer = lock_order_find_scene_with_layer;
	ops.find_scene_with_pair = lock_order_find_scene_with_pair;

	struct mfd_engine *eng = calloc(1U, sizeof(*eng));
	REQUIRE(eng != NULL);
	REQUIRE(mfd_engine_init_mutex(eng));
	eng->smooth_tau_ms = 12.0f;
	eng->scene_graph_ops = &ops;
	eng->scene_graph_user_data = &g;
	eng->studio_mode_override_set = true;
	eng->studio_mode_override = false;
	g.sg.owner_eng = eng;

	/* The three modes whose status needs the scene graph: Mix two layers (as in the hung runs), one layer, Volume */
	struct mfd_fader_slot *mix = &eng->fader_slots[2];
	snprintf(mix->id, sizeof(mix->id), "%s", "fader_2");
	mix->enabled = true;
	mix->target_type = MFD_TARGET_TWO_SOURCE_MIX;
	snprintf(mix->scene_name, sizeof(mix->scene_name), "%s", "Cold");
	snprintf(mix->source_a_name, sizeof(mix->source_a_name), "%s", "A");
	snprintf(mix->source_b_name, sizeof(mix->source_b_name), "%s", "B");
	struct mfd_fader_slot *layer = &eng->fader_slots[3];
	snprintf(layer->id, sizeof(layer->id), "%s", "fader_3");
	layer->enabled = true;
	layer->target_type = MFD_TARGET_SINGLE_LAYER;
	snprintf(layer->source_a_name, sizeof(layer->source_a_name), "%s", "A");
	struct mfd_fader_slot *vol = &eng->fader_slots[4];
	snprintf(vol->id, sizeof(vol->id), "%s", "fader_4");
	vol->enabled = true;
	vol->target_type = MFD_TARGET_AUDIO_VOLUME;
	snprintf(vol->source_a_name, sizeof(vol->source_a_name), "%s", "A");

	/* Re-evaluation (scene, item, source and frontend changes) still looks the layers up */
	mfd_engine_reevaluate_slots(eng, false);
	REQUIRE(mix->cached_available && layer->cached_available && vol->cached_available);
	REQUIRE(g.lookups > 0 && g.lookups_timed_out == 0);

	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *sh = signal_handler_create();
	REQUIRE(ph != NULL && sh != NULL);
	REQUIRE(mfd_bridge_register_with_handlers(ph, sh, eng));

	/* An OBS thread holds libobs's lock (the frontend save, a signal emission) while the poll runs */
	pthread_mutex_lock(&g.libobs_lock);
	const long lookups_before = g.lookups;
	struct lock_order_poll poll;
	memset(&poll, 0, sizeof(poll));
	poll.ph = ph;
	pthread_t th;
	REQUIRE(pthread_create(&th, NULL, lock_order_poll_worker, &poll) == 0);
	bool finished = false;
	for (int waited_ms = 0; waited_ms < LOCK_ORDER_WAIT_MS && !finished; waited_ms += 2) {
		finished = os_atomic_load_bool(&poll.done);
		if (!finished) {
			os_sleep_ms(2);
		}
	}
	/* That thread then calls into Midi Fadester, which takes the engine lock (on_save_callback) */
	if (finished) {
		mfd_engine_lock(eng);
		mfd_engine_unlock(eng);
	}
	pthread_mutex_unlock(&g.libobs_lock);
	pthread_join(th, NULL);

	mfd_bridge_register_with_handlers(NULL, NULL, NULL);
	signal_handler_destroy(sh);
	proc_handler_destroy(ph);
	const long status_lookups = g.lookups - lookups_before;
	const long timed_out = g.lookups_timed_out;
	mfd_engine_destroy_mutex(eng);
	free(eng);
	pthread_mutex_destroy(&g.libobs_lock);

	REQUIRE(finished);            /* the status read never waits on the libobs lock */
	REQUIRE(status_lookups == 0); /* and never asks the scene graph */
	REQUIRE(timed_out == 0);
	REQUIRE(poll.all_ok && poll.all_available); /* it still reports the cached availability */
	return 0;
}

/*
 * The cycle the OBS 33 shutdown trace caught (2026-11-08, Mix two layers slots loaded): during shutdown two threads
 * report removed or destroyed sources at once. One (libobs's destruction task) holds no libobs lock; the main thread
 * fires its source_remove callback while libobs holds the sources mutex. The old callback took the engine lock and
 * re-checked the slots at once, looking the scenes up through that same mutex: the first thread got the engine lock
 * and waited for the sources mutex, the main thread held the sources mutex and waited for the engine lock. Here the
 * real signal callbacks are connected to a test signal handler: thread "destroy" emits source_destroy with no
 * stand-in lock held, thread "main" emits source_remove while the stand-in lock is held.
 */
struct graph_signal_ctx {
	signal_handler_t *sh;
	const char *signal;
	volatile bool done;
};

static void *graph_signal_worker(void *arg)
{
	struct graph_signal_ctx *c = arg;
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "source", NULL);
	signal_handler_signal(c->sh, c->signal, &cd);
	calldata_free(&cd);
	os_atomic_set_bool(&c->done, true);
	return NULL;
}

static bool wait_flag(volatile bool *flag, int max_ms)
{
	for (int waited_ms = 0; waited_ms < max_ms; waited_ms += 2) {
		if (os_atomic_load_bool(flag)) {
			return true;
		}
		os_sleep_ms(2);
	}
	return os_atomic_load_bool(flag);
}

static void lock_order_mix_slot(struct mfd_engine *eng, int index, const char *id, const char *scene, const char *a,
				const char *b)
{
	struct mfd_fader_slot *s = &eng->fader_slots[index];
	snprintf(s->id, sizeof(s->id), "%s", id);
	s->enabled = true;
	s->target_type = MFD_TARGET_TWO_SOURCE_MIX;
	snprintf(s->scene_name, sizeof(s->scene_name), "%s", scene);
	snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", a);
	snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", b);
}

/* An initialized engine on a fake scene graph, with the real signal callbacks on a test signal handler. */
static struct mfd_engine *graph_test_engine(struct fake_scene_graph *sg, const struct mfd_scene_graph_ops *ops,
					    void *ops_user_data, signal_handler_t **out_sh)
{
	struct mfd_engine *eng = calloc(1U, sizeof(*eng));
	if (!eng || !mfd_engine_init_mutex(eng)) {
		free(eng);
		return NULL;
	}
	eng->smooth_tau_ms = 12.0f;
	eng->scene_graph_ops = ops;
	eng->scene_graph_user_data = ops_user_data;
	eng->studio_mode_override_set = true;
	eng->studio_mode_override = false;
	eng->initialized = true;
	sg->owner_eng = eng;
	*out_sh = signal_handler_create();
	mfd_engine_connect_signal_callbacks_for_test(eng, *out_sh);
	return eng;
}

static void graph_test_engine_free(struct mfd_engine *eng, signal_handler_t *sh)
{
	signal_handler_destroy(sh);
	mfd_engine_destroy_mutex(eng);
	free(eng);
}

static void emit_source_rename(signal_handler_t *sh, const char *prev_name, const char *new_name)
{
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "source", NULL);
	calldata_set_string(&cd, "prev_name", prev_name);
	calldata_set_string(&cd, "new_name", new_name);
	signal_handler_signal(sh, "source_rename", &cd);
	calldata_free(&cd);
}

static void emit_simple(signal_handler_t *sh, const char *signal)
{
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "source", NULL);
	signal_handler_signal(sh, signal, &cd);
	calldata_free(&cd);
}

static int test_source_removal_callbacks_never_wait_for_engine(void)
{
	struct lock_order_graph g;
	memset(&g, 0, sizeof(g));
	REQUIRE(pthread_mutex_init(&g.libobs_lock, NULL) == 0);
	fake_sg_add_source(&g.sg, "A", true);
	fake_sg_add_source(&g.sg, "B", true);
	fake_sg_add_source(&g.sg, "C", true);
	fake_sg_add_source(&g.sg, "D", true);
	fake_sg_add_scene(&g.sg, "Cold");
	fake_sg_scene_add_item(&g.sg, "Cold", "A");
	fake_sg_scene_add_item(&g.sg, "Cold", "B");
	fake_sg_add_scene(&g.sg, "Mix");
	fake_sg_scene_add_item(&g.sg, "Mix", "C");
	fake_sg_scene_add_item(&g.sg, "Mix", "D");

	struct mfd_scene_graph_ops ops = g_fake_sg_ops;
	ops.source_exists = lock_order_source_exists;
	ops.scene_exists = lock_order_scene_exists;
	ops.source_has_audio = lock_order_source_has_audio;
	ops.scene_item_stack_index = lock_order_scene_item_stack_index;
	ops.find_scene_with_layer = lock_order_find_scene_with_layer;
	ops.find_scene_with_pair = lock_order_find_scene_with_pair;

	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&g.sg, &ops, &g, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B"); /* the two slots of the hung runs */
	lock_order_mix_slot(eng, 1, "fader_1", "Mix", "C", "D");
	mfd_engine_reevaluate_slots(eng, false);
	REQUIRE(eng->fader_slots[1].cached_available && eng->fader_slots[2].cached_available);

	/* The stand-in sources lock is held (by libobs, for "main") until "main" has emitted */
	struct graph_signal_ctx main_ctx = {sh, "source_remove", false};
	struct graph_signal_ctx destroy_ctx = {sh, "source_destroy", false};
	pthread_mutex_lock(&g.libobs_lock);
	const long lookups_before = g.lookups;
	pthread_t th_destroy;
	REQUIRE(pthread_create(&th_destroy, NULL, graph_signal_worker, &destroy_ctx) == 0);
	/* Give "destroy" the engine lock and a lookup, as in the trace (the old callback gets that far) */
	for (int waited_ms = 0;
	     waited_ms < 500 && !os_atomic_load_bool(&destroy_ctx.done) && g.lookups == lookups_before;
	     waited_ms += 2) {
		os_sleep_ms(2);
	}
	pthread_t th_main;
	REQUIRE(pthread_create(&th_main, NULL, graph_signal_worker, &main_ctx) == 0);
	const bool main_done = wait_flag(&main_ctx.done, LOCK_ORDER_WAIT_MS);
	const bool destroy_done = wait_flag(&destroy_ctx.done, LOCK_ORDER_WAIT_MS);
	pthread_mutex_unlock(&g.libobs_lock); /* lets the old callback's stuck lookup finish, so the test ends */
	pthread_join(th_main, NULL);
	pthread_join(th_destroy, NULL);
	const long lookups_during_signals = g.lookups - lookups_before;
	const long timed_out = g.lookups_timed_out;

	/* The re-check runs later, where no libobs lock is held (the video tick, a frontend event) */
	const size_t applied = mfd_engine_process_scene_graph_changes(eng);
	const bool still_available = eng->fader_slots[1].cached_available && eng->fader_slots[2].cached_available;
	const long lookups_after = g.lookups;

	graph_test_engine_free(eng, sh);
	pthread_mutex_destroy(&g.libobs_lock);

	REQUIRE(main_done && destroy_done); /* neither callback waits for the other */
	REQUIRE(timed_out == 0);            /* no lookup ever waited on the stand-in sources lock */
	REQUIRE(lookups_during_signals == 0);
	REQUIRE(applied == 1U); /* both reports fold into one re-check */
	REQUIRE(lookups_after > lookups_before + lookups_during_signals);
	REQUIRE(still_available);
	return 0;
}

/* A status read reports the last re-check; an item_remove signal flips it once the queue is applied. */
static int test_cached_status_follows_scene_changes(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");

	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	mfd_engine_reevaluate_slots(eng, false);

	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *bridge_sh = signal_handler_create();
	REQUIRE(ph != NULL && bridge_sh != NULL);
	REQUIRE(mfd_bridge_register_with_handlers(ph, bridge_sh, eng));
	char reason[4][160];
	bool ok[4];
	for (int step = 0; step < 4; ++step) {
		if (step == 1) {
			fake_sg_scene_remove_item(&sg, "Cold", "B"); /* no signal yet: the cache still says available */
		} else if (step == 2) {
			emit_simple(sh, "item_remove");
			mfd_engine_tick(eng, 0.016f); /* the video tick applies the queued change */
		} else if (step == 3) {
			fake_sg_scene_add_item(&sg, "Cold", "B");
			emit_simple(sh, "item_add");
			mfd_engine_process_scene_graph_changes(eng);
		}
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "slot", "fader_2");
		ok[step] = proc_handler_call(ph, "midi_fadester_get_slot_value", &cd) && calldata_bool(&cd, "ok");
		const char *r = calldata_string(&cd, "reason");
		snprintf(reason[step], sizeof(reason[step]), "%s", r ? r : "");
		calldata_free(&cd);
	}
	mfd_bridge_register_with_handlers(NULL, NULL, NULL);
	signal_handler_destroy(bridge_sh);
	proc_handler_destroy(ph);
	graph_test_engine_free(eng, sh);

	REQUIRE(ok[0] && ok[1] && ok[2] && ok[3]);
	REQUIRE(reason[0][0] == '\0');
	REQUIRE(reason[1][0] == '\0');
	REQUIRE(strcmp(reason[2], "B is no longer in scene Cold.") == 0);
	REQUIRE(reason[3][0] == '\0');
	return 0;
}

/* Rename chains and swaps are applied as names first, then one re-check: no intermediate "unavailable". */
static int test_rename_batch_keeps_runtime_state(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");

	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_fader_slot *mix = &eng->fader_slots[2];
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	mfd_engine_reevaluate_slots(eng, false);
	REQUIRE(mix->cached_available);
	mix->auto_fade_active = true; /* a fade in progress must survive a rename it is not affected by */

	/* OBS renamed A -> X -> Y and swapped the scene's name through Tmp; the graph already has the final names */
	fake_sg_rename(&sg, "A", "Y");
	fake_sg_rename(&sg, "Cold", "Warm");
	emit_source_rename(sh, "A", "X");
	emit_source_rename(sh, "X", "Y");
	emit_source_rename(sh, "Cold", "Tmp");
	emit_source_rename(sh, "Tmp", "Warm");
	mfd_engine_process_scene_graph_changes(eng);
	const bool names_ok = strcmp(mix->source_a_name, "Y") == 0 && strcmp(mix->source_b_name, "B") == 0 &&
			      strcmp(mix->scene_name, "Warm") == 0;
	const bool still_available = mix->cached_available;
	const bool fade_kept = mix->auto_fade_active;

	/* A swap: A <-> B through a temporary name */
	fake_sg_rename(&sg, "Y", "T0");
	fake_sg_rename(&sg, "B", "Y");
	fake_sg_rename(&sg, "T0", "B");
	emit_source_rename(sh, "Y", "T0");
	emit_source_rename(sh, "B", "Y");
	emit_source_rename(sh, "T0", "B");
	mfd_engine_process_scene_graph_changes(eng);
	const bool swapped = strcmp(mix->source_a_name, "B") == 0 && strcmp(mix->source_b_name, "Y") == 0 &&
			     mix->cached_available && mix->auto_fade_active;
	graph_test_engine_free(eng, sh);

	REQUIRE(names_ok);
	REQUIRE(still_available);
	REQUIRE(fade_kept);
	REQUIRE(swapped);
	return 0;
}

/* More changes than the old 32-entry queue held: none is lost, removals of one name fold, the cap holds. */
static int test_graph_queue_holds_a_collection_load(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");

	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_fader_slot *mix = &eng->fader_slots[2];
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	mfd_engine_reevaluate_slots(eng, false);

	char prev_name[32];
	char new_name[32];
	for (int i = 0; i < 200; ++i) {
		snprintf(prev_name, sizeof(prev_name), "Other %d", i);
		snprintf(new_name, sizeof(new_name), "Renamed %d", i);
		emit_source_rename(sh, prev_name, new_name);
	}
	fake_sg_rename(&sg, "A", "Y");
	emit_source_rename(sh, "A", "Y"); /* the 201st change */
	for (int i = 0; i < 50; ++i) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_SOURCE_GONE, "Gone", NULL, NULL, false);
	}
	const size_t queued = eng->graph_count;
	mfd_engine_process_scene_graph_changes(eng);
	const bool late_rename_applied = strcmp(mix->source_a_name, "Y") == 0 && mix->cached_available;

	/* Renames are never dropped, even past MFD_GRAPH_CHANGE_MAX */
	for (int i = 0; i < MFD_GRAPH_CHANGE_MAX + 10; ++i) {
		snprintf(prev_name, sizeof(prev_name), "Many %d", i);
		snprintf(new_name, sizeof(new_name), "More %d", i);
		emit_source_rename(sh, prev_name, new_name);
	}
	const size_t renames_kept = eng->graph_count;
	const bool renames_not_flagged = !eng->graph_overflowed;
	mfd_engine_process_scene_graph_changes(eng);
	const bool drained = eng->graph_count == 0U && !eng->graph_rescan && !eng->graph_recheck;
	graph_test_engine_free(eng, sh);

	REQUIRE(queued == 202U); /* 201 renames and one "Gone" */
	REQUIRE(late_rename_applied);
	REQUIRE(renames_kept == (size_t)MFD_GRAPH_CHANGE_MAX + 10U);
	REQUIRE(renames_not_flagged);
	REQUIRE(drained);
	return 0;
}

/*
 * A flood of filter settings and enable changes: one entry per filter keeps the last value, past the cap the rest
 * are dropped for a full rescan, and a rename that arrives in the middle of the flood still applies.
 */
static int test_rename_survives_filter_settings_flood(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_fader_slot *mix = &eng->fader_slots[2];
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	mfd_engine_reevaluate_slots(eng, false);

	for (int i = 0; i < 100; ++i) { /* one filter driven every frame */
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_SETTINGS, "Cam", "Blur", NULL, false);
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "Blur", NULL, (i % 2) == 0);
	}
	const bool folded = eng->graph_count == 2U && !eng->graph_items[1].flag; /* the last enable value: off */

	char filter_name[32];
	for (int i = 0; i < MFD_GRAPH_CHANGE_MAX; ++i) {
		snprintf(filter_name, sizeof(filter_name), "Filter %d", i);
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_SETTINGS, "Cam", filter_name, NULL, false);
		if (i == MFD_GRAPH_CHANGE_MAX / 2) {
			fake_sg_rename(&sg, "A", "Y");
			emit_source_rename(sh, "A", "Y");
		}
	}
	fake_sg_rename(&sg, "B", "Z");
	emit_source_rename(sh, "B", "Z"); /* after the cap is reached */
	const bool capped = eng->graph_count == (size_t)MFD_GRAPH_CHANGE_MAX + 1U && eng->graph_overflowed &&
			    eng->graph_rescan && eng->graph_recheck;
	mfd_engine_process_scene_graph_changes(eng);
	const bool renamed = strcmp(mix->source_a_name, "Y") == 0 && strcmp(mix->source_b_name, "Z") == 0 &&
			     mix->cached_available;
	graph_test_engine_free(eng, sh);

	REQUIRE(folded);
	REQUIRE(capped);
	REQUIRE(renamed);
	return 0;
}

/* On the thread that holds the engine lock a change applies at once, after the changes queued before it. */
static int test_owner_thread_change_drains_older_ones_first(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");

	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_fader_slot *mix = &eng->fader_slots[2];
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	mfd_engine_reevaluate_slots(eng, false);

	fake_sg_rename(&sg, "A", "Y");
	emit_source_rename(sh, "A", "Y"); /* queued: this thread does not hold the engine lock */
	const bool queued = eng->graph_count == 1U && strcmp(mix->source_a_name, "A") == 0;

	mfd_engine_lock(eng);
	mfd_engine_lock(eng); /* recursion: the owner path runs at any depth */
	emit_simple(sh, "item_remove");
	const bool rename_first = strcmp(mix->source_a_name, "Y") == 0 && mix->cached_available;
	const bool queue_empty = eng->graph_count == 0U;
	mfd_engine_unlock(eng);
	mfd_engine_unlock(eng);
	graph_test_engine_free(eng, sh);

	REQUIRE(queued);
	REQUIRE(rename_first);
	REQUIRE(queue_empty);
	return 0;
}

/* Changes queued for a scene collection that is going away are dropped at collection change, cleanup and exit. */
static int test_graph_queue_discarded_at_collection_change_and_exit(void)
{
	static const int events[] = {OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED,
				     OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP, OBS_FRONTEND_EVENT_EXIT};
	for (size_t e = 0U; e < sizeof(events) / sizeof(events[0]); ++e) {
		struct fake_scene_graph sg;
		memset(&sg, 0, sizeof(sg));
		fake_sg_add_source(&sg, "A", true);
		fake_sg_add_source(&sg, "B", true);
		fake_sg_add_scene(&sg, "Cold");
		fake_sg_scene_add_item(&sg, "Cold", "A");
		fake_sg_scene_add_item(&sg, "Cold", "B");
		signal_handler_t *sh = NULL;
		struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
		REQUIRE(eng != NULL && sh != NULL);
		lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
		emit_source_rename(sh, "A", "Old collection's A");
		emit_simple(sh, "item_remove");
		mfd_engine_frontend_event_for_test(eng, events[e]);
		const bool dropped = eng->graph_count == 0U && !eng->graph_recheck && !eng->graph_rescan;
		mfd_engine_process_scene_graph_changes(eng);
		const bool name_kept = strcmp(eng->fader_slots[2].source_a_name, "A") == 0;
		graph_test_engine_free(eng, sh);
		REQUIRE(dropped);
		REQUIRE(name_kept);
	}
	return 0;
}

/*
 * An operator's eye toggle, queued, then applied while Midi Fadester writes the same filter's enable state (the
 * owner-thread path drains first, inside the write's guard): it is recorded as the new baseline, and only the write's
 * own echo is ignored.
 */
static int test_operator_toggle_drained_inside_own_write_is_kept(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_filter_bypass_lease *lease = &eng->filter_leases[0];
	lease->in_use = true;
	snprintf(lease->source_name, sizeof(lease->source_name), "%s", "Cam");
	snprintf(lease->filter_name, sizeof(lease->filter_name), "%s", "Blur");
	lease->pre_mfd_enabled = true;
	lease->last_mfd_enabled = true;
	lease->has_written_enable = true;

	/* The operator switches the filter off; the signal arrives on another thread, so it is queued */
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "Blur", NULL, false);
	const bool queued = eng->graph_count == 1U;

	/* Midi Fadester then writes "on" and libobs echoes it on the same thread, inside the write's guard */
	mfd_engine_lock(eng);
	lease->writing_enable = true;
	lease->writing_value = true;
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "Blur", NULL, true);
	lease->writing_enable = false;
	const bool baseline_off = !lease->pre_mfd_enabled; /* the operator's toggle, not the echo */
	mfd_engine_unlock(eng);
	graph_test_engine_free(eng, sh);

	REQUIRE(queued);
	REQUIRE(baseline_off);
	return 0;
}

/* On the owner-thread path a rescan is never run in place (it would enumerate sources inside a libobs call). */
static int test_owner_thread_rescan_stays_queued(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	mfd_engine_lock(eng);
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_ptr(&cd, "source", NULL);
	calldata_set_ptr(&cd, "filter", NULL);
	signal_handler_signal(sh, "filter_add", &cd); /* a filter appeared: asks for a rescan */
	calldata_free(&cd);
	const bool queued_while_owner = eng->graph_rescan;
	mfd_engine_unlock(eng);
	const size_t applied = mfd_engine_process_scene_graph_changes(eng);
	const bool cleared = !eng->graph_rescan;
	graph_test_engine_free(eng, sh);
	REQUIRE(queued_while_owner);
	REQUIRE(applied == 1U);
	REQUIRE(cleared);
	return 0;
}

/* Two threads: one queues renames and re-checks as fast as it can, the other drains as fast as it can. */
struct graph_stress_ctx {
	signal_handler_t *sh;
	int renames;
	volatile bool stop;
};

static void *graph_stress_reporter(void *arg)
{
	struct graph_stress_ctx *c = arg;
	char prev_name[32];
	char new_name[32];
	for (int i = 0; i < c->renames; ++i) {
		snprintf(prev_name, sizeof(prev_name), "S%d", i);
		snprintf(new_name, sizeof(new_name), "S%d", i + 1);
		emit_source_rename(c->sh, prev_name, new_name);
		emit_simple(c->sh, "item_remove");
	}
	os_atomic_set_bool(&c->stop, true);
	return NULL;
}

static int test_graph_queue_enqueue_versus_drain_stress(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "S3000", true); /* the final name; the slot starts at S0 */
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "S3000");
	fake_sg_scene_add_item(&sg, "Cold", "B");
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "S0", "B");

	struct graph_stress_ctx ctx = {sh, 3000, false};
	pthread_t th;
	REQUIRE(pthread_create(&th, NULL, graph_stress_reporter, &ctx) == 0);
	while (!os_atomic_load_bool(&ctx.stop)) {
		mfd_engine_process_scene_graph_changes(eng);
	}
	pthread_join(th, NULL);
	mfd_engine_process_scene_graph_changes(eng);
	const bool final_name = strcmp(eng->fader_slots[2].source_a_name, "S3000") == 0;
	const bool available = eng->fader_slots[2].cached_available;
	const bool empty = eng->graph_count == 0U && !eng->graph_recheck;
	graph_test_engine_free(eng, sh);

	REQUIRE(final_name); /* every rename applied, in order */
	REQUIRE(available);
	REQUIRE(empty);
	return 0;
}

/* Two drainers at once (the video tick and an obs-websocket request, say) must apply batches in queue order. */
struct graph_drainer_ctx {
	struct mfd_engine *eng;
	volatile bool *stop;
};

static void *graph_drainer(void *arg)
{
	struct graph_drainer_ctx *c = arg;
	while (!os_atomic_load_bool(c->stop)) {
		mfd_engine_process_scene_graph_changes(c->eng);
	}
	return NULL;
}

static int test_concurrent_drains_keep_rename_order(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "S3000", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "S3000");
	fake_sg_scene_add_item(&sg, "Cold", "B");
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "S0", "B");

	volatile bool stop = false;
	struct graph_drainer_ctx dc = {eng, &stop};
	pthread_t drainers[2];
	REQUIRE(pthread_create(&drainers[0], NULL, graph_drainer, &dc) == 0);
	REQUIRE(pthread_create(&drainers[1], NULL, graph_drainer, &dc) == 0);
	char prev_name[32];
	char new_name[32];
	for (int i = 0; i < 3000; ++i) {
		snprintf(prev_name, sizeof(prev_name), "S%d", i);
		snprintf(new_name, sizeof(new_name), "S%d", i + 1);
		emit_source_rename(sh, prev_name, new_name);
	}
	os_atomic_set_bool(&stop, true);
	pthread_join(drainers[0], NULL);
	pthread_join(drainers[1], NULL);
	mfd_engine_process_scene_graph_changes(eng);
	char final_name[MFD_SOURCE_NAME_MAX];
	snprintf(final_name, sizeof(final_name), "%s", eng->fader_slots[2].source_a_name);
	graph_test_engine_free(eng, sh);
	if (strcmp(final_name, "S3000") != 0) {
		fprintf(stderr, "rename chain ended at '%s'\n", final_name);
	}
	REQUIRE(strcmp(final_name, "S3000") == 0);
	return 0;
}

/*
 * The same ordering made deterministic: a drainer that has to wait for the engine lock must not have taken its
 * batch yet, or the lock holder applies a later rename (B->C) before that batch's earlier one (A->B).
 */
struct graph_one_drain_ctx {
	struct mfd_engine *eng;
	volatile bool done;
};

static void *graph_one_drain(void *arg)
{
	struct graph_one_drain_ctx *c = arg;
	mfd_engine_process_scene_graph_changes(c->eng);
	os_atomic_set_bool(&c->done, true);
	return NULL;
}

static int test_waiting_drainer_takes_no_batch(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "C", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "C");
	fake_sg_scene_add_item(&sg, "Cold", "B");
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "X");

	emit_source_rename(sh, "A", "B"); /* queued */
	mfd_engine_lock(eng);
	struct graph_one_drain_ctx dc = {eng, false};
	pthread_t th;
	REQUIRE(pthread_create(&th, NULL, graph_one_drain, &dc) == 0);
	os_sleep_ms(200); /* the drainer is now waiting for the engine lock */
	const bool batch_left = eng->graph_count == 1U;
	emit_source_rename(sh, "B", "C"); /* owner path: drains what is queued, then applies this */
	mfd_engine_unlock(eng);
	pthread_join(th, NULL);
	mfd_engine_process_scene_graph_changes(eng);
	char final_name[MFD_SOURCE_NAME_MAX];
	snprintf(final_name, sizeof(final_name), "%s", eng->fader_slots[2].source_a_name);
	graph_test_engine_free(eng, sh);

	REQUIRE(batch_left);
	REQUIRE(strcmp(final_name, "C") == 0);
	return 0;
}

/* Settings and enable changes fold only back to the last rename or removal: a reused name is another filter. */
static int test_fold_stops_at_rename_or_removal(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_filter_bypass_lease *lease = &eng->filter_leases[0];
	lease->in_use = true;
	snprintf(lease->source_name, sizeof(lease->source_name), "%s", "Cam");
	snprintf(lease->filter_name, sizeof(lease->filter_name), "%s", "A");
	lease->pre_mfd_enabled = false;
	lease->last_mfd_enabled = false;
	lease->has_written_enable = true;

	/* The operator turns the old A on, renames it to B, renames C to A and turns the new A off */
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "A", NULL, true);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_RENAMED, "Cam", "A", "B", false);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_RENAMED, "Cam", "C", "A", false);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "A", NULL, false);
	const size_t across_renames = eng->graph_count;
	mfd_engine_process_scene_graph_changes(eng);
	const bool old_filter_on = strcmp(lease->filter_name, "B") == 0 &&
				   lease->pre_mfd_enabled; /* the new A's "off" is not B's */

	/* enable, remove, re-add, enable: the second enable stays after the removal */
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "D", NULL, true);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_REMOVED, "Cam", "D", NULL, false);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "D", NULL, false);
	const size_t across_removal = eng->graph_count;
	mfd_engine_process_scene_graph_changes(eng);

	/* with nothing in between they still fold */
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "E", NULL, true);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_SETTINGS, "Cam", "F", NULL, false);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "E", NULL, false);
	const size_t folded = eng->graph_count;
	mfd_engine_process_scene_graph_changes(eng);
	graph_test_engine_free(eng, sh);

	REQUIRE(across_renames == 4U);
	REQUIRE(old_filter_on);
	REQUIRE(across_removal == 3U);
	REQUIRE(folded == 2U);
	return 0;
}

/* Past the cap settings changes are dropped, an operator's enable toggle never is. */
static int test_enable_toggle_survives_overflow(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	struct mfd_filter_bypass_lease *lease = &eng->filter_leases[0];
	lease->in_use = true;
	snprintf(lease->source_name, sizeof(lease->source_name), "%s", "Cam");
	snprintf(lease->filter_name, sizeof(lease->filter_name), "%s", "Blur");
	lease->pre_mfd_enabled = true;
	lease->last_mfd_enabled = true;
	lease->has_written_enable = true;

	char filter_name[32];
	for (int i = 0; i < MFD_GRAPH_CHANGE_MAX + 5; ++i) {
		snprintf(filter_name, sizeof(filter_name), "Filter %d", i);
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_SETTINGS, "Cam", filter_name, NULL, false);
	}
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, "Cam", "Blur", NULL, false);
	const bool kept = eng->graph_count == (size_t)MFD_GRAPH_CHANGE_MAX + 1U && eng->graph_overflowed;
	mfd_engine_process_scene_graph_changes(eng);
	const bool recorded = !lease->pre_mfd_enabled;
	graph_test_engine_free(eng, sh);
	REQUIRE(kept);
	REQUIRE(recorded);
	return 0;
}

/* get_state's view of one slot */
static bool vendor_slot_state(struct mfd_engine *eng, int index, bool *out_available, char *out_reason,
			      size_t reason_size)
{
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, eng);
	obs_data_array_t *arr = obs_data_get_array(res, "slots");
	obs_data_t *item = arr ? obs_data_array_item(arr, (size_t)index) : NULL;
	bool found = item != NULL;
	if (item) {
		*out_available = obs_data_get_bool(item, "available");
		snprintf(out_reason, reason_size, "%s", obs_data_get_string(item, "reason"));
		obs_data_release(item);
	}
	obs_data_array_release(arr);
	obs_data_release(res);
	obs_data_release(req);
	return found;
}

/*
 * An obs-websocket request right after a scene change, before any video tick (the live self-test's scene-signal check
 * removes and re-adds a layer and asks get_state at once): the reply must already reflect the change.
 */
static int test_vendor_request_sees_queued_scene_changes(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	mfd_engine_reevaluate_slots(eng, false);

	bool avail_removed = true;
	char reason_removed[160] = {0};
	fake_sg_scene_remove_item(&sg, "Cold", "B");
	emit_simple(sh, "item_remove");
	const bool got_removed = vendor_slot_state(eng, 2, &avail_removed, reason_removed, sizeof(reason_removed));

	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	fake_sg_scene_add_item(&sg, "Cold", "B");
	emit_simple(sh, "item_add");
	obs_data_set_string(req, "slot", "fader_2");
	obs_data_set_double(req, "value", 0.5);
	mfd_vendor_handle_set_fader_value(req, res, eng); /* a write right after the re-add is allowed */
	const bool write_ok = !obs_data_has_user_value(res, "error") && obs_data_has_user_value(res, "value");
	obs_data_release(res);
	obs_data_release(req);

	bool avail_readded = false;
	char reason_readded[160] = {0};
	const bool got_readded = vendor_slot_state(eng, 2, &avail_readded, reason_readded, sizeof(reason_readded));
	graph_test_engine_free(eng, sh);

	REQUIRE(got_removed && !avail_removed);
	REQUIRE(strcmp(reason_removed, "B is no longer in scene Cold.") == 0);
	REQUIRE(write_ok);
	REQUIRE(got_readded && avail_readded && reason_readded[0] == '\0');
	return 0;
}

/* A drain that took its batch before shutdown must apply nothing once the engine is shut down. */
static int test_graph_drain_after_shutdown_does_nothing(void)
{
	struct fake_scene_graph sg;
	memset(&sg, 0, sizeof(sg));
	fake_sg_add_source(&sg, "A", true);
	fake_sg_add_source(&sg, "B", true);
	fake_sg_add_scene(&sg, "Cold");
	fake_sg_scene_add_item(&sg, "Cold", "A");
	fake_sg_scene_add_item(&sg, "Cold", "B");
	signal_handler_t *sh = NULL;
	struct mfd_engine *eng = graph_test_engine(&sg, &g_fake_sg_ops, &sg, &sh);
	REQUIRE(eng != NULL && sh != NULL);
	lock_order_mix_slot(eng, 2, "fader_2", "Cold", "A", "B");
	emit_source_rename(sh, "A", "Y");
	mfd_engine_lock(eng);
	eng->initialized = false; /* what mfd_engine_shutdown does under the engine lock */
	mfd_engine_unlock(eng);
	const size_t applied = mfd_engine_process_scene_graph_changes(eng);
	const bool untouched = strcmp(eng->fader_slots[2].source_a_name, "A") == 0;
	graph_test_engine_free(eng, sh);
	REQUIRE(applied == 0U);
	REQUIRE(untouched);
	return 0;
}

int main(void)
{
	obs_module_lookup = text_lookup_create(MFD_TEST_LOCALE_FILE);
	REQUIRE(obs_module_lookup != NULL);
	mfd_text_set_lookup(test_locale_text);
	REQUIRE(strcmp(mfd_text("PluginDisplayName", "missing"), "Midi Fadester") == 0);
	REQUIRE(test_config_abort_and_movement_errors() == 0);
	REQUIRE(test_status_read_takes_no_libobs_lock() == 0);
	REQUIRE(test_source_removal_callbacks_never_wait_for_engine() == 0);
	REQUIRE(test_cached_status_follows_scene_changes() == 0);
	REQUIRE(test_rename_batch_keeps_runtime_state() == 0);
	REQUIRE(test_graph_queue_holds_a_collection_load() == 0);
	REQUIRE(test_rename_survives_filter_settings_flood() == 0);
	REQUIRE(test_owner_thread_change_drains_older_ones_first() == 0);
	REQUIRE(test_graph_queue_discarded_at_collection_change_and_exit() == 0);
	REQUIRE(test_graph_drain_after_shutdown_does_nothing() == 0);
	REQUIRE(test_vendor_request_sees_queued_scene_changes() == 0);
	REQUIRE(test_operator_toggle_drained_inside_own_write_is_kept() == 0);
	REQUIRE(test_owner_thread_rescan_stays_queued() == 0);
	REQUIRE(test_graph_queue_enqueue_versus_drain_stress() == 0);
	REQUIRE(test_concurrent_drains_keep_rename_order() == 0);
	REQUIRE(test_waiting_drainer_takes_no_batch() == 0);
	REQUIRE(test_fold_stops_at_rename_or_removal() == 0);
	REQUIRE(test_enable_toggle_survives_overflow() == 0);
	struct fake_scene_graph fake_sg;
	memset(&fake_sg, 0, sizeof(fake_sg));
	fake_sg_add_source(&fake_sg, "Cam 1", true);
	fake_sg_add_source(&fake_sg, "Loop 3", true);
	fake_sg_add_scene(&fake_sg, "Main");
	fake_sg_scene_add_item(&fake_sg, "Main", "Cam 1");  /* stack index 0 (bottom) */
	fake_sg_scene_add_item(&fake_sg, "Main", "Loop 3"); /* stack index 1 (top) */

	struct mfd_engine eng;
	memset(&eng, 0, sizeof(eng));
	eng.smooth_tau_ms = 12.0f;
	eng.tbar_bidirectional_flip = true;
	eng.tbar_driver = mock_tbar_driver;
	eng.tbar_unlatch = mock_tbar_unlatch;
	eng.scene_graph_ops = &g_fake_sg_ops;
	eng.scene_graph_user_data = &fake_sg;
	eng.studio_mode_override_set = true;
	eng.studio_mode_override = false;

	struct fake_ws_log ws_log;
	memset(&ws_log, 0, sizeof(ws_log));
	mfd_vendor_set_event_sink(on_fake_ws_event, &ws_log);

	/* A master slot sends the latest absolute position; only a matching provider reply confirms it. */
	struct mfd_fader_slot *mix = &eng.fader_slots[9];
	mix->enabled = mix->cached_available = true;
	mix->target_type = MFD_TARGET_MIX_MASTER;
	mix->addr = (struct mfd_midi_addr){.type = MFD_MIDI_CC, .number = 23};
	mfd_calibration_set(&mix->cal, false, 0, 127);
	push_midi_cc(&eng, 0, 23, 64);
	push_midi_cc(&eng, 0, 23, 127);
	mfd_engine_tick(&eng, 0.06f);
	REQUIRE(ws_log.mix_requests == 1 && ws_log.mix_db == 0.0 && strlen(ws_log.mix_id) == 36U);
	REQUIRE(mix->mix_delivery == MFD_MIX_WAITING && mix->output_write_count == 0);
	obs_data_t *mix_reply = obs_data_create();
	obs_data_t *receipt = obs_data_create();
	obs_data_set_string(mix_reply, "namespace", MFD_MIX_NAMESPACE);
	obs_data_set_string(mix_reply, "op", "reply");
	obs_data_set_string(mix_reply, "id", "an unrelated request");
	obs_data_set_bool(mix_reply, "ok", true);
	obs_data_set_bool(mix_reply, "enabled", true);
	obs_data_set_double(mix_reply, "db", 0.0);
	mfd_vendor_handle_mix_reply(mix_reply, receipt, &eng);
	REQUIRE(!obs_data_get_bool(receipt, "accepted") && mix->mix_delivery == MFD_MIX_WAITING);
	obs_data_set_string(mix_reply, "id", ws_log.mix_id);
	mfd_vendor_handle_mix_reply(mix_reply, receipt, &eng);
	REQUIRE(obs_data_get_bool(receipt, "accepted"));
	mfd_vendor_handle_mix_reply(mix_reply, receipt, &eng);
	REQUIRE(!obs_data_get_bool(receipt, "accepted"));
	REQUIRE(mix->mix_delivery == MFD_MIX_CONFIRMED && mix->output_write_count == 1);
	push_midi_cc(&eng, 0, 23, 0);
	mfd_engine_tick(&eng, 0.06f);
	REQUIRE(ws_log.mix_requests == 2 && ws_log.mix_db == -100.0 && fake_sg.total_volume_writes == 0);
	obs_data_set_string(mix_reply, "id", ws_log.mix_id);
	obs_data_set_bool(mix_reply, "ok", false);
	obs_data_set_string(mix_reply, "message", "Provider is disabled");
	mfd_vendor_handle_mix_reply(mix_reply, receipt, &eng);
	REQUIRE(mix->mix_delivery == MFD_MIX_DECLINED && mix->output_write_count == 1);
	obs_data_release(mix_reply);
	obs_data_release(receipt);
	memset(mix, 0, sizeof(*mix));

	/* Two master slots moved within one send interval each send their own latest request; neither is dropped. */
	struct mfd_fader_slot *mix2 = &eng.fader_slots[8];
	mix->enabled = mix->cached_available = true;
	mix->target_type = MFD_TARGET_MIX_MASTER;
	mix->addr = (struct mfd_midi_addr){.type = MFD_MIDI_CC, .number = 23};
	mfd_calibration_set(&mix->cal, false, 0, 127);
	mix2->enabled = mix2->cached_available = true;
	mix2->target_type = MFD_TARGET_MIX_MASTER;
	mix2->addr = (struct mfd_midi_addr){.type = MFD_MIDI_CC, .number = 24};
	mfd_calibration_set(&mix2->cal, false, 0, 127);
	const int mix_before = ws_log.mix_requests;
	push_midi_cc(&eng, 0, 23, 64);
	push_midi_cc(&eng, 0, 24, 127);
	mfd_engine_tick(&eng, 0.06f);
	REQUIRE(ws_log.mix_requests == mix_before + 2);
	REQUIRE(mix->mix_delivery == MFD_MIX_WAITING && mix2->mix_delivery == MFD_MIX_WAITING);
	REQUIRE(mix->mix_request_id[0] != '\0' && mix2->mix_request_id[0] != '\0');
	REQUIRE(strcmp(mix->mix_request_id, mix2->mix_request_id) != 0);
	memset(mix, 0, sizeof(*mix));
	memset(mix2, 0, sizeof(*mix2));

	/* 1. load_profile across verified + beta profiles and error check on unknown profile */
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	obs_data_set_string(req, "name", "nonexistent_desk");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Unknown profile 'nonexistent_desk'. Valid profiles: apc40mk2, apcminimk2, midimix, nanokontrol2, launchcontrolxl, generic.") ==
		0);
	REQUIRE(strcmp(obs_data_get_string(res, "error_code"), "unknown_profile") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* Beta profile load_profile check: Akai MIDImix (9 vertical faders, no crossfader) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "name", "midimix");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "loaded"), "midimix") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "profile_tier"), "beta") == 0);
	REQUIRE(!eng.fader_slots[0].enabled);
	REQUIRE(eng.fader_slots[0].addr.type == MFD_MIDI_NONE);
	REQUIRE(eng.fader_slots[1].enabled && eng.fader_slots[1].addr.number == 19U);
	REQUIRE(eng.fader_slots[9].enabled && eng.fader_slots[9].addr.number == 62U);
	obs_data_release(req);
	obs_data_release(res);

	/* Beta profile load_profile check: Korg nanoKONTROL2 (8 vertical sliders CC 0..7, no crossfader/master) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "name", "nanokontrol2");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "loaded"), "nanokontrol2") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "profile_tier"), "beta") == 0);
	REQUIRE(!eng.fader_slots[0].enabled && !eng.fader_slots[9].enabled);
	REQUIRE(eng.fader_slots[1].enabled && eng.fader_slots[1].addr.number == 0U);
	REQUIRE(eng.fader_slots[8].enabled && eng.fader_slots[8].addr.number == 7U);
	obs_data_release(req);
	obs_data_release(res);

	ws_log.state_changed_count = 0;

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "name", "apc40mk2");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "loaded"), "apc40mk2") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "profile_tier"), "verified") == 0);
	REQUIRE(ws_log.state_changed_count == 1);
	REQUIRE(strcmp(eng.fader_slots[0].id, "crossfader") == 0);
	REQUIRE(eng.fader_slots[0].target_type == MFD_TARGET_TBAR);
	REQUIRE(strcmp(eng.fader_slots[1].id, "fader_1") == 0);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	REQUIRE(strcmp(eng.fader_slots[9].id, "master") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* 2. get_state: verify version, build identity, profile, studio_mode, device, and slot availability/reason when Studio Mode is OFF */
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "version"), PLUGIN_VERSION) == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "version"), "0.6.0") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "profile"), "apc40mk2") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "profile_tier"), "verified") == 0);
	obs_data_t *build_obj = obs_data_get_obj(res, "build");
	REQUIRE(build_obj != NULL);
	const char *b_commit = obs_data_get_string(build_obj, "commit");
	const char *b_built_at = obs_data_get_string(build_obj, "built_at");
	REQUIRE(b_commit != NULL && strlen(b_commit) >= 7U && strcmp(b_commit, "unknown") != 0);
	REQUIRE(b_built_at != NULL && strlen(b_built_at) == 16U);
	REQUIRE(b_built_at[4] == '-' && b_built_at[7] == '-' && b_built_at[10] == ' ' && b_built_at[13] == ':');
	obs_data_release(build_obj);
	REQUIRE(obs_data_get_int(res, "api_version") == 1);
	REQUIRE(!obs_data_get_bool(res, "studio_mode"));
	obs_data_array_t *arr = obs_data_get_array(res, "slots");
	REQUIRE(arr != NULL);
	REQUIRE(obs_data_array_count(arr) == MFD_MAX_FADER_SLOTS);
	obs_data_t *s0 = obs_data_array_item(arr, 0);
	REQUIRE(strcmp(obs_data_get_string(s0, "id"), "crossfader") == 0);
	REQUIRE(strcmp(obs_data_get_string(s0, "target_mode"), "tbar") == 0);
	REQUIRE(!obs_data_get_bool(s0, "available"));
	REQUIRE(strcmp(obs_data_get_string(s0, "reason"), "Studio Mode is off.") == 0);
	REQUIRE(strcmp(obs_data_get_string(s0, "reason_code"), "studio_mode_off") == 0);
	/* A default label is stored empty; the API still names the slot with its English default (R-I18N-1). */
	REQUIRE(eng.fader_slots[0].label[0] == '\0');
	REQUIRE(strcmp(obs_data_get_string(s0, "name"), "Crossfader (A/B)") == 0);
	obs_data_release(s0);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);

	/* Turn Studio Mode ON -> crossfader becomes available */
	eng.studio_mode_override = true;
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "studio_mode"));
	arr = obs_data_get_array(res, "slots");
	s0 = obs_data_array_item(arr, 0);
	REQUIRE(obs_data_get_bool(s0, "available"));
	REQUIRE(strcmp(obs_data_get_string(s0, "reason"), "") == 0);
	obs_data_release(s0);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);

	/* 3. set_slot error sentences for invalid slot id and unknown source names */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_9");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.") ==
		0);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "NonExistentLoop");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Source 'NonExistentLoop' not found. Create the source in OBS first or check its spelling.") ==
		0);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	obs_data_release(req);
	obs_data_release(res);

	/* Valid set_slot on fader_1: returns updated slot object and emits state_changed */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "curve", "dip_free");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "Loop 3");
	obs_data_set_bool(req, "afv", true);
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "id"), "fader_1") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "target_mode"), "crossfade_ab") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "curve"), "dip_free") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "label"), "Cam 1 -> Loop 3") == 0);
	REQUIRE(obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "") == 0);
	REQUIRE(ws_log.state_changed_count == 2);
	obs_data_release(req);
	obs_data_release(res);

	/* 4. set_fader_value on fader_1: Dip-Free holds alpha_a=1.0 across entire range (including t=0.75) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.75);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "slot"), "fader_1") == 0);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.75) < 1e-5);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_a - 1.0f) < 1e-5f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 0.75f) < 1e-5f);
	REQUIRE(ws_log.fader_changed_count == 1);
	REQUIRE(strcmp(ws_log.last_fader_slot, "fader_1") == 0);
	REQUIRE(strcmp(ws_log.last_fader_origin, "api") == 0);
	REQUIRE(fabs(ws_log.last_fader_value - 0.75) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);

	/* 5. nudge_fader and 30 Hz per-slot fader_changed coalescing */
	/* Advance time by 0.05s (> 1/30s) so the first nudge emits immediately */
	mfd_engine_tick(&eng, 0.05f);
	int before_burst = ws_log.fader_changed_count;

	for (int i = 0; i < 5; ++i) {
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_1");
		obs_data_set_double(req, "delta", -0.05);
		mfd_vendor_handle_nudge_fader(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
	}
	/* 0.75 - 5 * 0.05 = 0.50; only 1 immediate event emitted during the 0ms burst */
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.50f) < 1e-5f);
	REQUIRE(ws_log.fader_changed_count == before_burst + 1);
	REQUIRE(eng.fader_slots[1].fader_event_pending);

	/* Advance 0.04s (> 33.3ms) -> coalesced pending event flushes with final value 0.50 */
	mfd_engine_tick(&eng, 0.04f);
	REQUIRE(!eng.fader_slots[1].fader_event_pending);
	REQUIRE(ws_log.fader_changed_count == before_burst + 2);
	REQUIRE(fabs(ws_log.last_fader_value - 0.50) < 1e-5);

	/* 6. auto_fade: timed ramp from 0.50 to 1.00 over 100 ms */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 10); /* invalid (< 50 ms) */
	mfd_vendor_handle_auto_fade(req, res, &eng);
	REQUIRE(obs_data_get_string(res, "comment")[0] != '\0');
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 100);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "slot"), "fader_1") == 0);
	REQUIRE(eng.fader_slots[1].auto_fade_active);
	obs_data_release(req);
	obs_data_release(res);

	mfd_engine_tick(&eng, 0.05f); /* halfway through 100ms fade: 0.50 -> 0.75 */
	REQUIRE(eng.fader_slots[1].auto_fade_active);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.75f) < 1e-5f);
	REQUIRE(strcmp(ws_log.last_fader_origin, "auto_fade") == 0);

	mfd_engine_tick(&eng, 0.05f); /* completed 100ms fade: 1.00 */
	REQUIRE(!eng.fader_slots[1].auto_fade_active);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.00f) < 1e-5f);

	/* 7. set_fader_value on crossfader (T-Bar) with release: true finishes transition */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.4);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(g_mock_tbar_calls == 1);
	REQUIRE(fabsf(g_mock_tbar_last_val - 0.4f) < 1e-5f);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.4);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(g_mock_tbar_calls == 2);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.0) < 1e-5);
	REQUIRE(eng.fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
	obs_data_release(req);
	obs_data_release(res);

	/* 8. JSON config Export and Import round-trip (Item 7: D-5 Import/Export without file watcher) */
	const char *tmp_json = "test-mfd-config-roundtrip.json";
	snprintf(eng.configured_device_name, sizeof(eng.configured_device_name), "APC40 mkII");
	eng.smooth_tau_ms = 24.5f;
	eng.tbar_bidirectional_flip = false;
	eng.stream_deck_companion_dismissed = true;
	REQUIRE(mfd_engine_export_config_file(&eng, tmp_json));

	mfd_engine_load_apc40_defaults(&eng);
	eng.configured_device_name[0] = '\0';
	eng.smooth_tau_ms = 12.0f;
	eng.tbar_bidirectional_flip = true;
	eng.stream_deck_companion_dismissed = false;
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);

	REQUIRE(mfd_engine_import_config_file(&eng, tmp_json));
	REQUIRE(strcmp(eng.configured_device_name, "APC40 mkII") == 0);
	REQUIRE(fabsf(eng.smooth_tau_ms - 24.5f) < 1e-4f);
	REQUIRE(!eng.tbar_bidirectional_flip);
	REQUIRE(eng.stream_deck_companion_dismissed);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_TWO_SOURCE_MIX);
	REQUIRE(strcmp(eng.fader_slots[1].source_a_name, "Cam 1") == 0);
	REQUIRE(strcmp(eng.fader_slots[1].source_b_name, "Loop 3") == 0);
	remove(tmp_json);

	/* 9. In-process bridge: midi_fadester_get_slot_value, midi_fadester_set_fader_value, midi_fadester_fader_changed */
	proc_handler_t *fake_ph = proc_handler_create();
	signal_handler_t *fake_sh = signal_handler_create();
	REQUIRE(fake_ph != NULL && fake_sh != NULL);
	REQUIRE(mfd_bridge_register_with_handlers(fake_ph, fake_sh, &eng));

	struct fake_signal_log sig_log;
	memset(&sig_log, 0, sizeof(sig_log));
	signal_handler_connect(fake_sh, "midi_fadester_fader_changed", on_fake_signal, &sig_log);

	/* Advance tick so slot 1 is ready to emit immediately */
	mfd_engine_tick(&eng, 0.05f);

	/* 9a. Invalid slot over proc_handler returns ok=false and reason sentence */
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_9");
	calldata_set_float(&cd, "value", 0.5);
	REQUIRE(proc_handler_call(fake_ph, "midi_fadester_set_fader_value", &cd));
	REQUIRE(!calldata_bool(&cd, "ok"));
	REQUIRE(strcmp(calldata_string(&cd, "reason"),
		       "Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master, fx_1 .. fx_8.") ==
		0);
	calldata_free(&cd);

	/* 9b. Valid midi_fadester_set_fader_value emits midi_fadester_fader_changed signal */
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_1");
	calldata_set_float(&cd, "value", 0.625);
	REQUIRE(proc_handler_call(fake_ph, "midi_fadester_set_fader_value", &cd));
	REQUIRE(calldata_bool(&cd, "ok"));
	REQUIRE(strcmp(calldata_string(&cd, "reason"), "") == 0);
	calldata_free(&cd);

	REQUIRE(sig_log.count == 1);
	REQUIRE(strcmp(sig_log.slot, "fader_1") == 0);
	REQUIRE(fabsf(sig_log.value - 0.625f) < 1e-5f);
	REQUIRE(strcmp(sig_log.origin, "api") == 0);

	/* 9c. midi_fadester_get_slot_value reads back 0.625 with ok=true */
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_1");
	REQUIRE(proc_handler_call(fake_ph, "midi_fadester_get_slot_value", &cd));
	REQUIRE(calldata_bool(&cd, "ok"));
	REQUIRE(fabs(calldata_float(&cd, "value") - 0.625) < 1e-5);
	REQUIRE(strcmp(calldata_string(&cd, "reason"), "") == 0);
	calldata_free(&cd);

	signal_handler_disconnect(fake_sh, "midi_fadester_fader_changed", on_fake_signal, &sig_log);
	mfd_bridge_register_with_handlers(NULL, NULL, NULL);
	signal_handler_destroy(fake_sh);
	proc_handler_destroy(fake_ph);

	/*
	 * 10. Rail Behavior Audit (R-FEEL-4):
	 *     - 10a: Two-source crossfade (crossfade_ab) and single-layer opacity (layer_opacity) at 0 and 100
	 *            (fader jitter at a rail must not flicker the picture; 0 extra output_write_count).
	 *     - 10b: Audio volume (audio_volume) at the bottom rail (no flicker between silent -inf dB and -60 dB).
	 *     - 10c: set_fader_value with release and auto_fade reaching a rail over the API (one-shot finish + parked).
	 *     - 10d: nudge_fader right after a completed throw (Stream Deck dial on T-bar and two-rail slots).
	 *     - 10e: "Finish T-bar" press (one-shot finish when active, no-op when already parked).
	 *     - get_state exposes `"parked": true | false` per API-CONTRACT.md.
	 */
	mfd_midi_queue_init(&eng.midi_queue);
	mfd_midi_parser_init(&eng.midi_parser);
	eng.learning_slot_index = -1;
	eng.tbar_bidirectional_flip = true;
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);

	/* Configure fader_1 (Ch 1, CC 7) as crossfade_ab and fader_2 (Ch 2, CC 7) as layer_opacity */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "Loop 3");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "parked"));
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_2");
	obs_data_set_string(req, "target_mode", "layer_opacity");
	obs_data_set_string(req, "source_a", "Cam 1");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "parked"));
	obs_data_release(req);
	obs_data_release(res);

	/* 10a-1: Two-source crossfade (fader_1, Ch 0 CC 7): linear to physical end (92%/95% linear & unparked, >= 97% parks linearly) */
	push_midi_cc(&eng, 0, 7, 0);
	push_midi_cc(&eng, 0, 7, 64);
	push_midi_cc(&eng, 0, 7, 117); /* 117/127 = 0.9213 (< 0.97): must stay linear ~0.9213 and NOT park! */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - (117.0f / 127.0f)) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	push_midi_cc(&eng, 0, 7, 121); /* 121/127 = 0.9528 (< 0.97): fader at 95% means 95%, NOT parked! */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - (121.0f / 127.0f)) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	push_midi_cc(&eng, 0, 7, 124); /* 124/127 = 0.9764 (>= 0.97): parks at linear 0.9764 (no snap!) */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - (124.0f / 127.0f)) < 1e-5f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	push_midi_cc(&eng, 0, 7, 127); /* physical end 127/127 = 1.0000 */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_a - 1.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 1.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	uint32_t writes_xfade_top = eng.fader_slots[1].output_write_count;

	/* Rail jitter at 100% rail (CC 126, 127, 122, 126, 118, 127 — all within 8% of peak 127) -> 0 writes */
	push_midi_cc(&eng, 0, 7, 126);
	push_midi_cc(&eng, 0, 7, 127);
	push_midi_cc(&eng, 0, 7, 122);
	push_midi_cc(&eng, 0, 7, 126);
	push_midi_cc(&eng, 0, 7, 118);
	push_midi_cc(&eng, 0, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[1].output_write_count == writes_xfade_top);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 1.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	/* Retreat > 8% (CC 114 = 0.8976 < 1.0 - 0.08) unparks with zero jump, stays linear at 10%, parks only <= 3% */
	push_midi_cc(&eng, 0, 7, 114);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.0f) < 1e-6f);

	push_midi_cc(&eng, 0, 7, 11); /* 11/114 = 0.0965 (> 0.03): must stay linear and unparked! */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - (11.0f / 114.0f)) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	push_midi_cc(&eng, 0, 7, 3); /* 3/114 = 0.0263 (<= 0.03): parks linearly inside 3% rail zone */
	push_midi_cc(&eng, 0, 7, 0); /* physical bottom stop 0.0000 */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	uint32_t writes_xfade_bot = eng.fader_slots[1].output_write_count;

	/* Rail jitter at 0% rail (CC 5, 0, 4, 1, 9, 0 — all within 8% of peak 0) -> 0 writes */
	push_midi_cc(&eng, 0, 7, 5);
	push_midi_cc(&eng, 0, 7, 0);
	push_midi_cc(&eng, 0, 7, 4);
	push_midi_cc(&eng, 0, 7, 1);
	push_midi_cc(&eng, 0, 7, 9);
	push_midi_cc(&eng, 0, 7, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[1].output_write_count == writes_xfade_bot);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	/* 10a-2: Single-layer opacity (fader_2, Ch 1 CC 7, starts at 1.0): linear down to <= 3% and up to >= 97% */
	push_midi_cc(&eng, 1, 7, 127);
	push_midi_cc(&eng, 1, 7, 12); /* 12/127 = 0.0945 (> 0.03): stays linear, NOT parked */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[2].last_applied_val - (12.0f / 127.0f)) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 2));

	push_midi_cc(&eng, 1, 7, 3); /* 3/127 = 0.0236 <= 0.03 -> parks linearly */
	push_midi_cc(&eng, 1, 7, 0); /* 0.0000 */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[2].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[2].last_alpha_a - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 2));
	uint32_t writes_layer_bot = eng.fader_slots[2].output_write_count;

	/* Bottom rail jitter on layer_opacity -> 0 writes */
	push_midi_cc(&eng, 1, 7, 4);
	push_midi_cc(&eng, 1, 7, 0);
	push_midi_cc(&eng, 1, 7, 7);
	push_midi_cc(&eng, 1, 7, 2);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[2].output_write_count == writes_layer_bot);

	/* Unpark upward (> 8% from 0: CC 13) -> linear at 95% (CC 121), parks inside >= 97% (CC 124 -> 127) */
	push_midi_cc(&eng, 1, 7, 13);
	push_midi_cc(&eng, 1, 7, 121);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 2));
	push_midi_cc(&eng, 1, 7, 125);
	push_midi_cc(&eng, 1, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[2].last_applied_val - 1.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[2].last_alpha_a - 1.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 2));
	uint32_t writes_layer_top = eng.fader_slots[2].output_write_count;

	/* Top rail jitter on layer_opacity -> 0 writes (never toggles >= 0.999 shader bypass) */
	push_midi_cc(&eng, 1, 7, 127);
	push_midi_cc(&eng, 1, 7, 121);
	push_midi_cc(&eng, 1, 7, 126);
	push_midi_cc(&eng, 1, 7, 118);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[2].output_write_count == writes_layer_top);

	/* 10b: Audio volume (fader_3, Ch 2 CC 7): bottom rail at <= 3% (NOT 10%), linear to 0, zero jitter flicker */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_3");
	obs_data_set_string(req, "target_mode", "audio_volume");
	obs_data_set_string(req, "source_a", "Cam 1");
	mfd_vendor_handle_set_slot(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);

	push_midi_cc(&eng, 2, 7, 127);
	push_midi_cc(&eng, 2, 7, 12); /* 12/127 = 0.0945 (> 3%): stays linear (~ -61.5 dB), NOT parked! */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - (12.0f / 127.0f)) < 1e-5f);
	REQUIRE(isfinite(eng.fader_slots[3].last_audio_db));
	REQUIRE(!get_slot_parked_via_get_state(&eng, 3));

	push_midi_cc(&eng, 2, 7, 6); /* 6/127 = 0.0472 (> 3%): still linear, NOT parked! */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - (6.0f / 127.0f)) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 3));

	push_midi_cc(&eng, 2, 7, 3); /* 3/127 = 0.0236 (<= 3%): enters parked inside 3% bottom rail zone */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - (3.0f / 127.0f)) < 1e-5f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 3));

	push_midi_cc(&eng, 2, 7, 0); /* physical bottom stop 0.0 -> silent (-inf dB) */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[3].last_audio_mul - 0.0f) < 1e-6f);
	REQUIRE(!isfinite(eng.fader_slots[3].last_audio_db));
	REQUIRE(get_slot_parked_via_get_state(&eng, 3));
	uint32_t writes_audio_bot = eng.fader_slots[3].output_write_count;

	/* Jitter at bottom rail (CC 6, 0, 9, 2, 10, 0) produces 0 volume writes and stays silent (-inf dB) */
	push_midi_cc(&eng, 2, 7, 6);
	push_midi_cc(&eng, 2, 7, 0);
	push_midi_cc(&eng, 2, 7, 9);
	push_midi_cc(&eng, 2, 7, 2);
	push_midi_cc(&eng, 2, 7, 10);
	push_midi_cc(&eng, 2, 7, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[3].output_write_count == writes_audio_bot);
	REQUIRE(fabsf(eng.fader_slots[3].last_audio_mul - 0.0f) < 1e-6f);
	REQUIRE(!isfinite(eng.fader_slots[3].last_audio_db));
	REQUIRE(get_slot_parked_via_get_state(&eng, 3));

	/* Advance > 8% (CC 13 = 0.1024 > 0.08) unparks smoothly from silent (0.0f) */
	push_midi_cc(&eng, 2, 7, 13);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 3));
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - 0.0f) < 1e-6f);

	/* 10c: set_fader_value with release and auto_fade reaching a rail over the API */
	/* 10c-1: set_fader_value with release on two-rail target (fader_1) snaps one-shot to rail and parks */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.50);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.80);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 1.0) < 1e-6);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	uint32_t writes_after_api_rel = eng.fader_slots[1].output_write_count;

	/* Repeating set_fader_value at the same rail while parked is a one-shot no-op */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(eng.fader_slots[1].output_write_count == writes_after_api_rel);

	/* 10c-2: auto_fade to 0.0 on fader_1 parks one-shot on completion */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 0.0);
	obs_data_set_int(req, "ms", 100);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));
	mfd_engine_tick(&eng, 0.05f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));
	mfd_engine_tick(&eng, 0.05f);
	REQUIRE(!eng.fader_slots[1].auto_fade_active);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	/* 10c-3: auto_fade 0.0 -> 1.0 on T-bar (crossfader) over 10 ticks completes ONE-SHOT at 90% and stops */
	g_mock_tbar_calls = 0;
	g_mock_tbar_completions = 0;
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 100);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);

	for (int tick = 0; tick < 11; ++tick) {
		mfd_engine_tick(&eng, 0.01f);
	}
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(!eng.fader_slots[0].auto_fade_active);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(fabsf(eng.fader_slots[0].last_applied_val - 0.0f) < 1e-6f);

	/* 10d: nudge_fader right after a completed throw (Stream Deck dial) */
	/* 10d-1: Physical MIDI throw 0 -> 127 on crossfader (T-bar), then negative Stream Deck dial nudge */
	mfd_engine_unlatch_tbar(&eng);
	eng.fader_slots[0].tbar_state = MFD_TBAR_IDLE;
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	eng.fader_slots[0].takeover.polarity_reversed = false;
	eng.fader_slots[0].takeover.rescale_origin = 0.0f;
	push_midi_cc(&eng, 0, 15, 0);
	push_midi_cc(&eng, 0, 15, 115);
	push_midi_cc(&eng, 0, 15, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* Turn Stream Deck dial counter-clockwise (delta: -0.05) right after completed throw */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "delta", -0.05);
	mfd_vendor_handle_nudge_fader(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.05) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 0));

	/* Second dial click in the same direction (-0.05) continues to 0.10 */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "delta", -0.05);
	mfd_vendor_handle_nudge_fader(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.10) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);

	/* 1-LSB physical MIDI jitter at top rail (CC 126) does NOT jump T-bar away from 0.10 */
	push_midi_cc(&eng, 0, 15, 126);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[0].last_applied_val - 0.10f) < 1e-5f);

	/* 10d-2: Physical MIDI throw 0 -> 127 on fader_1 (crossfade_ab), then small -0.02 dial nudge */
	eng.fader_slots[1].takeover.state = MFD_TAKEOVER_ENGAGED;
	eng.fader_slots[1].takeover.lower_rescale_origin = 0.0f;
	eng.fader_slots[1].takeover.upper_rescale_origin = 1.0f;
	eng.fader_slots[1].takeover.lower_rescale_val = 0.0f;
	eng.fader_slots[1].takeover.upper_rescale_val = 1.0f;
	eng.fader_slots[1].takeover.armed_upper_rail = true;
	push_midi_cc(&eng, 0, 7, 0);
	push_midi_cc(&eng, 0, 7, 115);
	push_midi_cc(&eng, 0, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.0f) < 1e-6f);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "delta", -0.02);
	mfd_vendor_handle_nudge_fader(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.98) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	/* 1-LSB MIDI jitter at top rail (CC 126) does NOT re-snap fader_1 to 1.0 or re-park */
	push_midi_cc(&eng, 0, 7, 126);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.98f) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	/* 10e: "Finish T-bar" press (one-shot finish when active, no-op when already parked) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.45);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 0));

	int calls_before_finish = g_mock_tbar_calls;
	int completions_before_finish = g_mock_tbar_completions;

	/* First "Finish T-bar" press -> completes throw once and parks */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(g_mock_tbar_calls == calls_before_finish + 1);
	REQUIRE(g_mock_tbar_completions == completions_before_finish + 1);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* Second and third "Finish T-bar" presses while already parked -> strict no-op (0 calls) */
	for (int press = 0; press < 2; ++press) {
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "crossfader");
		obs_data_set_double(req, "value", 1.0);
		obs_data_set_bool(req, "release", true);
		mfd_vendor_handle_set_fader_value(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
	}
	REQUIRE(g_mock_tbar_calls == calls_before_finish + 1);
	REQUIRE(g_mock_tbar_completions == completions_before_finish + 1);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/*
	 * 11. Per-transition T-bar release latch & zero-write parked invariant under 30 ms smoothing + 127/126/127 jitter:
	 *     - Sweep 0 -> 127 over 200 ms with 30 ms smoother enabled, then jitter 126/127 every 100 ms for 3.0 s.
	 *     - Assert: exactly 1 completion (release), g_mock_tbar_latched == true, 0 writes and 0 releases during
	 *       the entire 3-second rail jitter, and 0 writes/releases even if mfd_engine_apply_slot_output_ex is
	 *       called directly while parked!
	 *     - Retreat > 8% unparks, calls tbar_unlatch once, and allows the return throw 127 -> 0 to release once.
	 */
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 30.0f);
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	g_mock_tbar_calls = 0;
	g_mock_tbar_set_writes = 0;
	g_mock_tbar_completions = 0;

	for (int step = 0; step <= 12; ++step) {
		uint8_t cc_v = (uint8_t)((step * 127) / 12);
		push_midi_cc(&eng, 0, 15, cc_v);
		mfd_engine_tick(&eng, 0.016f);
	}
	/* Allow 30 ms smoother to cross 90% and park */
	for (int tick = 0; tick < 6; ++tick) {
		mfd_engine_tick(&eng, 0.016f);
	}
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(g_mock_tbar_latched);

	int writes_at_park = g_mock_tbar_set_writes;
	uint32_t slot_writes_at_park = eng.fader_slots[0].output_write_count;
	int unlatches_at_park = g_mock_tbar_unlatches;

	/* 30 steps of 126/127 jitter every 100 ms (3.0 s total) while parked -> 0 writes, 0 extra releases */
	for (int j = 0; j < 30; ++j) {
		push_midi_cc(&eng, 0, 15, ((j % 2) == 0) ? 126U : 127U);
		for (int sub = 0; sub < 6; ++sub) {
			mfd_engine_tick(&eng, 0.0167f);
		}
	}
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(g_mock_tbar_set_writes == writes_at_park);
	REQUIRE(eng.fader_slots[0].output_write_count == slot_writes_at_park);
	REQUIRE(g_mock_tbar_unlatches == unlatches_at_park);

	/* Direct mfd_engine_apply_slot_output_ex calls while parked -> strict no-op (0 writes, 0 releases) */
	mfd_engine_apply_slot_output(&eng, &eng.fader_slots[0], 1.0f);
	mfd_engine_apply_slot_output_ex(&eng, &eng.fader_slots[0], 1.0f, true);
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(g_mock_tbar_set_writes == writes_at_park);
	REQUIRE(eng.fader_slots[0].output_write_count == slot_writes_at_park);

	/* Retreat > 8% (CC 114 < 127 * 0.92) unparks and unlatches once; return throw to 0 releases once */
	push_midi_cc(&eng, 0, 15, 114);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(!g_mock_tbar_latched);
	REQUIRE(g_mock_tbar_unlatches == unlatches_at_park + 1);

	for (int step = 10; step >= 0; --step) {
		uint8_t cc_v = (uint8_t)((step * 114) / 10);
		push_midi_cc(&eng, 0, 15, cc_v);
		mfd_engine_tick(&eng, 0.016f);
	}
	for (int tick = 0; tick < 6; ++tick) {
		mfd_engine_tick(&eng, 0.016f);
	}
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(g_mock_tbar_completions == 2);
	REQUIRE(g_mock_tbar_latched);

	/*
	 * 12. 0.1.2 Timer-Free R-TBAR-SM State Machine & Calibrated Rail Zone Tests:
	 *     - 12a: 0 -> 100, then 1 CC step every 300 ms (30 x 10 ms ticks) from 100 to 127 with a 2 s pause
	 *            (200 x 10 ms ticks) at CC 116 (tau = 30 ms): 0 releases before CC 124 (including across the
	 *            entire 2 s pause at CC 116), ~93.7% mix at CC 119, monotonic T-bar value, and 1 release at CC 124.
	 *     - 12b: Reversed-polarity slow return 127 -> 0 with a 2 s pause at CC 12 (~90.55% throw progress):
	 *            0 extra releases before CC <= 3, and 1 release at CC 3.
	 *     - 12c: Throwing -> Idle aborted throw (0 -> 60 -> 0 + origin-rail jitter 2, 1, 0):
	 *            1 abort release (pos = 0 + release), 0 completions, Idle (t = 0, no polarity flip, no park),
	 *            0 further writes at CC 2/1/0; followed immediately by 0 -> 60 -> 2 s pause -> 127 asserting
	 *            0 releases during pause and 1 completion at the rail (CC 124) only.
	 *     - 12d: Calibrated control range [0, 125] via set_slot + persistence + auto-widening to [0, 127] on CC 127.
	 */
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 30.0f);
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	g_mock_tbar_calls = 0;
	g_mock_tbar_set_writes = 0;
	g_mock_tbar_completions = 0;
	g_mock_tbar_aborts = 0;
	g_mock_tbar_last_val = 0.0f;

	for (int cc = 0; cc <= 100; cc += 10) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		mfd_engine_tick(&eng, 0.010f);
	}
	float prev_slow_val = g_mock_tbar_last_val;
	float val_at_cc119 = 0.0f;
	for (int cc = 100; cc <= 127; ++cc) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		int ticks = (cc == 116) ? 230 : 30; /* 300 ms/step + 2000 ms pause at CC 116 */
		for (int sub = 0; sub < ticks; ++sub) {
			mfd_engine_tick(&eng, 0.010f);
			if (cc < 124) {
				REQUIRE(g_mock_tbar_completions == 0);
				REQUIRE(g_mock_tbar_aborts == 0);
				REQUIRE(!get_slot_parked_via_get_state(&eng, 0));
			}
		}
		if (cc < 124) {
			REQUIRE(g_mock_tbar_last_val + 1e-5f >= prev_slow_val);
			prev_slow_val = g_mock_tbar_last_val;
			if (cc == 119) {
				val_at_cc119 = g_mock_tbar_last_val;
			}
		} else if (cc == 124) {
			REQUIRE(g_mock_tbar_completions == 1);
			REQUIRE(get_slot_parked_via_get_state(&eng, 0));
		}
	}
	/* At CC 119 (~93.7%), T-bar mix must be ~93.7% (0.92..0.95), NOT snapped to 100% */
	REQUIRE(val_at_cc119 >= 0.92f && val_at_cc119 <= 0.95f);
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(g_mock_tbar_aborts == 0);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* 12b: Reversed-polarity return slow sweep 127 -> 0 with 2 s pause at CC 12 (~90.55% throw progress) */
	float prev_rev_val = 0.0f;
	for (int cc = 127; cc >= 0; --cc) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		int ticks = (cc == 12) ? 230 : 30;
		for (int sub = 0; sub < ticks; ++sub) {
			mfd_engine_tick(&eng, 0.010f);
			if (cc > 3) {
				REQUIRE(g_mock_tbar_completions == 1);
				REQUIRE(g_mock_tbar_aborts == 0);
			}
		}
		if (cc > 3) {
			if (!get_slot_parked_via_get_state(&eng, 0)) {
				REQUIRE(g_mock_tbar_last_val + 1e-5f >= prev_rev_val);
				prev_rev_val = g_mock_tbar_last_val;
			}
		} else if (cc == 3) {
			REQUIRE(g_mock_tbar_completions == 2);
			REQUIRE(get_slot_parked_via_get_state(&eng, 0));
		}
	}
	REQUIRE(g_mock_tbar_completions == 2);
	REQUIRE(g_mock_tbar_aborts == 0);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* 12c: Throwing -> Idle aborted throw (0 -> 60 -> 0), then immediate 0 -> 60 -> 2 s pause -> 127 */
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 30.0f);
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	g_mock_tbar_calls = 0;
	g_mock_tbar_set_writes = 0;
	g_mock_tbar_completions = 0;
	g_mock_tbar_aborts = 0;

	for (int cc = 0; cc <= 60; cc += 5) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		mfd_engine_tick(&eng, 0.010f);
	}
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_THROWING);
	REQUIRE(g_mock_tbar_aborts == 0);
	REQUIRE(g_mock_tbar_completions == 0);

	for (int cc = 55; cc >= 0; cc -= 5) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		mfd_engine_tick(&eng, 0.010f);
	}
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 0);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	REQUIRE(!eng.fader_slots[0].takeover.polarity_reversed);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(fabsf(eng.fader_slots[0].last_applied_val - 0.0f) < 1e-6f);

	int writes_after_abort = g_mock_tbar_set_writes;
	uint8_t origin_jitter[] = {2, 1, 3, 0};
	for (size_t ji = 0U; ji < sizeof(origin_jitter) / sizeof(origin_jitter[0]); ++ji) {
		push_midi_cc(&eng, 0, 15, origin_jitter[ji]);
		mfd_engine_tick(&eng, 0.016f);
	}
	REQUIRE(g_mock_tbar_set_writes == writes_after_abort);
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 0);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);

	/* Immediate follow-up without resetting slot: 0 -> 60 -> 2 s pause -> 127 */
	for (int cc = 0; cc <= 60; cc += 5) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		mfd_engine_tick(&eng, 0.010f);
	}
	for (int sub = 0; sub < 200; ++sub) {
		mfd_engine_tick(&eng, 0.010f);
	}
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_THROWING);
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 0);

	for (int cc = 61; cc <= 127; ++cc) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		mfd_engine_tick(&eng, 0.010f);
		if (cc < 124) {
			REQUIRE(g_mock_tbar_completions == 0);
		}
	}
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_PARKED);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* 12d: Calibrated control range [0, 125] via set_slot, persistence, and auto-widening on CC 127 */
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 30.0f);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_int(req, "cal_min", 0);
	obs_data_set_int(req, "cal_max", 125);
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_int(res, "cal_min") == 0);
	REQUIRE(obs_data_get_int(res, "cal_max") == 125);
	obs_data_release(req);
	obs_data_release(res);

	/* Reloading profile preserves persisted [0, 125] calibration for (apc40mk2, Ch 1 CC 15) */
	mfd_engine_load_apc40_defaults(&eng);
	REQUIRE(eng.fader_slots[0].cal.cal_min == 0);
	REQUIRE(eng.fader_slots[0].cal.cal_max == 125);
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	g_mock_tbar_completions = 0;

	/* Short-travel fader stopping mechanically at CC 125 completes cleanly at its calibrated rail [122..125] */
	for (int cc = 0; cc <= 125; ++cc) {
		push_midi_cc(&eng, 0, 15, (uint8_t)cc);
		mfd_engine_tick(&eng, 0.010f);
		if (cc <= 121) {
			REQUIRE(g_mock_tbar_completions == 0);
		}
	}
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* Export/Import round-trip preserves control_calibrations [0, 125], and CC 127 auto-widens to [0, 127] */
	const char *tmp_cal_json = "test-mfd-cal-roundtrip.json";
	REQUIRE(mfd_engine_export_config_file(&eng, tmp_cal_json));
	/* A full table of another desk's learned ranges: Import replaces the table instead of merging into it */
	for (int k = 0; k < MFD_MAX_CAL_ENTRIES; ++k) {
		eng.cal_entries[k].in_use = true;
		snprintf(eng.cal_entries[k].profile_name, sizeof(eng.cal_entries[k].profile_name), "%s", "zz-foreign");
		eng.cal_entries[k].addr = (struct mfd_midi_addr){MFD_MIDI_CC, 15U, (uint8_t)k, false};
		mfd_calibration_set(&eng.cal_entries[k].cal, false, 0, 100);
	}
	eng.fader_slots[0].cal.cal_max = 127;
	REQUIRE(mfd_engine_import_config_file(&eng, tmp_cal_json));
	remove(tmp_cal_json);
	struct mfd_control_calibration got;
	REQUIRE(mfd_engine_lookup_persisted_calibration(&eng, "apc40mk2",
							&(struct mfd_midi_addr){MFD_MIDI_CC, 0U, 15U, false}, &got));
	REQUIRE(got.cal_min == 0 && got.cal_max == 125);
	for (int k = 0; k < MFD_MAX_CAL_ENTRIES; ++k) {
		REQUIRE(!eng.cal_entries[k].in_use || strcmp(eng.cal_entries[k].profile_name, "zz-foreign") != 0);
	}
	REQUIRE(eng.fader_slots[0].cal.cal_min == 0);
	REQUIRE(eng.fader_slots[0].cal.cal_max == 125);

	push_midi_cc(&eng, 0, 15, 127);
	mfd_engine_tick(&eng, 0.010f);
	REQUIRE(eng.fader_slots[0].cal.cal_min == 0);
	REQUIRE(eng.fader_slots[0].cal.cal_max == 127);

	/* 12e: 14-bit CC pair (CC 7 MSB + CC 39 LSB) auto-upgrades 7-bit slot calibration [0, 127] -> [0, 16383] */
	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "Loop 3");
	mfd_vendor_handle_set_slot(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	eng.fader_slots[1].takeover.state = MFD_TAKEOVER_ENGAGED;
	REQUIRE(eng.fader_slots[1].cal.cal_max == 127);

	/* Send 14-bit 25% (v14 = 4096 -> MSB = 32, LSB = 0) on Ch 1 CC 7 / CC 39 */
	push_midi_cc(&eng, 0, 7, 32);
	push_midi_cc(&eng, 0, 39, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[1].addr.is_14bit);
	REQUIRE(eng.fader_slots[1].cal.cal_min == 0);
	REQUIRE(eng.fader_slots[1].cal.cal_max == 16383);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - (4096.0f / 16383.0f)) < 1e-4f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	/* 12f: MIDImix Master (CC 62) survives FX 4 (CC 30). Both carry timestamp 0, so only the mask of bound
	 * CC 32..63 controls can stop the pairing. */
	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_profile_defaults(&eng, MFD_PROFILE_AKAI_MIDIMIX);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);
	push_midi_cc(&eng, 0, 30, 64);
	push_midi_cc(&eng, 0, 62, 100);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[9].last_raw_norm - 100.0f / 127.0f) < 1e-3f);
	REQUIRE(!eng.fader_slots[13].addr.is_14bit);

	/* The same with Master switched off: its CC 62 still never pairs, so FX 4 (CC 30) is not promoted to 14-bit */
	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_profile_defaults(&eng, MFD_PROFILE_AKAI_MIDIMIX);
	eng.fader_slots[9].enabled = false;
	push_midi_cc(&eng, 0, 30, 64);
	push_midi_cc(&eng, 0, 62, 100);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.last_midi_decoded.addr.number == 62U && !eng.last_midi_decoded.addr.is_14bit);
	REQUIRE(!eng.fader_slots[13].addr.is_14bit);

	/* 12g: A binding already promoted to 14-bit and saved reads the high byte alone over its whole travel */
	mfd_engine_load_apc40_defaults(&eng);
	mfd_midi_parser_init(&eng.midi_parser);
	eng.fader_slots[1].addr.is_14bit = true;
	mfd_calibration_set(&eng.fader_slots[1].cal, true, 0, 16383);
	push_midi_cc(&eng, 0, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[1].last_raw_norm > 0.99f);

	/* 12h: A new layout forgets the old desk's pairing history (APC40 Device knob CC 16, then APC mini CC 48) */
	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_apc40_defaults(&eng);
	push_midi_cc(&eng, 0, 16, 64);
	mfd_engine_tick(&eng, 0.016f);
	mfd_engine_load_profile_defaults(&eng, MFD_PROFILE_APC_MINI_MK2);
	REQUIRE(!eng.midi_parser.cc_has_msb[0][16]);
	push_midi_cc(&eng, 0, 48, 100);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_raw_norm - 100.0f / 127.0f) < 1e-3f);

	/* 12i: A volume moved in OBS (mixer, script) makes an engaged Volume fader pick up instead of jumping */
	{
		struct mfd_scene_graph_ops vol_ops = g_fake_sg_ops;
		vol_ops.get_volume = fake_op_get_volume;
		eng.scene_graph_ops = &vol_ops;
		fake_sg_add_source(&fake_sg, "Mic/Aux", true);
		mfd_engine_load_apc40_defaults(&eng);
		mfd_engine_set_smooth_tau_ms(&eng, 0.0f);
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_2");
		obs_data_set_string(req, "target_mode", "audio_volume");
		obs_data_set_string(req, "source_a", "Mic/Aux");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		obs_data_release(req);
		obs_data_release(res);
		struct fake_sg_source *mic = fake_sg_find_source(&fake_sg, "Mic/Aux");
		REQUIRE(mic != NULL);
		mic->last_volume_mul = 1.0f;
		push_midi_cc(&eng, 1, 7, 127);
		mfd_engine_tick(&eng, 0.016f);
		push_midi_cc(&eng, 1, 7, 102);
		mfd_engine_tick(&eng, 0.016f);
		REQUIRE(eng.fader_slots[2].takeover.state == MFD_TAKEOVER_ENGAGED);
		REQUIRE(fabsf(eng.fader_slots[2].last_applied_val - 102.0f / 127.0f) < 0.01f);

		const int mic_writes = mic->volume_writes;
		mic->last_volume_mul = 0.027f; /* an OBS mixer drag to 0.3 deflection (0.3 cubed) */
		mfd_engine_tick(&eng, 0.016f);
		REQUIRE(eng.fader_slots[2].takeover.state == MFD_TAKEOVER_WAITING);
		REQUIRE(fabsf(eng.fader_slots[2].takeover.target_obs - 0.3f) < 0.01f);

		push_midi_cc(&eng, 1, 7, 100);
		mfd_engine_tick(&eng, 0.016f);
		REQUIRE(mic->volume_writes == mic_writes);
		REQUIRE(mic->last_volume_mul == 0.027f);

		/* The plugin's own state now matches OBS, so a quiet tick changes nothing (no false echo) */
		mfd_engine_tick(&eng, 0.016f);
		REQUIRE(eng.fader_slots[2].takeover.state == MFD_TAKEOVER_WAITING);
		REQUIRE(fabsf(eng.fader_slots[2].takeover.target_obs - 0.3f) < 0.01f);
		REQUIRE(mic->volume_writes == mic_writes);
		REQUIRE(mic->last_volume_mul == 0.027f);

		/* An unavailable Volume strip (its source has no audio now) is not followed */
		mic->has_audio = false;
		mfd_engine_reevaluate_slots(&eng, false);
		REQUIRE(!eng.fader_slots[2].cached_available);
		mic->last_volume_mul = 0.5f;
		mfd_engine_tick(&eng, 0.016f);
		REQUIRE(eng.fader_slots[2].last_audio_mul == 0.027f);
		REQUIRE(fabsf(eng.fader_slots[2].takeover.target_obs - 0.3f) < 0.01f);
		mic->has_audio = true;
		eng.scene_graph_ops = &g_fake_sg_ops;
	}

	/* 12j: Import opens the MIDI port the file names and keeps the imported bindings. No port has this name, so the
	 * open attempt fails as "not found" and no real port is opened. */
	{
		char saved_dev[MFD_MIDI_DEV_NAME_MAX];
		snprintf(saved_dev, sizeof(saved_dev), "%s", eng.configured_device_name);
		mfd_winmm_init(&eng.midi_dev, &eng.midi_queue);
		snprintf(eng.configured_device_name, sizeof(eng.configured_device_name), "%s", "Fadester Test Port B");
		eng.fader_slots[1].addr = (struct mfd_midi_addr){MFD_MIDI_CC, 0U, 99U, false};
		const char *tmp_reopen_json = "test-mfd-import-reopen.json";
		REQUIRE(mfd_engine_export_config_file(&eng, tmp_reopen_json));
		eng.configured_device_name[0] = '\0';
		REQUIRE(mfd_engine_import_config_file_and_reopen(&eng, tmp_reopen_json));
		remove(tmp_reopen_json);
		REQUIRE(strcmp(eng.configured_device_name, "Fadester Test Port B") == 0);
		REQUIRE(!eng.midi_dev.is_open);
		REQUIRE(eng.midi_dev.error_kind == MFD_WINMM_ERR_CONFIGURED_NOT_FOUND);
		REQUIRE(eng.fader_slots[1].addr.number == 99U);

		/* The same port already open: no reopen, but the import still starts with clean pairing history. The open
		 * state is set by hand (no handle), and the file has no slot list, so only this path resets the parser. */
		const char *tmp_same_json = "test-mfd-import-same-port.json";
		FILE *same_fp = fopen(tmp_same_json, "wb");
		REQUIRE(same_fp != NULL);
		fputs("{\"device_name\":\"Fadester Test Port B\",\"active_profile\":\"apc40mk2\"}\n", same_fp);
		fclose(same_fp);
		mfd_winmm_init(&eng.midi_dev, &eng.midi_queue);
		eng.midi_dev.is_open = true;
		snprintf(eng.midi_dev.opened_name, sizeof(eng.midi_dev.opened_name), "%s", "Fadester Test Port B");
		push_midi_cc(&eng, 0, 16, 64);
		mfd_engine_tick(&eng, 0.016f);
		REQUIRE(eng.midi_parser.cc_has_msb[0][16]);
		REQUIRE(mfd_engine_import_config_file_and_reopen(&eng, tmp_same_json));
		remove(tmp_same_json);
		REQUIRE(eng.midi_dev.is_open && eng.midi_dev.error_kind == MFD_WINMM_OK);
		REQUIRE(!eng.midi_parser.cc_has_msb[0][16]);

		snprintf(eng.configured_device_name, sizeof(eng.configured_device_name), "%s", saved_dev);
		mfd_winmm_init(&eng.midi_dev, &eng.midi_queue);
	}

	/* 12k: Generic after a desk without a crossfader brings back the slots that desk lacked as Not used; a strip the
	 * user turned off stays off, and bindings stay */
	mfd_engine_load_profile_defaults(&eng, MFD_PROFILE_AKAI_MIDIMIX);
	REQUIRE(!eng.fader_slots[0].enabled);
	eng.fader_slots[2].enabled = false;
	REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_GENERIC, false));
	REQUIRE(eng.fader_slots[0].enabled && eng.fader_slots[0].target_type == MFD_TARGET_UNASSIGNED);
	REQUIRE(!eng.fader_slots[2].enabled);
	REQUIRE(eng.fader_slots[1].addr.number == 19U);

	/* 12l: Only layer jobs keep a scene; a stale one is not reported, saved, or pinned again later */
	mfd_engine_load_apc40_defaults(&eng);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "scene", "Main");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "Loop 3");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_has_user_value(res, "error"));
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(strcmp(eng.fader_slots[1].scene_name, "Main") == 0);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "audio_volume");
	obs_data_set_string(req, "source_a", "Cam 1");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_has_user_value(res, "error"));
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(eng.fader_slots[1].scene_name[0] == '\0');
	REQUIRE(eng.fader_slots[1].resolved_scene_name[0] == '\0');
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	obs_data_array_t *scene_slots = obs_data_get_array(res, "slots");
	obs_data_t *scene_item = obs_data_array_item(scene_slots, 1U);
	REQUIRE(scene_item != NULL);
	REQUIRE(strcmp(obs_data_get_string(scene_item, "scene"), "") == 0);
	obs_data_release(scene_item);
	obs_data_array_release(scene_slots);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "layer_opacity");
	obs_data_set_string(req, "source_a", "Cam 1");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_has_user_value(res, "error"));
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(strcmp(eng.fader_slots[1].resolved_scene_name, "Main") == 0);
	REQUIRE(eng.fader_slots[1].scene_name[0] == '\0');

	/* Pin the layer job to "Main" again, so the Advanced tab's switch to Mix master starts from a pinned scene */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "scene", "Main");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_has_user_value(res, "error"));
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(strcmp(eng.fader_slots[1].scene_name, "Main") == 0);

	struct mfd_slot_config_edit scene_edit;
	memset(&scene_edit, 0, sizeof(scene_edit));
	scene_edit.enabled = true;
	scene_edit.target_type = MFD_TARGET_MIX_MASTER;
	scene_edit.preserve_scene = true;
	snprintf(scene_edit.source_a_name, sizeof(scene_edit.source_a_name), "%s", "Cam 1");
	REQUIRE(mfd_engine_update_slot_config(&eng, 1, &scene_edit));
	REQUIRE(eng.fader_slots[1].scene_name[0] == '\0');
	REQUIRE(eng.fader_slots[1].resolved_scene_name[0] == '\0');

	/* Re-evaluation never carries a resolved scene over to a job that does not use one */
	snprintf(eng.fader_slots[1].resolved_scene_name, sizeof(eng.fader_slots[1].resolved_scene_name), "%s", "Main");
	mfd_engine_reevaluate_slots(&eng, false);
	REQUIRE(eng.fader_slots[1].resolved_scene_name[0] == '\0');

	/*
	 * 13. 0.1.3 Bottom-Rail 6% Hysteresis Band (R-TBAR-SM / D-8):
	 *     - 13a: CC 3,4,3,4,3,4 five times (30 steps) on T-bar from Idle -> 0 driver calls, 0 writes,
	 *            0 completions, 0 aborts, stays Idle.
	 *     - 13b: CC 0 -> 8 -> 3 across ticks AND in a single tick -> 1 start (Throwing) and 1 cancel (Idle).
	 *     - 13c: Reversed polarity CC 124,123 five times from Idle -> 0 transitions; 127 -> 119 -> 124 -> 1 start & 1 cancel.
	 */
	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 30.0f);
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	g_mock_tbar_calls = 0;
	g_mock_tbar_set_writes = 0;
	g_mock_tbar_completions = 0;
	g_mock_tbar_aborts = 0;
	g_mock_tbar_latched = false;
	g_mock_tbar_last_pos = 0;

	for (int rep = 0; rep < 5; ++rep) {
		const uint8_t osc34[] = {3, 4, 3, 4, 3, 4};
		for (size_t oi = 0U; oi < sizeof(osc34) / sizeof(osc34[0]); ++oi) {
			push_midi_cc(&eng, 0, 15, osc34[oi]);
			mfd_engine_tick(&eng, 0.016f);
		}
	}
	REQUIRE(g_mock_tbar_calls == 0);
	REQUIRE(g_mock_tbar_set_writes == 0);
	REQUIRE(g_mock_tbar_completions == 0);
	REQUIRE(g_mock_tbar_aborts == 0);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);

	/* 13b-1: CC 0 -> 8 -> 3 across ticks */
	push_midi_cc(&eng, 0, 15, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	REQUIRE(g_mock_tbar_set_writes == 0);

	push_midi_cc(&eng, 0, 15, 8);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_THROWING);
	REQUIRE(g_mock_tbar_set_writes >= 1);
	REQUIRE(g_mock_tbar_aborts == 0);
	REQUIRE(g_mock_tbar_completions == 0);

	/* Retreat into (0.03, 0.06] deadband (CC 4) stays Throwing without aborting */
	push_midi_cc(&eng, 0, 15, 4);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_THROWING);
	REQUIRE(g_mock_tbar_aborts == 0);

	push_midi_cc(&eng, 0, 15, 3);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 0);

	/* 13b-2: CC 0 -> 8 -> 3 queued inside a single mfd_engine_tick */
	g_mock_tbar_set_writes = 0;
	g_mock_tbar_aborts = 0;
	g_mock_tbar_completions = 0;
	push_midi_cc(&eng, 0, 15, 0);
	push_midi_cc(&eng, 0, 15, 8);
	push_midi_cc(&eng, 0, 15, 3);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	REQUIRE(g_mock_tbar_set_writes == 2); /* pos=1 on start at CC 8, pos=0 on cancel at CC 3 */
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 0);

	/* 13c: Reversed polarity upper-rail 6% start band (CC 124,123 x5 = 0 transitions; 127 -> 119 -> 124 = 1 start & 1 cancel) */
	eng.fader_slots[0].takeover.polarity_reversed = true;
	eng.fader_slots[0].takeover.rescale_origin = 1.0f;
	g_mock_tbar_calls = 0;
	g_mock_tbar_set_writes = 0;
	g_mock_tbar_aborts = 0;
	g_mock_tbar_completions = 0;
	for (int rep = 0; rep < 5; ++rep) {
		const uint8_t osc_top[] = {124, 123, 124, 123, 124, 123};
		for (size_t oi = 0U; oi < sizeof(osc_top) / sizeof(osc_top[0]); ++oi) {
			push_midi_cc(&eng, 0, 15, osc_top[oi]);
			mfd_engine_tick(&eng, 0.016f);
		}
	}
	REQUIRE(g_mock_tbar_calls == 0);
	REQUIRE(g_mock_tbar_set_writes == 0);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);

	push_midi_cc(&eng, 0, 15, 127);
	push_midi_cc(&eng, 0, 15, 119);
	push_midi_cc(&eng, 0, 15, 124);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	REQUIRE(g_mock_tbar_set_writes == 2);
	REQUIRE(g_mock_tbar_aborts == 1);
	REQUIRE(g_mock_tbar_completions == 0);

	/*
	 * 14. 0.1.3 Per-Mode Preconditions, Canonical Status Sentences, Event-Driven Re-evaluation & Stack-Order Inversion:
	 *     - 14a: Every canonical status sentence + zero OBS writes while !available with live last_raw_norm movement:
	 *            "Not used. Click to set up.", "Studio Mode is off.", "<Layer> is no longer in scene <Scene>.",
	 *            "Scene <Scene> was deleted.", "<A> and <B> are not in the same scene.",
	 *            "Left and Right cannot be the same layer.", "<Layer> is not in any scene.", "<Source> has no audio."
	 *     - 14b: source_rename follows both layer and scene renames with available == true, reason == "", no error.
	 *     - 14c: Stack-order detection & inversion: Left ("Color") below Right ("Color 2") vs reordered ("Color" on top
	 *            of "Color 2") both show 100% Left at 0.0 and 100% Right at 1.0, and live reorder immediately swaps
	 *            applied opacities without moving the fader.
	 */
	memset(&fake_sg, 0, sizeof(fake_sg));
	fake_sg_add_source(&fake_sg, "Color", false);
	fake_sg_add_source(&fake_sg, "Color 2", false);
	fake_sg_add_source(&fake_sg, "Mic/Aux", true);
	fake_sg_add_source(&fake_sg, "Silent Card", false);
	fake_sg_add_scene(&fake_sg, "Red");
	fake_sg_scene_add_item(&fake_sg, "Red", "Color");   /* index 0 (bottom) */
	fake_sg_scene_add_item(&fake_sg, "Red", "Color 2"); /* index 1 (top) */
	fake_sg_add_scene(&fake_sg, "Blue");

	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);

	/* 14a-1: Unassigned slot -> "Not used. Click to set up." */
	bool st_avail = true;
	char st_reason[160] = {0};
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[1], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(!st_avail);
	REQUIRE(strcmp(st_reason, "Not used. Click to set up.") == 0);

	/* 14a-2: T-bar while Studio Mode is off -> "Studio Mode is off.", 0 T-bar writes on MIDI & API, live last_raw_norm */
	eng.studio_mode_override = false;
	mfd_engine_reevaluate_slots(&eng, true);
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[0], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(!st_avail);
	REQUIRE(strcmp(st_reason, "Studio Mode is off.") == 0);
	g_mock_tbar_calls = 0;
	uint32_t tbar_writes_off = eng.fader_slots[0].output_write_count;
	push_midi_cc(&eng, 0, 15, 64);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[0].last_raw_norm - (64.0f / 127.0f)) < 1e-5f);
	REQUIRE(g_mock_tbar_calls == 0);
	REQUIRE(eng.fader_slots[0].output_write_count == tbar_writes_off);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.5);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(g_mock_tbar_calls == 0);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	eng.studio_mode_override = true;
	mfd_engine_reevaluate_slots(&eng, true);

	/* 14a-3 & 14c: crossfade_ab on fader_1 (scene "Red", Left = "Color" [0], Right = "Color 2" [1]) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "scene", "Red");
	obs_data_set_string(req, "source_a", "Color");
	obs_data_set_string(req, "source_b", "Color 2");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "") == 0);
	REQUIRE(!eng.fader_slots[1].a_is_above_b);
	obs_data_release(req);
	obs_data_release(res);

	struct fake_sg_source *src_color = fake_sg_find_source(&fake_sg, "Color");
	struct fake_sg_source *src_color2 = fake_sg_find_source(&fake_sg, "Color 2");
	REQUIRE(src_color != NULL && src_color2 != NULL);

	/* Normal order ("Color" below "Color 2"): Left (0.0) -> Color=1, Color2=0; Right (1.0) -> Color=1, Color2=1; 0.25 -> Color=1, Color2=0.25 */
	eng.fader_slots[1].takeover.state = MFD_TAKEOVER_ENGAGED;
	push_midi_cc(&eng, 0, 7, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(src_color->last_opacity - 1.0f) < 1e-5f);
	REQUIRE(fabsf(src_color2->last_opacity - 0.0f) < 1e-5f);

	push_midi_cc(&eng, 0, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(src_color->last_opacity - 1.0f) < 1e-5f);
	REQUIRE(fabsf(src_color2->last_opacity - 1.0f) < 1e-5f);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.25);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(fabsf(src_color->last_opacity - 1.0f) < 1e-5f);
	REQUIRE(fabsf(src_color2->last_opacity - 0.25f) < 1e-5f);

	/* 14c: Reorder scene "Red" so "Color" is ON TOP (index 1) and "Color 2" is BELOW (index 0) */
	fake_sg_scene_remove_item(&fake_sg, "Red", "Color");
	fake_sg_scene_add_item(&fake_sg, "Red", "Color"); /* now Color2=[0], Color=[1] */
	int st_before_reorder = ws_log.state_changed_count;
	mfd_engine_reevaluate_slots(&eng, true);
	REQUIRE(ws_log.state_changed_count == st_before_reorder + 1);
	REQUIRE(eng.fader_slots[1].a_is_above_b);
	/* At t = 0.25 with "Color" on top: lower "Color 2" = 1.0, upper "Color" = 1 - 0.25 = 0.75 (75% Color / 25% Color 2!) */
	REQUIRE(fabsf(src_color2->last_opacity - 1.0f) < 1e-5f);
	REQUIRE(fabsf(src_color->last_opacity - 0.75f) < 1e-5f);

	/* Sweep Left (0.0) with "Color" on top -> Color=1.0, Color2=1.0 (100% Color visible); Right (1.0) -> Color=0.0, Color2=1.0 (100% Color 2 visible) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.0);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(fabsf(src_color->last_opacity - 1.0f) < 1e-5f);
	REQUIRE(fabsf(src_color2->last_opacity - 1.0f) < 1e-5f);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 1.0);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(fabsf(src_color->last_opacity - 0.0f) < 1e-5f);
	REQUIRE(fabsf(src_color2->last_opacity - 1.0f) < 1e-5f);

	/* 14a-4: Remove "Color 2" from scene "Red" -> "Color 2 is no longer in scene Red.", 0 writes while bar moves */
	fake_sg_scene_remove_item(&fake_sg, "Red", "Color 2");
	int st_before_rm = ws_log.state_changed_count;
	mfd_engine_reevaluate_slots(&eng, true);
	REQUIRE(ws_log.state_changed_count == st_before_rm + 1);
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[1], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(!st_avail);
	REQUIRE(strcmp(st_reason, "Color 2 is no longer in scene Red.") == 0);

	int op_writes_before = fake_sg.total_opacity_writes;
	uint32_t slot1_writes_before = eng.fader_slots[1].output_write_count;
	push_midi_cc(&eng, 0, 7, 51); /* 51/127 = 0.40157 */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_raw_norm - (51.0f / 127.0f)) < 1e-5f);
	REQUIRE(fake_sg.total_opacity_writes == op_writes_before);
	REQUIRE(eng.fader_slots[1].output_write_count == slot1_writes_before);

	/* Re-add "Color 2" to scene "Red" -> immediately available again with empty reason */
	fake_sg_scene_add_item(&fake_sg, "Red", "Color 2");
	mfd_engine_reevaluate_slots(&eng, true);
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[1], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(st_avail);
	REQUIRE(strcmp(st_reason, "") == 0);

	/* 14b: source_rename on layer ("Color 2" -> "Color 2 HD") and scene ("Red" -> "Red Live") -> follows with no error */
	fake_sg_rename(&fake_sg, "Color 2", "Color 2 HD");
	mfd_engine_handle_source_rename(&eng, "Color 2", "Color 2 HD");
	REQUIRE(strcmp(eng.fader_slots[1].source_b_name, "Color 2 HD") == 0);
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[1], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(st_avail);
	REQUIRE(strcmp(st_reason, "") == 0);

	fake_sg_rename(&fake_sg, "Red", "Red Live");
	mfd_engine_handle_source_rename(&eng, "Red", "Red Live");
	REQUIRE(strcmp(eng.fader_slots[1].scene_name, "Red Live") == 0);
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[1], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(st_avail);
	REQUIRE(strcmp(st_reason, "") == 0);

	/* Rename back to "Red" & "Color 2" and test scene deletion -> "Scene Red was deleted." */
	fake_sg_rename(&fake_sg, "Red Live", "Red");
	mfd_engine_handle_source_rename(&eng, "Red Live", "Red");
	fake_sg_rename(&fake_sg, "Color 2 HD", "Color 2");
	mfd_engine_handle_source_rename(&eng, "Color 2 HD", "Color 2");
	struct fake_sg_scene *sc_red = fake_sg_find_scene(&fake_sg, "Red");
	REQUIRE(sc_red != NULL);
	sc_red->in_use = false;
	mfd_engine_reevaluate_slots(&eng, true);
	mfd_engine_get_slot_status(&eng, &eng.fader_slots[1], NULL, 0U, &st_avail, st_reason, sizeof(st_reason));
	REQUIRE(!st_avail);
	REQUIRE(strcmp(st_reason, "Scene Red was deleted.") == 0);

	/* 14a-5: Scene-agnostic crossfade_ab when layers are in different scenes -> "Color and Color 2 are not in the same scene." */
	fake_sg_add_scene(&fake_sg, "Red");
	fake_sg_scene_add_item(&fake_sg, "Red", "Color");
	fake_sg_scene_add_item(&fake_sg, "Blue", "Color 2");
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "scene", "");
	obs_data_set_string(req, "source_a", "Color");
	obs_data_set_string(req, "source_b", "Color 2");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "Color and Color 2 are not in the same scene.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* Duplicate layer on Left and Right -> "Left and Right cannot be the same layer." */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "source_a", "Color");
	obs_data_set_string(req, "source_b", "Color");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "Left and Right cannot be the same layer.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* 14a-6: layer_opacity when layer is removed from all scenes -> "Color is not in any scene." */
	fake_sg_scene_remove_item(&fake_sg, "Red", "Color");
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_2");
	obs_data_set_string(req, "target_mode", "layer_opacity");
	obs_data_set_string(req, "source_a", "Color");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "Color is not in any scene.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* 14a-7: audio_volume on a source with no audio -> "Silent Card has no audio.", 0 volume writes while bar moves */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_3");
	obs_data_set_string(req, "target_mode", "audio_volume");
	obs_data_set_string(req, "source_a", "Silent Card");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "Silent Card has no audio.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	int vol_writes_before = fake_sg.total_volume_writes;
	push_midi_cc(&eng, 2, 7, 100);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_raw_norm - (100.0f / 127.0f)) < 1e-5f);
	REQUIRE(fake_sg.total_volume_writes == vol_writes_before);

	/*
	 * 15. 0.1.3 Exact Calibrated Output Readout & Mode-Switch Reset (R-FEEL-2 / D-12):
	 *     - Reproduce the exact 0.1.2 bug: park crossfader in T-bar mode at CC 127, unpark or park at bottom
	 *       with a non-identity rescale (or switch from parked/rescaled two-rail mode), leave physical fader at
	 *       CC 51 (40%), then switch crossfader mode to crossfade_ab!
	 *     - Assert: switching mode resets takeover cleanly to Idle (WAITING, parked=false, polarity_reversed=false,
	 *       lower_rescale_origin=0.0, upper_rescale_origin=1.0, tbar_state=MFD_TBAR_IDLE), preserves last_raw_norm
	 *       at 51/127 (40%), and once picked up and moved back to CC 51, BOTH calibrated readout AND applied mix
	 *       equal 51/127 (40%) with zero carried-over rescale!
	 */
	fake_sg_scene_add_item(&fake_sg, "Blue", "Color"); /* Blue now has both Color and Color 2 */
	mfd_midi_parser_init(&eng.midi_parser);
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);
	eng.studio_mode_override = true;
	mfd_engine_reevaluate_slots(&eng, false);

	/* Complete a T-bar throw 0 -> 127 (parked at top rail), unpark at CC 114 (reversed polarity, rescale_origin ~0.8976),
	 * move to CC 51 (40% physical, but ~70% reversed throw!) */
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	push_midi_cc(&eng, 0, 15, 0);
	push_midi_cc(&eng, 0, 15, 64);
	push_midi_cc(&eng, 0, 15, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	push_midi_cc(&eng, 0, 15, 114);
	push_midi_cc(&eng, 0, 15, 51);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_THROWING);
	REQUIRE(eng.fader_slots[0].takeover.polarity_reversed);
	REQUIRE(mfd_calibrated_percent(eng.fader_slots[0].last_raw_norm) == 40);

	/* Now switch crossfader mode from tbar -> crossfade_ab ("Color" <-> "Color 2" in scene "Blue") */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "scene", "Blue");
	obs_data_set_string(req, "source_a", "Color");
	obs_data_set_string(req, "source_b", "Color 2");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "available"));
	REQUIRE(!obs_data_get_bool(res, "parked"));
	REQUIRE(strcmp(obs_data_get_string(res, "takeover_pending"), "down") == 0);
	obs_data_release(req);
	obs_data_release(res);

	REQUIRE(eng.fader_slots[0].tbar_state == MFD_TBAR_IDLE);
	REQUIRE(eng.fader_slots[0].takeover.state == MFD_TAKEOVER_WAITING);
	REQUIRE(!eng.fader_slots[0].takeover.polarity_reversed);
	REQUIRE(fabsf(eng.fader_slots[0].takeover.lower_rescale_origin - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[0].takeover.upper_rescale_origin - 1.0f) < 1e-6f);
	REQUIRE(mfd_calibrated_percent(eng.fader_slots[0].last_raw_norm) == 40);

	/* Pick up 0% at CC 0 and move to CC 51 -> applied mix and calibrated readout are BOTH exact 51/127 (40%, NOT 33%!) */
	push_midi_cc(&eng, 0, 15, 0);
	mfd_engine_tick(&eng, 0.016f);
	push_midi_cc(&eng, 0, 15, 51);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[0].takeover.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(fabsf(eng.fader_slots[0].last_raw_norm - (51.0f / 127.0f)) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[0].last_applied_val - (51.0f / 127.0f)) < 1e-6f);
	REQUIRE(mfd_calibrated_percent(eng.fader_slots[0].last_raw_norm) == 40);
	REQUIRE(mfd_calibrated_percent(eng.fader_slots[0].last_applied_val) == 40);

	/*
	 * 16. 0.2.0 Step 1 Engine Contracts:
	 *     - 16a: Deleted audio source ("Mic/Aux was deleted.") vs silent source ("Silent Card has no audio.")
	 *     - 16b: Heap-allocated atomic UI catalog & snapshot sorting/stacking/dB units
	 *     - 16c: Setup session broadcast isolation, candidate_invert, [Not used] address preservation,
	 *            Learn mutual exclusion & multi-slot collision transfer
	 *     - 16d: slot_config_rev concurrent-edit conflict detection (set_slot, rename, import, load_profile)
	 *            & mid-setup precondition invalidation on commit
	 *     - 16e: First-Run Offer discriminator (clean vs 0.1.3 upgrade JSON) + [Yes] / [I'll set it up]
	 *     - 16f: Bounded 4-mode candidate-validator equivalence matrix (validate == true iff committed available == true)
	 *     - 16g: Multi-threaded recursive state_mutex stress test + mfd_vendor_api_shutdown in-flight barrier
	 */
	fake_sg.owner_eng = &eng;

	/* 16a: Deleted audio source vs silent source */
	fake_sg_add_source(&fake_sg, "Mic/Aux", true);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_3");
	obs_data_set_string(req, "target_mode", "audio_volume");
	obs_data_set_string(req, "source_a", "Mic/Aux");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "") == 0);
	obs_data_release(req);
	obs_data_release(res);

	fake_sg_remove_source(&fake_sg, "Mic/Aux");
	mfd_engine_reevaluate_slots(&eng, true);
	{
		bool avail_del = true;
		char rsn_del[160] = {0};
		mfd_engine_get_slot_status(&eng, &eng.fader_slots[3], NULL, 0U, &avail_del, rsn_del, sizeof(rsn_del));
		REQUIRE(!avail_del);
		REQUIRE(strcmp(rsn_del, "Mic/Aux was deleted.") == 0);
	}
	vol_writes_before = fake_sg.total_volume_writes;
	push_midi_cc(&eng, 2, 7, 77);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_raw_norm - (77.0f / 127.0f)) < 1e-5f);
	REQUIRE(fake_sg.total_volume_writes == vol_writes_before);

	/* 16b: Heap-allocated atomic UI catalog & snapshot */
	fake_sg_add_source(&fake_sg, "Mic/Aux", true);
	fake_sg_add_source(&fake_sg, "desktop audio", true);
	fake_sg_find_source(&fake_sg, "desktop audio")->last_volume_mul = 0.125f; /* d=0.5 -> -18.06 dB */
	fake_sg_add_scene(&fake_sg, "alpha Scene");
	fake_sg_scene_add_item(&fake_sg, "alpha Scene", "Color 2"); /* bottom in alpha Scene */
	fake_sg_scene_add_item(&fake_sg, "alpha Scene", "Mic/Aux"); /* top in alpha Scene (has audio) */
	fake_sg_scene_add_item(&fake_sg, "alpha Scene", "Color 2"); /* duplicate scene item -> deduplicated */

	struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
	REQUIRE(cat != NULL);
	mfd_engine_get_ui_catalog(&eng, cat);
	REQUIRE(!cat->scenes_truncated);
	REQUIRE(!cat->audio_truncated);
	REQUIRE(cat->scene_count >= 2U);
	/* Case-insensitive sort: "alpha Scene" comes before "Blue" */
	REQUIRE(strcmp(cat->scenes[0].name, "alpha Scene") == 0);
	REQUIRE(cat->scenes[0].layer_count == 2U);
	REQUIRE(strcmp(cat->scenes[0].layers[0], "Color 2") == 0);
	REQUIRE(!cat->scenes[0].layer_has_audio[0]);
	REQUIRE(strcmp(cat->scenes[0].layers[1], "Mic/Aux") == 0);
	REQUIRE(cat->scenes[0].layer_has_audio[1]);
	/* Audio catalog contains only audio-capable sources, sorted case-insensitively, in dB */
	bool saw_silent_in_audio = false;
	bool saw_desktop_db = false;
	for (size_t ai = 0U; ai < cat->audio_count; ++ai) {
		if (strcmp(cat->audio_sources[ai].name, "Silent Card") == 0) {
			saw_silent_in_audio = true;
		}
		if (strcmp(cat->audio_sources[ai].name, "desktop audio") == 0) {
			REQUIRE(fabsf(cat->audio_sources[ai].db - mfd_deflection_to_db(0.5f)) < 0.05f);
			saw_desktop_db = true;
		}
	}
	REQUIRE(!saw_silent_in_audio);
	REQUIRE(saw_desktop_db);

	/* 16c: Setup session broadcast isolation, candidate_invert, [Not used] address preservation,
	 *      Learn mutual exclusion & multi-slot collision transfer */
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "scene", "Blue");
	obs_data_set_string(req, "source_a", "Color");
	obs_data_set_string(req, "source_b", "Color 2");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "available"));
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_2");
	obs_data_set_string(req, "target_mode", "audio_volume");
	obs_data_set_string(req, "source_a", "Mic/Aux");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "available"));
	obs_data_release(req);
	obs_data_release(res);

	/* Start an auto-fade on fader_1, then open Set up on fader_1 -> auto_fade stops and live writes freeze */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 500);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(eng.fader_slots[1].auto_fade_active);

	mfd_engine_begin_setup_session(&eng, 1);
	REQUIRE(eng.setup_session.active);
	REQUIRE(eng.setup_session.slot_index == 1);
	REQUIRE(!eng.fader_slots[1].auto_fade_active);

	int op_before_setup = fake_sg.total_opacity_writes;
	int vol_before_setup = fake_sg.total_volume_writes;
	/* Move fader_1 CC (Ch 1 idx 0 -> Ch 0 CC 7 for fader_1 in 0-based APC40 profile) */
	push_midi_cc(&eng, eng.fader_slots[1].addr.channel, eng.fader_slots[1].addr.number, 100);
	/* Move fader_2 CC simultaneously -> fader_2 MUST continue driving live audio! */
	push_midi_cc(&eng, eng.fader_slots[2].addr.channel, eng.fader_slots[2].addr.number, 127);
	push_midi_cc(&eng, eng.fader_slots[2].addr.channel, eng.fader_slots[2].addr.number, 64);
	mfd_engine_tick(&eng, 0.050f);

	REQUIRE(fake_sg.total_opacity_writes == op_before_setup);
	REQUIRE(fake_sg.total_volume_writes > vol_before_setup);
	REQUIRE(eng.setup_session.has_candidate_input);
	REQUIRE(fabsf(eng.setup_session.candidate_raw_norm - (100.0f / 127.0f)) < 1e-5f);

	/* Flipping candidate_invert flips Step 3 live meter immediately without writing to OBS */
	mfd_engine_set_setup_invert(&eng, true);
	REQUIRE(eng.setup_session.candidate_invert);
	REQUIRE(fabsf(eng.setup_session.candidate_raw_norm - (1.0f - (100.0f / 127.0f))) < 1e-5f);
	REQUIRE(fake_sg.total_opacity_writes == op_before_setup);

	/* Vendor set_fader_value / nudge_fader / auto_fade and bridge proc_set_fader_value also respect broadcast pause */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.75);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(fake_sg.total_opacity_writes == op_before_setup);

	{
		proc_handler_t *s16_ph = proc_handler_create();
		signal_handler_t *s16_sh = signal_handler_create();
		REQUIRE(s16_ph != NULL && s16_sh != NULL);
		REQUIRE(mfd_bridge_register_with_handlers(s16_ph, s16_sh, &eng));
		struct calldata pause_cd;
		calldata_init(&pause_cd);
		calldata_set_string(&pause_cd, "slot", "fader_1");
		calldata_set_float(&pause_cd, "value", 0.5);
		REQUIRE(proc_handler_call(s16_ph, "midi_fadester_set_fader_value", &pause_cd));
		REQUIRE(!calldata_bool(&pause_cd, "ok"));
		calldata_free(&pause_cd);
		mfd_bridge_register_with_handlers(NULL, NULL, NULL);
		signal_handler_destroy(s16_sh);
		proc_handler_destroy(s16_ph);
		REQUIRE(fake_sg.total_opacity_writes == op_before_setup);
	}

	/* Step 1 [Not used] preserves APC40 hardware CC binding while clearing assignment */
	struct mfd_midi_addr f1_orig_addr = eng.fader_slots[1].addr;
	REQUIRE(f1_orig_addr.type == MFD_MIDI_CC);
	REQUIRE(mfd_engine_unassign_slot_preserve_addr(&eng, 1));
	REQUIRE(!eng.setup_session.active);
	REQUIRE(eng.fader_slots[1].enabled);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	REQUIRE(eng.fader_slots[1].addr.type == f1_orig_addr.type);
	REQUIRE(eng.fader_slots[1].addr.channel == f1_orig_addr.channel);
	REQUIRE(eng.fader_slots[1].addr.number == f1_orig_addr.number);
	push_midi_cc(&eng, f1_orig_addr.channel, f1_orig_addr.number, 32);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_raw_norm - (32.0f / 127.0f)) < 1e-5f);

	/* Learn mutual exclusion + multi-slot collision transfer on Done */
	eng.fader_slots[4].addr.type = MFD_MIDI_CC;
	eng.fader_slots[4].addr.channel = 1;
	eng.fader_slots[4].addr.number = 77;
	eng.fader_slots[4].addr.is_14bit = false;
	eng.fader_slots[5].addr = eng.fader_slots[4].addr;
	uint64_t rev4_before = eng.slot_config_rev[4];
	uint64_t rev5_before = eng.slot_config_rev[5];

	mfd_engine_start_learn(&eng, 7);
	REQUIRE(eng.learning_slot_index == 7);
	mfd_engine_begin_setup_session(&eng, 1);
	REQUIRE(eng.learning_slot_index == -1);
	mfd_engine_start_setup_learn(&eng);
	REQUIRE(eng.setup_session.learning);

	push_midi_cc(&eng, 1, 77, 90);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!eng.setup_session.learning);
	REQUIRE(eng.setup_session.candidate_addr.type == MFD_MIDI_CC);
	REQUIRE(eng.setup_session.candidate_addr.channel == 1);
	REQUIRE(eng.setup_session.candidate_addr.number == 77);

	int owners[MFD_MAX_FADER_SLOTS] = {0};
	size_t owner_cnt =
		mfd_engine_find_addr_owners(&eng, &eng.setup_session.candidate_addr, 1, owners, MFD_MAX_FADER_SLOTS);
	REQUIRE(owner_cnt == 2U);
	REQUIRE(owners[0] == 4 && owners[1] == 5);

	struct mfd_setup_candidate cand;
	memset(&cand, 0, sizeof(cand));
	cand.slot_index = 1;
	cand.enabled = true;
	cand.target_type = MFD_TARGET_TWO_SOURCE_MIX;
	snprintf(cand.scene_name, sizeof(cand.scene_name), "Blue");
	snprintf(cand.source_a_name, sizeof(cand.source_a_name), "Color");
	snprintf(cand.source_b_name, sizeof(cand.source_b_name), "Color 2");
	cand.curve = MFD_CURVE_DIP_FREE;
	char commit_err[192] = {0};
	REQUIRE(mfd_engine_commit_setup_session(&eng, &cand, commit_err, sizeof(commit_err)));
	REQUIRE(!eng.setup_session.active);
	REQUIRE(eng.fader_slots[1].addr.channel == 1 && eng.fader_slots[1].addr.number == 77);
	REQUIRE(eng.fader_slots[4].addr.type == MFD_MIDI_NONE);
	REQUIRE(eng.fader_slots[5].addr.type == MFD_MIDI_NONE);
	REQUIRE(eng.slot_config_rev[4] > rev4_before);
	REQUIRE(eng.slot_config_rev[5] > rev5_before);

	/* 16d: slot_config_rev conflict detection (set_slot, rename, import, load_profile)
	 *      & mid-setup precondition invalidation on commit */
	mfd_engine_begin_setup_session(&eng, 1);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "curve", "linear");
	mfd_vendor_handle_set_slot(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);

	REQUIRE(!mfd_engine_commit_setup_session(&eng, &cand, commit_err, sizeof(commit_err)));
	REQUIRE(strstr(commit_err, "changed externally") != NULL);
	/* base_slot_rev refreshed on first conflict notice, so pressing Done a second time succeeds */
	REQUIRE(mfd_engine_commit_setup_session(&eng, &cand, commit_err, sizeof(commit_err)));

	/* Rename bumps slot_config_rev on affected slot */
	mfd_engine_begin_setup_session(&eng, 1);
	fake_sg_rename(&fake_sg, "Color 2", "Color Two");
	mfd_engine_handle_source_rename(&eng, "Color 2", "Color Two");
	snprintf(cand.source_b_name, sizeof(cand.source_b_name), "Color Two");
	REQUIRE(!mfd_engine_commit_setup_session(&eng, &cand, commit_err, sizeof(commit_err)));
	REQUIRE(strstr(commit_err, "changed externally") != NULL);
	fake_sg_rename(&fake_sg, "Color Two", "Color 2");
	mfd_engine_handle_source_rename(&eng, "Color Two", "Color 2");
	snprintf(cand.source_b_name, sizeof(cand.source_b_name), "Color 2");
	mfd_engine_cancel_setup_session(&eng);

	/* load_profile bumps slot_config_rev on all slots */
	mfd_engine_begin_setup_session(&eng, 1);
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "name", "apc40mk2");
	mfd_vendor_handle_load_profile(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!mfd_engine_commit_setup_session(&eng, &cand, commit_err, sizeof(commit_err)));
	REQUIRE(strstr(commit_err, "changed externally") != NULL);
	mfd_engine_cancel_setup_session(&eng);

	/* Mid-setup precondition invalidation: layer removed from scene before pressing Done */
	mfd_engine_begin_setup_session(&eng, 2);
	cand.slot_index = 2;
	REQUIRE(mfd_engine_validate_setup_candidate(&eng, &cand, commit_err, sizeof(commit_err)));
	fake_sg_scene_remove_item(&fake_sg, "Blue", "Color 2");
	mfd_engine_reevaluate_slots(&eng, true);
	REQUIRE(!mfd_engine_commit_setup_session(&eng, &cand, commit_err, sizeof(commit_err)));
	REQUIRE(strcmp(commit_err, "Color 2 is no longer in scene Blue.") == 0);
	mfd_engine_cancel_setup_session(&eng);
	fake_sg_scene_add_item(&fake_sg, "Blue", "Color 2");
	mfd_engine_reevaluate_slots(&eng, true);

	/* 16e: First-Run Offer discriminator (clean vs 0.1.3 upgrade JSON) + [Yes] / [I'll set it up] */
	mfd_engine_load_apc40_defaults(&eng);
	eng.first_run_offer_dismissed = false;
	mfd_engine_set_test_frontend_state(&eng, true, true, "alpha Scene", "Blue", true);

	struct mfd_ui_snapshot *snap = (struct mfd_ui_snapshot *)calloc(1U, sizeof(*snap));
	REQUIRE(snap != NULL);
	mfd_engine_get_ui_snapshot(&eng, snap);
	REQUIRE(snap->first_run_offer_pending);

	/* [Yes]: Crossfader = Switch scenes (T-bar), Master = Volume on "desktop audio" (case-preserved) */
	mfd_engine_accept_first_run_usual_layout(&eng);
	mfd_engine_get_ui_snapshot(&eng, snap);
	REQUIRE(!snap->first_run_offer_pending);
	REQUIRE(snap->slots[0].target_type == MFD_TARGET_TBAR);
	REQUIRE(snap->slots[9].target_type == MFD_TARGET_AUDIO_VOLUME);
	REQUIRE(strcmp(snap->slots[9].source_a_name, "desktop audio") == 0);
	for (int fi = 1; fi <= 8; ++fi) {
		REQUIRE(snap->slots[fi].enabled);
		REQUIRE(snap->slots[fi].addr.type == MFD_MIDI_CC);
		REQUIRE(snap->slots[fi].target_type == MFD_TARGET_UNASSIGNED);
	}

	/* [I'll set it up]: all 10 APC40 addresses stay bound & live, all 10 modes = MFD_TARGET_UNASSIGNED */
	eng.first_run_offer_dismissed = false;
	mfd_engine_decline_first_run_usual_layout(&eng);
	mfd_engine_get_ui_snapshot(&eng, snap);
	REQUIRE(!snap->first_run_offer_pending);
	for (int fi = 0; fi < MFD_MAX_FADER_SLOTS; ++fi) {
		REQUIRE(snap->slots[fi].enabled);
		REQUIRE(snap->slots[fi].addr.type == MFD_MIDI_CC);
		REQUIRE(snap->slots[fi].target_type == MFD_TARGET_UNASSIGNED);
	}

	/* 0.1.3 upgrade global.json (missing "first_run_offer_dismissed") vs 0.2.0 clean config */
	{
		const char *tmp_v013_json = "test_upgrade_013_global.json";
		const char *tmp_v020_json = "test_clean_020_global.json";
		FILE *fp = fopen(tmp_v013_json, "wb");
		REQUIRE(fp != NULL);
		fputs("{\"device_name\":\"APC40 mkII\",\"active_profile\":\"apc40mk2\",\"smooth_tau_ms\":16.0}\n", fp);
		fclose(fp);

		eng.first_run_offer_dismissed = false;
		eng.stream_deck_companion_dismissed = true;
		mfd_engine_load_apc40_defaults(&eng);
		REQUIRE(mfd_engine_import_config_file(&eng, tmp_v013_json));
		REQUIRE(eng.first_run_offer_dismissed);
		REQUIRE(!eng.stream_deck_companion_dismissed);
		mfd_engine_get_ui_snapshot(&eng, snap);
		REQUIRE(!snap->first_run_offer_pending);
		os_unlink(tmp_v013_json);

		eng.first_run_offer_dismissed = false;
		mfd_engine_load_apc40_defaults(&eng);
		REQUIRE(mfd_engine_export_config_file(&eng, tmp_v020_json));
		eng.first_run_offer_dismissed = true;
		/* Also verify import bumps slot_config_rev on an active setup session */
		mfd_engine_begin_setup_session(&eng, 0);
		uint64_t rev0_pre_imp = eng.slot_config_rev[0];
		REQUIRE(mfd_engine_import_config_file(&eng, tmp_v020_json));
		REQUIRE(eng.slot_config_rev[0] > rev0_pre_imp);
		REQUIRE(!eng.first_run_offer_dismissed);
		mfd_engine_cancel_setup_session(&eng);
		os_unlink(tmp_v020_json);
	}

	/* 16f: Bounded 4-mode candidate-validator equivalence matrix (MIDI-FADESTER-UI.md Property Test)
	 *      Across Studio Mode {off, on}, scenes (0 layers, 1 layer, 2 layers, missing scene),
	 *      layer pairs (same, distinct, missing), and sources (audio, silent, missing):
	 *      1) mfd_engine_validate_setup_candidate(cand) == true IFF committing cand yields available == true & reason == "".
	 *      2) Every picker-filtered catalog choice yields validate == true and available == true. */
	fake_sg_add_scene(&fake_sg, "EmptyScene");
	fake_sg_add_scene(&fake_sg, "OneLayerScene");
	fake_sg_scene_add_item(&fake_sg, "OneLayerScene", "Color");

	const char *test_scenes[] = {"", "EmptyScene", "OneLayerScene", "Blue", "GhostScene"};
	const char *test_sources[] = {"", "Color", "Color 2", "Mic/Aux", "Silent Card", "GhostSource"};

	for (int sm = 0; sm <= 1; ++sm) {
		eng.studio_mode_override_set = true;
		eng.studio_mode_override = (sm != 0);
		mfd_engine_reevaluate_slots(&eng, false);

		/* Mode 1: Switch scenes (T-bar) */
		{
			struct mfd_setup_candidate c;
			memset(&c, 0, sizeof(c));
			c.slot_index = 1;
			c.enabled = true;
			c.target_type = MFD_TARGET_TBAR;
			char rsn[160] = {0};
			bool valid = mfd_engine_validate_setup_candidate(&eng, &c, rsn, sizeof(rsn));
			REQUIRE(valid == (sm != 0));
			if (valid) {
				mfd_engine_begin_setup_session(&eng, 1);
				REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
				REQUIRE(eng.fader_slots[1].cached_available);
				REQUIRE(eng.fader_slots[1].cached_reason[0] == '\0');
			} else {
				REQUIRE(strcmp(rsn, "Studio Mode is off.") == 0);
			}
		}

		/* Mode 2: Mix two layers */
		for (size_t si = 0U; si < sizeof(test_scenes) / sizeof(test_scenes[0]); ++si) {
			for (size_t ai = 0U; ai < sizeof(test_sources) / sizeof(test_sources[0]); ++ai) {
				for (size_t bi = 0U; bi < sizeof(test_sources) / sizeof(test_sources[0]); ++bi) {
					struct mfd_setup_candidate c;
					memset(&c, 0, sizeof(c));
					c.slot_index = 1;
					c.enabled = true;
					c.target_type = MFD_TARGET_TWO_SOURCE_MIX;
					c.curve = MFD_CURVE_DIP_FREE;
					snprintf(c.scene_name, sizeof(c.scene_name), "%s", test_scenes[si]);
					snprintf(c.source_a_name, sizeof(c.source_a_name), "%s", test_sources[ai]);
					snprintf(c.source_b_name, sizeof(c.source_b_name), "%s", test_sources[bi]);
					char rsn[160] = {0};
					bool valid = mfd_engine_validate_setup_candidate(&eng, &c, rsn, sizeof(rsn));
					if (valid) {
						mfd_engine_begin_setup_session(&eng, 1);
						REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
						REQUIRE(eng.fader_slots[1].cached_available);
						REQUIRE(eng.fader_slots[1].cached_reason[0] == '\0');
					} else {
						REQUIRE(rsn[0] != '\0');
						mfd_engine_begin_setup_session(&eng, 1);
						REQUIRE(!mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
						mfd_engine_cancel_setup_session(&eng);
					}
				}
			}
		}

		/* Mode 3: Show or hide a layer */
		for (size_t si = 0U; si < sizeof(test_scenes) / sizeof(test_scenes[0]); ++si) {
			for (size_t ai = 0U; ai < sizeof(test_sources) / sizeof(test_sources[0]); ++ai) {
				struct mfd_setup_candidate c;
				memset(&c, 0, sizeof(c));
				c.slot_index = 1;
				c.enabled = true;
				c.target_type = MFD_TARGET_SINGLE_LAYER;
				snprintf(c.scene_name, sizeof(c.scene_name), "%s", test_scenes[si]);
				snprintf(c.source_a_name, sizeof(c.source_a_name), "%s", test_sources[ai]);
				char rsn[160] = {0};
				bool valid = mfd_engine_validate_setup_candidate(&eng, &c, rsn, sizeof(rsn));
				if (valid) {
					mfd_engine_begin_setup_session(&eng, 1);
					REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
					REQUIRE(eng.fader_slots[1].cached_available);
					REQUIRE(eng.fader_slots[1].cached_reason[0] == '\0');
				} else {
					REQUIRE(rsn[0] != '\0');
				}
			}
		}

		/* Mode 4: Volume */
		for (size_t ai = 0U; ai < sizeof(test_sources) / sizeof(test_sources[0]); ++ai) {
			struct mfd_setup_candidate c;
			memset(&c, 0, sizeof(c));
			c.slot_index = 1;
			c.enabled = true;
			c.target_type = MFD_TARGET_AUDIO_VOLUME;
			snprintf(c.source_a_name, sizeof(c.source_a_name), "%s", test_sources[ai]);
			char rsn[160] = {0};
			bool valid = mfd_engine_validate_setup_candidate(&eng, &c, rsn, sizeof(rsn));
			if (valid) {
				mfd_engine_begin_setup_session(&eng, 1);
				REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
				REQUIRE(eng.fader_slots[1].cached_available);
				REQUIRE(eng.fader_slots[1].cached_reason[0] == '\0');
			} else {
				REQUIRE(rsn[0] != '\0');
			}
		}

		/* Exhaustive picker-filtered catalog traversal: every catalog-allowed combination MUST validate & commit! */
		mfd_engine_get_ui_catalog(&eng, cat);
		for (size_t ci = 0U; ci < cat->scene_count; ++ci) {
			const struct mfd_ui_scene_entry *se = &cat->scenes[ci];
			if (se->layer_count >= 1U) {
				for (size_t la = 0U; la < se->layer_count; ++la) {
					struct mfd_setup_candidate c;
					memset(&c, 0, sizeof(c));
					c.slot_index = 1;
					c.enabled = true;
					c.target_type = MFD_TARGET_SINGLE_LAYER;
					snprintf(c.scene_name, sizeof(c.scene_name), "%s", se->name);
					snprintf(c.source_a_name, sizeof(c.source_a_name), "%s", se->layers[la]);
					char rsn[160] = {0};
					REQUIRE(mfd_engine_validate_setup_candidate(&eng, &c, rsn, sizeof(rsn)));
					mfd_engine_begin_setup_session(&eng, 1);
					REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
					REQUIRE(eng.fader_slots[1].cached_available);
				}
			}
			if (se->layer_count >= 2U) {
				for (size_t la = 0U; la < se->layer_count; ++la) {
					for (size_t lb = 0U; lb < se->layer_count; ++lb) {
						if (la == lb) {
							continue;
						}
						struct mfd_setup_candidate c;
						memset(&c, 0, sizeof(c));
						c.slot_index = 1;
						c.enabled = true;
						c.target_type = MFD_TARGET_TWO_SOURCE_MIX;
						c.curve = MFD_CURVE_DIP_FREE;
						snprintf(c.scene_name, sizeof(c.scene_name), "%s", se->name);
						snprintf(c.source_a_name, sizeof(c.source_a_name), "%s",
							 se->layers[la]);
						snprintf(c.source_b_name, sizeof(c.source_b_name), "%s",
							 se->layers[lb]);
						char rsn[160] = {0};
						REQUIRE(mfd_engine_validate_setup_candidate(&eng, &c, rsn,
											    sizeof(rsn)));
						mfd_engine_begin_setup_session(&eng, 1);
						REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
						REQUIRE(eng.fader_slots[1].cached_available);
					}
				}
			}
		}
		for (size_t ai = 0U; ai < cat->audio_count; ++ai) {
			struct mfd_setup_candidate c;
			memset(&c, 0, sizeof(c));
			c.slot_index = 1;
			c.enabled = true;
			c.target_type = MFD_TARGET_AUDIO_VOLUME;
			snprintf(c.source_a_name, sizeof(c.source_a_name), "%s", cat->audio_sources[ai].name);
			char rsn[160] = {0};
			REQUIRE(mfd_engine_validate_setup_candidate(&eng, &c, rsn, sizeof(rsn)));
			mfd_engine_begin_setup_session(&eng, 1);
			REQUIRE(mfd_engine_commit_setup_session(&eng, &c, rsn, sizeof(rsn)));
			REQUIRE(eng.fader_slots[1].cached_available);
		}
	}

	free(snap);
	mfd_ui_catalog_destroy(cat);

	/* 16h: 0.4.0 FX Bank (fx_1..fx_8), filter_param (R-FADE-4), get_catalog, reset_fx,
	 *      single-writer uniqueness, bypass_at_zero lease aggregation & crash recovery,
	 *      external OBS "update" / "update_properties" sync, and profile switch preservation */
	{
		fake_sg_cleanup_filters(&fake_sg);
		fake_sg.owner_eng = &eng;
		mfd_engine_load_apc40_defaults(&eng);

		/* Attach grouped Blur + internal opacity filter to Color, and AudioModulator to scene Blue */
		struct fake_sg_filter *flt_blur =
			fake_sg_add_filter(&fake_sg, "Color", "Blur", "blur_filter_v2", true, FAKE_SCHEMA_BLUR_GROUPED);
		REQUIRE(flt_blur != NULL);
		obs_data_set_double(flt_blur->settings, "radius", 12.5);
		obs_data_set_int(flt_blur->settings, "passes", 3);

		REQUIRE(fake_sg_add_filter(&fake_sg, "Color", "__midi_fadester_opacity__",
					   "midi_fadester_opacity_filter", true, FAKE_SCHEMA_INTERNAL) != NULL);

		struct fake_sg_filter *flt_amod = fake_sg_add_filter(
			&fake_sg, "Blue", "AudioMod", "visual_sidechain_audio_mod", true, FAKE_SCHEMA_AUDIO_MOD);
		REQUIRE(flt_amod != NULL);
		obs_data_set_double(flt_amod->settings, "slot_1_amount_pct", 50.0);

		/* 1. get_state returns 18 slots; fx_1..fx_8 have kind == "fx" and enabled == true */
		req = obs_data_create();
		res = obs_data_create();
		mfd_vendor_handle_get_state(req, res, &eng);
		obs_data_array_t *slots18 = obs_data_get_array(res, "slots");
		REQUIRE(slots18 != NULL && obs_data_array_count(slots18) == 18U);
		obs_data_t *fx1_obj = obs_data_array_item(slots18, 10U);
		REQUIRE(strcmp(obs_data_get_string(fx1_obj, "id"), "fx_1") == 0);
		REQUIRE(strcmp(obs_data_get_string(fx1_obj, "kind"), "fx") == 0);
		REQUIRE(obs_data_get_bool(fx1_obj, "enabled"));
		obs_data_release(fx1_obj);
		obs_data_array_release(slots18);
		obs_data_release(req);
		obs_data_release(res);

		/* 2. get_catalog (source omitted vs source provided vs missing source) */
		req = obs_data_create();
		res = obs_data_create();
		mfd_vendor_handle_get_catalog(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		obs_data_array_t *cat_hosts = obs_data_get_array(res, "filter_hosts");
		REQUIRE(cat_hosts != NULL && obs_data_array_count(cat_hosts) >= 2U);
		bool saw_color_host = false;
		bool saw_blue_host = false;
		for (size_t hi = 0U; hi < obs_data_array_count(cat_hosts); ++hi) {
			obs_data_t *ho = obs_data_array_item(cat_hosts, hi);
			const char *hsrc = obs_data_get_string(ho, "source");
			if (strcmp(hsrc, "Color") == 0) {
				saw_color_host = true;
				REQUIRE(!obs_data_get_bool(ho, "is_scene"));
				/* Internal __midi_fadester_opacity__ excluded -> filter_count == 1 (Blur only) */
				REQUIRE(obs_data_get_int(ho, "filter_count") == 1);
			} else if (strcmp(hsrc, "Blue") == 0) {
				saw_blue_host = true;
				REQUIRE(obs_data_get_bool(ho, "is_scene"));
				REQUIRE(obs_data_get_int(ho, "filter_count") == 1);
			}
			obs_data_release(ho);
		}
		REQUIRE(saw_color_host && saw_blue_host);
		obs_data_array_release(cat_hosts);
		obs_data_release(req);
		obs_data_release(res);

		/* Source-specific get_catalog recurses nested OBS_PROPERTY_GROUPs & excludes LIST/BOOL/internal */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "source", "Color");
		mfd_vendor_handle_get_catalog(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(strcmp(obs_data_get_string(res, "source"), "Color") == 0);
		REQUIRE(!obs_data_get_bool(res, "is_scene"));
		obs_data_array_t *c_flts = obs_data_get_array(res, "filters");
		REQUIRE(c_flts != NULL && obs_data_array_count(c_flts) == 1U);
		obs_data_t *c_f0 = obs_data_array_item(c_flts, 0U);
		REQUIRE(strcmp(obs_data_get_string(c_f0, "name"), "Blur") == 0);
		obs_data_array_t *c_props = obs_data_get_array(c_f0, "properties");
		REQUIRE(c_props != NULL && obs_data_array_count(c_props) == 2U);
		obs_data_t *p0 = obs_data_array_item(c_props, 0U);
		obs_data_t *p1 = obs_data_array_item(c_props, 1U);
		REQUIRE(strcmp(obs_data_get_string(p0, "name"), "radius") == 0);
		REQUIRE(strcmp(obs_data_get_string(p0, "type"), "float") == 0);
		REQUIRE(fabs(obs_data_get_double(p0, "max") - 50.0) < 1e-6);
		REQUIRE(strcmp(obs_data_get_string(p0, "suffix"), " px") == 0);
		REQUIRE(fabs(obs_data_get_double(p0, "current") - 12.5) < 1e-6);
		REQUIRE(strcmp(obs_data_get_string(p1, "name"), "passes") == 0);
		REQUIRE(strcmp(obs_data_get_string(p1, "type"), "int") == 0);
		REQUIRE(obs_data_get_int(p1, "current") == 3);
		obs_data_release(p0);
		obs_data_release(p1);
		obs_data_array_release(c_props);
		obs_data_release(c_f0);
		obs_data_array_release(c_flts);
		obs_data_release(req);
		obs_data_release(res);

		/* Missing source in get_catalog returns explicit error + comment sentence */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "source", "NoSuchLayer");
		mfd_vendor_handle_get_catalog(req, res, &eng);
		REQUIRE(obs_data_has_user_value(res, "error"));
		REQUIRE(strcmp(obs_data_get_string(res, "error"), obs_data_get_string(res, "comment")) == 0);
		REQUIRE(strstr(obs_data_get_string(res, "comment"), "NoSuchLayer") != NULL);
		obs_data_release(req);
		obs_data_release(res);

		/* 3. set_slot(filter_param): reject internal filter, reject non-numeric LIST prop, accept default & inverted ranges */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "__midi_fadester_opacity__");
		obs_data_set_string(req, "filter_prop", "opacity");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(obs_data_has_user_value(res, "error"));
		REQUIRE(strcmp(obs_data_get_string(res, "error"), obs_data_get_string(res, "comment")) == 0);
		obs_data_release(req);
		obs_data_release(res);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "mode"); /* OBS_PROPERTY_LIST */
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(obs_data_has_user_value(res, "error"));
		REQUIRE(strstr(obs_data_get_string(res, "comment"), "not a numeric") != NULL);
		obs_data_release(req);
		obs_data_release(res);

		/* Bind fx_1 to (Color, Blur, radius) with omitted from_val/to_val -> defaults to [0, 50], live 12.5 -> value 0.25 */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(obs_data_get_bool(res, "available"));
		REQUIRE(fabs(obs_data_get_double(res, "from_val") - 0.0) < 1e-6);
		REQUIRE(fabs(obs_data_get_double(res, "to_val") - 50.0) < 1e-6);
		REQUIRE(fabs(obs_data_get_double(res, "value") - 0.25) < 1e-4);
		REQUIRE(strcmp(obs_data_get_string(res, "display_value"), "12.5 px") == 0);
		obs_data_release(req);
		obs_data_release(res);

		/* Re-bind fx_1 with inverted sub-range [40.0, 10.0] and verify display_value + signal bridge */
		proc_handler_t *fx_ph = proc_handler_create();
		signal_handler_t *fx_sh = signal_handler_create();
		struct fake_signal_log fx_sig;
		memset(&fx_sig, 0, sizeof(fx_sig));
		REQUIRE(mfd_bridge_register_with_handlers(fx_ph, fx_sh, &eng));
		signal_handler_connect(fx_sh, "midi_fadester_fader_changed", on_fake_signal, &fx_sig);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		obs_data_set_double(req, "from_val", 40.0);
		obs_data_set_double(req, "to_val", 10.0);
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		obs_data_release(req);
		obs_data_release(res);

		eng.fader_slots[10].sec_since_last_fader_event = 1.0f;
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_double(req, "value", 0.5); /* mapped = 25.0 px */
		mfd_vendor_handle_set_fader_value(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(fabs(obs_data_get_double(flt_blur->settings, "radius") - 25.0) < 1e-6);
		REQUIRE(strcmp(ws_log.last_fader_slot, "fx_1") == 0);
		REQUIRE(strcmp(ws_log.last_fader_display_value, "25.0 px") == 0);
		REQUIRE(strcmp(fx_sig.slot, "fx_1") == 0);
		REQUIRE(strcmp(fx_sig.display_value, "25.0 px") == 0);
		obs_data_release(req);
		obs_data_release(res);

		/* 4. Universal single-writer conflict rejection across set_slot, Set up, Advanced (disabled->enabled!), and JSON import */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(obs_data_has_user_value(res, "error"));
		REQUIRE(strstr(obs_data_get_string(res, "comment"), "already controlled") != NULL);
		obs_data_release(req);
		obs_data_release(res);

		struct mfd_setup_candidate dup_cand;
		memset(&dup_cand, 0, sizeof(dup_cand));
		dup_cand.slot_index = 11; /* fx_2 */
		dup_cand.enabled = true;
		dup_cand.target_type = MFD_TARGET_FILTER_PARAM;
		snprintf(dup_cand.source_a_name, sizeof(dup_cand.source_a_name), "Color");
		snprintf(dup_cand.filter_name, sizeof(dup_cand.filter_name), "Blur");
		snprintf(dup_cand.filter_prop, sizeof(dup_cand.filter_prop), "radius");
		dup_cand.filter_from_val = 0.0;
		dup_cand.filter_to_val = 50.0;
		char dup_err[192] = {0};
		REQUIRE(!mfd_engine_validate_setup_candidate(&eng, &dup_cand, dup_err, sizeof(dup_err)));
		REQUIRE(strstr(dup_err, "already controlled") != NULL);
		mfd_engine_begin_setup_session(&eng, 11);
		REQUIRE(!mfd_engine_commit_setup_session(&eng, &dup_cand, dup_err, sizeof(dup_err)));
		mfd_engine_cancel_setup_session(&eng);

		/* API enabled=false overrides target autoenable and permits an inactive duplicate binding. */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_bool(req, "enabled", false);
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(!obs_data_get_bool(res, "enabled"));
		REQUIRE(!eng.fader_slots[11].enabled && !eng.fader_slots[11].cached_available);
		REQUIRE(strcmp(eng.fader_slots[11].filter_prop, "radius") == 0);
		obs_data_release(req);
		obs_data_release(res);

		/* Enabling a duplicate is rejected before revision, binding, or lease mutation. */
		uint64_t disabled_dup_rev = eng.slot_config_rev[11];
		int dup_enable_writes = flt_blur->enable_writes;
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_bool(req, "enabled", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(strstr(obs_data_get_string(res, "error"), "already controlled") != NULL);
		REQUIRE(strcmp(obs_data_get_string(res, "error_code"), "setting_already_controlled") == 0);
		REQUIRE(strcmp(obs_data_get_string(res, "error"), obs_data_get_string(res, "comment")) == 0);
		REQUIRE(!eng.fader_slots[11].enabled);
		REQUIRE(eng.slot_config_rev[11] == disabled_dup_rev);
		REQUIRE(flt_blur->enable_writes == dup_enable_writes);
		obs_data_release(req);
		obs_data_release(res);

		/* Truthy strings are not booleans, and must not silently enable or disable a slot. */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "enabled", "true");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(strcmp(obs_data_get_string(res, "error"), "Parameter 'enabled' must be a boolean.") == 0);
		REQUIRE(strcmp(obs_data_get_string(res, "error"), obs_data_get_string(res, "comment")) == 0);
		REQUIRE(!eng.fader_slots[11].enabled && eng.slot_config_rev[11] == disabled_dup_rev);
		obs_data_release(req);
		obs_data_release(res);

		/* Legacy target-mode edits still autoenable when no enabled override is supplied. */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "target_mode", "unassigned");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(obs_data_get_bool(res, "enabled") && eng.fader_slots[11].enabled);
		obs_data_release(req);
		obs_data_release(res);

		/* Disabled slot configured with duplicate binding cannot be toggled enabled while fx_1 owns it */
		struct mfd_slot_config_edit adv_dup;
		memset(&adv_dup, 0, sizeof(adv_dup));
		adv_dup.enabled = false;
		adv_dup.target_type = MFD_TARGET_FILTER_PARAM;
		snprintf(adv_dup.source_a_name, sizeof(adv_dup.source_a_name), "Color");
		snprintf(adv_dup.filter_name, sizeof(adv_dup.filter_name), "Blur");
		snprintf(adv_dup.filter_prop, sizeof(adv_dup.filter_prop), "radius");
		adv_dup.has_filter_range = true;
		adv_dup.filter_from_val = 0.0;
		adv_dup.filter_to_val = 50.0;
		REQUIRE(mfd_engine_update_slot_config_ex(&eng, 11, &adv_dup, dup_err, sizeof(dup_err)));
		adv_dup.enabled = true;
		REQUIRE(!mfd_engine_update_slot_config_ex(&eng, 11, &adv_dup, dup_err, sizeof(dup_err)));
		REQUIRE(strstr(dup_err, "already controlled") != NULL);
		/* Reset fx_2 back to enabled + unassigned */
		memset(&adv_dup, 0, sizeof(adv_dup));
		adv_dup.enabled = true;
		adv_dup.target_type = MFD_TARGET_UNASSIGNED;
		REQUIRE(mfd_engine_update_slot_config_ex(&eng, 11, &adv_dup, dup_err, sizeof(dup_err)));

		/* Hand-edited JSON with fx_2 listed BEFORE fx_1 for the same (Color, Blur, radius):
		 * canonical 0..17 order guarantees fx_1 (10) wins and fx_2 (11) downgrades to unassigned! */
		const char *tmp_dup_json = "test-mfd-dup-filter-import.json";
		FILE *jf = fopen(tmp_dup_json, "wb");
		REQUIRE(jf != NULL);
		fputs("{\n"
		      "  \"active_profile\": \"apc40mk2\",\n"
		      "  \"slots\": [\n"
		      "    {\"id\": \"fx_2\", \"enabled\": true, \"target_mode\": \"filter_param\", "
		      "\"source_a\": \"Color\", \"filter_name\": \"Blur\", \"filter_prop\": \"radius\", "
		      "\"from_val\": 0.0, \"to_val\": 50.0, \"bypass_at_zero\": true},\n"
		      "    {\"id\": \"fx_1\", \"enabled\": true, \"target_mode\": \"filter_param\", "
		      "\"source_a\": \"Color\", \"filter_name\": \"Blur\", \"filter_prop\": \"radius\", "
		      "\"from_val\": 0.0, \"to_val\": 50.0, \"bypass_at_zero\": true}\n"
		      "  ]\n"
		      "}\n",
		      jf);
		fclose(jf);
		REQUIRE(mfd_engine_import_config_file(&eng, tmp_dup_json));
		remove(tmp_dup_json);
		REQUIRE(eng.fader_slots[10].target_type == MFD_TARGET_FILTER_PARAM);
		REQUIRE(eng.fader_slots[10].cached_available);
		REQUIRE(eng.fader_slots[11].target_type == MFD_TARGET_UNASSIGNED);

		/* 5. Integer quantization gating + shared-filter bypass_at_zero lease aggregation */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "passes"); /* int 1..8 */
		obs_data_set_double(req, "from_val", 1.0);
		obs_data_set_double(req, "to_val", 5.0);
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		obs_data_release(req);
		obs_data_release(res);

		/* Tiny sub-integer deflections on fx_2 around 0.50 (passes == 3) do NOT spam obs_source_update */
		char sval_err[128] = {0};
		REQUIRE(mfd_engine_set_slot_value(&eng, 11, 0.50f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(obs_data_get_int(flt_blur->settings, "passes") == 3);
		int writes_before_substep = flt_blur->settings_writes;
		REQUIRE(mfd_engine_set_slot_value(&eng, 11, 0.52f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(mfd_engine_set_slot_value(&eng, 11, 0.48f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(flt_blur->settings_writes == writes_before_substep);

		/* Multi-slot shared-filter bypass aggregation:
		 * fx_1 -> 0.0 while fx_2 == 0.48 -> Blur stays enabled!
		 * fx_2 -> 0.0 -> both at 0.0 -> Blur becomes disabled! */
		REQUIRE(flt_blur->enabled);
		REQUIRE(mfd_engine_set_slot_value(&eng, 10, 0.0f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(flt_blur->enabled);
		REQUIRE(mfd_engine_set_slot_value(&eng, 11, 0.0f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(!flt_blur->enabled);

		/* Disabling API owners preserves the sibling lease, then restores the last owner's original state. */
		for (int owner = 11; owner >= 10; --owner) {
			req = obs_data_create();
			res = obs_data_create();
			obs_data_set_string(req, "slot", eng.fader_slots[owner].id);
			obs_data_set_bool(req, "enabled", false);
			mfd_vendor_handle_set_slot(req, res, &eng);
			REQUIRE(!obs_data_has_user_value(res, "error"));
			REQUIRE(!obs_data_get_bool(res, "enabled") && !eng.fader_slots[owner].cached_available);
			REQUIRE(flt_blur->enabled == (owner == 10));
			obs_data_release(req);
			obs_data_release(res);
		}
		for (int owner = 10; owner <= 11; ++owner) {
			const int writes_before_reenable = flt_blur->enable_writes;
			req = obs_data_create();
			res = obs_data_create();
			obs_data_set_string(req, "slot", eng.fader_slots[owner].id);
			obs_data_set_bool(req, "enabled", true);
			mfd_vendor_handle_set_slot(req, res, &eng);
			REQUIRE(!obs_data_has_user_value(res, "error"));
			REQUIRE(obs_data_get_bool(res, "enabled") && eng.fader_slots[owner].cached_available);
			/* Reacquiring ownership must not write the live enable state by itself. */
			REQUIRE(flt_blur->enable_writes == writes_before_reenable);
			REQUIRE(flt_blur->enabled == (owner == 10));
			obs_data_release(req);
			obs_data_release(res);
			if (owner == 10) {
				/* Leave the parked rail before a fresh gesture returns to bypass. */
				REQUIRE(mfd_engine_set_slot_value(&eng, owner, 0.25f, false, "api", sval_err,
								  sizeof(sval_err)));
				REQUIRE(flt_blur->enabled);
				REQUIRE(mfd_engine_set_slot_value(&eng, owner, 0.0f, false, "api", sval_err,
								  sizeof(sval_err)));
				REQUIRE(!flt_blur->enabled);
			}
		}

		/* Persisted filter_pre_bypass_enabled crash-recovery check while Blur is bypassed (enabled == false): */
		const char *tmp_crash_json = "test-mfd-bypass-crash-recovery.json";
		REQUIRE(mfd_engine_export_config_file(&eng, tmp_crash_json));
		{
			struct mfd_engine rec_eng;
			memset(&rec_eng, 0, sizeof(rec_eng));
			REQUIRE(mfd_engine_init_mutex(&rec_eng));
			mfd_midi_queue_init(&rec_eng.midi_queue);
			mfd_midi_parser_init(&rec_eng.midi_parser);
			rec_eng.scene_graph_ops = &g_fake_sg_ops;
			rec_eng.scene_graph_user_data = &fake_sg;
			fake_sg.owner_eng = NULL;
			/* Simulate OBS restarting after crash with Blur still saved as enabled == false */
			REQUIRE(!flt_blur->enabled);
			REQUIRE(mfd_engine_import_config_file(&rec_eng, tmp_crash_json));
			/* Graceful shutdown on recovered engine restores pre-MFD enabled == true! */
			mfd_engine_restore_all_bypass_leases(&rec_eng);
			REQUIRE(flt_blur->enabled);
			mfd_engine_destroy_mutex(&rec_eng);
			/* Put Blur back to bypassed false for main engine continuation */
			flt_blur->enabled = false;
			fake_sg.owner_eng = &eng;
		}
		remove(tmp_crash_json);

		/* Unbinding fx_2 alone keeps lease owned by fx_1 (still at 0.0 -> Blur stays false);
		 * unbinding fx_1 releases last owner -> Blur restores to pre_mfd_enabled == true! */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "target_mode", "unassigned");
		mfd_vendor_handle_set_slot(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
		REQUIRE(!flt_blur->enabled);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "unassigned");
		mfd_vendor_handle_set_slot(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
		REQUIRE(flt_blur->enabled);

		/* Filter initially DISABLED in OBS before Midi Fadester binds -> unbind restores false! */
		flt_blur->enabled = false;
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		obs_data_set_double(req, "from_val", 0.0);
		obs_data_set_double(req, "to_val", 50.0);
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);

		REQUIRE(mfd_engine_set_slot_value(&eng, 10, 0.6f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(flt_blur->enabled); /* Driven above 0% turns filter ON */

		/* Rename host source + rename filter while lease active, then unbind -> still restores initial false! */
		fake_sg_rename(&fake_sg, "Color", "Color Hero");
		mfd_engine_handle_source_rename(&eng, "Color", "Color Hero");
		REQUIRE(strcmp(eng.fader_slots[10].source_a_name, "Color Hero") == 0);
		REQUIRE(eng.fader_slots[10].cached_available);

		fake_sg_rename_filter(&fake_sg, "Color Hero", "Blur", "Hero Blur");
		mfd_engine_handle_filter_rename(&eng, "Color Hero", "Blur", "Hero Blur");
		REQUIRE(strcmp(eng.fader_slots[10].filter_name, "Hero Blur") == 0);
		REQUIRE(eng.fader_slots[10].cached_available);

		REQUIRE(mfd_engine_unassign_slot_preserve_addr(&eng, 10));
		REQUIRE(!flt_blur->enabled); /* Restored to pre-MFD false despite both renames! */

		/* Rename back to Color / Blur and set enabled = true */
		fake_sg_rename_filter(&fake_sg, "Color Hero", "Hero Blur", "Blur");
		mfd_engine_handle_filter_rename(&eng, "Color Hero", "Hero Blur", "Blur");
		fake_sg_rename(&fake_sg, "Color Hero", "Color");
		mfd_engine_handle_source_rename(&eng, "Color Hero", "Color");
		flt_blur->enabled = true;

		/* 6. External OBS "update" syncs t_ext + Soft Takeover + origin="obs" without perturbing sibling property */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		obs_data_set_double(req, "from_val", 0.0);
		obs_data_set_double(req, "to_val", 50.0);
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_2");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "passes");
		obs_data_set_double(req, "from_val", 1.0);
		obs_data_set_double(req, "to_val", 5.0);
		obs_data_set_bool(req, "bypass_at_zero", false);
		mfd_vendor_handle_set_slot(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);

		/* Engage physical knob CC 16 at 127 (1.0 -> 50.0 px) on fx_1, set fx_2 to 0.5 (passes = 3) */
		eng.fader_slots[10].takeover.state = MFD_TAKEOVER_ENGAGED;
		push_midi_cc(&eng, 0, 16, 127);
		for (int t = 0; t < 20; ++t) {
			mfd_engine_tick(&eng, 0.016f);
		}
		REQUIRE(fabs(obs_data_get_double(flt_blur->settings, "radius") - 50.0) < 0.5);
		REQUIRE(mfd_engine_set_slot_value(&eng, 11, 0.5f, false, "api", sval_err, sizeof(sval_err)));

		/* User drags Radius slider inside OBS Filters dialog to 20.0 px (t_ext = 0.40) */
		obs_data_set_double(flt_blur->settings, "radius", 20.0);
		mfd_engine_handle_filter_settings_updated(&eng, "Color", "Blur");
		REQUIRE(fabsf(eng.fader_slots[10].last_applied_val - 0.40f) < 1e-4f);
		REQUIRE(eng.fader_slots[10].takeover.state == MFD_TAKEOVER_WAITING);
		REQUIRE(strcmp(ws_log.last_fader_slot, "fx_1") == 0);
		REQUIRE(strcmp(ws_log.last_fader_origin, "obs") == 0);
		REQUIRE(strcmp(ws_log.last_fader_display_value, "20.0 px") == 0);
		REQUIRE(fabsf(eng.fader_slots[11].last_applied_val - 0.50f) < 1e-4f); /* Sibling passes untouched! */

		/* 7. Dynamic "update_properties" range drift and missing-property invalidation */
		flt_blur->float_max_override = 30.0; /* fx_1 to_val=50.0 now exceeds max=30.0! */
		mfd_engine_handle_filter_properties_updated(&eng, "Color", "Blur");
		REQUIRE(!eng.fader_slots[10].cached_available);
		REQUIRE(strstr(eng.fader_slots[10].cached_reason, "outside") != NULL);

		flt_blur->omit_radius_prop = true;
		mfd_engine_handle_filter_properties_updated(&eng, "Color", "Blur");
		REQUIRE(!eng.fader_slots[10].cached_available);
		REQUIRE(strstr(eng.fader_slots[10].cached_reason, "no longer available") != NULL);

		flt_blur->omit_radius_prop = false;
		flt_blur->float_max_override = 50.0;
		mfd_engine_handle_filter_properties_updated(&eng, "Color", "Blur");
		REQUIRE(eng.fader_slots[10].cached_available);

		/* 8. Profile switch apc40mk2 -> apcminimk2 -> apc40mk2 preserves fx_1..fx_8 enabled & bindings */
		REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_APC_MINI_MK2, false));
		REQUIRE(eng.fader_slots[10].enabled);
		REQUIRE(eng.fader_slots[10].addr.type == MFD_MIDI_NONE);
		REQUIRE(eng.fader_slots[10].target_type == MFD_TARGET_FILTER_PARAM);
		REQUIRE(eng.fader_slots[10].cached_available);
		REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_APC40_MK2, false));
		REQUIRE(eng.fader_slots[10].addr.type == MFD_MIDI_CC && eng.fader_slots[10].addr.number == 16U);

		/* 9. reset_fx: ms validation, channel layer_opacity isolation, instant snap, timed ramp, and unavailable queue recovery */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_1");
		obs_data_set_string(req, "target_mode", "layer_opacity");
		obs_data_set_string(req, "scene", "Blue");
		obs_data_set_string(req, "source_a", "Color");
		mfd_vendor_handle_set_slot(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
		REQUIRE(mfd_engine_set_slot_value(&eng, 1, 0.75f, false, "api", sval_err, sizeof(sval_err)));

		/* Also bind fader_4 (channel fader) to AudioMod slot_1_amount_pct on scene Blue */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Blue");
		obs_data_set_string(req, "filter_name", "AudioMod");
		obs_data_set_string(req, "filter_prop", "slot_1_amount_pct");
		obs_data_set_double(req, "from_val", 0.0);
		obs_data_set_double(req, "to_val", 100.0);
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		obs_data_release(req);
		obs_data_release(res);
		REQUIRE(mfd_engine_set_slot_value(&eng, 4, 0.80f, false, "api", sval_err, sizeof(sval_err)));

		/* Nonfilter controls stay protected even when assigned to the FX bank. */
		const char *nonfilter_modes[] = {"audio_volume", "crossfade_ab", "layer_opacity"};
		uint32_t nonfilter_writes[3];
		for (int n = 0; n < 3; ++n) {
			int slot_index = 12 + n; /* fx_3, fx_4, fx_5 */
			req = obs_data_create();
			res = obs_data_create();
			obs_data_set_string(req, "slot", eng.fader_slots[slot_index].id);
			obs_data_set_string(req, "target_mode", nonfilter_modes[n]);
			obs_data_set_string(req, "scene", "Blue");
			obs_data_set_string(req, "source_a", n == 0 ? "Mic/Aux" : "Color");
			if (n == 1) {
				obs_data_set_string(req, "source_b", "Color 2");
			}
			mfd_vendor_handle_set_slot(req, res, &eng);
			REQUIRE(!obs_data_has_user_value(res, "error"));
			obs_data_release(req);
			obs_data_release(res);
			REQUIRE(mfd_engine_set_slot_value(&eng, slot_index, 0.65f, false, "api", sval_err,
							  sizeof(sval_err)));
			nonfilter_writes[n] = eng.fader_slots[slot_index].output_write_count;
		}
		/* A disabled filter is excluded even though its retained binding is otherwise valid. */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_6");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_bool(req, "enabled", false);
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		obs_data_release(req);
		obs_data_release(res);
		float disabled_filter_value = eng.fader_slots[15].last_applied_val;

		/* Reject ms = 10 */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_int(req, "ms", 10);
		mfd_vendor_handle_reset_fx(req, res, &eng);
		REQUIRE(obs_data_has_user_value(res, "error"));
		REQUIRE(strcmp(obs_data_get_string(res, "error"), obs_data_get_string(res, "comment")) == 0);
		obs_data_release(req);
		obs_data_release(res);

		/* Instant reset_fx ({ ms: 0 }) zeroes fx_1, fx_2, and fader_4 (filter_param), leaving fader_1 at 0.75! */
		REQUIRE(mfd_engine_set_slot_value(&eng, 10, 0.60f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(mfd_engine_set_slot_value(&eng, 11, 0.50f, false, "api", sval_err, sizeof(sval_err)));
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_int(req, "ms", 0);
		mfd_vendor_handle_reset_fx(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(obs_data_get_int(res, "eligible_count") == 3);
		REQUIRE(obs_data_get_int(res, "reset_count") == 3);
		REQUIRE(obs_data_get_int(res, "queued_unavailable_count") == 0);
		obs_data_release(req);
		obs_data_release(res);

		REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.75f) <
			1e-5f); /* fader_1 layer_opacity untouched! */
		REQUIRE(fabsf(eng.fader_slots[10].last_applied_val) < 1e-6f);
		REQUIRE(fabsf(eng.fader_slots[11].last_applied_val) < 1e-6f);
		REQUIRE(fabsf(eng.fader_slots[4].last_applied_val) < 1e-6f);
		REQUIRE(fabs(obs_data_get_double(flt_blur->settings, "radius") - 0.0) < 1e-6);
		REQUIRE(obs_data_get_int(flt_blur->settings, "passes") == 1);
		REQUIRE(fabs(obs_data_get_double(flt_amod->settings, "slot_1_amount_pct") - 0.0) < 1e-6);
		REQUIRE(!flt_amod->enabled);

		for (int n = 0; n < 3; ++n) {
			struct mfd_fader_slot *protected_slot = &eng.fader_slots[12 + n];
			REQUIRE(fabsf(protected_slot->last_applied_val - 0.65f) < 1e-6f);
			REQUIRE(protected_slot->output_write_count == nonfilter_writes[n]);
			REQUIRE(!protected_slot->auto_fade_active && !protected_slot->pending_clean_apply_on_avail);
		}
		REQUIRE(eng.fader_slots[15].last_applied_val == disabled_filter_value);
		REQUIRE(!eng.fader_slots[15].pending_clean_apply_on_avail);

		/* Timed reset_fx ({ ms: 400 }) + unavailable-slot recovery via pending_clean_apply_on_avail */
		REQUIRE(mfd_engine_set_slot_value(&eng, 10, 1.0f, false, "api", sval_err, sizeof(sval_err)));
		REQUIRE(mfd_engine_set_slot_value(&eng, 4, 1.0f, false, "api", sval_err, sizeof(sval_err)));
		/* Temporarily remove AudioMod filter before reset_fx */
		fake_sg_remove_filter(&fake_sg, "Blue", "AudioMod");
		mfd_engine_handle_filter_removed(&eng, "Blue", "AudioMod");
		REQUIRE(!eng.fader_slots[4].cached_available);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_int(req, "ms", 400);
		mfd_vendor_handle_reset_fx(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(obs_data_get_int(res, "eligible_count") == 3);
		REQUIRE(obs_data_get_int(res, "reset_count") == 2); /* fx_1 + fx_2 writable; fader_4 queued */
		REQUIRE(obs_data_get_int(res, "queued_unavailable_count") == 1); /* fader_4 queued for clean on avail */
		obs_data_release(req);
		obs_data_release(res);

		for (int n = 0; n < 3; ++n) {
			struct mfd_fader_slot *protected_slot = &eng.fader_slots[12 + n];
			REQUIRE(fabsf(protected_slot->last_applied_val - 0.65f) < 1e-6f);
			REQUIRE(protected_slot->output_write_count == nonfilter_writes[n]);
			REQUIRE(!protected_slot->auto_fade_active && !protected_slot->pending_clean_apply_on_avail);
		}
		REQUIRE(eng.fader_slots[15].last_applied_val == disabled_filter_value);
		REQUIRE(!eng.fader_slots[15].auto_fade_active && !eng.fader_slots[15].pending_clean_apply_on_avail);

		mfd_engine_tick(&eng, 0.20f);
		REQUIRE(eng.fader_slots[10].last_applied_val > 0.35f && eng.fader_slots[10].last_applied_val < 0.65f);
		mfd_engine_tick(&eng, 0.25f);
		REQUIRE(fabsf(eng.fader_slots[10].last_applied_val) < 1e-6f);

		/* Re-add AudioMod with non-zero 77% setting -> pending_clean_apply_on_avail immediately zeroes it! */
		flt_amod = fake_sg_add_filter(&fake_sg, "Blue", "AudioMod", "visual_sidechain_audio_mod", true,
					      FAKE_SCHEMA_AUDIO_MOD);
		obs_data_set_double(flt_amod->settings, "slot_1_amount_pct", 77.0);
		mfd_engine_reevaluate_slots(&eng, true);
		REQUIRE(eng.fader_slots[4].cached_available);
		REQUIRE(fabsf(eng.fader_slots[4].last_applied_val) < 1e-6f);
		REQUIRE(fabs(obs_data_get_double(flt_amod->settings, "slot_1_amount_pct") - 0.0) < 1e-6);
		REQUIRE(!flt_amod->enabled);

		/* Explicit disable cancels a running clean, rather than queuing it for a later reenable. */
		REQUIRE(mfd_engine_set_slot_value(&eng, 4, 0.80f, false, "api", sval_err, sizeof(sval_err)));
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_int(req, "ms", 400);
		mfd_vendor_handle_reset_fx(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(eng.fader_slots[4].auto_fade_active && eng.fader_slots[4].auto_fade_is_reset_fx);
		obs_data_release(req);
		obs_data_release(res);
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_bool(req, "enabled", false);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(!eng.fader_slots[4].auto_fade_active && !eng.fader_slots[4].auto_fade_is_reset_fx);
		REQUIRE(!eng.fader_slots[4].pending_clean_apply_on_avail);
		obs_data_release(req);
		obs_data_release(res);
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_bool(req, "enabled", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error") && eng.fader_slots[4].cached_available);
		obs_data_release(req);
		obs_data_release(res);
		mfd_engine_tick(&eng, 0.50f);
		REQUIRE(fabs(obs_data_get_double(flt_amod->settings, "slot_1_amount_pct") - 80.0) < 1e-5);

		/* A missing binding can be disabled, and its queued clean must not replay after reenable. */
		fake_sg_remove_filter(&fake_sg, "Blue", "AudioMod");
		mfd_engine_handle_filter_removed(&eng, "Blue", "AudioMod");
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_int(req, "ms", 0);
		mfd_vendor_handle_reset_fx(req, res, &eng);
		REQUIRE(eng.fader_slots[4].pending_clean_apply_on_avail);
		obs_data_release(req);
		obs_data_release(res);
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_bool(req, "enabled", false);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error"));
		REQUIRE(!obs_data_get_bool(res, "enabled") && !eng.fader_slots[4].pending_clean_apply_on_avail);
		REQUIRE(strcmp(eng.fader_slots[4].filter_name, "AudioMod") == 0);
		REQUIRE(strcmp(eng.fader_slots[4].filter_prop, "slot_1_amount_pct") == 0);
		obs_data_release(req);
		obs_data_release(res);

		uint64_t missing_disabled_rev = eng.slot_config_rev[4];
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_bool(req, "enabled", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(strstr(obs_data_get_string(res, "error"), "not found") != NULL);
		REQUIRE(!eng.fader_slots[4].enabled && eng.slot_config_rev[4] == missing_disabled_rev);
		obs_data_release(req);
		obs_data_release(res);
		/* A disable carrying a binding/range edit must still validate the requested edit. */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_bool(req, "enabled", false);
		obs_data_set_double(req, "to_val", 101.0);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(obs_data_has_user_value(res, "error"));
		REQUIRE(eng.fader_slots[4].filter_to_val == 100.0 && eng.slot_config_rev[4] == missing_disabled_rev);
		obs_data_release(req);
		obs_data_release(res);

		flt_amod = fake_sg_add_filter(&fake_sg, "Blue", "AudioMod", "visual_sidechain_audio_mod", true,
					      FAKE_SCHEMA_AUDIO_MOD);
		obs_data_set_double(flt_amod->settings, "slot_1_amount_pct", 77.0);
		mfd_engine_reevaluate_slots(&eng, true);
		REQUIRE(!eng.fader_slots[4].cached_available);
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_bool(req, "enabled", true);
		mfd_vendor_handle_set_slot(req, res, &eng);
		REQUIRE(!obs_data_has_user_value(res, "error") && eng.fader_slots[4].cached_available);
		REQUIRE(!eng.fader_slots[4].pending_clean_apply_on_avail);
		REQUIRE(fabsf(eng.fader_slots[4].last_applied_val - 0.77f) < 1e-5f);
		REQUIRE(fabsf(eng.fader_slots[4].takeover.target_obs - 0.77f) < 1e-5f);
		REQUIRE(fabs(obs_data_get_double(res, "value") - 0.77) < 1e-5);
		obs_data_release(req);
		obs_data_release(res);
		mfd_engine_tick(&eng, 0.50f);
		REQUIRE(fabs(obs_data_get_double(flt_amod->settings, "slot_1_amount_pct") - 77.0) < 1e-5);
		/* The next relative command continues from the live value, not the cancelled clean. */
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_4");
		obs_data_set_double(req, "delta", 0.02);
		mfd_vendor_handle_nudge_fader(req, res, &eng);
		REQUIRE(fabs(obs_data_get_double(res, "value") - 0.79) < 1e-5);
		REQUIRE(fabs(obs_data_get_double(flt_amod->settings, "slot_1_amount_pct") - 79.0) < 1e-5);
		obs_data_release(req);
		obs_data_release(res);

		mfd_engine_restore_all_bypass_leases(&eng);
		REQUIRE(flt_blur->enabled && flt_amod->enabled);

		mfd_bridge_register_with_handlers(NULL, NULL, NULL);
		signal_handler_destroy(fx_sh);
		proc_handler_destroy(fx_ph);
		fake_sg.owner_eng = NULL;
	}

	mfd_vendor_set_event_sink(NULL, NULL);

	/* 16g: Multi-threaded recursive state_mutex concurrency stress test + mfd_vendor_api_shutdown fencing */
	{
		struct mfd_engine *mt_eng = (struct mfd_engine *)calloc(1U, sizeof(*mt_eng));
		REQUIRE(mt_eng != NULL);
		REQUIRE(mfd_engine_init_mutex(mt_eng));
		mfd_midi_queue_init(&mt_eng->midi_queue);
		mfd_midi_parser_init(&mt_eng->midi_parser);
		mt_eng->scene_graph_ops = &g_fake_sg_ops;
		mt_eng->scene_graph_user_data = &fake_sg;
		mt_eng->studio_mode_override_set = true;
		mt_eng->studio_mode_override = true;
		mt_eng->tbar_driver = mock_tbar_driver;
		mt_eng->tbar_unlatch = mock_tbar_unlatch;
		fake_sg.owner_eng = mt_eng;
		mfd_engine_load_apc40_defaults(mt_eng);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_2");
		obs_data_set_string(req, "target_mode", "crossfade_ab");
		obs_data_set_string(req, "scene", "Blue");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "source_b", "Color 2");
		mfd_vendor_handle_set_slot(req, res, mt_eng);
		obs_data_release(req);
		obs_data_release(res);

		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fx_1");
		obs_data_set_string(req, "target_mode", "filter_param");
		obs_data_set_string(req, "source_a", "Color");
		obs_data_set_string(req, "filter_name", "Blur");
		obs_data_set_string(req, "filter_prop", "radius");
		obs_data_set_double(req, "from_val", 0.0);
		obs_data_set_double(req, "to_val", 50.0);
		obs_data_set_bool(req, "bypass_at_zero", true);
		mfd_vendor_handle_set_slot(req, res, mt_eng);
		obs_data_release(req);
		obs_data_release(res);

		struct mt_stress_ctx mt_ctx;
		memset(&mt_ctx, 0, sizeof(mt_ctx));
		mt_ctx.eng = mt_eng;
		mfd_engine_set_ui_notify_fn(mt_eng, on_mt_ui_notify, &mt_ctx);

		pthread_t th_tick;
		pthread_t th_vendor;
		pthread_t th_ui;
		REQUIRE(pthread_create(&th_tick, NULL, mt_tick_worker, &mt_ctx) == 0);
		REQUIRE(pthread_create(&th_vendor, NULL, mt_vendor_worker, &mt_ctx) == 0);
		REQUIRE(pthread_create(&th_ui, NULL, mt_ui_worker, &mt_ctx) == 0);
		REQUIRE(pthread_join(th_tick, NULL) == 0);
		REQUIRE(pthread_join(th_vendor, NULL) == 0);
		REQUIRE(pthread_join(th_ui, NULL) == 0);

		REQUIRE(!mt_ctx.bad_snapshot);
		REQUIRE(os_atomic_load_long(&mt_ctx.ui_notify_calls) > 0L);

		/* Verify mfd_vendor_api_shutdown blocks subsequent vendor & proc calls */
		proc_handler_t *mt_ph = proc_handler_create();
		signal_handler_t *mt_sh = signal_handler_create();
		REQUIRE(mt_ph != NULL && mt_sh != NULL);
		REQUIRE(mfd_bridge_register_with_handlers(mt_ph, mt_sh, mt_eng));

		mfd_vendor_api_shutdown(mt_eng);
		req = obs_data_create();
		res = obs_data_create();
		mfd_vendor_handle_get_state(req, res, mt_eng);
		REQUIRE(!obs_data_has_user_value(res, "version"));
		obs_data_release(req);
		obs_data_release(res);

		struct calldata shut_cd;
		calldata_init(&shut_cd);
		calldata_set_string(&shut_cd, "slot", "crossfader");
		REQUIRE(proc_handler_call(mt_ph, "midi_fadester_get_slot_value", &shut_cd));
		REQUIRE(!calldata_bool(&shut_cd, "ok"));
		calldata_free(&shut_cd);

		signal_handler_destroy(mt_sh);
		proc_handler_destroy(mt_ph);
		mfd_engine_set_ui_notify_fn(mt_eng, NULL, NULL);
		fake_sg.owner_eng = NULL;
		mfd_engine_destroy_mutex(mt_eng);
		free(mt_eng);
	}

	fake_sg_cleanup_filters(&fake_sg);
	mfd_vendor_set_event_sink(NULL, NULL);
	mfd_text_set_lookup(NULL);
	text_lookup_destroy(obs_module_lookup);
	obs_module_lookup = NULL;
	printf("test-vendor-api: PASS\n");
	return 0;
}
