/*
 * Visual Sidechain - End-to-End Dual-FFT Audio Analyzer Pipeline
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "analyzer.h"
#include "fader-bridge.h"

#include <math.h>
#include <string.h>

static void remove_dc_offset(float *buf, size_t n, uint64_t total_written)
{
	if (!buf || n == 0 || total_written == 0)
		return;
	const size_t valid = (total_written < (uint64_t)n) ? (size_t)total_written : n;
	float *active = buf + (n - valid);
	double sum = 0.0;
	for (size_t i = 0; i < valid; i++)
		sum += (double)active[i];
	const float mean = (float)(sum / (double)valid);
	if (fabsf(mean) > 1e-7f) {
		for (size_t i = 0; i < valid; i++)
			active[i] -= mean;
	}
}

void vs_analyzer_default_config(struct vs_analyzer_config *cfg)
{
	if (!cfg)
		return;
	cfg->sample_rate = 48000u;
	cfg->fft_size_low = VS_FFT_LOW_SIZE;
	cfg->fft_size_mid = VS_FFT_MID_SIZE;
	cfg->sync_offset_ms = 0.0f;
	cfg->auto_gain = true;
	cfg->gate_db = VS_ENV_DEFAULT_GATE_DB;
	cfg->floor_db = VS_ENV_DEFAULT_FLOOR_DB;
	cfg->ceiling_db = VS_ENV_DEFAULT_CEILING_DB;
	cfg->attack_ms = VS_ENV_DEFAULT_ATTACK_MS;
	cfg->release_ms = VS_ENV_DEFAULT_RELEASE_MS;
	cfg->kick_sensitivity = 0.5f;
	cfg->snare_sensitivity = 0.5f;
}

static void sync_subconfigs(struct vs_analyzer *ana)
{
	ana->env_cfg.auto_gain = ana->cfg.auto_gain;
	ana->env_cfg.gate_db = ana->cfg.gate_db;
	ana->env_cfg.floor_db = ana->cfg.floor_db;
	ana->env_cfg.ceiling_db = ana->cfg.ceiling_db;
	ana->env_cfg.attack_ms = ana->cfg.attack_ms;
	ana->env_cfg.release_ms = ana->cfg.release_ms;

	ana->onset.cfg.kick_sensitivity = ana->cfg.kick_sensitivity;
	ana->onset.cfg.snare_sensitivity = ana->cfg.snare_sensitivity;
	ana->onset.cfg.sync_offset_ms = ana->cfg.sync_offset_ms;
}

bool vs_analyzer_init(struct vs_analyzer *ana, const struct vs_analyzer_config *cfg)
{
	if (!ana || !cfg)
		return false;

	memset(ana, 0, sizeof(*ana));
	ana->cfg = *cfg;
	if (ana->cfg.sample_rate == 0)
		ana->cfg.sample_rate = 48000u;
	if (ana->cfg.fft_size_low == 0)
		ana->cfg.fft_size_low = VS_FFT_LOW_SIZE;
	if (ana->cfg.fft_size_mid == 0)
		ana->cfg.fft_size_mid = VS_FFT_MID_SIZE;

	if (!vs_audio_ring_init(&ana->ring))
		return false;
	if (!vs_fft_plan_init(&ana->fft_low, ana->cfg.fft_size_low)) {
		vs_audio_ring_free(&ana->ring);
		return false;
	}
	if (!vs_fft_plan_init(&ana->fft_mid, ana->cfg.fft_size_mid)) {
		vs_fft_plan_free(&ana->fft_low);
		vs_audio_ring_free(&ana->ring);
		return false;
	}
	if (!vs_spectrum_init(&ana->spectrum, ana->cfg.sample_rate, ana->cfg.fft_size_low, ana->cfg.fft_size_mid)) {
		vs_fft_plan_free(&ana->fft_mid);
		vs_fft_plan_free(&ana->fft_low);
		vs_audio_ring_free(&ana->ring);
		return false;
	}

	struct vs_onset_config onset_cfg;
	vs_onset_default_config(&onset_cfg);
	onset_cfg.sample_rate = ana->cfg.sample_rate;
	onset_cfg.fft_size_low = ana->cfg.fft_size_low;
	onset_cfg.fft_size_mid = ana->cfg.fft_size_mid;
	onset_cfg.kick_sensitivity = ana->cfg.kick_sensitivity;
	onset_cfg.snare_sensitivity = ana->cfg.snare_sensitivity;
	onset_cfg.sync_offset_ms = ana->cfg.sync_offset_ms;
	if (!vs_onset_init(&ana->onset, &onset_cfg)) {
		vs_fft_plan_free(&ana->fft_mid);
		vs_fft_plan_free(&ana->fft_low);
		vs_audio_ring_free(&ana->ring);
		return false;
	}

	sync_subconfigs(ana);
	ana->initialized = true;
	return true;
}

void vs_analyzer_free(struct vs_analyzer *ana)
{
	if (!ana || !ana->initialized)
		return;
	vs_fft_plan_free(&ana->fft_mid);
	vs_fft_plan_free(&ana->fft_low);
	vs_audio_ring_free(&ana->ring);
	ana->initialized = false;
}

void vs_analyzer_reset(struct vs_analyzer *ana)
{
	if (!ana || !ana->initialized)
		return;
	vs_audio_ring_reset(&ana->ring);
	vs_spectrum_reset(&ana->spectrum);
	vs_onset_reset(&ana->onset);
	vs_envelope_channel_reset(&ana->env_level);
	vs_envelope_channel_reset(&ana->env_sub_bass);
	vs_envelope_channel_reset(&ana->env_bass);
	vs_envelope_channel_reset(&ana->env_mid);
	vs_envelope_channel_reset(&ana->env_high);
	for (uint32_t i = 0; i < VS_SPECTRUM_BANDS; i++)
		vs_envelope_channel_reset(&ana->env_spectrum[i]);
	ana->last_written_samples = 0;
	ana->time_since_audio_sec = 0.0f;
}

void vs_analyzer_update_config(struct vs_analyzer *ana, const struct vs_analyzer_config *cfg)
{
	if (!ana || !cfg)
		return;
	ana->cfg.sync_offset_ms = cfg->sync_offset_ms;
	ana->cfg.auto_gain = cfg->auto_gain;
	ana->cfg.gate_db = cfg->gate_db;
	ana->cfg.floor_db = cfg->floor_db;
	ana->cfg.ceiling_db = cfg->ceiling_db;
	ana->cfg.attack_ms = cfg->attack_ms;
	ana->cfg.release_ms = cfg->release_ms;
	ana->cfg.kick_sensitivity = cfg->kick_sensitivity;
	ana->cfg.snare_sensitivity = cfg->snare_sensitivity;
	sync_subconfigs(ana);
}

void vs_analyzer_push_mono(struct vs_analyzer *ana, const float *samples, size_t frames, uint64_t timestamp_ns)
{
	if (!ana || !ana->initialized)
		return;
	vs_audio_ring_push_mono(&ana->ring, samples, frames, timestamp_ns);
}

void vs_analyzer_push_planar(struct vs_analyzer *ana, const float *const *planes, size_t channels, size_t frames,
			     bool muted, bool react_when_muted, uint64_t timestamp_ns)
{
	if (!ana || !ana->initialized)
		return;
	vs_audio_ring_push_planar(&ana->ring, planes, channels, frames, muted, react_when_muted, timestamp_ns);
}

void vs_analyzer_step(struct vs_analyzer *ana, float dt_sec, struct vs_audio_frame *out_frame)
{
	if (!ana || !ana->initialized || !out_frame)
		return;
	if (!isfinite(dt_sec) || dt_sec < 0.0f)
		dt_sec = 0.0f;

	memset(out_frame, 0, sizeof(*out_frame));

	size_t offset_samples = 0;
	const size_t low_size = ana->cfg.fft_size_low;
	const size_t mid_size = ana->cfg.fft_size_mid;
	const size_t window_size = low_size > mid_size ? low_size : mid_size;
	if (isfinite(ana->cfg.sync_offset_ms) && ana->cfg.sync_offset_ms > 0.0f) {
		const double delay_samples = (double)ana->cfg.sync_offset_ms * 0.001 * (double)ana->cfg.sample_rate;
		const size_t max_offset = VS_AUDIO_RING_CAPACITY - window_size;
		offset_samples = delay_samples >= (double)max_offset ? max_offset : (size_t)(delay_samples + 0.5);
	}

	uint64_t written = 0;
	uint64_t ts = 0;
	/* One locked snapshot gives both FFTs the same audio endpoint and keeps the
	 * capture callback from waiting for two separate ring copies per video tick. */
	if (low_size >= mid_size) {
		vs_audio_ring_read_window(&ana->ring, low_size, offset_samples, ana->win_low, &written, &ts);
		memcpy(ana->win_mid, ana->win_low + low_size - mid_size, mid_size * sizeof(float));
	} else {
		vs_audio_ring_read_window(&ana->ring, mid_size, offset_samples, ana->win_mid, &written, &ts);
		memcpy(ana->win_low, ana->win_mid + mid_size - low_size, low_size * sizeof(float));
	}

	if (written > ana->last_written_samples) {
		ana->last_written_samples = written;
		ana->time_since_audio_sec = 0.0f;
	} else {
		ana->time_since_audio_sec += dt_sec;
	}

	const uint64_t available = written > offset_samples ? written - offset_samples : 0u;
	remove_dc_offset(ana->win_low, low_size, available);
	remove_dc_offset(ana->win_mid, mid_size, available);

	vs_fft_compute(&ana->fft_low, ana->win_low, ana->mag_low);
	vs_fft_compute(&ana->fft_mid, ana->win_mid, ana->mag_mid);

	struct vs_spectrum_frame spec_frame;
	vs_spectrum_compute(&ana->spectrum, ana->win_mid, ana->mag_low, ana->mag_mid, &spec_frame);

	struct vs_onset_result onset_res;
	vs_onset_step(&ana->onset, ana->mag_low, ana->mag_mid, dt_sec, &onset_res);

	const float starved = ana->time_since_audio_sec;
	out_frame->level = vs_envelope_step(&ana->env_level, &ana->env_cfg, spec_frame.level, dt_sec, starved);
	out_frame->sub_bass = vs_envelope_step(&ana->env_sub_bass, &ana->env_cfg, spec_frame.sub_bass, dt_sec, starved);
	out_frame->bass = vs_envelope_step(&ana->env_bass, &ana->env_cfg, spec_frame.bass, dt_sec, starved);
	out_frame->mid = vs_envelope_step(&ana->env_mid, &ana->env_cfg, spec_frame.mid, dt_sec, starved);
	out_frame->high = vs_envelope_step(&ana->env_high, &ana->env_cfg, spec_frame.high, dt_sec, starved);

	for (uint32_t b = 0; b < VS_SPECTRUM_BANDS; b++) {
		out_frame->spectrum[b] =
			vs_envelope_step(&ana->env_spectrum[b], &ana->env_cfg, spec_frame.spectrum[b], dt_sec, starved);
	}

	const float gate_lin = powf(10.0f, ana->cfg.gate_db / 20.0f);
	if (starved > VS_ENV_STARVATION_THRESHOLD_SEC || spec_frame.level <= gate_lin) {
		memset(out_frame->fft_bins, 0, sizeof(out_frame->fft_bins));
		for (uint32_t i = 0; i < VS_FFT_TEX_BINS; i++)
			out_frame->waveform[i] = 0.5f;
		out_frame->spectral_centroid = 0.0f;
		out_frame->spectral_flux = 0.0f;
		out_frame->kick = (starved > VS_ENV_STARVATION_THRESHOLD_SEC) ? 0.0f : onset_res.kick;
		out_frame->snare = (starved > VS_ENV_STARVATION_THRESHOLD_SEC) ? 0.0f : onset_res.snare;
	} else {
		memcpy(out_frame->fft_bins, spec_frame.fft_bins, sizeof(out_frame->fft_bins));
		memcpy(out_frame->waveform, spec_frame.waveform, sizeof(out_frame->waveform));
		out_frame->spectral_centroid = spec_frame.spectral_centroid;
		out_frame->spectral_flux = spec_frame.spectral_flux;
		out_frame->kick = onset_res.kick;
		out_frame->snare = onset_res.snare;
	}

	out_frame->beat = onset_res.beat;
	out_frame->peak_raw = spec_frame.peak_raw;
	out_frame->magnitude_raw = spec_frame.magnitude_raw;
	out_frame->estimated_bpm = onset_res.estimated_bpm;
}

float vs_analyzer_get_signal(const struct vs_analyzer *ana, const struct vs_audio_frame *frame, enum vs_signal_id sig,
			     float custom_low_hz, float custom_high_hz, enum vs_hz_band_mode custom_mode)
{
	if ((int)sig >= (int)VS_SIG_FADER_CROSSFADER && (int)sig < (int)VS_SIG_FADER_CROSSFADER + VS_FADER_SLOT_COUNT) {
		return vs_fader_bridge_get_slot((int)sig - (int)VS_SIG_FADER_CROSSFADER);
	}

	if (!frame)
		return 0.0f;

	switch (sig) {
	case VS_SIG_LEVEL:
		return frame->level;
	case VS_SIG_SUB_BASS:
		return frame->sub_bass;
	case VS_SIG_BASS:
		return frame->bass;
	case VS_SIG_MID:
		return frame->mid;
	case VS_SIG_HIGH:
		return frame->high;
	case VS_SIG_KICK:
		return frame->kick;
	case VS_SIG_SNARE:
		return frame->snare;
	case VS_SIG_BEAT:
		return frame->beat;
	case VS_SIG_CENTROID:
		return frame->spectral_centroid;
	case VS_SIG_FLUX:
		return frame->spectral_flux;
	case VS_SIG_CUSTOM_HZ:
		if (!ana || !ana->initialized)
			return 0.0f;
		return vs_spectrum_extract_hz_band(&ana->spectrum, ana->mag_low, ana->mag_mid, custom_low_hz,
						   custom_high_hz, custom_mode);
	default:
		if ((int)sig >= (int)VS_SIG_SPECTRUM_BAND_0 &&
		    (int)sig < (int)(VS_SIG_SPECTRUM_BAND_0 + VS_SPECTRUM_BANDS)) {
			return frame->spectrum[(int)sig - (int)VS_SIG_SPECTRUM_BAND_0];
		}
		return 0.0f;
	}
}
