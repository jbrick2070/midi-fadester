/*
 * Visual Sidechain - Target Baseline Capture, Save Protection & Restoration
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "baseline.h"

#include <math.h>
#include <string.h>

#define VS_USER_EDIT_FLOAT_TOL 1e-3

void vs_baseline_init(struct vs_baseline_table *table)
{
	if (!table)
		return;
	memset(table, 0, sizeof(*table));
}

static struct vs_baseline_entry *find_mutable(struct vs_baseline_table *table, const char *filter_name,
					      const char *setting_name)
{
	if (!table || !filter_name || !setting_name || filter_name[0] == '\0' || setting_name[0] == '\0')
		return NULL;

	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		struct vs_baseline_entry *e = &table->entries[i];
		if (e->in_use && strcmp(e->filter_name, filter_name) == 0 &&
		    strcmp(e->setting_name, setting_name) == 0) {
			return e;
		}
	}
	return NULL;
}

static struct vs_baseline_entry *alloc_entry(struct vs_baseline_table *table, const char *filter_name,
					     const char *setting_name, enum vs_prop_type prop_type)
{
	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		struct vs_baseline_entry *e = &table->entries[i];
		if (!e->in_use) {
			memset(e, 0, sizeof(*e));
			e->in_use = true;
			strncpy(e->filter_name, filter_name, sizeof(e->filter_name) - 1u);
			e->filter_name[sizeof(e->filter_name) - 1u] = '\0';
			strncpy(e->setting_name, setting_name, sizeof(e->setting_name) - 1u);
			e->setting_name[sizeof(e->setting_name) - 1u] = '\0';
			e->prop_type = prop_type;
			return e;
		}
	}
	return NULL;
}

const struct vs_baseline_entry *vs_baseline_find(const struct vs_baseline_table *table, const char *filter_name,
						 const char *setting_name)
{
	if (!table || !filter_name || !setting_name)
		return NULL;
	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		const struct vs_baseline_entry *e = &table->entries[i];
		if (e->in_use && strcmp(e->filter_name, filter_name) == 0 &&
		    strcmp(e->setting_name, setting_name) == 0) {
			return e;
		}
	}
	return NULL;
}

bool vs_baseline_record_before_write_float(struct vs_baseline_table *table, const char *filter_name,
					   const char *setting_name, double current_target_val,
					   double new_modulated_val)
{
	if (!table || !filter_name || !setting_name || filter_name[0] == '\0' || setting_name[0] == '\0')
		return false;

	struct vs_baseline_entry *e = find_mutable(table, filter_name, setting_name);
	if (!e) {
		e = alloc_entry(table, filter_name, setting_name, VS_PROP_FLOAT);
		if (!e)
			return false;
		e->original_float = current_target_val;
		e->original_int = (int64_t)llround(current_target_val);
	} else {
		/* If the user manually edited the target filter's setting in OBS UI, update baseline. */
		if (e->has_last_written && fabs(current_target_val - e->last_written_float) > VS_USER_EDIT_FLOAT_TOL) {
			e->original_float = current_target_val;
			e->original_int = (int64_t)llround(current_target_val);
		}
	}

	e->prop_type = VS_PROP_FLOAT;
	e->last_written_float = new_modulated_val;
	e->last_written_int = (int64_t)llround(new_modulated_val);
	e->has_last_written = true;
	return true;
}

bool vs_baseline_record_before_write_int(struct vs_baseline_table *table, const char *filter_name,
					 const char *setting_name, int64_t current_target_val,
					 int64_t new_modulated_val)
{
	if (!table || !filter_name || !setting_name || filter_name[0] == '\0' || setting_name[0] == '\0')
		return false;

	struct vs_baseline_entry *e = find_mutable(table, filter_name, setting_name);
	if (!e) {
		e = alloc_entry(table, filter_name, setting_name, VS_PROP_INT);
		if (!e)
			return false;
		e->original_int = current_target_val;
		e->original_float = (double)current_target_val;
	} else {
		if (e->has_last_written && current_target_val != e->last_written_int) {
			e->original_int = current_target_val;
			e->original_float = (double)current_target_val;
		}
	}

	e->prop_type = VS_PROP_INT;
	e->last_written_int = new_modulated_val;
	e->last_written_float = (double)new_modulated_val;
	e->has_last_written = true;
	return true;
}

bool vs_baseline_restore_entry(struct vs_baseline_table *table, const char *filter_name, const char *setting_name,
			       vs_baseline_apply_fn apply_cb, void *user_data)
{
	struct vs_baseline_entry *e = find_mutable(table, filter_name, setting_name);
	if (!e)
		return false;

	if (apply_cb) {
		apply_cb(user_data, e->filter_name, e->setting_name, e->prop_type, e->original_float, e->original_int);
	}
	memset(e, 0, sizeof(*e));
	return true;
}

size_t vs_baseline_restore_all(struct vs_baseline_table *table, vs_baseline_apply_fn apply_cb, void *user_data)
{
	if (!table)
		return 0;

	size_t count = 0;
	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		struct vs_baseline_entry *e = &table->entries[i];
		if (e->in_use) {
			if (apply_cb) {
				apply_cb(user_data, e->filter_name, e->setting_name, e->prop_type, e->original_float,
					 e->original_int);
			}
			memset(e, 0, sizeof(*e));
			count++;
		}
	}
	return count;
}

size_t vs_baseline_on_pre_save(struct vs_baseline_table *table, vs_baseline_apply_fn apply_cb, void *user_data)
{
	if (!table)
		return 0;

	size_t count = 0;
	for (size_t i = 0; i < VS_MAX_BASELINE_ENTRIES; i++) {
		struct vs_baseline_entry *e = &table->entries[i];
		if (e->in_use) {
			if (apply_cb) {
				apply_cb(user_data, e->filter_name, e->setting_name, e->prop_type, e->original_float,
					 e->original_int);
			}
			/* Record that the target currently holds original_float/original_int so the next
			 * video_tick does not mistake the pre-save restoration for a manual user edit. */
			e->last_written_float = e->original_float;
			e->last_written_int = e->original_int;
			e->has_last_written = true;
			count++;
		}
	}
	return count;
}

bool vs_baseline_on_load_entry(struct vs_baseline_table *table, const char *filter_name, const char *setting_name,
			       enum vs_prop_type prop_type, double original_float, int64_t original_int,
			       vs_baseline_apply_fn apply_cb, void *user_data)
{
	if (!table || !filter_name || !setting_name || filter_name[0] == '\0' || setting_name[0] == '\0')
		return false;

	struct vs_baseline_entry *e = find_mutable(table, filter_name, setting_name);
	if (!e) {
		e = alloc_entry(table, filter_name, setting_name, prop_type);
		if (!e)
			return false;
	}

	e->prop_type = prop_type;
	e->original_float = original_float;
	e->original_int = original_int;
	e->last_written_float = original_float;
	e->last_written_int = original_int;
	e->has_last_written = true;

	if (apply_cb) {
		apply_cb(user_data, e->filter_name, e->setting_name, e->prop_type, e->original_float, e->original_int);
	}
	return true;
}
