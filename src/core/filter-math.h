/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_FILTER_BOUND_LIMIT 1000000.0
#define MFD_FILTER_MIN_RANGE_SPAN 1e-6
#define MFD_FILTER_BYPASS_ZERO_THRESHOLD 0.001f

double mfd_filter_clamp_bound(double raw_bound, bool is_max);

void mfd_filter_normalize_bounds(double raw_min, double raw_max, double *out_min, double *out_max);

bool mfd_filter_range_is_valid(double from_val, double to_val, double prop_min, double prop_max);

double mfd_filter_map_value(float norm_01, double from_val, double to_val, double prop_min, double prop_max,
			    bool is_int);

float mfd_filter_unmap_value(double live_val, double from_val, double to_val);

bool mfd_filter_should_write_mapped(bool has_prev, bool is_int, int64_t prev_int, double prev_float, double new_mapped,
				    double from_val, double to_val, float new_norm_01);

int mfd_filter_decimals_for_step(bool is_int, double step);

void mfd_filter_format_display_value(float norm_01, double from_val, double to_val, double prop_min, double prop_max,
				     bool is_int, double step, const char *suffix, bool bypass_at_zero, char *out,
				     size_t out_size);

#ifdef __cplusplus
}
#endif
