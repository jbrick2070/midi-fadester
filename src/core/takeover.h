/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "core/midi-types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_DEFAULT_PICKUP_THRESHOLD 0.015f
#define MFD_THROW_COMPLETE_THRESHOLD 0.90f
#define MFD_PARKED_UNPARK_HYSTERESIS 0.08f

enum mfd_takeover_state {
	MFD_TAKEOVER_ENGAGED = 0,
	MFD_TAKEOVER_WAITING = 1,
	MFD_TAKEOVER_PARKED = 2,
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
	float last_raw_fader;
	float target_obs;
	float pickup_threshold;
	float parked_peak;
	float rescale_origin;
	bool has_last_fader;
	bool bidirectional_flip;
	bool polarity_reversed;
	bool parked_at_upper_rail;
};

void mfd_takeover_init(struct mfd_takeover *t, float initial_obs_val, float pickup_threshold, bool bidirectional_flip);
void mfd_takeover_sync_external(struct mfd_takeover *t, float new_obs_val);
bool mfd_takeover_step(struct mfd_takeover *t, float raw_fader_norm, float *out_effective_norm);
void mfd_takeover_notify_tbar_completed(struct mfd_takeover *t);
float mfd_takeover_throw_to_mix_value(const struct mfd_takeover *t, float throw_progress);
float mfd_takeover_mix_to_throw_progress(const struct mfd_takeover *t, float mix_value);

int mfd_takeover_format_output_bar(const struct mfd_takeover *t, enum mfd_target_type target_type, float applied_val,
				   float alpha_a, float alpha_b, float db_val, char *out_text, size_t text_size);

#ifdef __cplusplus
}
#endif
