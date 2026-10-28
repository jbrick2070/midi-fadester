/*
 * Visual Sidechain - Target Baseline Capture, Save Protection & Restoration
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "modulation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_MAX_BASELINE_ENTRIES 32u

struct vs_baseline_entry {
	bool in_use;
	char filter_name[128];
	char setting_name[128];
	enum vs_prop_type prop_type;
	double original_float;
	int64_t original_int;
	double last_written_float;
	int64_t last_written_int;
	bool has_last_written;
};

struct vs_baseline_table {
	struct vs_baseline_entry entries[VS_MAX_BASELINE_ENTRIES];
};

typedef void (*vs_baseline_apply_fn)(void *user_data, const char *filter_name, const char *setting_name,
				     enum vs_prop_type prop_type, double float_val, int64_t int_val);

void vs_baseline_init(struct vs_baseline_table *table);

/*
 * Captures the unmodulated baseline value for (filter_name, setting_name) once
 * before the first modulated write, or updates last_written on subsequent ticks.
 * If the user manually edited the target filter setting outside the modulator
 * (current != last_written), updates the baseline to the user's new value.
 */
bool vs_baseline_record_before_write_float(struct vs_baseline_table *table, const char *filter_name,
					   const char *setting_name, double current_target_val,
					   double new_modulated_val);

bool vs_baseline_record_before_write_int(struct vs_baseline_table *table, const char *filter_name,
					 const char *setting_name, int64_t current_target_val,
					 int64_t new_modulated_val);

const struct vs_baseline_entry *vs_baseline_find(const struct vs_baseline_table *table, const char *filter_name,
						 const char *setting_name);

/*
 * Restores a single (filter_name, setting_name) back to its captured original
 * value via apply_cb and removes it from the baseline table (used when a slot
 * is disabled or retargeted).
 */
bool vs_baseline_restore_entry(struct vs_baseline_table *table, const char *filter_name, const char *setting_name,
			       vs_baseline_apply_fn apply_cb, void *user_data);

/*
 * Restores all captured baselines via apply_cb and clears the table (used when
 * the modulator filter is disabled, removed, or destroyed).
 */
size_t vs_baseline_restore_all(struct vs_baseline_table *table, vs_baseline_apply_fn apply_cb, void *user_data);

/*
 * Pre-save hook (R-MOD-6): restores all active targets to their original
 * unmodulated baseline values so OBS serializes clean baseline settings into
 * the scene collection JSON, while keeping the baseline entries intact so the
 * next video_tick seamlessly resumes modulation.
 */
size_t vs_baseline_on_pre_save(struct vs_baseline_table *table, vs_baseline_apply_fn apply_cb, void *user_data);

/*
 * Crash-recovery reload hook (R-MOD-6): given a persisted baseline entry from a
 * prior session, immediately restores the target filter setting to its original
 * baseline value and registers it in the table.
 */
bool vs_baseline_on_load_entry(struct vs_baseline_table *table, const char *filter_name, const char *setting_name,
			       enum vs_prop_type prop_type, double original_float, int64_t original_int,
			       vs_baseline_apply_fn apply_cb, void *user_data);

#ifdef __cplusplus
}
#endif
