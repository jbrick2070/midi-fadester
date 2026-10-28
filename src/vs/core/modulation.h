/*
 * Visual Sidechain - Pure-C Modulation Slot Math & Rate/Epsilon Gate
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "analyzer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_MOD_MAX_SLOTS 8u
#define VS_MOD_FLOAT_EPSILON 1e-4

enum vs_prop_type {
	VS_PROP_NONE = 0,
	VS_PROP_FLOAT = 1,
	VS_PROP_INT = 2,
};

struct vs_mod_slot_config {
	bool enabled;
	char target_filter[128];
	char target_setting[128];
	enum vs_prop_type prop_type;
	enum vs_signal_id signal_id;
	float custom_low_hz;
	float custom_high_hz;
	enum vs_hz_band_mode custom_mode;
	double amount;    /* [-1.0, +1.0], negative = ducking */
	double min_value; /* lower bound of target range */
	double max_value; /* upper bound of target range */
	int64_t int_step; /* step size for VS_PROP_INT (>= 1) */
	bool invert;
	uint32_t rate_divider; /* 1, 2, or 4 video frames */
};

struct vs_mod_slot_state {
	uint32_t tick_counter;
	bool has_last_written;
	double last_written_float;
	int64_t last_written_int;
};

struct vs_mod_eval_result {
	bool should_write;
	enum vs_prop_type prop_type;
	double float_val;
	int64_t int_val;
};

void vs_mod_slot_default_config(struct vs_mod_slot_config *cfg);
void vs_mod_slot_reset_state(struct vs_mod_slot_state *state);

/*
 * Evaluates one modulation slot for the current video tick.
 * Applies bipolar amount, [min_value, max_value] range mapping, integer step
 * quantization, frame-rate divider (1/2/4), and epsilon change-only gate.
 */
void vs_mod_slot_eval(const struct vs_mod_slot_config *cfg, struct vs_mod_slot_state *state, float signal_01,
		      struct vs_mod_eval_result *out_res);

struct vs_mod_target_write {
	uint32_t slot_index;
	const char *target_setting;
	enum vs_prop_type prop_type;
	double float_val;
	int64_t int_val;
};

struct vs_mod_target_batch {
	const char *target_filter;
	struct vs_mod_target_write writes[VS_MOD_MAX_SLOTS];
	size_t write_count;
};

/*
 * Evaluates up to `slot_count` (<= VS_MOD_MAX_SLOTS) modulation slots for one
 * video tick and groups all dirty writes targeting the same `target_filter`
 * into a single `vs_mod_target_batch` so the host filter performs at most one
 * `obs_source_update` per distinct target filter per frame (R-MOD-4).
 */
size_t vs_mod_eval_and_batch_slots(const struct vs_mod_slot_config *cfgs, struct vs_mod_slot_state *states,
				   const float *signals_01, size_t slot_count,
				   struct vs_mod_target_batch out_batches[VS_MOD_MAX_SLOTS]);

#ifdef __cplusplus
}
#endif
