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

void mfd_calibration_init(struct mfd_control_calibration *cal, bool is_14bit)
{
	if (!cal) {
		return;
	}
	cal->cal_min = MFD_CAL_DEFAULT_MIN;
	cal->cal_max = is_14bit ? MFD_CAL_DEFAULT_MAX_14BIT : MFD_CAL_DEFAULT_MAX_7BIT;
}

void mfd_calibration_set(struct mfd_control_calibration *cal, bool is_14bit, int cal_min, int cal_max)
{
	if (!cal) {
		return;
	}
	int max_limit = is_14bit ? MFD_CAL_DEFAULT_MAX_14BIT : MFD_CAL_DEFAULT_MAX_7BIT;
	if (cal_min < 0) {
		cal_min = 0;
	}
	if (cal_max > max_limit) {
		cal_max = max_limit;
	}
	if (cal_max <= cal_min) {
		mfd_calibration_init(cal, is_14bit);
		return;
	}
	cal->cal_min = cal_min;
	cal->cal_max = cal_max;
}

bool mfd_calibration_observe_and_widen(struct mfd_control_calibration *cal, int raw_value)
{
	if (!cal) {
		return false;
	}
	bool widened = false;
	if (raw_value < cal->cal_min) {
		cal->cal_min = raw_value;
		widened = true;
	}
	if (raw_value > cal->cal_max) {
		cal->cal_max = raw_value;
		widened = true;
	}
	return widened;
}

float mfd_calibration_normalize(const struct mfd_control_calibration *cal, int raw_value, bool invert)
{
	float x = 0.0f;
	if (!cal || cal->cal_max <= cal->cal_min) {
		x = clamp01((float)raw_value / (float)MFD_CAL_DEFAULT_MAX_7BIT);
	} else {
		x = clamp01((float)(raw_value - cal->cal_min) / (float)(cal->cal_max - cal->cal_min));
	}
	return invert ? (1.0f - x) : x;
}

bool mfd_tbar_is_in_target_rail_zone(float x_cal, bool polarity_reversed)
{
	float clamped = clamp01(x_cal);
	if (polarity_reversed) {
		return clamped <= (MFD_TBAR_RAIL_ZONE + 1e-6f);
	}
	return clamped >= ((1.0f - MFD_TBAR_RAIL_ZONE) - 1e-6f);
}

bool mfd_tbar_is_in_origin_rail_zone(float x_cal, bool polarity_reversed)
{
	float clamped = clamp01(x_cal);
	if (polarity_reversed) {
		return clamped >= ((1.0f - MFD_TBAR_RAIL_ZONE) - 1e-6f);
	}
	return clamped <= (MFD_TBAR_RAIL_ZONE + 1e-6f);
}

bool mfd_tbar_is_in_rail_zone(float x_cal, bool polarity_reversed)
{
	return mfd_tbar_is_in_target_rail_zone(x_cal, polarity_reversed);
}

static int clamp_throwing_pos(float t_norm)
{
	int pos = (int)lroundf(clamp01(t_norm) * (float)MFD_TBAR_RANGE_MAX);
	if (pos < 1) {
		pos = 1;
	}
	if (pos > (MFD_TBAR_RANGE_MAX - 1)) {
		pos = MFD_TBAR_RANGE_MAX - 1;
	}
	return pos;
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
		out_action->next_state = MFD_TBAR_IDLE;
		return;
	}

	if (!in->slider_tbar_found) {
		out_action->allow_frontend_calls = false;
		out_action->report_missing_slider_error = true;
		out_action->next_state = MFD_TBAR_IDLE;
		return;
	}

	out_action->allow_frontend_calls = true;
	out_action->report_missing_slider_error = false;

	if (in->state == MFD_TBAR_PARKED || in->release_latched) {
		out_action->next_state = MFD_TBAR_PARKED;
		out_action->target_pos_1024 = 0;
		out_action->call_set_tbar = false;
		out_action->call_release_tbar = false;
		return;
	}

	if (in->state == MFD_TBAR_IDLE) {
		if (in->force_complete || in->in_target_rail_zone) {
			out_action->next_state = MFD_TBAR_PARKED;
			out_action->target_pos_1024 = MFD_TBAR_RANGE_MAX;
			out_action->call_set_tbar = true;
			out_action->call_release_tbar = true;
			out_action->completed_throw = true;
			return;
		}
		if (in->force_abort || in->in_origin_rail_zone) {
			out_action->next_state = MFD_TBAR_IDLE;
			out_action->target_pos_1024 = 0;
			out_action->call_set_tbar = false;
			out_action->call_release_tbar = false;
			return;
		}
		int pos = clamp_throwing_pos(in->t_norm);
		out_action->next_state = MFD_TBAR_THROWING;
		out_action->target_pos_1024 = pos;
		out_action->call_set_tbar = (pos != in->last_pos_1024);
		out_action->call_release_tbar = false;
		return;
	}

	/* MFD_TBAR_THROWING */
	if (in->force_complete || in->in_target_rail_zone) {
		out_action->next_state = MFD_TBAR_PARKED;
		out_action->target_pos_1024 = MFD_TBAR_RANGE_MAX;
		out_action->call_set_tbar = true;
		out_action->call_release_tbar = true;
		out_action->completed_throw = true;
		return;
	}

	if (in->force_abort || in->in_origin_rail_zone) {
		out_action->next_state = MFD_TBAR_IDLE;
		out_action->target_pos_1024 = 0;
		out_action->call_set_tbar = true;
		out_action->call_release_tbar = true;
		out_action->aborted_throw = true;
		return;
	}

	int pos = clamp_throwing_pos(in->t_norm);
	out_action->next_state = MFD_TBAR_THROWING;
	out_action->target_pos_1024 = pos;
	out_action->call_set_tbar = (pos != in->last_pos_1024);
	out_action->call_release_tbar = false;
}

void mfd_tbar_compute_action_ex(bool studio_mode_active, bool slider_tbar_found, bool release_latched, float t_norm,
				int last_pos_1024, struct mfd_tbar_action *out_action)
{
	struct mfd_tbar_release_input in;
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = studio_mode_active;
	in.slider_tbar_found = slider_tbar_found;
	in.state = release_latched ? MFD_TBAR_PARKED : MFD_TBAR_IDLE;
	in.release_latched = release_latched;
	in.force_complete = false;
	in.force_abort = false;
	in.in_target_rail_zone = mfd_tbar_is_in_target_rail_zone(t_norm, false);
	in.in_origin_rail_zone = mfd_tbar_is_in_origin_rail_zone(t_norm, false);
	in.t_norm = t_norm;
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
