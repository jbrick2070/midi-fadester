/*
 * Visual Sidechain - Spectral-Flux Onset Detection (Kick/Snare) & Beat Tracker
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "onset.h"

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

void vs_onset_default_config(struct vs_onset_config *cfg)
{
	if (!cfg)
		return;
	cfg->sample_rate = 48000u;
	cfg->fft_size_low = 4096u;
	cfg->fft_size_mid = 2048u;
	cfg->kick_sensitivity = 0.5f;
	cfg->snare_sensitivity = 0.5f;
	cfg->sync_offset_ms = 0.0f;
	cfg->refractory_sec = VS_ONSET_REFRACTORY_SEC;
}

bool vs_onset_init(struct vs_onset *onset, const struct vs_onset_config *cfg)
{
	if (!onset || !cfg || cfg->sample_rate == 0)
		return false;
	if (cfg->fft_size_low == 0 || cfg->fft_size_low / 2u + 1u > VS_MAX_BINS_LOW)
		return false;
	if (cfg->fft_size_mid == 0 || cfg->fft_size_mid / 2u + 1u > VS_MAX_BINS_MID)
		return false;

	memset(onset, 0, sizeof(*onset));
	onset->cfg = *cfg;
	if (onset->cfg.refractory_sec <= 0.0f)
		onset->cfg.refractory_sec = VS_ONSET_REFRACTORY_SEC;
	onset->last_kick_onset_sec = -1.0;
	onset->estimated_bpm = VS_BEAT_DEFAULT_BPM;
	return true;
}

void vs_onset_reset(struct vs_onset *onset)
{
	if (!onset)
		return;
	struct vs_onset_config saved = onset->cfg;
	vs_onset_init(onset, &saved);
}

void vs_onset_freeze_beat(struct vs_onset *onset)
{
	if (!onset)
		return;
	onset->beat_phase = 0.0f;
	onset->beat_confidence = 0.0f;
	onset->last_kick_onset_sec = -1.0;
	onset->bpm_history_count = 0u;
	onset->bpm_history_pos = 0u;
}

static float compute_beat_confidence(const struct vs_onset *onset)
{
	if (!onset || onset->bpm_history_count == 0u || onset->last_kick_onset_sec < 0.0)
		return 0.0f;

	const uint32_t count = onset->bpm_history_count;
	float periods[VS_BEAT_IOI_HISTORY];
	float sum_period = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		const float bpm = onset->bpm_history[i];
		const float p = (bpm >= VS_BEAT_MIN_BPM * 0.5f) ? (60.0f / bpm) : 0.5f;
		periods[i] = p;
		sum_period += p;
	}
	const float mean_period = sum_period / (float)count;
	if (mean_period <= 1e-6f)
		return 0.0f;

	float sum_sq = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		const float d = periods[i] - mean_period;
		sum_sq += d * d;
	}
	const float stddev = sqrtf(sum_sq / (float)count);
	const float cv = stddev / mean_period;
	const float regularity = clamp01(1.0f - (cv / 0.20f));
	const float fill = clamp01((float)count / 4.0f);

	const double beat_period = (onset->estimated_bpm > 1.0f) ? (60.0 / (double)onset->estimated_bpm) : 0.5;
	const double since_kick = onset->elapsed_sec - onset->last_kick_onset_sec;
	float recency = 1.0f;
	if (since_kick > 1.5 * beat_period) {
		const float missed = (float)((since_kick / beat_period) - 1.5);
		recency = clamp01(1.0f - (missed / 6.5f));
	}

	return clamp01(fill * regularity * recency);
}

void vs_onset_step(struct vs_onset *onset, const float *mag_low, const float *mag_mid, float dt_sec,
		   struct vs_onset_result *out_res)
{
	if (!onset || !mag_low || !mag_mid || !out_res)
		return;

	memset(out_res, 0, sizeof(*out_res));
	if (!is_finite_f32(dt_sec) || dt_sec < 0.0f)
		dt_sec = 0.0f;

	const uint32_t sr = onset->cfg.sample_rate;
	const uint32_t n_low = onset->cfg.fft_size_low;
	const uint32_t n_mid = onset->cfg.fft_size_mid;
	const uint32_t max_low = n_low / 2u;
	const uint32_t max_mid = n_mid / 2u;

	/*
	 * 1. Kick spectral flux on the 4096-pt FFT across 55..130 Hz (bins 5..11 at 48 kHz).
	 * Starting at bin 5 (58.6 Hz) keeps a continuous 35 Hz sub-bass drone (bin 3,
	 * Hann main lobe bins 2..4) completely isolated from kick onset detection.
	 */
	uint32_t k_kick_0 = (uint32_t)ceilf((55.0f * (float)n_low) / (float)sr);
	uint32_t k_kick_1 = (uint32_t)floorf((130.0f * (float)n_low) / (float)sr);
	if (k_kick_0 < 1u)
		k_kick_0 = 1u;
	if (k_kick_1 > max_low)
		k_kick_1 = max_low;

	float kick_flux = 0.0f;
	float kick_peak_mag = 0.0f;
	for (uint32_t k = k_kick_0; k <= k_kick_1; k++) {
		const float m = mag_low[k];
		const float diff = m - onset->prev_mag_low[k];
		if (diff > 0.0f)
			kick_flux += diff;
		if (m > kick_peak_mag)
			kick_peak_mag = m;
	}

	/*
	 * 2. Snare spectral flux on the 2048-pt FFT across 250..4000 Hz.
	 */
	uint32_t k_snare_0 = (uint32_t)ceilf((250.0f * (float)n_mid) / (float)sr);
	uint32_t k_snare_1 = (uint32_t)floorf((4000.0f * (float)n_mid) / (float)sr);
	if (k_snare_0 < 1u)
		k_snare_0 = 1u;
	if (k_snare_1 > max_mid)
		k_snare_1 = max_mid;

	float snare_flux = 0.0f;
	float snare_peak_mag = 0.0f;
	for (uint32_t k = k_snare_0; k <= k_snare_1; k++) {
		const float m = mag_mid[k];
		const float diff = m - onset->prev_mag_mid[k];
		if (diff > 0.0f)
			snare_flux += diff;
		if (m > snare_peak_mag)
			snare_peak_mag = m;
	}

	/* Update cooldowns and decay existing envelopes. */
	if (onset->kick_cooldown_sec > 0.0f) {
		onset->kick_cooldown_sec -= dt_sec;
		if (onset->kick_cooldown_sec < 0.0f)
			onset->kick_cooldown_sec = 0.0f;
	}
	if (onset->snare_cooldown_sec > 0.0f) {
		onset->snare_cooldown_sec -= dt_sec;
		if (onset->snare_cooldown_sec < 0.0f)
			onset->snare_cooldown_sec = 0.0f;
	}

	if (dt_sec > 0.0f) {
		const float decay = (float)exp(-(double)dt_sec / (double)VS_ONSET_DECAY_TAU_SEC);
		onset->kick_env *= decay;
		onset->snare_env *= decay;
	}

	/* Adaptive flux thresholds scaled by (1.0 - sensitivity). */
	const float kick_sens = clamp01(onset->cfg.kick_sensitivity);
	const float snare_sens = clamp01(onset->cfg.snare_sensitivity);
	const float kick_thresh = 0.12f + (1.0f - kick_sens) * 0.25f + onset->kick_flux_avg * 1.5f;
	const float snare_thresh = 0.15f + (1.0f - snare_sens) * 0.30f + onset->snare_flux_avg * 1.5f;

	onset->kick_flux_avg = 0.90f * onset->kick_flux_avg + 0.10f * kick_flux;
	onset->snare_flux_avg = 0.90f * onset->snare_flux_avg + 0.10f * snare_flux;

	const float refractory = onset->cfg.refractory_sec;

	if (kick_flux > kick_thresh && kick_peak_mag > 0.08f && onset->kick_cooldown_sec <= 0.0f) {
		onset->kick_env = 1.0f;
		onset->kick_cooldown_sec = refractory;
		out_res->kick_triggered = true;

		/* Update beat tracker from inter-onset interval (IOI). */
		if (onset->last_kick_onset_sec >= 0.0) {
			const double ioi = onset->elapsed_sec - onset->last_kick_onset_sec;
			if (ioi >= 0.28 && ioi <= 1.10) {
				float inst_bpm = (float)(60.0 / ioi);
				while (inst_bpm < VS_BEAT_MIN_BPM)
					inst_bpm *= 2.0f;
				while (inst_bpm > VS_BEAT_MAX_BPM)
					inst_bpm *= 0.5f;

				onset->bpm_history[onset->bpm_history_pos] = inst_bpm;
				onset->bpm_history_pos = (onset->bpm_history_pos + 1u) % VS_BEAT_IOI_HISTORY;
				if (onset->bpm_history_count < VS_BEAT_IOI_HISTORY)
					onset->bpm_history_count++;

				float sum_bpm = 0.0f;
				for (uint32_t i = 0; i < onset->bpm_history_count; i++)
					sum_bpm += onset->bpm_history[i];
				onset->estimated_bpm = sum_bpm / (float)onset->bpm_history_count;
			}
		}
		onset->last_kick_onset_sec = onset->elapsed_sec;
		onset->beat_phase = 0.0f;
	} else if (dt_sec > 0.0f) {
		onset->beat_phase += dt_sec * (onset->estimated_bpm / 60.0f);
		onset->beat_phase -= floorf(onset->beat_phase);
	}

	if (snare_flux > snare_thresh && snare_peak_mag > 0.04f && onset->snare_cooldown_sec <= 0.0f) {
		onset->snare_env = 1.0f;
		onset->snare_cooldown_sec = refractory;
		out_res->snare_triggered = true;
	}

	/* Apply negative sync_offset_ms phase advance to beat output (R-ANA-3, R-ANA-9). */
	float out_beat = onset->beat_phase;
	if (onset->cfg.sync_offset_ms < 0.0f) {
		const float advance_sec = -onset->cfg.sync_offset_ms * 0.001f;
		out_beat += advance_sec * (onset->estimated_bpm / 60.0f);
		out_beat -= floorf(out_beat);
	}

	onset->beat_confidence = compute_beat_confidence(onset);

	out_res->kick = clamp01(onset->kick_env);
	out_res->snare = clamp01(onset->snare_env);
	out_res->beat = clamp01(out_beat);
	out_res->estimated_bpm = onset->estimated_bpm;
	out_res->beat_confidence = onset->beat_confidence;

	onset->elapsed_sec += (double)dt_sec;

	memcpy(onset->prev_mag_low, mag_low, (max_low + 1u) * sizeof(float));
	memcpy(onset->prev_mag_mid, mag_mid, (max_mid + 1u) * sizeof(float));
}

void vs_flash_limiter_reset(struct vs_flash_limiter *lim)
{
	if (lim)
		memset(lim, 0, sizeof(*lim));
}

float vs_flash_limiter_step(struct vs_flash_limiter *lim, float over_01, float max_rate_hz, float dt_sec)
{
	if (!lim)
		return 0.0f;

	double rate = is_finite_f32(max_rate_hz) ? (double)max_rate_hz : VS_FLASH_RATE_DEFAULT_HZ;
	if (rate < VS_FLASH_RATE_MIN_HZ)
		rate = VS_FLASH_RATE_MIN_HZ;
	if (rate > VS_FLASH_RATE_MAX_HZ)
		rate = VS_FLASH_RATE_MAX_HZ;
	const double interval = 1.0 / rate;
	/* At most 0.15 s: at 3 Hz or less every flash is dark again well before the next may start. */
	const double fall = fmin(0.5 * interval, VS_FLASH_FALL_MAX_SEC);

	if (is_finite_f32(dt_sec) && dt_sec > 0.0f)
		lim->since_admit_sec += (double)dt_sec;

	float over = isnan(over_01) ? 0.0f : over_01;
	if (over < 0.0f)
		over = 0.0f;
	if (over > 1.0f)
		over = 1.0f;

	/* The 1e-9 s slack absorbs rounding when the interval is a whole number of frames. */
	if (over > 0.0f && (!lim->has_admitted || lim->since_admit_sec + 1e-9 >= interval)) {
		lim->peak = over;
		lim->since_admit_sec = 0.0;
		lim->has_admitted = true;
	}

	/* Within a flash the envelope only falls: each admitted flash is exactly one rise and one fall. */
	if (!lim->has_admitted || lim->since_admit_sec >= fall)
		return 0.0f;
	return (float)((double)lim->peak * (1.0 - lim->since_admit_sec / fall));
}
