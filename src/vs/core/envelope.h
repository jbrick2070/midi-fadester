/*
 * Visual Sidechain - Auto-Gain, dB Normalization & Asymmetric Envelope Follower
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_ENV_DEFAULT_GATE_DB (-55.0f)
#define VS_ENV_DEFAULT_FLOOR_DB (-45.0f)
#define VS_ENV_DEFAULT_CEILING_DB (-6.0f)
#define VS_ENV_DEFAULT_ATTACK_MS 10.0f
#define VS_ENV_DEFAULT_RELEASE_MS 180.0f
#define VS_ENV_CREST_DECAY_SEC 4.0f
#define VS_ENV_STARVATION_THRESHOLD_SEC 0.050f

struct vs_envelope_config {
	bool auto_gain;
	float gate_db;
	float floor_db;
	float ceiling_db;
	float attack_ms;
	float release_ms;
};

struct vs_envelope_channel {
	float value;
	float running_peak;
};

void vs_envelope_default_config(struct vs_envelope_config *cfg);
void vs_envelope_channel_reset(struct vs_envelope_channel *ch);

/*
 * Normalizes a raw linear amplitude (>= 0) into [0.0, 1.0] using either
 * Auto-Gain (with noise-gate floor gate_db and slow running peak decay)
 * or manual [floor_db, ceiling_db] dB mapping.
 */
float vs_envelope_normalize(struct vs_envelope_channel *ch, const struct vs_envelope_config *cfg, float raw_linear,
			    float dt_sec);

/*
 * Applies frame-rate-independent asymmetric attack/release smoothing to an
 * already-normalized [0.0, 1.0] target value.
 */
float vs_envelope_smooth(struct vs_envelope_channel *ch, const struct vs_envelope_config *cfg, float normalized_target,
			 float dt_sec);

/*
 * Full single-channel step: handles > 50 ms audio starvation decay, normalizes
 * raw_linear, and applies attack/release smoothing.
 */
float vs_envelope_step(struct vs_envelope_channel *ch, const struct vs_envelope_config *cfg, float raw_linear,
		       float dt_sec, float time_since_audio_sec);

#ifdef __cplusplus
}
#endif
