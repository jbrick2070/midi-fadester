/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/filter-math.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static float clamp01f(float v)
{
	if (!isfinite(v) || v <= 0.0f) {
		return 0.0f;
	}
	if (v >= 1.0f) {
		return 1.0f;
	}
	return v;
}

double mfd_filter_clamp_bound(double raw_bound, bool is_max)
{
	if (!isfinite(raw_bound)) {
		return is_max ? MFD_FILTER_BOUND_LIMIT : -MFD_FILTER_BOUND_LIMIT;
	}
	if (raw_bound > MFD_FILTER_BOUND_LIMIT) {
		return MFD_FILTER_BOUND_LIMIT;
	}
	if (raw_bound < -MFD_FILTER_BOUND_LIMIT) {
		return -MFD_FILTER_BOUND_LIMIT;
	}
	return raw_bound;
}

void mfd_filter_normalize_bounds(double raw_min, double raw_max, double *out_min, double *out_max)
{
	double pmin = mfd_filter_clamp_bound(raw_min, false);
	double pmax = mfd_filter_clamp_bound(raw_max, true);
	if (pmin >= pmax) {
		pmin = -MFD_FILTER_BOUND_LIMIT;
		pmax = MFD_FILTER_BOUND_LIMIT;
	}
	if (out_min) {
		*out_min = pmin;
	}
	if (out_max) {
		*out_max = pmax;
	}
}

bool mfd_filter_range_is_valid(double from_val, double to_val, double prop_min, double prop_max)
{
	if (!isfinite(from_val) || !isfinite(to_val) || !isfinite(prop_min) || !isfinite(prop_max)) {
		return false;
	}
	if (prop_min >= prop_max) {
		return false;
	}
	if (from_val < prop_min - 1e-9 || from_val > prop_max + 1e-9) {
		return false;
	}
	if (to_val < prop_min - 1e-9 || to_val > prop_max + 1e-9) {
		return false;
	}
	if (fabs(to_val - from_val) < MFD_FILTER_MIN_RANGE_SPAN) {
		return false;
	}
	return true;
}

double mfd_filter_map_value(float norm_01, double from_val, double to_val, double prop_min, double prop_max,
			    bool is_int)
{
	float t = clamp01f(norm_01);
	double pmin = 0.0;
	double pmax = 1.0;
	mfd_filter_normalize_bounds(prop_min, prop_max, &pmin, &pmax);

	double f = isfinite(from_val) ? from_val : pmin;
	double to = isfinite(to_val) ? to_val : pmax;
	double mapped = f + (to - f) * (double)t;

	if (mapped < pmin) {
		mapped = pmin;
	} else if (mapped > pmax) {
		mapped = pmax;
	}

	if (is_int) {
		int64_t q = (int64_t)llround(mapped);
		mapped = (double)q;
		if (mapped < pmin) {
			mapped = ceil(pmin);
		} else if (mapped > pmax) {
			mapped = floor(pmax);
		}
	}

	if (fabs(mapped) < 1e-12) {
		mapped = 0.0;
	}
	return mapped;
}

float mfd_filter_unmap_value(double live_val, double from_val, double to_val)
{
	if (!isfinite(live_val) || !isfinite(from_val) || !isfinite(to_val)) {
		return 0.0f;
	}
	double span = to_val - from_val;
	if (fabs(span) < MFD_FILTER_MIN_RANGE_SPAN) {
		return 0.0f;
	}
	double t = (live_val - from_val) / span;
	if (t <= 0.0) {
		return 0.0f;
	}
	if (t >= 1.0) {
		return 1.0f;
	}
	return (float)t;
}

bool mfd_filter_should_write_mapped(bool has_prev, bool is_int, int64_t prev_int, double prev_float, double new_mapped,
				    double from_val, double to_val, float new_norm_01)
{
	if (!has_prev) {
		return true;
	}
	if (is_int) {
		int64_t q = (int64_t)llround(new_mapped);
		return q != prev_int;
	}
	double span = fabs(to_val - from_val);
	double eps = fmax(1e-6, span * 0.001);
	double diff = fabs(new_mapped - prev_float);
	if (diff >= eps) {
		return true;
	}
	float t = clamp01f(new_norm_01);
	if ((t <= 0.0005f || t >= 0.9995f) && diff > 1e-9) {
		return true;
	}
	return false;
}

int mfd_filter_decimals_for_step(bool is_int, double step)
{
	if (is_int) {
		return 0;
	}
	double s = fabs(step);
	if (!isfinite(s) || s <= 0.0) {
		return 3;
	}
	if (s >= 1.0 - 1e-9) {
		return 0;
	}
	if (s >= 0.1 - 1e-9) {
		return 1;
	}
	if (s >= 0.01 - 1e-9) {
		return 2;
	}
	return 3;
}

void mfd_filter_format_display_value(float norm_01, double from_val, double to_val, double prop_min, double prop_max,
				     bool is_int, double step, const char *suffix, bool bypass_at_zero, char *out,
				     size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';

	float t = clamp01f(norm_01);
	double mapped = mfd_filter_map_value(t, from_val, to_val, prop_min, prop_max, is_int);
	const char *sfx = suffix ? suffix : "";
	const char *off_tag = (bypass_at_zero && t <= MFD_FILTER_BYPASS_ZERO_THRESHOLD) ? " (Off)" : "";

	if (is_int) {
		int64_t q = (int64_t)llround(mapped);
		snprintf(out, out_size, "%" PRId64 "%s%s", q, sfx, off_tag);
		return;
	}

	int dec = mfd_filter_decimals_for_step(false, step);
	double scale = pow(10.0, (double)dec);
	double qf = round(mapped * scale + (mapped >= 0.0 ? 1e-12 : -1e-12)) / scale;
	if (fabs(qf) < 0.5 / scale) {
		qf = 0.0;
	}
	snprintf(out, out_size, "%.*f%s%s", dec, qf, sfx, off_tag);
}
