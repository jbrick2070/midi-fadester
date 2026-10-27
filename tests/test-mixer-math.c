/*
 * Midi Fadester — test-mixer-math
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/mixer-math.h"
#include <math.h>
#include <stdio.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	float a = 0.0f;
	float b = 0.0f;

	/* 1. Naive linear crossfade (A = 1-t, B = t) at t = 0.5 has only 75% Porter-Duff coverage (25% black dip) */
	mfd_crossfade_eval(MFD_CURVE_LINEAR, 0.5f, &a, &b);
	REQUIRE(fabsf(a - 0.5f) < 1e-6f);
	REQUIRE(fabsf(b - 0.5f) < 1e-6f);
	REQUIRE(fabsf(mfd_porter_duff_over_coverage(a, b) - 0.75f) < 1e-6f);

	/* 2. Dip-Free Video Mixer Dissolve has 100% Porter-Duff coverage at t = 0.0, 0.25, 0.50, 1.00 (0% black dip at midpoint!) */
	mfd_crossfade_eval(MFD_CURVE_DIP_FREE, 0.0f, &a, &b);
	REQUIRE(fabsf(a - 1.0f) < 1e-6f && fabsf(b - 0.0f) < 1e-6f);
	REQUIRE(fabsf(mfd_porter_duff_over_coverage(a, b) - 1.0f) < 1e-6f);

	mfd_crossfade_eval(MFD_CURVE_DIP_FREE, 0.25f, &a, &b);
	REQUIRE(fabsf(a - 1.0f) < 1e-6f && fabsf(b - 0.25f) < 1e-6f);
	REQUIRE(fabsf(mfd_porter_duff_over_coverage(a, b) - 1.0f) < 1e-6f);

	mfd_crossfade_eval(MFD_CURVE_DIP_FREE, 0.50f, &a, &b);
	REQUIRE(fabsf(a - 1.0f) < 1e-6f && fabsf(b - 0.50f) < 1e-6f);
	REQUIRE(fabsf(mfd_porter_duff_over_coverage(a, b) - 1.0f) < 1e-6f);

	mfd_crossfade_eval(MFD_CURVE_DIP_FREE, 1.0f, &a, &b);
	REQUIRE(fabsf(a - 0.0f) < 1e-6f && fabsf(b - 1.0f) < 1e-6f);
	REQUIRE(fabsf(mfd_porter_duff_over_coverage(a, b) - 1.0f) < 1e-6f);

	/* 3. Full-Center Mix puts both A and B at 1.0 at t = 0.5 */
	mfd_crossfade_eval(MFD_CURVE_FULL_CENTER, 0.5f, &a, &b);
	REQUIRE(fabsf(a - 1.0f) < 1e-6f && fabsf(b - 1.0f) < 1e-6f);

	/* 4. Equal-Power Cosine puts A^2 + B^2 = 1.0 everywhere */
	for (int i = 0; i <= 10; ++i) {
		float t = (float)i / 10.0f;
		mfd_crossfade_eval(MFD_CURVE_COSINE, t, &a, &b);
		REQUIRE(fabsf((a * a + b * b) - 1.0f) < 1e-5f);
	}

	/* 5. OBS Cubic Audio Law (def^3 and cbrtf round-trip) */
	REQUIRE(mfd_deflection_to_mul(0.0f) == 0.0f);
	REQUIRE(isinf(mfd_deflection_to_db(0.0f)) && mfd_deflection_to_db(0.0f) < 0.0f);
	REQUIRE(mfd_deflection_to_mul(1.0f) == 1.0f);
	REQUIRE(mfd_deflection_to_db(1.0f) == 0.0f);
	REQUIRE(fabsf(mfd_deflection_to_mul(0.5f) - 0.125f) < 1e-6f);
	REQUIRE(fabsf(mfd_mul_to_deflection(0.125f) - 0.5f) < 1e-6f);

	for (int i = 0; i <= 127; ++i) {
		float def = (float)i / 127.0f;
		float mul = mfd_deflection_to_mul(def);
		float rt = mfd_mul_to_deflection(mul);
		REQUIRE(fabsf(rt - def) < 1e-5f);
	}

	printf("test-mixer-math: PASS\n");
	return 0;
}
