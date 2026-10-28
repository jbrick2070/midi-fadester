/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include "core/midi-types.h"

#ifdef __cplusplus
extern "C" {
#endif

void mfd_crossfade_eval(enum mfd_crossfade_curve curve, float t, float *out_alpha_a, float *out_alpha_b);
void mfd_crossfade_eval_stacked(enum mfd_crossfade_curve curve, float t, bool a_is_above_b, float *out_alpha_a,
				float *out_alpha_b);
float mfd_porter_duff_over_coverage(float alpha_bottom, float alpha_top);
float mfd_deflection_to_mul(float def);
float mfd_mul_to_deflection(float mul);
float mfd_deflection_to_db(float def);

#ifdef __cplusplus
}
#endif
