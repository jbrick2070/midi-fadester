/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/mixer-math.h"
#include <math.h>

#define MFD_HALF_PI 1.5707963267948966f

static float clamp01(float v)
{
	if (!isfinite(v) || v <= 0.0f) {
		return 0.0f;
	}
	if (v >= 1.0f) {
		return 1.0f;
	}
	return v;
}

void mfd_crossfade_eval(enum mfd_crossfade_curve curve, float t, float *out_alpha_a, float *out_alpha_b)
{
	if (!out_alpha_a || !out_alpha_b) {
		return;
	}

	float x = clamp01(t);

	switch (curve) {
	case MFD_CURVE_DIP_FREE:
		/*
		 * Dip-Free Video Mixer Dissolve (D-2):
		 * Source A (bottom) stays at 1.0 across [0.0, 0.5] and ramps 1.0 -> 0.0 across [0.5, 1.0],
		 * while Source B (top) ramps 0.0 -> 1.0 across [0.0, 1.0]. At midpoint (t = 0.5),
		 * Porter-Duff Source Over yields 0.5*B + (1 - 0.5)*1.0*A = 0.5*B + 0.5*A with 0.0% black dip.
		 */
		*out_alpha_a = clamp01(2.0f * (1.0f - x));
		*out_alpha_b = x;
		break;

	case MFD_CURVE_FULL_CENTER:
		*out_alpha_a = clamp01(2.0f * (1.0f - x));
		*out_alpha_b = clamp01(2.0f * x);
		break;

	case MFD_CURVE_COSINE:
		*out_alpha_a = clamp01(cosf(x * MFD_HALF_PI));
		*out_alpha_b = clamp01(sinf(x * MFD_HALF_PI));
		break;

	case MFD_CURVE_LINEAR:
	default:
		*out_alpha_a = 1.0f - x;
		*out_alpha_b = x;
		break;
	}
}

float mfd_porter_duff_over_coverage(float alpha_bottom, float alpha_top)
{
	float a = clamp01(alpha_bottom);
	float b = clamp01(alpha_top);
	return b + (1.0f - b) * a;
}

float mfd_deflection_to_mul(float def)
{
	float d = clamp01(def);
	if (d <= 0.0f) {
		return 0.0f;
	}
	if (d >= 1.0f) {
		return 1.0f;
	}
	return d * d * d;
}

float mfd_mul_to_deflection(float mul)
{
	float m = clamp01(mul);
	if (m <= 0.0f) {
		return 0.0f;
	}
	if (m >= 1.0f) {
		return 1.0f;
	}
	return cbrtf(m);
}

float mfd_deflection_to_db(float def)
{
	float d = clamp01(def);
	if (d <= 0.0f) {
		return -INFINITY;
	}
	if (d >= 1.0f) {
		return 0.0f;
	}
	return 60.0f * log10f(d);
}
