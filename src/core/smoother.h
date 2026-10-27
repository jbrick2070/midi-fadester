/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_DEFAULT_SMOOTH_TAU_MS 30.0f
#define MFD_SMOOTH_SNAP_EPSILON 0.0005f

struct mfd_smoother {
	float current;
	float target;
	float tau_ms;
	bool active;
};

void mfd_smoother_init(struct mfd_smoother *s, float tau_ms, float initial_value);
void mfd_smoother_set_tau_ms(struct mfd_smoother *s, float tau_ms);
void mfd_smoother_set_target(struct mfd_smoother *s, float target);
void mfd_smoother_reset(struct mfd_smoother *s, float value);
float mfd_smoother_tick(struct mfd_smoother *s, float dt_sec);
bool mfd_smoother_is_active(const struct mfd_smoother *s);

#ifdef __cplusplus
}
#endif
