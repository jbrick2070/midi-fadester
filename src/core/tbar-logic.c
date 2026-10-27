/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/tbar-logic.h"
#include <math.h>
#include <string.h>

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

void mfd_tbar_compute_action(bool studio_mode_active, bool slider_tbar_found, float t_norm, int last_pos_1024,
			     struct mfd_tbar_action *out_action)
{
	if (!out_action) {
		return;
	}
	memset(out_action, 0, sizeof(*out_action));

	if (!studio_mode_active) {
		out_action->allow_frontend_calls = false;
		out_action->report_missing_slider_error = false;
		return;
	}

	if (!slider_tbar_found) {
		out_action->allow_frontend_calls = false;
		out_action->report_missing_slider_error = true;
		return;
	}

	float clamped = clamp01(t_norm);
	int pos = (int)lroundf(clamped * (float)MFD_TBAR_RANGE_MAX);
	if (pos < 0) {
		pos = 0;
	}
	if (pos > MFD_TBAR_RANGE_MAX) {
		pos = MFD_TBAR_RANGE_MAX;
	}

	out_action->allow_frontend_calls = true;
	out_action->report_missing_slider_error = false;
	out_action->target_pos_1024 = pos;
	if (last_pos_1024 >= MFD_TBAR_COMPLETE_POS && pos >= MFD_TBAR_COMPLETE_POS) {
		out_action->call_set_tbar = false;
		out_action->call_release_tbar = false;
	} else {
		out_action->call_set_tbar = (pos != last_pos_1024);
		out_action->call_release_tbar = (pos >= MFD_TBAR_COMPLETE_POS && last_pos_1024 < MFD_TBAR_COMPLETE_POS);
	}
}

bool mfd_should_show_studio_mode_note(bool studio_mode_active, size_t active_tbar_slot_count)
{
	return (!studio_mode_active) && (active_tbar_slot_count > 0U);
}
