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

#define MFD_DEFAULT_PICKUP_THRESHOLD 0.015f

enum mfd_takeover_state {
	MFD_TAKEOVER_ENGAGED = 0,
	MFD_TAKEOVER_WAITING = 1,
};

enum mfd_takeover_hint {
	MFD_HINT_NONE = 0,
	MFD_HINT_MOVE_UP = 1,
	MFD_HINT_MOVE_DOWN = 2,
};

struct mfd_takeover {
	enum mfd_takeover_state state;
	enum mfd_takeover_hint hint;
	float last_fader;
	float target_obs;
	float pickup_threshold;
	bool has_last_fader;
	bool bidirectional_flip;
	bool polarity_reversed;
};

void mfd_takeover_init(struct mfd_takeover *t, float initial_obs_val, float pickup_threshold, bool bidirectional_flip);
void mfd_takeover_sync_external(struct mfd_takeover *t, float new_obs_val);
bool mfd_takeover_step(struct mfd_takeover *t, float raw_fader_norm, float *out_effective_norm);
void mfd_takeover_notify_tbar_completed(struct mfd_takeover *t);

#ifdef __cplusplus
}
#endif
