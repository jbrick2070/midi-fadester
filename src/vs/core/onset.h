/*
 * Visual Sidechain - Spectral-Flux Onset Detection (Kick/Snare) & Beat Tracker
 * Copyright (C) 2026
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
};

void vs_onset_default_config(struct vs_onset_config *cfg);
bool vs_onset_init(struct vs_onset *onset, const struct vs_onset_config *cfg);
void vs_onset_reset(struct vs_onset *onset);

void vs_onset_step(struct vs_onset *onset, const float *mag_low, const float *mag_mid, float dt_sec,
		   struct vs_onset_result *out_res);

#ifdef __cplusplus
}
#endif
