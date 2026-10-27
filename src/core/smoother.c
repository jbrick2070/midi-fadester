/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/smoother.h"
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

void mfd_smoother_init(struct mfd_smoother *s, float tau_ms, float initial_value)
{
	if (!s) {
		return;
	}
	float clamped = clamp01(initial_value);
	s->current = clamped;
	s->target = clamped;
	s->tau_ms = (isfinite(tau_ms) && tau_ms >= 0.0f) ? tau_ms : MFD_DEFAULT_SMOOTH_TAU_MS;
	s->active = false;
}

void mfd_smoother_set_tau_ms(struct mfd_smoother *s, float tau_ms)
{
	if (!s) {
		return;
	}
	s->tau_ms = (isfinite(tau_ms) && tau_ms >= 0.0f) ? tau_ms : 0.0f;
}

void mfd_smoother_set_target(struct mfd_smoother *s, float target)
{
	if (!s) {
		return;
	}
	float clamped = clamp01(target);
	s->target = clamped;
	if (fabsf(s->target - s->current) <= MFD_SMOOTH_SNAP_EPSILON) {
		s->current = s->target;
		s->active = false;
	} else {
		s->active = true;
	}
}

void mfd_smoother_reset(struct mfd_smoother *s, float value)
{
	if (!s) {
		return;
	}
	float clamped = clamp01(value);
	s->current = clamped;
	s->target = clamped;
	s->active = false;
}

float mfd_smoother_tick(struct mfd_smoother *s, float dt_sec)
{
	if (!s) {
		return 0.0f;
	}
	if (!s->active) {
		return s->current;
	}
	if (s->tau_ms <= 0.0f || !isfinite(dt_sec) || dt_sec <= 0.0f) {
		s->current = s->target;
		s->active = false;
		return s->current;
	}

	float tau_sec = s->tau_ms * 0.001f;
	float alpha = 1.0f - expf(-dt_sec / tau_sec);
	s->current += (s->target - s->current) * alpha;
	s->current = clamp01(s->current);

	if (fabsf(s->target - s->current) <= MFD_SMOOTH_SNAP_EPSILON) {
		s->current = s->target;
		s->active = false;
	}

	return s->current;
}

bool mfd_smoother_is_active(const struct mfd_smoother *s)
{
	return s ? s->active : false;
}
