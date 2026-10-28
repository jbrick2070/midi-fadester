/*
 * Visual Sidechain - Numeric Modulator Catalog Properties Tests
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "obs/modulator-filter.h"
#include "obs/vendor-api.h"

#include <assert.h>
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

int main(void)
{
	/* Headless libobs initialization only: no OBS executable, modules, video, audio, or user configuration. */
	assert(obs_startup("en-US", NULL, NULL));
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
	obs_shutdown();
	puts("test-modulator-properties: all assertions passed");
	return 0;
}
