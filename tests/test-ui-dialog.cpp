/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui/mfd-controller-svg.hpp"
#include "ui/mfd-settings-dialog.hpp"
#include "ui/mfd-svg-canvas.hpp"
#include "obs/mfd-engine.h"
#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include "core/tbar-logic.h"
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("midi-fadester", "en-US")

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QStackedWidget>
#include <QString>
#include <QTabWidget>
#include <QTableWidget>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define REQUIRE(cond)                                                                        \
	do {                                                                                 \
		if (!(cond)) {                                                               \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			std::exit(1);                                                        \
		}                                                                            \
	} while (0)

namespace {

static int g_mock_tbar_set_writes = 0;
static int g_mock_tbar_completions = 0;
static int g_mock_tbar_aborts = 0;
static int g_mock_tbar_last_pos = 0;
static bool g_mock_tbar_latched = false;

static void mock_tbar_unlatch()
{
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
	struct mfd_tbar_release_input in {};
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.state = state;
	in.release_latched = g_mock_tbar_latched;
	in.t_norm = t_norm;
	in.force_complete = force_complete;
	in.force_abort = force_abort;
	in.in_target_rail_zone = in_target_rail_zone;
	in.in_origin_rail_zone = in_origin_rail_zone;
	in.in_start_deadband = in_start_deadband;
	in.last_pos_1024 = g_mock_tbar_last_pos;

	struct mfd_tbar_action act {};
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
	const uint32_t raw_msg = static_cast<uint32_t>(0xB0U | (ch & 0x0FU)) |
				 (static_cast<uint32_t>(cc & 0x7FU) << 8) | (static_cast<uint32_t>(val & 0x7FU) << 16);
	mfd_midi_queue_push(&eng->midi_queue, raw_msg, 0U);
}

static void pump_ui()
{
	QCoreApplication::processEvents();
	QCoreApplication::sendPostedEvents(nullptr, 0);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents();
}

constexpr int FAKE_MAX_SOURCES = 20;
constexpr int FAKE_MAX_SCENES = 10;
constexpr int FAKE_MAX_SCENE_ITEMS = 10;
constexpr int FAKE_MAX_FILTERS = 8;

struct fake_sg_filter {
	bool in_use;
	char host_name[64];
	char filter_name[64];
	char type_id[64];
	bool enabled;
	double radius;
	int passes;
	double reactivity_pct;
	int update_writes;
	int enable_writes;
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
	char items[FAKE_MAX_SCENE_ITEMS][64];
};

struct fake_scene_graph {
	struct fake_sg_source sources[FAKE_MAX_SOURCES];
	struct fake_sg_scene scenes[FAKE_MAX_SCENES];
	struct fake_sg_filter filters[FAKE_MAX_FILTERS];
	int total_opacity_writes;
	int total_volume_writes;
	int total_filter_writes;
	struct mfd_engine *owner_eng;
};

static struct fake_sg_source *fake_sg_find_source(struct fake_scene_graph *sg, const char *name)
{
	if (!sg || !name || name[0] == '\0') {
		return nullptr;
	}
	for (int i = 0; i < FAKE_MAX_SOURCES; ++i) {
		if (sg->sources[i].in_use && std::strcmp(sg->sources[i].name, name) == 0) {
			return &sg->sources[i];
		}
	}
	return nullptr;
}

static struct fake_sg_scene *fake_sg_find_scene(struct fake_scene_graph *sg, const char *name)
{
	if (!sg || !name || name[0] == '\0') {
		return nullptr;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (sg->scenes[i].in_use && std::strcmp(sg->scenes[i].name, name) == 0) {
			return &sg->scenes[i];
		}
	}
	return nullptr;
}

static void fake_sg_add_source(struct fake_scene_graph *sg, const char *name, bool has_audio, float vol_mul = 1.0f)
{
	struct fake_sg_source *existing = fake_sg_find_source(sg, name);
	if (existing) {
		existing->has_audio = has_audio;
		existing->last_volume_mul = vol_mul;
		return;
	}
	for (int i = 0; i < FAKE_MAX_SOURCES; ++i) {
		if (!sg->sources[i].in_use) {
			std::memset(&sg->sources[i], 0, sizeof(sg->sources[i]));
			sg->sources[i].in_use = true;
			std::snprintf(sg->sources[i].name, sizeof(sg->sources[i].name), "%s", name);
			sg->sources[i].has_audio = has_audio;
			sg->sources[i].last_opacity = 1.0f;
			sg->sources[i].last_volume_mul = vol_mul;
			return;
		}
	}
}

static void fake_sg_add_scene(struct fake_scene_graph *sg, const char *name)
{
	if (fake_sg_find_scene(sg, name)) {
		return;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			std::memset(&sg->scenes[i], 0, sizeof(sg->scenes[i]));
			sg->scenes[i].in_use = true;
			std::snprintf(sg->scenes[i].name, sizeof(sg->scenes[i].name), "%s", name);
			return;
		}
	}
}

static void fake_sg_scene_add_item(struct fake_scene_graph *sg, const char *scene_name, const char *source_name)
{
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (!sc || sc->item_count >= FAKE_MAX_SCENE_ITEMS) {
		return;
	}
	std::snprintf(sc->items[sc->item_count], sizeof(sc->items[sc->item_count]), "%s", source_name);
	++sc->item_count;
}

static void fake_sg_scene_remove_item(struct fake_scene_graph *sg, const char *scene_name, const char *source_name)
{
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (!sc) {
		return;
	}
	for (int i = 0; i < sc->item_count; ++i) {
		if (std::strcmp(sc->items[i], source_name) == 0) {
			for (int j = i; j + 1 < sc->item_count; ++j) {
				std::snprintf(sc->items[j], sizeof(sc->items[j]), "%s", sc->items[j + 1]);
			}
			sc->items[sc->item_count - 1][0] = '\0';
			--sc->item_count;
			return;
		}
	}
}

static void fake_sg_remove_scene(struct fake_scene_graph *sg, const char *scene_name)
{
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (sc) {
		std::memset(sc, 0, sizeof(*sc));
	}
}

static void fake_sg_remove_source(struct fake_scene_graph *sg, const char *name)
{
	struct fake_sg_source *src = fake_sg_find_source(sg, name);
	if (src) {
		std::memset(src, 0, sizeof(*src));
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (sg->scenes[i].in_use) {
			fake_sg_scene_remove_item(sg, sg->scenes[i].name, name);
		}
	}
}

static bool fake_op_source_exists(const char *source_name, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	return fake_sg_find_source(sg, source_name) != nullptr || fake_sg_find_scene(sg, source_name) != nullptr;
}

static bool fake_op_scene_exists(const char *scene_name, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	return fake_sg_find_scene(sg, scene_name) != nullptr;
}

static bool fake_op_source_has_audio(const char *source_name, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_source *src = fake_sg_find_source(sg, source_name);
	return src ? src->has_audio : false;
}

static int fake_op_scene_item_stack_index(const char *scene_name, const char *source_name, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_scene *sc = fake_sg_find_scene(sg, scene_name);
	if (!sc || !source_name) {
		return -1;
	}
	for (int i = 0; i < sc->item_count; ++i) {
		if (std::strcmp(sc->items[i], source_name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool fake_op_find_scene_with_layer(const char *source_name, const char *exclude_scene, char *out_scene,
					  size_t out_size, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	if (!sg || !source_name) {
		return false;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			continue;
		}
		if (exclude_scene && exclude_scene[0] != '\0' && std::strcmp(sg->scenes[i].name, exclude_scene) == 0) {
			continue;
		}
		if (fake_op_scene_item_stack_index(sg->scenes[i].name, source_name, sg) >= 0) {
			if (out_scene && out_size > 0U) {
				std::snprintf(out_scene, out_size, "%s", sg->scenes[i].name);
			}
			return true;
		}
	}
	return false;
}

static bool fake_op_find_scene_with_pair(const char *source_a, const char *source_b, char *out_scene, size_t out_size,
					 int *out_idx_a, int *out_idx_b, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	if (!sg || !source_a || !source_b) {
		return false;
	}
	for (int i = 0; i < FAKE_MAX_SCENES; ++i) {
		if (!sg->scenes[i].in_use) {
			continue;
		}
		const int ia = fake_op_scene_item_stack_index(sg->scenes[i].name, source_a, sg);
		const int ib = fake_op_scene_item_stack_index(sg->scenes[i].name, source_b, sg);
		if (ia >= 0 && ib >= 0) {
			if (out_scene && out_size > 0U) {
				std::snprintf(out_scene, out_size, "%s", sg->scenes[i].name);
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
	Q_UNUSED(afv);
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
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
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
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

static void fake_op_fill_catalog(struct mfd_ui_catalog *out, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
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
		std::memset(se, 0, sizeof(*se));
		std::snprintf(se->name, sizeof(se->name), "%s", sg->scenes[i].name);
		for (int j = 0; j < sg->scenes[i].item_count; ++j) {
			const char *lnm = sg->scenes[i].items[j];
			bool dup = false;
			for (size_t k = 0U; k < se->layer_count; ++k) {
				if (std::strcmp(se->layers[k], lnm) == 0) {
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
			std::snprintf(se->layers[se->layer_count], sizeof(se->layers[se->layer_count]), "%s", lnm);
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
		std::snprintf(ae->name, sizeof(ae->name), "%s", sg->sources[i].name);
		ae->db = mfd_mul_to_db(sg->sources[i].last_volume_mul);
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (!sg->filters[i].in_use) {
			continue;
		}
		const char *hnm = sg->filters[i].host_name;
		struct mfd_ui_filter_host_summary *existing = nullptr;
		for (size_t k = 0U; k < out->filter_host_count; ++k) {
			if (std::strcmp(out->filter_hosts[k].name, hnm) == 0) {
				existing = &out->filter_hosts[k];
				break;
			}
		}
		if (existing) {
			++existing->filter_count;
		} else if (out->filter_host_count < MFD_CATALOG_MAX_FILTER_HOSTS) {
			struct mfd_ui_filter_host_summary *fh = &out->filter_hosts[out->filter_host_count++];
			std::memset(fh, 0, sizeof(*fh));
			std::snprintf(fh->name, sizeof(fh->name), "%s", hnm);
			fh->is_scene = (fake_sg_find_scene(sg, hnm) != nullptr);
			fh->filter_count = 1U;
		}
	}
}

static struct fake_sg_filter *fake_sg_find_filter(struct fake_scene_graph *sg, const char *host_name,
						  const char *filter_name)
{
	if (!sg || !host_name || !filter_name) {
		return nullptr;
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (sg->filters[i].in_use && std::strcmp(sg->filters[i].host_name, host_name) == 0 &&
		    std::strcmp(sg->filters[i].filter_name, filter_name) == 0) {
			return &sg->filters[i];
		}
	}
	return nullptr;
}

static void fake_sg_add_filter(struct fake_scene_graph *sg, const char *host_name, const char *filter_name,
			       const char *type_id, bool enabled = true)
{
	if (fake_sg_find_filter(sg, host_name, filter_name)) {
		return;
	}
	for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
		if (!sg->filters[i].in_use) {
			std::memset(&sg->filters[i], 0, sizeof(sg->filters[i]));
			sg->filters[i].in_use = true;
			std::snprintf(sg->filters[i].host_name, sizeof(sg->filters[i].host_name), "%s", host_name);
			std::snprintf(sg->filters[i].filter_name, sizeof(sg->filters[i].filter_name), "%s",
				      filter_name);
			std::snprintf(sg->filters[i].type_id, sizeof(sg->filters[i].type_id), "%s", type_id);
			sg->filters[i].enabled = enabled;
			sg->filters[i].radius = 12.5;
			sg->filters[i].passes = 2;
			sg->filters[i].reactivity_pct = 50.0;
			return;
		}
	}
}

static void fake_op_enum_filter_hosts(mfd_filter_host_cb cb, void *cb_param, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
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
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	if (!sg || !source_name || !fake_op_source_exists(source_name, sg)) {
		return false;
	}
	if (out_is_scene) {
		*out_is_scene = (fake_sg_find_scene(sg, source_name) != nullptr);
	}
	if (cb) {
		for (int i = 0; i < FAKE_MAX_FILTERS; ++i) {
			if (sg->filters[i].in_use && std::strcmp(sg->filters[i].host_name, source_name) == 0) {
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
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_filter *flt = fake_sg_find_filter(sg, source_name, filter_name);
	if (!flt) {
		return nullptr;
	}
	obs_properties_t *props = obs_properties_create();
	if (std::strcmp(flt->type_id, "blur_filter") == 0) {
		obs_property_t *pr = obs_properties_add_float_slider(props, "radius", "Blur Radius", 0.0, 50.0, 0.5);
		obs_property_float_set_suffix(pr, " px");
		obs_properties_add_int_slider(props, "passes", "Passes", 1, 8, 1);
	} else if (std::strcmp(flt->type_id, "VisualSidechain.AudioModulator") == 0) {
		obs_properties_t *grp = obs_properties_create();
		obs_property_t *pa =
			obs_properties_add_float_slider(grp, "slot_1_amount_pct", "Slot 1 Amount", 0.0, 100.0, 1.0);
		obs_property_float_set_suffix(pa, "%");
		obs_properties_add_group(props, "slot_1_group", "Modulation Slot 1", OBS_GROUP_NORMAL, grp);
	}
	return props;
}

static obs_data_t *fake_op_get_filter_settings(const char *source_name, const char *filter_name, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_filter *flt = fake_sg_find_filter(sg, source_name, filter_name);
	if (!flt) {
		return nullptr;
	}
	obs_data_t *d = obs_data_create();
	if (std::strcmp(flt->type_id, "blur_filter") == 0) {
		obs_data_set_double(d, "radius", flt->radius);
		obs_data_set_int(d, "passes", flt->passes);
	} else if (std::strcmp(flt->type_id, "VisualSidechain.AudioModulator") == 0) {
		obs_data_set_double(d, "slot_1_amount_pct", flt->reactivity_pct);
	}
	return d;
}

static void fake_op_update_filter_settings(const char *source_name, const char *filter_name, obs_data_t *settings,
					   void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_filter *flt = fake_sg_find_filter(sg, source_name, filter_name);
	if (!flt || !settings) {
		return;
	}
	if (obs_data_has_user_value(settings, "radius")) {
		flt->radius = obs_data_get_double(settings, "radius");
	}
	if (obs_data_has_user_value(settings, "passes")) {
		flt->passes = static_cast<int>(obs_data_get_int(settings, "passes"));
	}
	if (obs_data_has_user_value(settings, "slot_1_amount_pct")) {
		flt->reactivity_pct = obs_data_get_double(settings, "slot_1_amount_pct");
	}
	++flt->update_writes;
	++sg->total_filter_writes;
}

static bool fake_op_get_filter_enabled(const char *source_name, const char *filter_name, bool *out_enabled,
				       void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_filter *flt = fake_sg_find_filter(sg, source_name, filter_name);
	if (!flt) {
		return false;
	}
	if (out_enabled) {
		*out_enabled = flt->enabled;
	}
	return true;
}

static void fake_op_set_filter_enabled(const char *source_name, const char *filter_name, bool enabled, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	struct fake_sg_filter *flt = fake_sg_find_filter(sg, source_name, filter_name);
	if (!flt) {
		return;
	}
	flt->enabled = enabled;
	++flt->enable_writes;
}

static void fake_op_set_studio_mode(bool enabled, void *user_data)
{
	auto *sg = static_cast<struct fake_scene_graph *>(user_data);
	if (sg && sg->owner_eng) {
		sg->owner_eng->studio_mode_override_set = true;
		sg->owner_eng->studio_mode_override = enabled;
	}
}

static const struct mfd_scene_graph_ops k_fake_sg_ops = {
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
};

template<typename T> static T *must_find(QObject *root, const QString &name)
{
	T *w = root->findChild<T *>(name);
	if (!w) {
		std::fprintf(stderr, "FAIL: missing widget '%s'\n", name.toUtf8().constData());
		std::exit(1);
	}
	return w;
}

static void capture_and_verify_png(QWidget *w, const QString &out_dir, const QString &filename)
{
	pump_ui();
	QPixmap pm = w->grab();
	REQUIRE(!pm.isNull());
	REQUIRE(pm.width() >= 520);
	REQUIRE(pm.height() >= 420);

	const QImage img = pm.toImage().convertToFormat(QImage::Format_RGBA8888);
	REQUIRE(!img.isNull());

	QSet<uint32_t> distinct_colors;
	const int step_x = std::max(1, img.width() / 64);
	const int step_y = std::max(1, img.height() / 48);
	for (int y = 0; y < img.height(); y += step_y) {
		for (int x = 0; x < img.width(); x += step_x) {
			const QRgb px = img.pixel(x, y);
			distinct_colors.insert(static_cast<uint32_t>(px) & 0x00FFFFFFU);
		}
	}
	REQUIRE(distinct_colors.size() >= 12);

	const QString full_path = QDir(out_dir).filePath(filename);
	REQUIRE(pm.save(full_path, "PNG"));
	const QFileInfo fi(full_path);
	REQUIRE(fi.exists());
	REQUIRE(fi.size() > 4096);
	std::printf("  captured %s (%dx%d, %lld bytes, %d sampled colors)\n", filename.toUtf8().constData(), pm.width(),
		    pm.height(), static_cast<long long>(fi.size()), static_cast<int>(distinct_colors.size()));
}

} // namespace

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	REQUIRE(QGuiApplication::platformName() == QStringLiteral("windows"));

	QString screenshot_dir = QDir::current().filePath(QStringLiteral("ui_screenshots"));
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], "--export-screenshots") == 0) {
			screenshot_dir = QString::fromUtf8(argv[i + 1]);
			++i;
		}
	}
	REQUIRE(QDir().mkpath(screenshot_dir));

	/* Initialize standalone engine with recursive state_mutex and fake OBS scene graph */
	struct mfd_engine eng {};
	REQUIRE(mfd_engine_init_mutex(&eng));
	mfd_midi_queue_init(&eng.midi_queue);
	mfd_midi_parser_init(&eng.midi_parser);
	eng.learning_slot_index = -1;
	eng.last_moved_slot_index = -1;
	eng.smooth_tau_ms = 0.0f;
	eng.tbar_bidirectional_flip = true;
	eng.tbar_driver = mock_tbar_driver;
	eng.tbar_unlatch = mock_tbar_unlatch;
	std::snprintf(eng.configured_device_name, sizeof(eng.configured_device_name), "APC40 mkII");
	std::snprintf(eng.midi_dev.opened_name, sizeof(eng.midi_dev.opened_name), "APC40 mkII");
	eng.midi_dev.is_open = true;
	eng.midi_dev.error_kind = MFD_WINMM_OK;

	struct fake_scene_graph sg {};
	sg.owner_eng = &eng;
	eng.scene_graph_ops = &k_fake_sg_ops;
	eng.scene_graph_user_data = &sg;

	fake_sg_add_source(&sg, "Color", false);
	fake_sg_add_source(&sg, "Color 2", false);
	fake_sg_add_source(&sg, "Cam", true, 1.0f);
	fake_sg_add_source(&sg, "Desktop Audio", true, 0.501187f); /* ~ -6.0 dB */
	fake_sg_add_source(&sg, "Mic/Aux", true, 1.0f);            /* 0.0 dB */
	fake_sg_add_source(&sg, "Guest Mic", true, 0.75f);
	fake_sg_add_source(&sg, "Title Card", false);

	fake_sg_add_scene(&sg, "Red");
	fake_sg_scene_add_item(&sg, "Red", "Color");
	fake_sg_scene_add_item(&sg, "Red", "Color 2");

	fake_sg_add_scene(&sg, "Blue");
	fake_sg_scene_add_item(&sg, "Blue", "Cam");
	fake_sg_scene_add_item(&sg, "Blue", "Color 2"); /* shared across Red and Blue */

	fake_sg_add_scene(&sg, "SoloScene");
	fake_sg_scene_add_item(&sg, "SoloScene", "Title Card"); /* 1-layer scene */

	fake_sg_add_scene(&sg, "OldScene");
	fake_sg_scene_add_item(&sg, "OldScene", "Cam");

	fake_sg_add_filter(&sg, "Cam", "Blur", "blur_filter", true);
	fake_sg_add_filter(&sg, "Blue", "AudioMod", "VisualSidechain.AudioModulator", true);

	mfd_engine_load_apc40_defaults(&eng);
	eng.first_run_offer_dismissed = false;
	mfd_engine_set_test_frontend_state(&eng, true, true, "Blue", "Red", true);

	std::unique_ptr<QDialog> dlg(mfd_ui_create_settings_dialog(&eng, nullptr));
	REQUIRE(dlg != nullptr);
	dlg->setAttribute(Qt::WA_DontShowOnScreen, true);
	dlg->show();
	pump_ui();

	/* --- 1. First-Run Offer: screenshot, [I'll set it up], and [Yes] --- */
	auto *fr_banner = must_find<QWidget>(dlg.get(), QStringLiteral("mfdFirstRunBanner"));
	auto *fr_text = must_find<QLabel>(dlg.get(), QStringLiteral("mfdFirstRunText"));
	auto *fr_yes = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdFirstRunYesBtn"));
	auto *fr_custom = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdFirstRunCustomBtn"));
	REQUIRE(!fr_banner->isHidden());
	REQUIRE(fr_text->text().contains(QStringLiteral("Start with the usual layout?")));

	auto *z1_name = must_find<QLabel>(dlg.get(), QStringLiteral("mfdControllerNameLabel"));
	auto *z1_status = must_find<QLabel>(dlg.get(), QStringLiteral("mfdControllerStatusLabel"));
	auto *z1_reconnect = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnReconnect"));
	REQUIRE(z1_name->text() == QStringLiteral("Controller: APC40 mkII"));
	REQUIRE(z1_status->text() == QStringLiteral("Connected"));
	REQUIRE(z1_reconnect->isHidden());

	capture_and_verify_png(dlg.get(), screenshot_dir, QStringLiteral("01-first-run-offer.png"));

	/* Click [I'll set it up] -> all 10 strips become Not used, APC40 CCs remain bound and live */
	fr_custom->click();
	pump_ui();
	REQUIRE(fr_banner->isHidden());
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		auto *assign_lbl = must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_%1").arg(i));
		REQUIRE(assign_lbl->text() == QStringLiteral("Not used. Click to set up."));
		REQUIRE(eng.fader_slots[i].addr.type == MFD_MIDI_CC);
	}
	const int writes_before_unassigned = g_mock_tbar_set_writes + sg.total_opacity_writes + sg.total_volume_writes;
	push_midi_cc(&eng, 0, 15, 64); /* Crossfader CC 15 = 50% */
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	auto *bar0 = must_find<QWidget>(dlg.get(), QStringLiteral("mfdStripBar_0"));
	REQUIRE(bar0->property("barPercent").toInt() == 50);
	REQUIRE((g_mock_tbar_set_writes + sg.total_opacity_writes + sg.total_volume_writes) ==
		writes_before_unassigned);

	/* Re-arm First-Run Offer and click [Yes] -> Crossfader = T-bar, Master = Desktop Audio volume */
	mfd_engine_lock(&eng);
	eng.first_run_offer_dismissed = false;
	mfd_engine_mark_ui_dirty(&eng, true);
	mfd_engine_unlock(&eng);
	pump_ui();
	REQUIRE(!fr_banner->isHidden());

	fr_yes->click();
	pump_ui();
	REQUIRE(fr_banner->isHidden());
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_0"))->text() ==
		QStringLiteral("Switches scenes (T-bar)"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_0"))->text() == QStringLiteral("Ready"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_9"))->text() ==
		QStringLiteral("Volume: Desktop Audio"));

	/* --- 2. Studio Mode Off & One-Click Recovery --- */
	mfd_engine_set_test_frontend_state(&eng, true, false, "", "Red", true);
	pump_ui();

	auto *studio_banner = must_find<QWidget>(dlg.get(), QStringLiteral("mfdStudioNote"));
	auto *strip0_status = must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_0"));
	auto *strip0_action = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdStripActionBtn_0"));
	auto *z3_label = must_find<QLabel>(dlg.get(), QStringLiteral("mfdZone3StatusLabel"));
	REQUIRE(!studio_banner->isHidden());
	REQUIRE(!strip0_status->isHidden());
	REQUIRE(strip0_status->text() == QStringLiteral("Studio Mode is off."));
	REQUIRE(strip0_action->text() == QStringLiteral("Turn on Studio Mode"));
	REQUIRE(z3_label->text().startsWith(QStringLiteral("Studio Mode off")));

	/* Moving physical crossfader while Studio Mode is off updates bar to 63% with zero T-bar writes */
	const int tbar_writes_before = g_mock_tbar_set_writes;
	push_midi_cc(&eng, 0, 15, 80); /* 80 / 127 = 63% */
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	REQUIRE(bar0->property("barPercent").toInt() == 63);
	REQUIRE(g_mock_tbar_set_writes == tbar_writes_before);

	capture_and_verify_png(dlg.get(), screenshot_dir, QStringLiteral("02-studio-mode-off.png"));

	/* Click Crossfader's [Turn on Studio Mode] button */
	strip0_action->click();
	mfd_engine_set_test_frontend_state(&eng, true, true, "Blue", "Red", true);
	pump_ui();
	REQUIRE(studio_banner->isHidden());
	REQUIRE(strip0_status->isHidden());
	REQUIRE(strip0_action->text() == QStringLiteral("Set up"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_0"))->text() == QStringLiteral("Ready"));

	/* --- 3. Active Strips Across All Modes + Throwing & [Finish now] --- */
	{
		/* Strip 1: Mix two layers (Color & Color 2 in Red) */
		struct mfd_slot_config_edit e1 {};
		e1.enabled = true;
		e1.target_type = MFD_TARGET_TWO_SOURCE_MIX;
		e1.preserve_scene = false;
		std::snprintf(e1.scene_name, sizeof(e1.scene_name), "Red");
		std::snprintf(e1.source_a_name, sizeof(e1.source_a_name), "Color");
		std::snprintf(e1.source_b_name, sizeof(e1.source_b_name), "Color 2");
		e1.curve = MFD_CURVE_DIP_FREE;
		mfd_engine_update_slot_config(&eng, 1, &e1);

		/* Strip 2: Show or hide a layer (Cam in Blue) */
		struct mfd_slot_config_edit e2 {};
		e2.enabled = true;
		e2.target_type = MFD_TARGET_SINGLE_LAYER;
		e2.preserve_scene = false;
		std::snprintf(e2.scene_name, sizeof(e2.scene_name), "Blue");
		std::snprintf(e2.source_a_name, sizeof(e2.source_a_name), "Cam");
		mfd_engine_update_slot_config(&eng, 2, &e2);

		/* Strip 3: Volume (Mic/Aux) */
		struct mfd_slot_config_edit e3 {};
		e3.enabled = true;
		e3.target_type = MFD_TARGET_AUDIO_VOLUME;
		std::snprintf(e3.source_a_name, sizeof(e3.source_a_name), "Mic/Aux");
		mfd_engine_update_slot_config(&eng, 3, &e3);
	}

	/* Drive live MIDI into strips 0, 1, 2, 3, and 9 (crossing initial Soft Takeover targets first) */
	push_midi_cc(&eng, 0, 7, 0);    /* Strip 1 pickup at 0% */
	push_midi_cc(&eng, 0, 7, 61);   /* Strip 1 (Ch 1 CC 7): 48% */
	push_midi_cc(&eng, 1, 7, 127);  /* Strip 2 pickup at 100% */
	push_midi_cc(&eng, 1, 7, 95);   /* Strip 2 (Ch 2 CC 7): 75% */
	push_midi_cc(&eng, 2, 7, 127);  /* Strip 3 pickup at 100% */
	push_midi_cc(&eng, 2, 7, 101);  /* Strip 3 (Ch 3 CC 7): 80% -> -6.0 dB */
	push_midi_cc(&eng, 0, 14, 127); /* Master (Ch 1 CC 14): 100% -> 0.0 dB */
	push_midi_cc(&eng, 0, 15, 0);   /* Crossfader origin */
	mfd_engine_tick(&eng, 0.016f);
	push_midi_cc(&eng, 0, 15, 83); /* Crossfader throwing 63% after 6% start-band rescale */
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();

	auto *strip0_finish = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdStripFinishBtn_0"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_0"))->text() ==
		QStringLiteral("Throwing 63%"));
	REQUIRE(!strip0_finish->isHidden());
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_1"))->text() ==
		QStringLiteral("Mixes Color and Color 2 in Red"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_1"))->text() ==
		QStringLiteral("Color 100%, Color 2 48%"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_2"))->text() ==
		QStringLiteral("Shows or hides Cam in Blue"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_2"))->text() ==
		QStringLiteral("Cam 75% visible"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_3"))->text() ==
		QStringLiteral("Volume: Mic/Aux"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_3"))->text() == QStringLiteral("-6.0 dB"));
	REQUIRE(z3_label->text().contains(QStringLiteral("Preview: Blue \xE2\x86\x92 Program: Red")));
	REQUIRE(z3_label->text().contains(QStringLiteral("Last move: Crossfader")));

	capture_and_verify_png(dlg.get(), screenshot_dir, QStringLiteral("03-strips-throwing-and-active.png"));

	/* Click [Finish now] on Crossfader -> completes throw and parks ("At end.") */
	strip0_finish->click();
	pump_ui();
	REQUIRE(strip0_finish->isHidden());
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_0"))->text() == QStringLiteral("At end."));

	/* --- 4. Pickup Pending Arrows + All Red Precondition Sentences + Controller Busy --- */
	{
		/* Put Strip 1 into pickup waiting towards 12% while physical fader is at 48% */
		char err_tmp[128] = {0};
		REQUIRE(mfd_engine_set_slot_value(&eng, 1, 0.12f, false, "api", err_tmp, sizeof(err_tmp)));

		/* Strip 4: Mix two layers in Red, then remove Color 2 from Red */
		struct mfd_slot_config_edit e4 {};
		e4.enabled = true;
		e4.target_type = MFD_TARGET_TWO_SOURCE_MIX;
		e4.preserve_scene = false;
		std::snprintf(e4.scene_name, sizeof(e4.scene_name), "Red");
		std::snprintf(e4.source_a_name, sizeof(e4.source_a_name), "Color");
		std::snprintf(e4.source_b_name, sizeof(e4.source_b_name), "Color 2");
		mfd_engine_update_slot_config(&eng, 4, &e4);

		/* Strip 5: Show or hide Cam in OldScene, then delete OldScene */
		struct mfd_slot_config_edit e5 {};
		e5.enabled = true;
		e5.target_type = MFD_TARGET_SINGLE_LAYER;
		e5.preserve_scene = false;
		std::snprintf(e5.scene_name, sizeof(e5.scene_name), "OldScene");
		std::snprintf(e5.source_a_name, sizeof(e5.source_a_name), "Cam");
		mfd_engine_update_slot_config(&eng, 5, &e5);

		/* Strip 6: Volume on Guest Mic, then delete Guest Mic */
		struct mfd_slot_config_edit e6 {};
		e6.enabled = true;
		e6.target_type = MFD_TARGET_AUDIO_VOLUME;
		std::snprintf(e6.source_a_name, sizeof(e6.source_a_name), "Guest Mic");
		mfd_engine_update_slot_config(&eng, 6, &e6);

		/* Strip 7: Volume on Title Card (has no audio) */
		struct mfd_slot_config_edit e7 {};
		e7.enabled = true;
		e7.target_type = MFD_TARGET_AUDIO_VOLUME;
		std::snprintf(e7.source_a_name, sizeof(e7.source_a_name), "Title Card");
		mfd_engine_update_slot_config(&eng, 7, &e7);

		fake_sg_scene_remove_item(&sg, "Red", "Color 2");
		fake_sg_remove_scene(&sg, "OldScene");
		fake_sg_remove_source(&sg, "Guest Mic");

		mfd_engine_lock(&eng);
		eng.midi_dev.is_open = false;
		eng.midi_dev.error_kind = MFD_WINMM_ERR_BUSY_ALLOCATED;
		mfd_engine_reevaluate_slots(&eng, true);
		mfd_engine_unlock(&eng);
		pump_ui();
	}

	REQUIRE(z1_status->text() ==
		QStringLiteral("Another program has the APC40 mkII open. Close it, then press Reconnect."));
	REQUIRE(!z1_reconnect->isHidden());

	auto *bar1 = must_find<QWidget>(dlg.get(), QStringLiteral("mfdStripBar_1"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_1"))->text() ==
		QStringLiteral("Color 2 is no longer in scene Red."));
	/* Restore Color 2 briefly to verify Strip 1's pickup sentence while Strips 5, 6, 7 stay red */
	fake_sg_scene_add_item(&sg, "Red", "Color 2");
	mfd_engine_reevaluate_slots(&eng, true);
	pump_ui();
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_1"))->text() ==
		QStringLiteral("Move the fader down to 12% to pick up."));
	REQUIRE(bar1->property("showingPickup").toBool());
	REQUIRE(bar1->property("arrowGlyph").toString() == QStringLiteral("\xE2\x86\x93"));

	/* Remove Color 2 from Red exclusively for Strip 4 by pinning Strip 1 to Blue */
	{
		struct mfd_slot_config_edit e1b {};
		e1b.enabled = true;
		e1b.target_type = MFD_TARGET_TWO_SOURCE_MIX;
		e1b.preserve_scene = false;
		std::snprintf(e1b.scene_name, sizeof(e1b.scene_name), "Blue");
		std::snprintf(e1b.source_a_name, sizeof(e1b.source_a_name), "Cam");
		std::snprintf(e1b.source_b_name, sizeof(e1b.source_b_name), "Color 2");
		mfd_engine_update_slot_config(&eng, 1, &e1b);
		push_midi_cc(&eng, 0, 7, 0);
		push_midi_cc(&eng, 0, 7, 61);
		mfd_engine_tick(&eng, 0.016f);
		char err_tmp[128] = {0};
		REQUIRE(mfd_engine_set_slot_value(&eng, 1, 0.12f, false, "api", err_tmp, sizeof(err_tmp)));
		fake_sg_scene_remove_item(&sg, "Red", "Color 2");
		mfd_engine_reevaluate_slots(&eng, true);
		pump_ui();
	}

	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_1"))->text() ==
		QStringLiteral("Move the fader down to 12% to pick up."));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_4"))->text() ==
		QStringLiteral("Color 2 is no longer in scene Red."));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_5"))->text() ==
		QStringLiteral("Scene OldScene was deleted."));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_6"))->text() ==
		QStringLiteral("Guest Mic was deleted."));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_7"))->text() ==
		QStringLiteral("Title Card has no audio."));

	capture_and_verify_png(dlg.get(), screenshot_dir, QStringLiteral("04-pickup-pending-and-errors.png"));

	/* Restore clean controller state and Red scene items, clear strips 4..7 */
	fake_sg_scene_add_item(&sg, "Red", "Color 2");
	for (int sidx = 4; sidx <= 7; ++sidx) {
		mfd_engine_unassign_slot_preserve_addr(&eng, sidx);
	}
	mfd_engine_lock(&eng);
	eng.midi_dev.is_open = true;
	eng.midi_dev.error_kind = MFD_WINMM_OK;
	mfd_engine_reevaluate_slots(&eng, true);
	mfd_engine_unlock(&eng);
	pump_ui();

	/* --- 5. Modeless 3-Step Set up Sheet: Cards, Pickers, Warnings, Live Meter, Bounce-Back & Conflict --- */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdStripActionBtn_1"))->click();
	pump_ui();

	auto *sheet = must_find<QDialog>(dlg.get(), QStringLiteral("mfdSetupSheet"));
	auto *stack = must_find<QStackedWidget>(sheet, QStringLiteral("mfdSetupStack"));
	REQUIRE(!sheet->isHidden());
	REQUIRE(stack->currentIndex() == 0);

	capture_and_verify_png(sheet, screenshot_dir, QStringLiteral("05-setup-step1-cards.png"));

	/* Click "Mix two layers" card -> Step 2 */
	must_find<QPushButton>(sheet, QStringLiteral("mfdCardMixTwoLayers"))->click();
	pump_ui();
	REQUIRE(stack->currentIndex() == 1);

	auto *combo_scene = must_find<QComboBox>(sheet, QStringLiteral("mfdComboScene"));
	auto *combo_left = must_find<QComboBox>(sheet, QStringLiteral("mfdComboLeft"));
	auto *combo_right = must_find<QComboBox>(sheet, QStringLiteral("mfdComboRight"));
	auto *same_refusal = must_find<QLabel>(sheet, QStringLiteral("mfdSameLayerRefusal"));
	auto *live_note = must_find<QLabel>(sheet, QStringLiteral("mfdLiveSceneNote"));
	auto *shared_note = must_find<QLabel>(sheet, QStringLiteral("mfdSharedLayerNote"));
	auto *btn_next = must_find<QPushButton>(sheet, QStringLiteral("mfdSetupNextBtn"));

	/* 1-layer scene SoloScene MUST be filtered out of Mix two layers */
	REQUIRE(combo_scene->findData(QStringLiteral("SoloScene")) == -1);
	const int red_idx = combo_scene->findData(QStringLiteral("Red"));
	REQUIRE(red_idx >= 0);
	combo_scene->setCurrentIndex(red_idx);
	pump_ui();

	/* Select same layer on Left and Right -> inline refusal + Next disabled */
	combo_left->setCurrentIndex(combo_left->findData(QStringLiteral("Color 2")));
	combo_right->setCurrentIndex(combo_right->findData(QStringLiteral("Color 2")));
	pump_ui();
	REQUIRE(!same_refusal->isHidden());
	REQUIRE(same_refusal->text() == QStringLiteral("Left and Right cannot be the same layer."));
	REQUIRE(!btn_next->isEnabled());

	/* Select Color and Color 2 -> refusal hides, live note & shared-layer note appear, Next enabled */
	combo_left->setCurrentIndex(combo_left->findData(QStringLiteral("Color")));
	combo_right->setCurrentIndex(combo_right->findData(QStringLiteral("Color 2")));
	pump_ui();
	REQUIRE(same_refusal->isHidden());
	REQUIRE(live_note->text() == QStringLiteral("You will see this when scene Red is live."));
	REQUIRE(!shared_note->isHidden());
	REQUIRE(shared_note->text() == QStringLiteral("Color 2 is also in scene Blue; it will fade there too."));
	REQUIRE(btn_next->isEnabled());

	capture_and_verify_png(sheet, screenshot_dir, QStringLiteral("06-setup-step2-pickers.png"));

	/* Advance to Step 3 ("Try it.") */
	btn_next->click();
	pump_ui();
	REQUIRE(stack->currentIndex() == 2);

	auto *fin_lbl = must_find<QLabel>(sheet, QStringLiteral("mfdFinishedSentence"));
	auto *cand_meter = must_find<QProgressBar>(sheet, QStringLiteral("mfdCandidateMeter"));
	auto *btn_learn = must_find<QPushButton>(sheet, QStringLiteral("mfdSetupLearnBtn"));
	auto *collision_lbl = must_find<QLabel>(sheet, QStringLiteral("mfdCollisionNotice"));
	auto *err_banner = must_find<QLabel>(sheet, QStringLiteral("mfdSetupErrorBanner"));
	auto *btn_done = must_find<QPushButton>(sheet, QStringLiteral("mfdSetupDoneBtn"));

	REQUIRE(fin_lbl->text() == QStringLiteral("1 mixes Color and Color 2 in scene Red"));

	/* Move physical fader 1 -> candidate meter updates to 75% while live opacity writes stay frozen */
	const int op_writes_during_setup = sg.total_opacity_writes;
	push_midi_cc(&eng, 0, 7, 95); /* 95 / 127 = 75% */
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	REQUIRE(cand_meter->value() == 75);
	REQUIRE(cand_meter->format().contains(QStringLiteral("75%")));
	REQUIRE(sg.total_opacity_writes == op_writes_during_setup);

	/* Learn Strip 2's hardware CC -> shows collision notice naming strip 2 */
	btn_learn->click();
	pump_ui();
	push_midi_cc(&eng, 1, 7, 80); /* Ch 2 CC 7 belongs to strip 2 */
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	REQUIRE(!collision_lbl->isHidden());
	REQUIRE(collision_lbl->text() ==
		QStringLiteral("Currently assigned to strip 2. Pressing Done will move it to this strip."));

	capture_and_verify_png(sheet, screenshot_dir, QStringLiteral("07-setup-step3-try-it.png"));

	/* Learn back Strip 1's own CC (Ch 1 CC 7) so Strip 2 is not displaced */
	btn_learn->click();
	pump_ui();
	push_midi_cc(&eng, 0, 7, 95);
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	REQUIRE(collision_lbl->isHidden());

	/* Mid-dialog invalidation: remove Color 2 from Red and click Done -> bounces back to Step 2 with error */
	fake_sg_scene_remove_item(&sg, "Red", "Color 2");
	btn_done->click();
	pump_ui();
	REQUIRE(!sheet->isHidden());
	REQUIRE(stack->currentIndex() == 1);
	REQUIRE(!err_banner->isHidden());
	REQUIRE(err_banner->text() == QStringLiteral("Color 2 is no longer in scene Red."));

	/* Restore Color 2 into Red, re-pick Red / Color / Color 2, go back to Step 3 */
	fake_sg_scene_add_item(&sg, "Red", "Color 2");
	mfd_engine_reevaluate_slots(&eng, true);
	pump_ui();
	combo_scene->setCurrentIndex(combo_scene->findData(QStringLiteral("Red")));
	combo_left->setCurrentIndex(combo_left->findData(QStringLiteral("Color")));
	combo_right->setCurrentIndex(combo_right->findData(QStringLiteral("Color 2")));
	pump_ui();
	btn_next->click();
	pump_ui();
	REQUIRE(stack->currentIndex() == 2);

	/* Simulate concurrent external edit (bumps slot_config_rev[1]) -> first Done warns, second Done commits */
	mfd_engine_set_slot_label(&eng, 1, "Fader 1");
	pump_ui();
	btn_done->click();
	pump_ui();
	REQUIRE(!sheet->isHidden());
	REQUIRE(!err_banner->isHidden());
	REQUIRE(err_banner->text().contains(QStringLiteral("changed externally")));

	btn_done->click();
	pump_ui();
	REQUIRE(sheet->isHidden());
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_1"))->text() ==
		QStringLiteral("Mixes Color and Color 2 in Red"));

	/* --- 6. [Show controller] Live SVG Desk Map --- */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnShowController"))->click();
	pump_ui();
	auto *svg_dlg = must_find<QDialog>(dlg.get(), QStringLiteral("mfdControllerSvgDialog"));
	REQUIRE(!svg_dlg->isHidden());
	auto *svg_canvas =
		static_cast<MfdSvgCanvas *>(must_find<QWidget>(svg_dlg, QStringLiteral("mfdControllerSvgCanvas")));
	REQUIRE(svg_canvas != nullptr);
	REQUIRE(svg_canvas->is_valid());
	{
		struct mfd_ui_snapshot svg_snap {};
		mfd_engine_get_ui_snapshot(&eng, &svg_snap);
		const QByteArray desk_xml = mfd_make_apc40_desk_svg(svg_snap);
		REQUIRE(desk_xml.contains("Clip launch pads"));
		REQUIRE(desk_xml.contains("Scene pads"));
	}

	capture_and_verify_png(svg_dlg, screenshot_dir, QStringLiteral("08-show-controller-svg.png"));
	svg_dlg->close();
	pump_ui();

	/* --- 7. Advanced Tab Round-Trip Synchronization --- */
	auto *tabs = must_find<QTabWidget>(dlg.get(), QStringLiteral("mfdMainTabs"));
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnOpenAdvanced"))->click();
	pump_ui();
	REQUIRE(tabs->currentIndex() == 1);

	auto *table = must_find<QTableWidget>(dlg.get(), QStringLiteral("mfdAdvancedTable"));
	auto *adv_help = must_find<QLabel>(dlg.get(), QStringLiteral("mfdAdvHelpLabel"));
	REQUIRE(!adv_help->isHidden());
	REQUIRE(adv_help->text().contains(QStringLiteral("Switch scenes (T-bar):")));
	REQUIRE(adv_help->text().contains(QStringLiteral("Mix two layers:")));
	REQUIRE(table->horizontalHeaderItem(4)->text() == QStringLiteral("What it does"));
	REQUIRE(table->horizontalHeaderItem(5)->text() == QStringLiteral("Left"));
	REQUIRE(table->horizontalHeaderItem(6)->text() == QStringLiteral("Right"));
	REQUIRE(table->horizontalHeaderItem(7)->text() == QStringLiteral("Feel"));
	REQUIRE(table->horizontalHeaderItem(8)->text() == QStringLiteral("Also fade audio"));

	auto *adv_mode1 = must_find<QComboBox>(dlg.get(), QStringLiteral("mfdAdvMode_1"));
	REQUIRE(adv_mode1->currentData().toInt() == static_cast<int>(MFD_TARGET_TWO_SOURCE_MIX));

	/* Edit Strip 4 in Advanced -> Volume on Mic/Aux, capture screenshot, verify Strips tab reflects it */
	auto *adv_mode4 = must_find<QComboBox>(dlg.get(), QStringLiteral("mfdAdvMode_4"));
	adv_mode4->setCurrentIndex(adv_mode4->findData(static_cast<int>(MFD_TARGET_AUDIO_VOLUME)));
	pump_ui();
	auto *adv_left4 = must_find<QComboBox>(dlg.get(), QStringLiteral("mfdAdvLeft_4"));
	const int mic_idx = adv_left4->findData(QStringLiteral("Mic/Aux"));
	REQUIRE(mic_idx >= 0);
	adv_left4->setCurrentIndex(mic_idx);
	pump_ui();

	capture_and_verify_png(dlg.get(), screenshot_dir, QStringLiteral("09-advanced-tab.png"));

	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnBackToStrips"))->click();
	pump_ui();
	REQUIRE(tabs->currentIndex() == 0);
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_4"))->text() ==
		QStringLiteral("Volume: Mic/Aux"));

	/* --- 8. Multi-Controller Beta Profiles (MIDImix, nanoKONTROL2, APC mini mk2, Launch Control XL) --- */
	auto *beta_badge = must_find<QLabel>(dlg.get(), QStringLiteral("mfdBetaProfileBadge"));
	auto *adv_beta_note = must_find<QLabel>(dlg.get(), QStringLiteral("mfdAdvBetaNote"));
	auto *adv_defaults_btn = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdAdvDefaultsBtn"));
	REQUIRE(beta_badge->isHidden());
	for (int i = 0; i < (int)MFD_FADER_STRIP_COUNT; ++i) {
		REQUIRE(!must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
	}

	/* Switch to Beta profile: Akai MIDImix (9 vertical faders: 1..8 + Master, no Crossfader) */
	REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_AKAI_MIDIMIX, false));
	pump_ui();
	REQUIRE(!beta_badge->isHidden());
	REQUIRE(beta_badge->text() == QStringLiteral("BETA: MIDImix (untested spec)"));
	REQUIRE(must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_0"))->isHidden());
	for (int i = 1; i <= 9; ++i) {
		REQUIRE(!must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
	}
	/* Strip 1 preserved its OBS assignment while rebinding to MIDImix Fader 1 (Ch 1 CC 19) */
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_1"))->text() ==
		QStringLiteral("Mixes Color and Color 2 in Red"));
	REQUIRE(eng.fader_slots[1].addr.channel == 0U && eng.fader_slots[1].addr.number == 19U);
	push_midi_cc(&eng, 0, 19, 127);
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	REQUIRE(must_find<QWidget>(dlg.get(), QStringLiteral("mfdStripBar_1"))->property("barPercent").toInt() == 100);

	/* Open [Show controller] on Akai MIDImix -> verify generalized SVG hardware map & export screenshot */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnShowController"))->click();
	pump_ui();
	svg_dlg = must_find<QDialog>(dlg.get(), QStringLiteral("mfdControllerSvgDialog"));
	REQUIRE(!svg_dlg->isHidden());
	svg_canvas = static_cast<MfdSvgCanvas *>(must_find<QWidget>(svg_dlg, QStringLiteral("mfdControllerSvgCanvas")));
	REQUIRE(svg_canvas->is_valid());
	{
		struct mfd_ui_snapshot mm_snap {};
		mfd_engine_get_ui_snapshot(&eng, &mm_snap);
		const QByteArray mm_xml = mfd_make_controller_desk_svg(mm_snap);
		REQUIRE(mm_xml.contains("Akai MIDImix"));
		REQUIRE(mm_xml.contains("BETA"));
		REQUIRE(mm_xml.contains("Channel knobs"));
		REQUIRE(mm_xml.contains("Ch 1 CC 19"));
		REQUIRE(mm_xml.contains("Ch 1 CC 62"));
	}
	capture_and_verify_png(svg_dlg, screenshot_dir, QStringLiteral("10-beta-midimix-svg.png"));
	svg_dlg->close();
	pump_ui();

	/* Switch to Beta profile: Korg nanoKONTROL2 (8 vertical sliders CC 0..7, no Crossfader, no Master) */
	REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_KORG_NANOKONTROL2, false));
	pump_ui();
	REQUIRE(!beta_badge->isHidden());
	REQUIRE(beta_badge->text() == QStringLiteral("BETA: nanoKONTROL2 (untested spec)"));
	REQUIRE(must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_0"))->isHidden());
	REQUIRE(must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_9"))->isHidden());
	for (int i = 1; i <= 8; ++i) {
		REQUIRE(!must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
	}

	/* Verify Advanced tab Beta notice & dynamic Load Defaults button */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnOpenAdvanced"))->click();
	pump_ui();
	REQUIRE(!adv_beta_note->isHidden());
	REQUIRE(adv_beta_note->text().contains(QStringLiteral("Beta (untested on hardware)")));
	REQUIRE(adv_defaults_btn->text() == QStringLiteral("Load nanoKONTROL2 Defaults"));

	/* Verify SVG validity across every registered controller profile (including Generic, APC mini mk2, LCXL) */
	for (size_t pi = 0U; pi < mfd_profile_count(); ++pi) {
		const struct mfd_controller_profile_def *pdef = mfd_profile_get_by_index(pi);
		REQUIRE(pdef != nullptr);
		REQUIRE(mfd_engine_select_profile(&eng, pdef->id, false));
		struct mfd_ui_snapshot psnap {};
		mfd_engine_get_ui_snapshot(&eng, &psnap);
		MfdSvgCanvas tmp_svg;
		tmp_svg.set_svg_utf8(mfd_make_controller_desk_svg(psnap));
		REQUIRE(tmp_svg.is_valid());
	}

	/* Restore verified Akai APC40 mkII profile -> Beta notices hide and all 10 strips return */
	REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_APC40_MK2, false));
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnBackToStrips"))->click();
	pump_ui();
	REQUIRE(beta_badge->isHidden());
	REQUIRE(adv_beta_note->isHidden());
	REQUIRE(dlg->width() <= 1380);
	for (int i = 0; i < (int)MFD_FADER_STRIP_COUNT; ++i) {
		REQUIRE(!must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
	}
	for (int i = (int)MFD_FADER_STRIP_COUNT; i < MFD_MAX_FADER_SLOTS; ++i) {
		REQUIRE(must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
	}

	/* --- 9. 0.4.0 FX & Knobs Bank, Filter or Effect Setting Set up, Clean / All FX Off & Advanced 18-Row --- */
	auto *btn_bank_faders = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnBankFaders"));
	auto *btn_bank_fx = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnBankFx"));
	auto *btn_clean_fx = must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnCleanAllFx"));
	REQUIRE(btn_bank_faders->isChecked());
	REQUIRE(!btn_bank_fx->isChecked());

	/* Switch to [FX & Knobs (8)] bank -> slots 0..9 hide, slots 10..17 (FX 1 .. FX 8) show */
	btn_bank_fx->click();
	pump_ui();
	REQUIRE(!btn_bank_faders->isChecked());
	REQUIRE(btn_bank_fx->isChecked());
	for (int i = 0; i < (int)MFD_FADER_STRIP_COUNT; ++i) {
		REQUIRE(must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
	}
	for (int i = (int)MFD_FADER_STRIP_COUNT; i < MFD_MAX_FADER_SLOTS; ++i) {
		REQUIRE(!must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
		REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripTitle_%1").arg(i))->text() ==
			QStringLiteral("FX %1").arg(i - 9));
	}

	/* Switch to knobless APC mini mk2 -> FX 1..8 remain visible & enabled with zero red "No MIDI control bound." */
	REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_APC_MINI_MK2, false));
	pump_ui();
	for (int i = (int)MFD_FADER_STRIP_COUNT; i < MFD_MAX_FADER_SLOTS; ++i) {
		REQUIRE(!must_find<QWidget>(dlg.get(), QStringLiteral("mfdStrip_%1").arg(i))->isHidden());
		REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripStatus_%1").arg(i))->isHidden());
	}
	REQUIRE(mfd_engine_select_profile(&eng, MFD_PROFILE_APC40_MK2, false));
	pump_ui();

	/* Open Set up on FX 1 (slot 10) -> defaults to Filter or effect setting card checked */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdStripActionBtn_10"))->click();
	pump_ui();
	sheet = must_find<QDialog>(dlg.get(), QStringLiteral("mfdSetupSheet"));
	stack = must_find<QStackedWidget>(sheet, QStringLiteral("mfdSetupStack"));
	auto *card_filter = must_find<QPushButton>(sheet, QStringLiteral("mfdCardFilterSetting"));
	REQUIRE(!sheet->isHidden());
	REQUIRE(stack->currentIndex() == 0);
	REQUIRE(card_filter->isChecked());

	/* Click Filter or effect setting card -> Step 2 */
	card_filter->click();
	pump_ui();
	REQUIRE(stack->currentIndex() == 1);

	auto *combo_fhost = must_find<QComboBox>(sheet, QStringLiteral("mfdComboFilterHost"));
	auto *combo_fname = must_find<QComboBox>(sheet, QStringLiteral("mfdComboFilterName"));
	auto *combo_fprop = must_find<QComboBox>(sheet, QStringLiteral("mfdComboFilterProp"));
	auto *spin_ffrom = must_find<QDoubleSpinBox>(sheet, QStringLiteral("mfdSpinFilterFrom"));
	auto *spin_fto = must_find<QDoubleSpinBox>(sheet, QStringLiteral("mfdSpinFilterTo"));
	auto *btn_use_cur_from = must_find<QPushButton>(sheet, QStringLiteral("mfdBtnUseCurrentFrom"));
	auto *chk_fbypass = must_find<QCheckBox>(sheet, QStringLiteral("mfdChkFilterBypass"));
	auto *flt_refusal = must_find<QLabel>(sheet, QStringLiteral("mfdFilterValidationRefusal"));
	btn_next = must_find<QPushButton>(sheet, QStringLiteral("mfdSetupNextBtn"));

	REQUIRE(combo_fhost->findData(QStringLiteral("Cam")) >= 0);
	REQUIRE(combo_fhost->findData(QStringLiteral("Blue")) >= 0);
	combo_fhost->setCurrentIndex(combo_fhost->findData(QStringLiteral("Cam")));
	pump_ui();
	REQUIRE(combo_fname->currentData().toString() == QStringLiteral("Blur"));
	combo_fprop->setCurrentIndex(combo_fprop->findData(QStringLiteral("radius")));
	pump_ui();

	/* Click [Use current] on At 0% -> reads live 12.5 px, then test equal endpoints refusal */
	btn_use_cur_from->click();
	pump_ui();
	REQUIRE(std::fabs(spin_ffrom->value() - 12.5) < 1e-6);

	spin_ffrom->setValue(40.0);
	spin_fto->setValue(40.0);
	pump_ui();
	REQUIRE(!flt_refusal->isHidden());
	REQUIRE(flt_refusal->text().contains(QStringLiteral("must differ")));
	REQUIRE(!btn_next->isEnabled());

	/* Configure Clean = 0.0 px, Max = 40.0 px, Turn filter off at 0% = true */
	spin_ffrom->setValue(0.0);
	spin_fto->setValue(40.0);
	chk_fbypass->setChecked(true);
	pump_ui();
	REQUIRE(flt_refusal->isHidden());
	REQUIRE(btn_next->isEnabled());

	capture_and_verify_png(sheet, screenshot_dir, QStringLiteral("12-setup-filter-step2.png"));

	/* Advance to Step 3 -> simulated display_value preview while live filter writes stay paused */
	btn_next->click();
	pump_ui();
	REQUIRE(stack->currentIndex() == 2);
	fin_lbl = must_find<QLabel>(sheet, QStringLiteral("mfdFinishedSentence"));
	cand_meter = must_find<QProgressBar>(sheet, QStringLiteral("mfdCandidateMeter"));
	btn_done = must_find<QPushButton>(sheet, QStringLiteral("mfdSetupDoneBtn"));
	REQUIRE(fin_lbl->text() == QStringLiteral("FX 1 controls Blur (Blur Radius) on Cam"));

	const int flt_writes_before_try = sg.total_filter_writes;
	push_midi_cc(&eng, 0, 16, 64); /* APC40 Device Knob 1 (Ch 1 CC 16) = ~50% -> 20.0 px */
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	REQUIRE(cand_meter->value() == 50);
	REQUIRE(cand_meter->format().contains(QStringLiteral("px")));
	REQUIRE(sg.total_filter_writes == flt_writes_before_try);

	btn_done->click();
	pump_ui();
	REQUIRE(sheet->isHidden());
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripAssignLabel_10"))->text() ==
		QStringLiteral("Cam: Blur (Blur Radius)"));

	/* Drive FX 1 via APC40 Device Knob 1 (cross initial takeover target first, then 50% -> 20.0 px) */
	push_midi_cc(&eng, 0, 16, 30);
	push_midi_cc(&eng, 0, 16, 50);
	push_midi_cc(&eng, 0, 16, 64);
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	struct fake_sg_filter *blur_flt = fake_sg_find_filter(&sg, "Cam", "Blur");
	REQUIRE(blur_flt != nullptr);
	REQUIRE(blur_flt->enabled);
	REQUIRE(std::fabs(blur_flt->radius - 20.157) < 0.5);
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_10"))->text().contains(QStringLiteral("px")));

	/* Open Set up on FX 2 (slot 11): verify single-writer conflict on Cam/Blur/radius, then bind scene AudioMod */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdStripActionBtn_11"))->click();
	pump_ui();
	must_find<QPushButton>(sheet, QStringLiteral("mfdCardFilterSetting"))->click();
	pump_ui();
	combo_fhost->setCurrentIndex(combo_fhost->findData(QStringLiteral("Cam")));
	pump_ui();
	combo_fprop->setCurrentIndex(combo_fprop->findData(QStringLiteral("radius")));
	pump_ui();
	REQUIRE(!flt_refusal->isHidden());
	REQUIRE(flt_refusal->text() == QStringLiteral("Setting 'radius' on 'Blur' is already controlled by FX 1."));
	REQUIRE(!btn_next->isEnabled());

	/* Switch FX 2 to whole-scene Visual Sidechain AudioMod nested group setting (inverted 100% -> 0%) */
	combo_fhost->setCurrentIndex(combo_fhost->findData(QStringLiteral("Blue")));
	pump_ui();
	REQUIRE(combo_fname->currentData().toString() == QStringLiteral("AudioMod"));
	REQUIRE(combo_fprop->currentData().toString() == QStringLiteral("slot_1_amount_pct"));
	spin_ffrom->setValue(100.0);
	spin_fto->setValue(0.0);
	chk_fbypass->setChecked(false);
	pump_ui();
	REQUIRE(flt_refusal->isHidden());
	REQUIRE(btn_next->isEnabled());
	btn_next->click();
	pump_ui();
	btn_done->click();
	pump_ui();
	REQUIRE(sheet->isHidden());

	/* Drive FX 2 to 75% deflection -> inverted 25% reactivity */
	push_midi_cc(&eng, 0, 17, 50);
	push_midi_cc(&eng, 0, 17, 95);
	mfd_engine_tick(&eng, 0.016f);
	pump_ui();
	struct fake_sg_filter *amod_flt = fake_sg_find_filter(&sg, "Blue", "AudioMod");
	REQUIRE(amod_flt != nullptr);
	REQUIRE(std::fabs(amod_flt->reactivity_pct - 25.2) < 1.5);

	capture_and_verify_png(dlg.get(), screenshot_dir, QStringLiteral("11-fx-bank-strips.png"));

	/* Click [Clean / All FX Off] -> resets FX 1 (0.0 px (Off), Blur disabled) & FX 2 (100% clean) without touching Strip 1 */
	{
		char err_s1[128] = {0};
		REQUIRE(mfd_engine_set_slot_value(&eng, 1, 0.62f, false, "api", err_s1, sizeof(err_s1)));
		mfd_engine_tick(&eng, 0.016f);
	}
	const float strip1_val_before_clean = eng.fader_slots[1].last_applied_val;
	REQUIRE(std::fabs(strip1_val_before_clean - 0.62f) < 1e-5f);
	REQUIRE(btn_clean_fx->styleSheet().contains(QStringLiteral("#b45309")));
	btn_clean_fx->click();
	pump_ui();
	REQUIRE(std::fabs(eng.fader_slots[10].last_applied_val) < 1e-5f);
	REQUIRE(std::fabs(eng.fader_slots[11].last_applied_val) < 1e-5f);
	REQUIRE(std::fabs(blur_flt->radius - 0.0) < 1e-5);
	REQUIRE(!blur_flt->enabled);
	REQUIRE(std::fabs(amod_flt->reactivity_pct - 100.0) < 1e-5);
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdStripState_10"))->text() ==
		QStringLiteral("0.0 px (Off)"));
	REQUIRE(std::fabs(eng.fader_slots[1].last_applied_val - strip1_val_before_clean) < 1e-5f);

	REQUIRE(btn_clean_fx->styleSheet().contains(QStringLiteral("#164e63")));

	/* Active nonfilter assignments in the FX bank do not light up Clean FX or get reset. */
	{
		const enum mfd_target_type protected_modes[] = {MFD_TARGET_AUDIO_VOLUME, MFD_TARGET_TWO_SOURCE_MIX,
								MFD_TARGET_SINGLE_LAYER};
		uint32_t writes_before_clean[3] = {0};
		for (int n = 0; n < 3; ++n) {
			struct mfd_slot_config_edit edit {};
			edit.enabled = true;
			edit.target_type = protected_modes[n];
			std::snprintf(edit.scene_name, sizeof(edit.scene_name), "Blue");
			std::snprintf(edit.source_a_name, sizeof(edit.source_a_name), "%s", n == 0 ? "Mic/Aux" : "Cam");
			if (n == 1) {
				std::snprintf(edit.source_b_name, sizeof(edit.source_b_name), "Color 2");
			}
			edit.curve = MFD_CURVE_DIP_FREE;
			REQUIRE(mfd_engine_update_slot_config(&eng, 12 + n, &edit));
			char err[128] = {0};
			REQUIRE(mfd_engine_set_slot_value(&eng, 12 + n, 0.65f, false, "api", err, sizeof(err)));
			writes_before_clean[n] = eng.fader_slots[12 + n].output_write_count;
		}
		pump_ui();
		REQUIRE(btn_clean_fx->styleSheet().contains(QStringLiteral("#164e63")));
		REQUIRE(!btn_clean_fx->styleSheet().contains(QStringLiteral("#b45309")));
		const int opacity_writes_before_clean = sg.total_opacity_writes;
		const int volume_writes_before_clean = sg.total_volume_writes;
		btn_clean_fx->click();
		pump_ui();
		for (int n = 0; n < 3; ++n) {
			REQUIRE(std::fabs(eng.fader_slots[12 + n].last_applied_val - 0.65f) < 1e-5f);
			REQUIRE(eng.fader_slots[12 + n].output_write_count == writes_before_clean[n]);
			REQUIRE(!eng.fader_slots[12 + n].pending_clean_apply_on_avail);
		}
		REQUIRE(sg.total_opacity_writes == opacity_writes_before_clean);
		REQUIRE(sg.total_volume_writes == volume_writes_before_clean);
		REQUIRE(btn_clean_fx->styleSheet().contains(QStringLiteral("#164e63")));
	}

	/* Switch to Advanced tab -> verify 18 rows, <= 1380 px width, filter summary, Off at 0% toggle & conflict banner */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdBtnOpenAdvanced"))->click();
	pump_ui();
	REQUIRE(tabs->currentIndex() == 1);
	REQUIRE(table->rowCount() == 18);
	REQUIRE(dlg->width() <= 1380);
	REQUIRE(must_find<QComboBox>(dlg.get(), QStringLiteral("mfdAdvMode_10"))->currentData().toInt() ==
		static_cast<int>(MFD_TARGET_FILTER_PARAM));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdAdvFilterSummary_10"))->text() ==
		QStringLiteral("Blur: Blur Radius [0.0..40.0]"));
	REQUIRE(must_find<QLabel>(dlg.get(), QStringLiteral("mfdAdvFeelLabel_10"))->text() == QStringLiteral("Linear"));

	/* Uncheck Off at 0% on row 10 -> lease detaches and restores Blur enabled == true immediately */
	auto *adv_byp_10 = must_find<QCheckBox>(dlg.get(), QStringLiteral("mfdAdvFilterBypass_10"));
	REQUIRE(adv_byp_10->isChecked());
	adv_byp_10->setChecked(false);
	pump_ui();
	REQUIRE(blur_flt->enabled);

	/* Click [Edit...] on row 10 -> opens Set up sheet for FX 1, then cancel */
	must_find<QPushButton>(dlg.get(), QStringLiteral("mfdAdvFilterEdit_10"))->click();
	pump_ui();
	REQUIRE(!sheet->isHidden());
	must_find<QPushButton>(sheet, QStringLiteral("mfdSetupCancelBtn"))->click();
	pump_ui();
	REQUIRE(sheet->isHidden());

	/* Configure disabled duplicate on FX 3 (row 12), then toggle On in Advanced -> banner surfaces single-writer error */
	{
		struct mfd_slot_config_edit dup_edit {};
		dup_edit.enabled = false;
		dup_edit.target_type = MFD_TARGET_FILTER_PARAM;
		std::snprintf(dup_edit.source_a_name, sizeof(dup_edit.source_a_name), "Cam");
		std::snprintf(dup_edit.filter_name, sizeof(dup_edit.filter_name), "Blur");
		std::snprintf(dup_edit.filter_prop, sizeof(dup_edit.filter_prop), "radius");
		dup_edit.has_filter_range = true;
		dup_edit.filter_from_val = 0.0;
		dup_edit.filter_to_val = 25.0;
		REQUIRE(mfd_engine_update_slot_config(&eng, 12, &dup_edit));
		pump_ui();
	}
	auto *adv_on_12 = must_find<QCheckBox>(dlg.get(), QStringLiteral("mfdAdvOn_12"));
	REQUIRE(!adv_on_12->isChecked());
	adv_on_12->setChecked(true);
	pump_ui();
	auto *adv_err_banner = must_find<QLabel>(dlg.get(), QStringLiteral("mfdAdvErrorBanner"));
	REQUIRE(!adv_err_banner->isHidden());
	REQUIRE(adv_err_banner->text() == QStringLiteral("Setting 'radius' on 'Blur' is already controlled by FX 1."));
	REQUIRE(!eng.fader_slots[12].enabled);

	dlg->close();
	dlg.reset();
	mfd_engine_destroy_mutex(&eng);

	std::printf(
		"test-ui-dialog: PASS (all end-to-end flows, FX bank, filter_param, multi-controller profiles & 12 screenshots verified in %s)\n",
		screenshot_dir.toUtf8().constData());
	return 0;
}
