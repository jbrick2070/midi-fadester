/*
 * Midi Fadester — test-filter-mod
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/filter-math.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	/* 1. Bound clamping & normalization for third-party ±DBL_MAX / non-finite sliders */
	REQUIRE(fabs(mfd_filter_clamp_bound(-DBL_MAX, false) - (-MFD_FILTER_BOUND_LIMIT)) < 1e-6);
	REQUIRE(fabs(mfd_filter_clamp_bound(DBL_MAX, true) - MFD_FILTER_BOUND_LIMIT) < 1e-6);
	REQUIRE(fabs(mfd_filter_clamp_bound(NAN, false) - (-MFD_FILTER_BOUND_LIMIT)) < 1e-6);
	REQUIRE(fabs(mfd_filter_clamp_bound(INFINITY, true) - MFD_FILTER_BOUND_LIMIT) < 1e-6);

	double pmin = 0.0;
	double pmax = 0.0;
	mfd_filter_normalize_bounds(10.0, -5.0, &pmin, &pmax);
	REQUIRE(fabs(pmin - (-MFD_FILTER_BOUND_LIMIT)) < 1e-6);
	REQUIRE(fabs(pmax - MFD_FILTER_BOUND_LIMIT) < 1e-6);

	/* 2. Range validation (forward, inverted, equal-endpoint rejection, out-of-bounds rejection) */
	REQUIRE(mfd_filter_range_is_valid(0.0, 0.35, -1.0, 1.0));
	REQUIRE(mfd_filter_range_is_valid(0.0, -1.0, -1.0, 5.0)); /* inverted monochrome sweep */
	REQUIRE(!mfd_filter_range_is_valid(0.5, 0.5, 0.0, 1.0));  /* equal endpoints rejected */
	REQUIRE(!mfd_filter_range_is_valid(-1.5, 0.5, -1.0, 1.0));
	REQUIRE(!mfd_filter_range_is_valid(0.0, 1.5, -1.0, 1.0));
	REQUIRE(!mfd_filter_range_is_valid(NAN, 1.0, 0.0, 1.0));

	/* 3. Forward & inverted [from_val, to_val] linear mapping + int rounding + unmap */
	REQUIRE(fabs(mfd_filter_map_value(0.0f, 0.0, 0.35, 0.0, 1.0, false) - 0.0) < 1e-9);
	REQUIRE(fabs(mfd_filter_map_value(0.5f, 0.0, 0.35, 0.0, 1.0, false) - 0.175) < 1e-9);
	REQUIRE(fabs(mfd_filter_map_value(1.0f, 0.0, 0.35, 0.0, 1.0, false) - 0.35) < 1e-9);

	/* Inverted sweep: Saturation 0.0 -> -1.0 across t in [0..1] (float norm_01 -> ~1e-7 precision) */
	REQUIRE(fabs(mfd_filter_map_value(0.0f, 0.0, -1.0, -1.0, 5.0, false) - 0.0) < 1e-9);
	REQUIRE(fabs(mfd_filter_map_value(0.4f, 0.0, -1.0, -1.0, 5.0, false) - (-0.4)) < 1e-6);
	REQUIRE(fabs(mfd_filter_map_value(1.0f, 0.0, -1.0, -1.0, 5.0, false) - (-1.0)) < 1e-9);

	/* Integer rounding (Blur radius 0..28 px at t = 0.5 -> 14, t = 0.24 -> 7) */
	REQUIRE(fabs(mfd_filter_map_value(0.5f, 0.0, 28.0, 0.0, 100.0, true) - 14.0) < 1e-9);
	REQUIRE(fabs(mfd_filter_map_value(0.24f, 0.0, 28.0, 0.0, 100.0, true) - 7.0) < 1e-9);

	/* Inverse unmap for startup & external OBS edit sync */
	REQUIRE(fabsf(mfd_filter_unmap_value(0.175, 0.0, 0.35) - 0.5f) < 1e-6f);
	REQUIRE(fabsf(mfd_filter_unmap_value(-0.4, 0.0, -1.0) - 0.4f) < 1e-6f);
	REQUIRE(fabsf(mfd_filter_unmap_value(0.80, 0.0, 0.35) - 1.0f) < 1e-6f); /* clamped above range */
	REQUIRE(fabsf(mfd_filter_unmap_value(-0.5, 0.0, 0.35) - 0.0f) < 1e-6f); /* clamped below range */

	/* 4. Quantized mapped-value write gate (int quantization + float 0.001*span epsilon + endpoint snap) */
	REQUIRE(mfd_filter_should_write_mapped(false, true, 0, 0.0, 0.0, 0.0, 28.0, 0.0f));
	REQUIRE(!mfd_filter_should_write_mapped(true, true, 14, 0.0, 14.0, 0.0, 28.0, 0.501f));
	REQUIRE(mfd_filter_should_write_mapped(true, true, 14, 0.0, 15.0, 0.0, 28.0, 0.536f));

	/* Float span = 100.0 -> eps = 0.1; sub-epsilon mid-throw skipped, endpoint 0.0/1.0 snaps cleanly */
	REQUIRE(!mfd_filter_should_write_mapped(true, false, 0, 50.0, 50.04, 0.0, 100.0, 0.5004f));
	REQUIRE(mfd_filter_should_write_mapped(true, false, 0, 50.0, 50.12, 0.0, 100.0, 0.5012f));
	REQUIRE(mfd_filter_should_write_mapped(true, false, 0, 0.04, 0.0, 0.0, 100.0, 0.0f));
	REQUIRE(!mfd_filter_should_write_mapped(true, false, 0, 0.0, 0.0, 0.0, 100.0, 0.0f));
	REQUIRE(mfd_filter_should_write_mapped(true, false, 0, 99.96, 100.0, 0.0, 100.0, 1.0f));

	/* 5. Display value formatting (step precision, -0.0 suppression, units, and "(Off)" suffix) */
	char disp[48];
	mfd_filter_format_display_value(0.5f, 0.0, 28.0, 0.0, 100.0, true, 1.0, " px", true, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "14 px") == 0);

	mfd_filter_format_display_value(0.0f, 0.0, 28.0, 0.0, 100.0, true, 1.0, " px", true, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "0 px (Off)") == 0);

	mfd_filter_format_display_value(0.0f, 0.0, 28.0, 0.0, 100.0, true, 1.0, " px", false, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "0 px") == 0);

	mfd_filter_format_display_value(0.5f, 0.0, 0.35, 0.0, 1.0, false, 0.01, "", true, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "0.18") == 0);

	mfd_filter_format_display_value(0.4f, 0.0, -100.0, -100.0, 100.0, false, 1.0, "%", false, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "-40%") == 0);

	/* Near-zero negative value (-0.0001 with step 0.01) must print "0.00", never "-0.00" */
	mfd_filter_format_display_value(0.0001f, 0.0, -1.0, -1.0, 1.0, false, 0.01, "", false, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "0.00") == 0);

	/* Fine shader uniform step 0.001 -> 3 decimals */
	mfd_filter_format_display_value(0.125f, 0.0, 1.0, 0.0, 1.0, false, 0.001, "", false, disp, sizeof(disp));
	REQUIRE(strcmp(disp, "0.125") == 0);

	printf("test-filter-mod: PASS\n");
	return 0;
}
