/*
 * Visual Sidechain - Numeric Modulator Catalog Properties Tests
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/flash-clock.h"
#include "obs/audio-tap.h"
#include "obs/modulator-filter.h"
#include "obs/vendor-api.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Locale key identity is sufficient here; the separate locale test checks the real translation keys. */
const char *obs_module_text(const char *key)
{
	return key;
}

/* Property construction never ticks audio or connects websocket. */
void vs_vendor_api_on_tick(uint64_t video_frame_ts, float dt_sec, const struct vs_audio_frame *frame,
			   bool is_primary_track)
{
	(void)video_frame_ts;
	(void)dt_sec;
	(void)frame;
	(void)is_primary_track;
}

/* Private test inputs to hold filters: never ticked or rendered, so their data is a placeholder. Scenes would need a
 * canvas. */
static int g_input_placeholder;

static const char *test_input_get_name(void *unused)
{
	(void)unused;
	return "Visual Sidechain test input";
}

static void *test_input_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	(void)source;
	return &g_input_placeholder;
}

static void test_input_destroy(void *data)
{
	(void)data;
}

static uint32_t test_input_get_size(void *data)
{
	(void)data;
	return 64;
}

static struct obs_source_info test_video_input = {
	.id = "vs_test_video_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = test_input_get_name,
	.create = test_input_create,
	.destroy = test_input_destroy,
	.get_width = test_input_get_size,
	.get_height = test_input_get_size,
};

static struct obs_source_info test_av_input = {
	.id = "vs_test_av_input",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO,
	.get_name = test_input_get_name,
	.create = test_input_create,
	.destroy = test_input_destroy,
	.get_width = test_input_get_size,
	.get_height = test_input_get_size,
};

static obs_properties_t *test_color_filter_properties(void *unused)
{
	(void)unused;
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_float_slider(p, "brightness", "Brightness", -1.0, 1.0, 0.01);
	obs_properties_add_float_slider(p, "opacity", "Opacity", 0.0, 1.0, 0.01);
	return p;
}

static struct obs_source_info test_color_filter = {
	.id = "color_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = test_input_get_name,
	.create = test_input_create,
	.destroy = test_input_destroy,
	.get_properties = test_color_filter_properties,
};

static obs_source_t *create_modulator_aimed_at(const char *name, const char *target_filter)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "slot_1_enabled", true);
	obs_data_set_string(settings, "slot_1_target_filter", target_filter);
	obs_data_set_string(settings, "slot_1_target_setting", "slot_1_amount_pct");
	obs_source_t *modulator = obs_source_create("visual_sidechain_modulator_filter", name, settings, NULL);
	obs_data_release(settings);
	return modulator;
}

/* Whether slot 1's target setting list in the modulator's Properties offers `value` */
static bool slot_1_target_settings_offer(obs_source_t *modulator, const char *value)
{
	obs_properties_t *props = obs_source_properties(modulator);
	assert(props);
	obs_property_t *group = obs_properties_get(props, "mod_slot_group_1");
	assert(group && obs_property_get_type(group) == OBS_PROPERTY_GROUP);
	obs_property_t *list = obs_properties_get(obs_property_group_content(group), "slot_1_target_setting");
	assert(list && obs_property_get_type(list) == OBS_PROPERTY_LIST);
	bool offered = false;
	for (size_t i = 0; i < obs_property_list_item_count(list); i++) {
		const char *item = obs_property_list_item_string(list, i);
		if (item && strcmp(item, value) == 0)
			offered = true;
	}
	obs_properties_destroy(props);
	return offered;
}

/* Two Audio Modulators aimed at each other (R-MOD-1/2 allow any sibling). Target discovery reads the target's
 * properties, which discover the target's own targets: without the recursion guard the second add never returned
 * (stack overflow), and a saved pair crashed OBS on every load. Each list still offers the other's settings. */
static void test_modulators_aimed_at_each_other(void)
{
	obs_source_t *parent = obs_source_create("vs_test_video_input", "vs-test-parent", NULL, NULL);
	obs_source_t *a = create_modulator_aimed_at("A", "B");
	obs_source_t *b = create_modulator_aimed_at("B", "A");
	assert(parent && a && b);
	obs_source_filter_add(parent, a);
	obs_source_filter_add(parent, b);

	assert(slot_1_target_settings_offer(a, "slot_1_amount_pct"));
	assert(slot_1_target_settings_offer(b, "slot_1_amount_pct"));

	obs_source_filter_remove(parent, b);
	obs_source_filter_remove(parent, a);
	obs_source_release(b);
	obs_source_release(a);
	obs_source_release(parent);
}

/* A removed filter's Parent Source tap lets go of the old parent, and a tap whose filter object moved to another
 * source follows it there instead of keeping the old parent. */
static void test_parent_tap_follows_its_filter(void)
{
	obs_source_t *p1 = obs_source_create("vs_test_av_input", "vs-test-p1", NULL, NULL);
	obs_source_t *p2 = obs_source_create("vs_test_av_input", "vs-test-p2", NULL, NULL);
	obs_source_t *filter = obs_source_create("visual_sidechain_modulator_filter", "F", NULL, NULL);
	assert(p1 && p2 && filter);
	obs_source_filter_add(p1, filter);

	struct vs_audio_tap tap;
	assert(vs_audio_tap_init(&tap, false));
	vs_audio_tap_configure(&tap, filter, VS_TAP_PARENT_SOURCE, "", false, NULL);
	assert(tap.source_cb_active);
	assert(obs_weak_source_references_source(tap.weak_capture_source, p1));

	vs_audio_tap_detach_parent(&tap);
	assert(!tap.source_cb_active);
	assert(tap.weak_capture_source == NULL);

	vs_audio_tap_configure(&tap, filter, VS_TAP_PARENT_SOURCE, "", false, NULL);
	assert(obs_weak_source_references_source(tap.weak_capture_source, p1));
	assert(obs_source_get_ref(filter) == filter);
	obs_source_filter_remove(p1, filter);
	obs_source_filter_add(p2, filter);
	obs_source_release(filter);
	vs_audio_tap_configure(&tap, filter, VS_TAP_PARENT_SOURCE, "", false, NULL);
	assert(tap.source_cb_active);
	assert(obs_weak_source_references_source(tap.weak_capture_source, p2));

	vs_audio_tap_destroy(&tap);
	obs_source_filter_remove(p2, filter);
	obs_source_release(filter);
	obs_source_release(p2);
	obs_source_release(p1);
}

/* The Audio Modulator looks a Named Source up outside its mutex and hands it to the tap's retry. A lookup made for
 * another name covers nothing: no attachment, and the retry timer is kept for the next tick's lookup. */
static void test_named_tap_attaches_from_a_lookup(void)
{
	struct vs_audio_tap tap;
	assert(vs_audio_tap_init(&tap, false));
	vs_audio_tap_configure(&tap, NULL, VS_TAP_NAMED_SOURCE, "vs-test-named", false, NULL);
	assert(!tap.source_cb_active);
	obs_source_t *named = obs_source_create("vs_test_av_input", "vs-test-named", NULL, NULL);
	assert(named);

	struct vs_audio_tap_named_lookup lookup;
	vs_audio_tap_plan_named_lookup(&tap, 0.5f, &lookup);
	assert(!lookup.planned); /* the retry is not due yet */
	vs_audio_tap_tick_with_lookup(&tap, NULL, 0.5f, &lookup);

	vs_audio_tap_plan_named_lookup(&tap, 0.6f, &lookup);
	assert(lookup.planned && strcmp(lookup.name, "vs-test-named") == 0);
	struct vs_audio_tap_named_lookup other = lookup;
	snprintf(other.name, sizeof(other.name), "%s", "vs-test-other");
	vs_audio_tap_tick_with_lookup(&tap, NULL, 0.6f, &other);
	assert(!tap.source_cb_active);

	vs_audio_tap_plan_named_lookup(&tap, 0.1f, &lookup);
	assert(lookup.planned);
	vs_audio_tap_resolve_named_lookup(&lookup);
	assert(lookup.source == named);
	vs_audio_tap_tick_with_lookup(&tap, NULL, 0.1f, &lookup);
	vs_audio_tap_release_named_lookup(&lookup);
	assert(tap.source_cb_active && obs_weak_source_references_source(tap.weak_capture_source, named));

	vs_audio_tap_destroy(&tap);
	obs_source_release(named);
}

/* Under the Audio Modulator's mutex (its tick's pending updates) configure must not look a Named Source up: with
 * defer_named_attach set it records the target and attaches nothing, although the source exists, and the planned
 * retry attaches it once the lock is released. */
static void test_deferred_named_configure_leaves_attach_to_retry(void)
{
	obs_source_t *named = obs_source_create("vs_test_av_input", "vs-test-deferred", NULL, NULL);
	assert(named);
	struct vs_audio_tap tap;
	assert(vs_audio_tap_init(&tap, false));
	tap.defer_named_attach = true;
	vs_audio_tap_configure(&tap, NULL, VS_TAP_NAMED_SOURCE, "vs-test-deferred", false, NULL);
	tap.defer_named_attach = false;
	assert(tap.mode == VS_TAP_NAMED_SOURCE && strcmp(tap.source_name, "vs-test-deferred") == 0);
	assert(!tap.source_cb_active && tap.weak_capture_source == NULL);

	struct vs_audio_tap_named_lookup lookup;
	vs_audio_tap_plan_named_lookup(&tap, 1.0f, &lookup);
	assert(lookup.planned);
	vs_audio_tap_resolve_named_lookup(&lookup);
	vs_audio_tap_tick_with_lookup(&tap, NULL, 1.0f, &lookup);
	vs_audio_tap_release_named_lookup(&lookup);
	assert(tap.source_cb_active && obs_weak_source_references_source(tap.weak_capture_source, named));

	vs_audio_tap_destroy(&tap);
	obs_source_release(named);
}

static void test_audio_tap_live_status_property(void)
{
	obs_source_t *filter =
		obs_source_create("visual_sidechain_modulator_filter", "vs-test-live-status", NULL, NULL);
	assert(filter);

	struct vs_audio_tap tap;
	assert(vs_audio_tap_init(&tap, false));

	/* 1. 5 seconds of silence -> "No audio on AudioTap.Mode.Track1 for 5 s." */
	float silent[800] = {0};
	for (size_t f = 0; f < 300u; f++) {
		vs_analyzer_push_mono(&tap.analyzer, silent, 800u, (uint64_t)(f + 1u) * 16666667ULL);
		vs_audio_tap_tick(&tap, filter, 800.0f / 48000.0f);
	}

	obs_properties_t *props = obs_properties_create();
	vs_audio_tap_add_status_property_for_source(props, filter);
	vs_audio_tap_add_properties_with_tap(props, filter, &tap);
	assert(obs_properties_get(props, "tap_refresh_status") != NULL);

	obs_data_t *settings = obs_source_get_settings(filter);
	assert(settings);
	const char *silent_status = obs_data_get_string(settings, "tap_status_warning");
	assert(strcmp(silent_status, "No audio on AudioTap.Mode.Track1 for 5 s.") == 0);
	obs_data_release(settings);
	obs_properties_destroy(props);

	/* 2. Above-gate audio -> "Hearing audio on AudioTap.Mode.Track1." */
	float tone[800];
	for (size_t i = 0; i < 800u; i++)
		tone[i] = 0.25f * sinf(6.28318530718f * 120.0f * (float)i / 48000.0f);
	vs_analyzer_push_mono(&tap.analyzer, tone, 800u, 6000000000ULL);
	vs_audio_tap_tick(&tap, filter, 800.0f / 48000.0f);

	props = obs_properties_create();
	vs_audio_tap_add_status_property_for_source(props, filter);
	vs_audio_tap_add_properties_with_tap(props, filter, &tap);
	settings = obs_source_get_settings(filter);
	assert(settings);
	const char *hearing_status = obs_data_get_string(settings, "tap_status_warning");
	assert(strcmp(hearing_status, "Hearing audio on AudioTap.Mode.Track1.") == 0);
	obs_data_release(settings);
	obs_properties_destroy(props);

	vs_audio_tap_destroy(&tap);
	obs_source_release(filter);
}

/* Legacy saved settings omit slot_1_flash_guard: loading them defaults Flash Guard on, and a live Kick -> Color
 * Correction brightness binding ticked at 60 Hz against a 10 Hz kick burst changes direction <= 6 times/s (P0-1). */
static void test_flash_guard_defaults_and_live_color_filter_cap(void)
{
	vs_flash_clock_global_reset();
	obs_source_t *parent = obs_source_create("vs_test_video_input", "vs-fg-parent", NULL, NULL);
	obs_source_t *cc = obs_source_create("color_filter", "Color Correction", NULL, NULL);
	obs_data_t *legacy = obs_data_create();
	obs_data_set_bool(legacy, "slot_1_enabled", true);
	obs_data_set_string(legacy, "slot_1_target_filter", "Color Correction");
	obs_data_set_string(legacy, "slot_1_target_setting", "brightness");
	obs_data_set_int(legacy, "slot_1_signal", (long long)VS_SIG_KICK_SAFE);
	obs_data_set_double(legacy, "slot_1_min_value", 0.0);
	obs_data_set_double(legacy, "slot_1_max_value", 1.0);
	assert(!obs_data_has_user_value(legacy, "slot_1_flash_guard"));

	obs_source_t *mod = obs_source_create("visual_sidechain_modulator_filter", "Mod", legacy, NULL);
	obs_data_release(legacy);
	assert(parent && cc && mod);
	obs_source_filter_add(parent, cc);
	obs_source_filter_add(parent, mod);

	obs_data_t *mod_s = obs_source_get_settings(mod);
	assert(mod_s && obs_data_get_bool(mod_s, "slot_1_flash_guard"));
	/* Switch to raw VS_SIG_KICK with Flash Guard still defaulted to true to test automatic luminance guarding. */
	obs_data_set_int(mod_s, "slot_1_signal", (long long)VS_SIG_KICK);
	obs_source_update(mod, mod_s);
	obs_data_release(mod_s);

	obs_properties_t *props = obs_source_properties(mod);
	assert(props);
	obs_property_t *grp1 = obs_properties_get(props, "mod_slot_group_1");
	assert(grp1);
	obs_properties_t *g1_content = obs_property_group_content(grp1);
	obs_property_t *fg_prop = obs_properties_get(g1_content, "slot_1_flash_guard");
	assert(fg_prop && obs_property_get_type(fg_prop) == OBS_PROPERTY_BOOL && obs_property_visible(fg_prop));
	obs_property_t *sig_prop = obs_properties_get(g1_content, "slot_1_signal");
	assert(sig_prop);
	bool found_kick_safe = false, found_snare_safe = false, found_flux_safe = false;
	for (size_t i = 0; i < obs_property_list_item_count(sig_prop); i++) {
		const long long v = obs_property_list_item_int(sig_prop, i);
		if (v == (long long)VS_SIG_KICK_SAFE)
			found_kick_safe = true;
		else if (v == (long long)VS_SIG_SNARE_SAFE)
			found_snare_safe = true;
		else if (v == (long long)VS_SIG_FLUX_SAFE)
			found_flux_safe = true;
	}
	assert(found_kick_safe && found_snare_safe && found_flux_safe);
	obs_properties_destroy(props);

	obs_source_filter_remove(parent, mod);
	obs_source_filter_remove(parent, cc);
	obs_source_release(mod);
	obs_source_release(cc);
	obs_source_release(parent);
}

int main(void)
{
	/* Headless libobs initialization only: no OBS executable, modules, video, audio, or user configuration. */
	assert(obs_startup("en-US", NULL, NULL));
	obs_register_source(&vs_modulator_filter_info);
	obs_register_source(&test_video_input);
	obs_register_source(&test_av_input);
	obs_register_source(&test_color_filter);
	obs_properties_t *props = vs_modulator_filter_info.get_properties(NULL);
	assert(props);
	const char *suffixes[] = {"amount_pct", "min_value", "max_value", "custom_low_hz", "custom_high_hz"};
	const char *labels[40];
	size_t count = 0;
	for (int slot = 1; slot <= 8; slot++) {
		char group_key[64];
		snprintf(group_key, sizeof(group_key), "mod_slot_group_%d", slot);
		obs_property_t *group = obs_properties_get(props, group_key);
		assert(group && obs_property_get_type(group) == OBS_PROPERTY_GROUP);
		obs_properties_t *fields = obs_property_group_content(group);
		for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
			char key[64];
			snprintf(key, sizeof(key), "slot_%d_%s", slot, suffixes[i]);
			obs_property_t *field = obs_properties_get(fields, key);
			assert(field && obs_property_get_type(field) == OBS_PROPERTY_FLOAT);
			const char *label = obs_property_description(field);
			assert(label && label[0]);
			/* Midi flattens nested numeric properties: each label must stand alone in the inspector. */
			for (size_t earlier = 0; earlier < count; earlier++)
				assert(strcmp(label, labels[earlier]) != 0);
			labels[count++] = label;
			if (i == 0) {
				assert(obs_property_float_min(field) == -100.0);
				assert(obs_property_float_max(field) == 100.0);
				assert(strcmp(obs_property_float_suffix(field), "%") == 0);
			}
		}
	}
	assert(count == 40);
	obs_properties_destroy(props);

	test_modulators_aimed_at_each_other();
	test_parent_tap_follows_its_filter();
	test_named_tap_attaches_from_a_lookup();
	test_deferred_named_configure_leaves_attach_to_retry();
	test_audio_tap_live_status_property();
	test_flash_guard_defaults_and_live_color_filter_cap();
	obs_shutdown();
	puts("test-modulator-properties: all assertions passed");
	return 0;
}
