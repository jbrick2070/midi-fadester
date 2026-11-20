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

/* D-07: a renamed target keeps its baselines, so disabling the modulator still puts the real value back. */
static void test_renamed_target_keeps_its_baseline(void)
{
	struct vs_baseline_table table;
	vs_baseline_init(&table);
	assert(vs_baseline_record_before_write_float(&table, "Color", "opacity", 0.80, 0.20));
	assert(vs_baseline_record_before_write_float(&table, "Color", "gamma", 0.10, 0.50));
	assert(vs_baseline_record_before_write_float(&table, "Blur", "radius", 4.0, 9.0));
	assert(vs_baseline_rename_filter(&table, "Color", "Colour grade") == 2);
	assert(vs_baseline_find(&table, "Color", "opacity") == NULL);
	const struct vs_baseline_entry *e = vs_baseline_find(&table, "Colour grade", "opacity");
	assert(e && fabs(e->original_float - 0.80) < 1e-12 && fabs(e->last_written_float - 0.20) < 1e-12);
	assert(vs_baseline_find(&table, "Blur", "radius") != NULL);
	assert(vs_baseline_rename_filter(&table, "Color", "Other") == 0); /* nothing left under the old name */
	assert(vs_baseline_rename_filter(&table, "Blur", "Blur") == 0);
	assert(vs_baseline_rename_filter(&table, NULL, "x") == 0);
}

/* D-08, D-09: what a modulator does with the record it finds on a target. */
static void test_target_claims(void)
{
	assert(vs_baseline_claim(false, false, false, false) == VS_CLAIM_FREE);
	assert(vs_baseline_claim(false, true, true, true) == VS_CLAIM_FREE);
	assert(vs_baseline_claim(true, true, true, true) == VS_CLAIM_MINE);
	assert(vs_baseline_claim(true, false, true, false) == VS_CLAIM_TAKEN); /* a second modulator: refused */
	assert(vs_baseline_claim(true, false, true, true) == VS_CLAIM_TAKEN);  /* also when it held it before a save */
	assert(vs_baseline_claim(true, false, false, false) ==
	       VS_CLAIM_ORPHAN); /* a copied filter: value put back first */
	/* QA-B03: this modulator's id on a setting it holds no baseline for is a copy pasted on its own source */
	assert(vs_baseline_claim(true, true, true, false) == VS_CLAIM_ORPHAN);
	assert(vs_baseline_claim(true, true, false, false) == VS_CLAIM_ORPHAN);
}

/* QA-B05: a slot that is refused drops the baseline it still held, and nothing is written to the target. */
static int g_forget_applies;
static void count_apply(void *user_data, const char *filter_name, const char *setting_name, enum vs_prop_type prop_type,
			double float_val, int64_t int_val)
{
	(void)user_data;
	(void)filter_name;
	(void)setting_name;
	(void)prop_type;
	(void)float_val;
	(void)int_val;
	g_forget_applies++;
}

static void test_refused_slot_forgets_its_baseline_unwritten(void)
{
	struct vs_baseline_table t;
	vs_baseline_init(&t);
	assert(vs_baseline_record_before_write_float(&t, "Color", "opacity", 1.0, 0.4));
	assert(vs_baseline_record_before_write_float(&t, "Color", "gamma", 0.0, 0.2));
	assert(vs_baseline_forget_entry(&t, "Color", "opacity"));
	assert(!vs_baseline_find(&t, "Color", "opacity"));
	assert(vs_baseline_find(&t, "Color", "gamma"));
	assert(!vs_baseline_forget_entry(&t, "Color", "opacity"));
	/* what the other modulator wrote there (0.9) is not read as an edit, and switching off writes only gamma */
	assert(!vs_baseline_reconcile_float(&t, "Color", "opacity", 0.9));
	g_forget_applies = 0;
	assert(vs_baseline_restore_all(&t, count_apply, NULL) == 1);
	assert(g_forget_applies == 1);
}

int main(void)
{
	test_baseline_capture_once_during_pulse();
	test_baseline_restore_on_disable_retarget_remove();
	test_baseline_scene_save_mid_pulse();
	test_manual_edit_during_steady_signal_survives_restore_and_save();
	test_baseline_crash_recovery_reload();
	test_removed_filter_forgets_old_baseline_before_recreation();
	test_renamed_target_keeps_its_baseline();
	test_target_claims();
	test_refused_slot_forgets_its_baseline_unwritten();
	printf("test-modulator-baseline: all assertions passed\n");
	return 0;
}
