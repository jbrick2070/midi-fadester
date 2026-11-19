/*
 * Visual Sidechain - Target Baseline Capture & Scene-Save Protection Unit Tests
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

#include "core/baseline.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct mock_target_state {
	double intensity;
	double threshold;
	int64_t interval;
	int applies; /* how many values the baseline table wrote */
};

static bool double_near(double a, double b, double eps)
{
	return fabs(a - b) <= eps;
}

static void apply_to_mock_target(void *user_data, const char *filter_name, const char *setting_name,
				 enum vs_prop_type prop_type, double float_val, int64_t int_val)
{
	struct mock_target_state *target = (struct mock_target_state *)user_data;
	if (!target || !filter_name || !setting_name)
		return;

	target->applies++;
	if (prop_type == VS_PROP_FLOAT) {
		if (strcmp(setting_name, "intensity") == 0)
			target->intensity = float_val;
		else if (strcmp(setting_name, "threshold") == 0)
			target->threshold = float_val;
	} else if (prop_type == VS_PROP_INT) {
		if (strcmp(setting_name, "interval") == 0)
			target->interval = int_val;
	}
}

static void test_baseline_capture_once_during_pulse(void)
{
	struct vs_baseline_table table;
	struct mock_target_state target = {0.25, 0.50, 1};
	vs_baseline_init(&table);

	/* 120 ticks of modulation pulsing intensity from 0.25 up to 0.85 and back. */
	for (int tick = 0; tick < 120; tick++) {
		const double new_val = 0.25 + 0.60 * ((double)(tick + 1) / 120.0);
		assert(vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity,
							     new_val));
		target.intensity = new_val;
	}

	assert(double_near(target.intensity, 0.85, 1e-6));

	/* Captured baseline must remain 0.25, never overwritten by mid-pulse 0.85. */
	const struct vs_baseline_entry *e = vs_baseline_find(&table, "NVIDIA Blur", "intensity");
	assert(e != NULL);
	assert(double_near(e->original_float, 0.25, 1e-6));
}

static void test_baseline_restore_on_disable_retarget_remove(void)
{
	struct vs_baseline_table table;
	struct mock_target_state target = {0.25, 0.50, 1};
	vs_baseline_init(&table);

	/* 1. Pulse intensity from 0.25 -> 0.80, then disable/retarget the slot. */
	assert(vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity, 0.80));
	target.intensity = 0.80;
	assert(double_near(target.intensity, 0.80, 1e-6));

	/* Retargeting from 'intensity' to 'threshold' restores 'intensity' to 0.25. */
	assert(vs_baseline_restore_entry(&table, "NVIDIA Blur", "intensity", apply_to_mock_target, &target));
	assert(double_near(target.intensity, 0.25, 1e-6));
	assert(vs_baseline_find(&table, "NVIDIA Blur", "intensity") == NULL);

	/* 2. Modulate both 'intensity' (0.25 -> 0.90) and 'threshold' (0.50 -> 0.95), then disable/destroy modulator. */
	assert(vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity, 0.90));
	target.intensity = 0.90;
	assert(vs_baseline_record_before_write_float(&table, "NVIDIA Background Removal", "threshold", target.threshold,
						     0.95));
	target.threshold = 0.95;

	assert(vs_baseline_restore_all(&table, apply_to_mock_target, &target) == 2u);
	assert(double_near(target.intensity, 0.25, 1e-6));
	assert(double_near(target.threshold, 0.50, 1e-6));
}

static void test_baseline_scene_save_mid_pulse(void)
{
	struct vs_baseline_table table;
	struct mock_target_state target = {0.25, 0.50, 1};
	vs_baseline_init(&table);

	/* Pulse intensity from original 0.25 up to 0.80 mid-beat. */
	assert(vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity, 0.80));
	target.intensity = 0.80;
	assert(double_near(target.intensity, 0.80, 1e-6));

	/* Scene collection is saved mid-pulse: pre-save hook restores target to 0.25 for serialization. */
	assert(vs_baseline_on_pre_save(&table, apply_to_mock_target, &target) == 1u);
	assert(double_near(target.intensity, 0.25, 1e-6));

	/* The frontend writes the JSON only at the end of its save, so writes stay suspended until the save has
	 * returned: no record (the tick writes nothing), and the target and baseline keep 0.25. */
	assert(!vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity, 0.75));
	assert(double_near(target.intensity, 0.25, 1e-6));
	const struct vs_baseline_entry *e = vs_baseline_find(&table, "NVIDIA Blur", "intensity");
	assert(e != NULL);
	assert(double_near(e->original_float, 0.25, 1e-6));
	assert(double_near(e->last_written_float, 0.25, 1e-6));
	assert(!vs_baseline_record_before_write_int(&table, "NVIDIA Blur", "interval", target.interval, 4));
	assert(vs_baseline_find(&table, "NVIDIA Blur", "interval") == NULL);

	/* After the save, the next video_tick resumes modulation (to 0.75) while keeping original baseline at 0.25. */
	vs_baseline_resume_after_save(&table);
	assert(vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity, 0.75));
	target.intensity = 0.75;
	assert(double_near(e->original_float, 0.25, 1e-6));

	/* Disabling the modulator later still restores the original 0.25. */
	assert(vs_baseline_restore_all(&table, apply_to_mock_target, &target) == 1u);
	assert(double_near(target.intensity, 0.25, 1e-6));

	/* A pre-save that restored nothing suspends nothing. */
	assert(vs_baseline_on_pre_save(&table, apply_to_mock_target, &target) == 0u);
	assert(vs_baseline_record_before_write_float(&table, "NVIDIA Blur", "intensity", target.intensity, 0.5));
}

static void test_manual_edit_during_steady_signal_survives_restore_and_save(void)
{
	struct vs_baseline_table table;
	struct mock_target_state target = {0.25, 0.50, 1};
	vs_baseline_init(&table);

	/* The modulator writes 0.5 once (0.25 captured). A still fader then holds the mapped value steady, so the
	 * epsilon gate skips every later write. */
	assert(vs_baseline_record_before_write_float(&table, "Color", "intensity", target.intensity, 0.5));
	target.intensity = 0.5;

	/* The user sets 0.8 meanwhile. No write records it, so the modulator reconciles before restoring or saving. */
	target.intensity = 0.8;
	assert(vs_baseline_reconcile_float(&table, "Color", "intensity", target.intensity));

	/* A save writes the user's 0.8, not the stale 0.25 ... */
	target.applies = 0;
	assert(vs_baseline_on_pre_save(&table, apply_to_mock_target, &target) == 1u);
	assert(target.applies == 1 && double_near(target.intensity, 0.8, 1e-9));
	const struct vs_baseline_entry *e = vs_baseline_find(&table, "Color", "intensity");
	assert(e != NULL);
	assert(double_near(e->original_float, 0.8, 1e-9) && double_near(e->last_written_float, 0.8, 1e-9));
	vs_baseline_resume_after_save(&table);

	/* ... and so does disabling the modulator. */
	assert(vs_baseline_restore_all(&table, apply_to_mock_target, &target) == 1u);
	assert(target.applies == 2 && double_near(target.intensity, 0.8, 1e-9));

	/* With the target still holding the last written value (or within the edit tolerance), a reconcile returns false
	 * and changes nothing; a missing entry is no edit either. */
	assert(vs_baseline_record_before_write_float(&table, "Color", "intensity", 0.25, 0.5));
	assert(!vs_baseline_reconcile_float(&table, "Color", "intensity", 0.5));
	assert(!vs_baseline_reconcile_float(&table, "Color", "intensity", 0.5 + 5e-4));
	e = vs_baseline_find(&table, "Color", "intensity");
	assert(e != NULL && e->prop_type == VS_PROP_FLOAT && e->has_last_written);
	assert(e->original_float == 0.25 && e->original_int == 0);
	assert(e->last_written_float == 0.5 && e->last_written_int == 1);
	assert(!vs_baseline_reconcile_float(&table, "Color", "threshold", 0.9));

	/* Integer targets follow the same rule with any difference. */
	assert(vs_baseline_record_before_write_int(&table, "Color", "interval", 1, 4));
	assert(!vs_baseline_reconcile_int(&table, "Color", "interval", 4));
	assert(vs_baseline_reconcile_int(&table, "Color", "interval", 7));
	target.interval = 4;
	target.applies = 0;
	assert(vs_baseline_restore_entry(&table, "Color", "interval", apply_to_mock_target, &target));
	assert(target.applies == 1 && target.interval == 7);
}

static void test_baseline_crash_recovery_reload(void)
{
	struct vs_baseline_table table;
	/* Simulate loading a scene collection where a prior crash left intensity stuck at 0.80. */
	struct mock_target_state target = {0.80, 0.50, 1};
	vs_baseline_init(&table);

	/* On load, the modulator's persisted baseline map records original_float == 0.25. */
	assert(vs_baseline_on_load_entry(&table, "NVIDIA Blur", "intensity", VS_PROP_FLOAT, 0.25, 0,
					 apply_to_mock_target, &target));

	/* Target filter setting is immediately restored to 0.25 on load. */
	assert(double_near(target.intensity, 0.25, 1e-6));
}

static void test_removed_filter_forgets_old_baseline_before_recreation(void)
{
	struct vs_baseline_table table;
	struct mock_target_state target = {0.6, 0.5, 1};
	vs_baseline_init(&table);
	assert(vs_baseline_record_before_write_float(&table, "Color", "intensity", 0.6, 0.2));
	assert(vs_baseline_record_before_write_float(&table, "Other", "threshold", 0.5, 0.8));
	target.intensity = 0.2;
	assert(vs_baseline_restore_filter(&table, "Color", apply_to_mock_target, &target) == 1);
	assert(double_near(target.intensity, 0.6, 1e-6));
	assert(vs_baseline_find(&table, "Color", "intensity") == NULL);
	assert(vs_baseline_find(&table, "Other", "threshold") != NULL);
	/* A recreated same-name filter owns its own original value. */
	assert(vs_baseline_record_before_write_float(&table, "Color", "intensity", 0.9, 0.2));
	assert(vs_baseline_restore_filter(&table, "Color", apply_to_mock_target, &target) == 1);
	assert(double_near(target.intensity, 0.9, 1e-6));
}

int main(void)
{
	test_baseline_capture_once_during_pulse();
	test_baseline_restore_on_disable_retarget_remove();
	test_baseline_scene_save_mid_pulse();
	test_manual_edit_during_steady_signal_survives_restore_and_save();
	test_baseline_crash_recovery_reload();
	test_removed_filter_forgets_old_baseline_before_recreation();
	printf("test-modulator-baseline: all assertions passed\n");
	return 0;
}
