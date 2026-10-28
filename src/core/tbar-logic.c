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

bool mfd_tbar_is_in_rail_zone(float raw_fader_norm, bool polarity_reversed)
{
	float clamped = clamp01(raw_fader_norm);
	if (polarity_reversed) {
		return clamped <= (MFD_TBAR_RAIL_ZONE + 1e-6f);
	}
	return clamped >= ((1.0f - MFD_TBAR_RAIL_ZONE) - 1e-6f);
}

void mfd_tbar_compute_action_full(const struct mfd_tbar_release_input *in, struct mfd_tbar_action *out_action)
{
	if (!in || !out_action) {
		return;
	}
	memset(out_action, 0, sizeof(*out_action));

	if (!in->studio_mode_active) {
		out_action->allow_frontend_calls = false;
		out_action->report_missing_slider_error = false;
		return;
	}

	if (!in->slider_tbar_found) {
		out_action->allow_frontend_calls = false;
		out_action->report_missing_slider_error = true;
		return;
	}

	out_action->allow_frontend_calls = true;
	out_action->report_missing_slider_error = false;

	if (in->release_latched) {
		out_action->target_pos_1024 = 0;
		out_action->call_set_tbar = false;
		out_action->call_release_tbar = false;
		return;
	}

	float clamped = clamp01(in->force_release ? 1.0f : in->t_norm);
	int pos = (int)lroundf(clamped * (float)MFD_TBAR_RANGE_MAX);
	if (pos < 0) {
		pos = 0;
	}
	if (pos > MFD_TBAR_RANGE_MAX) {
		pos = MFD_TBAR_RANGE_MAX;
	}

	out_action->target_pos_1024 = pos;

	bool should_release = false;
	if (pos >= MFD_TBAR_OBS_MIN_RELEASE_POS) {
		const bool rail_trigger = in->in_rail_zone &&
					  (pos >= MFD_TBAR_RAIL_RELEASE_POS || in->smoother_settled);
		const bool stall_fallback = isfinite(in->fader_idle_sec) &&
					    (in->fader_idle_sec >= (MFD_TBAR_FALLBACK_STALL_SEC - 1e-6f));
		if (in->force_release || rail_trigger || stall_fallback) {
			should_release = true;
		}
	}

	if (should_release) {
		out_action->call_set_tbar = true;
		out_action->call_release_tbar = true;
	} else {
		out_action->call_set_tbar = (pos != in->last_pos_1024);
		out_action->call_release_tbar = false;
	}
}

void mfd_tbar_compute_action_ex(bool studio_mode_active, bool slider_tbar_found, bool release_latched, float t_norm,
				int last_pos_1024, struct mfd_tbar_action *out_action)
{
	struct mfd_tbar_release_input in;
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = studio_mode_active;
	in.slider_tbar_found = slider_tbar_found;
	in.release_latched = release_latched;
	in.force_release = false;
	in.in_rail_zone = mfd_tbar_is_in_rail_zone(t_norm, false);
	in.smoother_settled = true;
	in.t_norm = t_norm;
	in.fader_idle_sec = 0.0f;
	in.last_pos_1024 = last_pos_1024;
	mfd_tbar_compute_action_full(&in, out_action);
}

void mfd_tbar_compute_action(bool studio_mode_active, bool slider_tbar_found, float t_norm, int last_pos_1024,
			     struct mfd_tbar_action *out_action)
{
	mfd_tbar_compute_action_ex(studio_mode_active, slider_tbar_found, false, t_norm, last_pos_1024, out_action);
}

bool mfd_should_show_studio_mode_note(bool studio_mode_active, size_t active_tbar_slot_count)
{
	return (!studio_mode_active) && (active_tbar_slot_count > 0U);
}
