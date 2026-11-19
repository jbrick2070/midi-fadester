/*
 * Visual Sidechain - OBS 33 Audio Tap Manager & Shared GPU FFT Textures
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
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
	volatile bool react_when_muted; /* Audio callback reads via os_atomic_load_bool(). */
	bool upload_gpu_textures;

	bool raw_cb_active;
	size_t active_mix_idx;
	obs_weak_source_t *weak_capture_source;
	bool source_cb_active;
	bool warned_attachment_failure;
	float retry_accum_sec;
	/* Set by a caller holding a lock that the frontend's save takes after the sources lock: configure then records a
	 * Named Source target without looking it up, and the 1 s retry attaches it (see vs_audio_tap_named_lookup). */
	bool defer_named_attach;

	uint32_t sample_rate;
	size_t channels;

	struct vs_analyzer analyzer;
	struct vs_audio_frame current_frame;
	uint64_t last_video_frame_ts;

	/* Lock-free live audio status snapshot read by Properties UI without taking filter/graphics locks. */
	volatile long status_mode;
	volatile bool status_has_stepped;
	volatile bool status_audio_present;
	volatile long status_silent_seconds;
	obs_source_t *ui_filter_source;

	gs_texture_t *tex_spectrum; /* 32x1 GS_R32F dynamic texture */
	gs_texture_t *tex_fft;      /* 512x2 GS_R32F dynamic texture (Row 0: FFT, Row 1: Waveform) */
	float fft_tex_staging[VS_FFT_TEX_BINS * 2u];
};

bool vs_audio_tap_init(struct vs_audio_tap *tap, bool upload_gpu_textures);
void vs_audio_tap_destroy(struct vs_audio_tap *tap);

void vs_audio_tap_configure(struct vs_audio_tap *tap, obs_source_t *filter_source, enum vs_tap_mode mode,
			    const char *source_name, bool react_when_muted, const struct vs_analyzer_config *ana_cfg);
/* For a filter being removed from its parent: a Parent Source tap lets go of that parent (the 1 s retry starts over,
 * and finds no parent until the filter is added again). Other modes are left as they are. */
void vs_audio_tap_detach_parent(struct vs_audio_tap *tap);

void vs_audio_tap_tick(struct vs_audio_tap *tap, obs_source_t *filter_source, float seconds);

/* The Named Source that this tick's once-a-second retry will look up, found by the caller outside a lock that the
 * frontend's save takes after the sources lock (the Audio Modulator's mutex): obs_get_source_by_name takes the sources
 * lock, so it must not run under such a lock. Plan with the tap's lock held, resolve without it, then tick with it. */
struct vs_audio_tap_named_lookup {
	bool planned;
	char name[256];
	obs_source_t *source; /* a reference, or NULL when no source has that name */
};
void vs_audio_tap_plan_named_lookup(const struct vs_audio_tap *tap, float seconds,
				    struct vs_audio_tap_named_lookup *lookup);
void vs_audio_tap_resolve_named_lookup(struct vs_audio_tap_named_lookup *lookup);
void vs_audio_tap_release_named_lookup(struct vs_audio_tap_named_lookup *lookup);
/* vs_audio_tap_tick, except that a Named Source retry takes the source from the lookup instead of looking it up. A
 * retry the lookup does not cover (the tap changed in between) waits for the next tick with its timer kept. */
void vs_audio_tap_tick_with_lookup(struct vs_audio_tap *tap, obs_source_t *filter_source, float seconds,
				   const struct vs_audio_tap_named_lookup *lookup);

/* Shared OBS settings & properties helpers used by both Shader Filter and Audio Modulator. */
void vs_audio_tap_get_defaults(obs_data_t *settings);
void vs_audio_tap_update_from_settings(struct vs_audio_tap *tap, obs_source_t *filter_source, obs_data_t *settings);
/* Adds the tap status warning row at the current position (top-level) so it stays visible when the
 * analysis group sits at the bottom of a panel. vs_audio_tap_add_properties then skips adding it again. */
void vs_audio_tap_add_status_property(obs_properties_t *props);
void vs_audio_tap_add_status_property_for_source(obs_properties_t *props, obs_source_t *filter_source);
void vs_audio_tap_add_properties(obs_properties_t *props, obs_source_t *filter_source);
void vs_audio_tap_add_properties_with_tap(obs_properties_t *props, obs_source_t *filter_source,
					  struct vs_audio_tap *tap);

#ifdef __cplusplus
}
#endif
