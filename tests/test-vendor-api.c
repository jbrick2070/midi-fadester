/*
 * Midi Fadester — test-vendor-api
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include "core/tbar-logic.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("midi-fadester", "en-US")

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
	int total_opacity_writes;
	int total_volume_writes;
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

static const struct mfd_scene_graph_ops g_fake_sg_ops = {
	fake_op_source_exists,          fake_op_scene_exists,          fake_op_source_has_audio,
	fake_op_scene_item_stack_index, fake_op_find_scene_with_layer, fake_op_find_scene_with_pair,
	fake_op_apply_opacity,          fake_op_apply_volume,
};

struct fake_ws_log {
	int fader_changed_count;
	int state_changed_count;
	char last_fader_slot[32];
	double last_fader_value;
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
		snprintf(log->last_fader_slot, sizeof(log->last_fader_slot), "%s", s ? s : "");
		snprintf(log->last_fader_origin, sizeof(log->last_fader_origin), "%s", o ? o : "");
		log->last_fader_value = obs_data_get_double(event_data, "value");
	} else if (strcmp(event_name, "state_changed") == 0) {
		++log->state_changed_count;
	}
}

struct fake_signal_log {
	int count;
	char slot[32];
	float value;
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
	snprintf(sig->slot, sizeof(sig->slot), "%s", s ? s : "");
	snprintf(sig->origin, sizeof(sig->origin), "%s", o ? o : "");
	sig->value = (float)calldata_float(cd, "value");
}

int main(void)
{
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

	/* 1. load_profile { name: "apc40mk2" } and error check on unknown profile */
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	obs_data_set_string(req, "name", "nonexistent_desk");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Unknown profile 'nonexistent_desk'. Valid profiles: apc40mk2.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "name", "apc40mk2");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "loaded"), "apc40mk2") == 0);
	REQUIRE(ws_log.state_changed_count == 1);
	REQUIRE(strcmp(eng.fader_slots[0].id, "crossfader") == 0);
	REQUIRE(eng.fader_slots[0].target_type == MFD_TARGET_TBAR);
	REQUIRE(strcmp(eng.fader_slots[1].id, "fader_1") == 0);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	REQUIRE(strcmp(eng.fader_slots[9].id, "master") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* 2. get_state: verify version, build identity, studio_mode, device, and slot availability/reason when Studio Mode is OFF */
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "version"), PLUGIN_VERSION) == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "version"), "0.1.2") == 0);
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
		       "Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.") == 0);
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
	REQUIRE(mfd_engine_export_config_file(&eng, tmp_json));

	mfd_engine_load_apc40_defaults(&eng);
	eng.configured_device_name[0] = '\0';
	eng.smooth_tau_ms = 12.0f;
	eng.tbar_bidirectional_flip = true;
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);

	REQUIRE(mfd_engine_import_config_file(&eng, tmp_json));
	REQUIRE(strcmp(eng.configured_device_name, "APC40 mkII") == 0);
	REQUIRE(fabsf(eng.smooth_tau_ms - 24.5f) < 1e-4f);
	REQUIRE(!eng.tbar_bidirectional_flip);
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
		       "Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.") == 0);
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
	memset(eng.cal_entries, 0, sizeof(eng.cal_entries));
	eng.fader_slots[0].cal.cal_max = 127;
	REQUIRE(mfd_engine_import_config_file(&eng, tmp_cal_json));
	remove(tmp_cal_json);
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

	mfd_vendor_set_event_sink(NULL, NULL);
	printf("test-vendor-api: PASS\n");
	return 0;
}
