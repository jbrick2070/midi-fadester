/*
 * Visual Sidechain - Auto-Gain, dB Normalization & Asymmetric Envelope Follower
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "envelope.h"

#include <math.h>
#include <string.h>

static inline bool is_finite_f32(float v)
{
	uint32_t bits = 0;
	memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;
}

static float clamp01(float v)
{
	if (!is_finite_f32(v) || v <= 0.0f)
		return 0.0f;
	if (v >= 1.0f)
		return 1.0f;
	return v;
}

void vs_envelope_default_config(struct vs_envelope_config *cfg)
{
	if (!cfg)
		return;
	cfg->auto_gain = true;
	cfg->gate_db = VS_ENV_DEFAULT_GATE_DB;
	cfg->floor_db = VS_ENV_DEFAULT_FLOOR_DB;
	cfg->ceiling_db = VS_ENV_DEFAULT_CEILING_DB;
	cfg->attack_ms = VS_ENV_DEFAULT_ATTACK_MS;
	cfg->release_ms = VS_ENV_DEFAULT_RELEASE_MS;
}

void vs_envelope_channel_reset(struct vs_envelope_channel *ch)
{
	if (!ch)
		return;
	ch->value = 0.0f;
	ch->running_peak = 0.0f;
}

float vs_envelope_normalize(struct vs_envelope_channel *ch, const struct vs_envelope_config *cfg, float raw_linear,
			    float dt_sec)
{
	if (!ch || !cfg)
		return 0.0f;

	if (!is_finite_f32(raw_linear) || raw_linear < 0.0f)
		raw_linear = 0.0f;
	if (!is_finite_f32(dt_sec) || dt_sec < 0.0f)
		dt_sec = 0.0f;

	if (!cfg->auto_gain) {
		const float floor_db = cfg->floor_db;
		const float ceiling_db = (cfg->ceiling_db > floor_db + 1.0f) ? cfg->ceiling_db : (floor_db + 1.0f);
		if (raw_linear <= 1e-9f)
			return 0.0f;
		const float db = 20.0f * log10f(raw_linear);
		return clamp01((db - floor_db) / (ceiling_db - floor_db));
	}

	/* Auto-Gain mode with noise-gate floor (gate_db, default -55 dBFS). */
	const float gate_lin = powf(10.0f, cfg->gate_db / 20.0f);

	if (dt_sec > 0.0f && ch->running_peak > gate_lin) {
		const float decay = expf(-dt_sec / VS_ENV_CREST_DECAY_SEC);
		ch->running_peak *= decay;
		if (ch->running_peak < gate_lin)
			ch->running_peak = gate_lin;
	}

	if (raw_linear <= gate_lin)
		return 0.0f;

	if (raw_linear > ch->running_peak)
		ch->running_peak = raw_linear;

	const float denom = (ch->running_peak > gate_lin * 2.0f) ? ch->running_peak : (gate_lin * 2.0f);
	return clamp01(raw_linear / denom);
}

float vs_envelope_smooth(struct vs_envelope_channel *ch, const struct vs_envelope_config *cfg, float normalized_target,
			 float dt_sec)
{
	if (!ch || !cfg)
		return 0.0f;

	const float target = clamp01(normalized_target);
	if (!is_finite_f32(dt_sec) || dt_sec <= 0.0f) {
		ch->value = target;
		return ch->value;
	}

	const float attack_ms = cfg->attack_ms;
	const float release_ms = cfg->release_ms;
	float tau_ms = release_ms;
	if (target >= ch->value)
		tau_ms = attack_ms;

	if (tau_ms <= 0.0f) {
		ch->value = target;
		return ch->value;
	}

	const double ratio = ((double)dt_sec * 1000.0) / (double)tau_ms;
	const float alpha = (float)(1.0 - exp(-ratio));
	ch->value += alpha * (target - ch->value);
	ch->value = clamp01(ch->value);
	return ch->value;
}

float vs_envelope_step(struct vs_envelope_channel *ch, const struct vs_envelope_config *cfg, float raw_linear,
		       float dt_sec, float time_since_audio_sec)
{
	if (!ch || !cfg)
		return 0.0f;

	if (time_since_audio_sec > VS_ENV_STARVATION_THRESHOLD_SEC) {
		/* Audio starvation (> 50 ms without new samples): decay smoothly to zero. */
		struct vs_envelope_config decay_cfg = *cfg;
		if (decay_cfg.release_ms <= 0.0f || decay_cfg.release_ms > 40.0f)
			decay_cfg.release_ms = 40.0f;
		return vs_envelope_smooth(ch, &decay_cfg, 0.0f, dt_sec);
	}

	const float norm = vs_envelope_normalize(ch, cfg, raw_linear, dt_sec);
	return vs_envelope_smooth(ch, cfg, norm, dt_sec);
}
