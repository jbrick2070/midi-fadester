/*
 * Visual Sidechain - OBS 33 Audio Tap Manager & Shared GPU FFT Textures
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "core/analyzer.h"

#include <graphics/graphics.h>
#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vs_tap_mode {
	VS_TAP_TRACK_1 = 0, /* Default: Track 1 (Main Mix), mix_idx = 0 */
	VS_TAP_TRACK_2 = 1,
	VS_TAP_TRACK_3 = 2,
	VS_TAP_TRACK_4 = 3,
	VS_TAP_TRACK_5 = 4,
	VS_TAP_TRACK_6 = 5,
	VS_TAP_PARENT_SOURCE = 6,
	VS_TAP_NAMED_SOURCE = 7,
};

struct vs_audio_tap {
	enum vs_tap_mode mode;
	char source_name[256];
	bool react_when_muted;
	bool upload_gpu_textures;

	bool raw_cb_active;
	size_t active_mix_idx;
	obs_weak_source_t *weak_capture_source;
	bool source_cb_active;
	float retry_accum_sec;

	uint32_t sample_rate;
	size_t channels;

	struct vs_analyzer analyzer;
	struct vs_audio_frame current_frame;
	uint64_t last_video_frame_ts;

	gs_texture_t *tex_spectrum; /* 32x1 GS_R32F dynamic texture */
	gs_texture_t *tex_fft;      /* 512x2 GS_R32F dynamic texture (Row 0: FFT, Row 1: Waveform) */
	float fft_tex_staging[VS_FFT_TEX_BINS * 2u];
};

bool vs_audio_tap_init(struct vs_audio_tap *tap, bool upload_gpu_textures);
void vs_audio_tap_destroy(struct vs_audio_tap *tap);

void vs_audio_tap_configure(struct vs_audio_tap *tap, obs_source_t *filter_source, enum vs_tap_mode mode,
			    const char *source_name, bool react_when_muted, const struct vs_analyzer_config *ana_cfg);

void vs_audio_tap_tick(struct vs_audio_tap *tap, obs_source_t *filter_source, float seconds);

/* Shared OBS settings & properties helpers used by both Shader Filter and Audio Modulator. */
void vs_audio_tap_get_defaults(obs_data_t *settings);
void vs_audio_tap_update_from_settings(struct vs_audio_tap *tap, obs_source_t *filter_source, obs_data_t *settings);
void vs_audio_tap_add_properties(obs_properties_t *props);

#ifdef __cplusplus
}
#endif
