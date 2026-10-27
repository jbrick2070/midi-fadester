/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/takeover.h"
#include <math.h>

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

static enum mfd_takeover_hint compute_hint(float fader_val, float target_val, float threshold)
{
	if (fabsf(target_val - fader_val) <= threshold) {
		return MFD_HINT_NONE;
	}
	return (target_val > fader_val) ? MFD_HINT_MOVE_UP : MFD_HINT_MOVE_DOWN;
}

void mfd_takeover_init(struct mfd_takeover *t, float initial_obs_val, float pickup_threshold, bool bidirectional_flip)
{
	if (!t) {
		return;
	}
	t->state = MFD_TAKEOVER_WAITING;
	t->hint = MFD_HINT_NONE;
	t->last_fader = 0.0f;
	t->target_obs = clamp01(initial_obs_val);
	t->pickup_threshold = (isfinite(pickup_threshold) && pickup_threshold > 0.0f) ? pickup_threshold
										      : MFD_DEFAULT_PICKUP_THRESHOLD;
	t->has_last_fader = false;
	t->bidirectional_flip = bidirectional_flip;
	t->polarity_reversed = false;
}

void mfd_takeover_sync_external(struct mfd_takeover *t, float new_obs_val)
{
	if (!t) {
		return;
	}
	float clamped = clamp01(new_obs_val);
	if (fabsf(clamped - t->target_obs) > t->pickup_threshold) {
		t->target_obs = clamped;
		t->state = MFD_TAKEOVER_WAITING;
		if (t->has_last_fader) {
			t->hint = compute_hint(t->last_fader, t->target_obs, t->pickup_threshold);
		}
	}
}

bool mfd_takeover_step(struct mfd_takeover *t, float raw_fader_norm, float *out_effective_norm)
{
	if (!t || !out_effective_norm) {
		return false;
	}

	float raw_clamped = clamp01(raw_fader_norm);
	float effective = (t->bidirectional_flip && t->polarity_reversed) ? (1.0f - raw_clamped) : raw_clamped;

	if (t->state == MFD_TAKEOVER_WAITING) {
		bool within = (fabsf(effective - t->target_obs) <= t->pickup_threshold);
		bool crossed = false;
		if (t->has_last_fader) {
			crossed = ((t->last_fader <= t->target_obs && effective >= t->target_obs) ||
				   (t->last_fader >= t->target_obs && effective <= t->target_obs));
		}

		t->last_fader = effective;
		t->has_last_fader = true;

		if (within || crossed) {
			t->state = MFD_TAKEOVER_ENGAGED;
			t->hint = MFD_HINT_NONE;
			t->target_obs = effective;
			*out_effective_norm = effective;
			return true;
		}

		t->hint = compute_hint(effective, t->target_obs, t->pickup_threshold);
		*out_effective_norm = t->target_obs;
		return false;
	}

	t->last_fader = effective;
	t->has_last_fader = true;
	t->target_obs = effective;
	t->hint = MFD_HINT_NONE;
	*out_effective_norm = effective;
	return true;
}

void mfd_takeover_notify_tbar_completed(struct mfd_takeover *t)
{
	if (!t) {
		return;
	}
	if (t->bidirectional_flip) {
		t->polarity_reversed = !t->polarity_reversed;
		t->target_obs = 0.0f;
		t->last_fader = 0.0f;
		t->has_last_fader = true;
		t->state = MFD_TAKEOVER_ENGAGED;
		t->hint = MFD_HINT_NONE;
	} else {
		t->target_obs = 0.0f;
		t->state = MFD_TAKEOVER_WAITING;
		t->hint = MFD_HINT_MOVE_DOWN;
	}
}
