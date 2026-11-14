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

#pragma once

#include "spectrum.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_ONSET_REFRACTORY_SEC 0.100f
#define VS_ONSET_DECAY_TAU_SEC 0.065f
#define VS_BEAT_MIN_BPM 60.0f
#define VS_BEAT_MAX_BPM 180.0f
#define VS_BEAT_DEFAULT_BPM 120.0f
#define VS_BEAT_IOI_HISTORY 8u

struct vs_onset_config {
	uint32_t sample_rate;
	uint32_t fft_size_low;
	uint32_t fft_size_mid;
	float kick_sensitivity;
	float snare_sensitivity;
	float sync_offset_ms;
	float refractory_sec;
};

struct vs_onset_result {
	float kick;
	float snare;
	float beat;
	float estimated_bpm;
	float beat_confidence;
	bool kick_triggered;
	bool snare_triggered;
};

struct vs_onset {
	struct vs_onset_config cfg;
	float prev_mag_low[VS_MAX_BINS_LOW];
	float prev_mag_mid[VS_MAX_BINS_MID];
	float kick_env;
	float snare_env;
	float kick_cooldown_sec;
	float snare_cooldown_sec;
	float kick_flux_avg;
	float snare_flux_avg;
	double elapsed_sec;
	double last_kick_onset_sec;
	float bpm_history[VS_BEAT_IOI_HISTORY];
	uint32_t bpm_history_count;
	uint32_t bpm_history_pos;
	float estimated_bpm;
	float beat_phase;
	float beat_confidence;
};

void vs_onset_default_config(struct vs_onset_config *cfg);
bool vs_onset_init(struct vs_onset *onset, const struct vs_onset_config *cfg);
void vs_onset_reset(struct vs_onset *onset);
/* Freezes beat_phase and beat_confidence at 0 during extended silence while keeping estimated_bpm ready for resume. */
void vs_onset_freeze_beat(struct vs_onset *onset);

void vs_onset_step(struct vs_onset *onset, const float *mag_low, const float *mag_mid, float dt_sec,
		   struct vs_onset_result *out_res);

/* The Maximum Flash Rate range of fft-strobe-flash.shader; the limiter enforces it whatever the setting says. */
#define VS_FLASH_RATE_MIN_HZ 0.5
#define VS_FLASH_RATE_MAX_HZ 3.0
#define VS_FLASH_RATE_DEFAULT_HZ 3.0
#define VS_FLASH_FALL_MAX_SEC 0.15

/* Photosensitivity flash limiter (R-SHD-3 (8)). A flash starts only when the trigger is over its threshold and
 * at least 1 / max_rate_hz seconds have passed since the last admitted flash; each flash falls back to exactly 0
 * well before the next may start. */
struct vs_flash_limiter {
	double since_admit_sec;
	float peak;
	bool has_admitted;
};
void vs_flash_limiter_reset(struct vs_flash_limiter *lim);
/* over_01: how far the trigger is past its threshold, 0..1 (0 = below). max_rate_hz is clamped to 0.5..3 (the
 * shader slider's range); NaN or infinity means 3. Call once per video frame. Returns the envelope, 0..1. */
float vs_flash_limiter_step(struct vs_flash_limiter *lim, float over_01, float max_rate_hz, float dt_sec);

#ifdef __cplusplus
}
#endif
