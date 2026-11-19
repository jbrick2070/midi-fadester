/*
 * Visual Sidechain - Operator Tempo Control, Tap Tempo, Lock & 4-Beat Bar Clock
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
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

#define VS_TEMPO_MIN_BPM 30.0f
#define VS_TEMPO_MAX_BPM 300.0f
#define VS_TEMPO_DEFAULT_BPM 120.0f
#define VS_TEMPO_BEATS_PER_BAR 4u
#define VS_TEMPO_TAP_HISTORY 8u
#define VS_TEMPO_TAP_RESET_SEC 2.5
#define VS_TEMPO_TAP_MIN_SEC 0.20
#define VS_TEMPO_LOCK_PHASE_WINDOW_BEATS 0.15f
#define VS_TEMPO_LOCK_PHASE_GAIN 0.50f

struct vs_tempo_snapshot {
	float bpm;
	bool bpm_locked;
	float beat_phase;    /* [0.0, 1.0) */
	uint32_t beat_index; /* 0..3 */
	float bar_phase;     /* [0.0, 1.0) */
	uint64_t bar_index;  /* >= 0 */
	uint32_t tap_count;  /* 0..8 */
};

struct vs_tempo {
	float bpm;
	bool bpm_locked;
	double bar_beats; /* [0.0, 4.0) in double precision for zero multi-hour drift */
	uint64_t bar_index;
	uint64_t tap_chain_start_bar;
	uint32_t total_chain_taps;
	double tap_times_sec[VS_TEMPO_TAP_HISTORY];
	uint32_t tap_count;
	bool has_kick_aligned;
	uint64_t synced_global_seq;
};

void vs_tempo_init(struct vs_tempo *t, float initial_bpm);
void vs_tempo_snapshot(const struct vs_tempo *t, struct vs_tempo_snapshot *out);
void vs_tempo_snapshot_with_sync_offset(const struct vs_tempo *t, float sync_offset_ms, struct vs_tempo_snapshot *out);

void vs_tempo_advance(struct vs_tempo *t, float dt_sec);
void vs_tempo_on_kick(struct vs_tempo *t, float estimated_bpm_from_ioi, bool has_valid_ioi);
void vs_tempo_freeze(struct vs_tempo *t);

bool vs_tempo_tap(struct vs_tempo *t, double now_sec);
bool vs_tempo_set_bpm(struct vs_tempo *t, float bpm);
void vs_tempo_set_locked(struct vs_tempo *t, bool locked);
void vs_tempo_resync_downbeat(struct vs_tempo *t);

/*
 * Process-wide operator tempo commands shared across vendor requests, OBS
 * frontend hotkeys, and active filter analyzer instances.
 */
void vs_tempo_global_reset(void);
void vs_tempo_global_get_snapshot(struct vs_tempo_snapshot *out_snap);
void vs_tempo_global_observe_live(float bpm, uint32_t beat_index, float bar_phase, uint64_t bar_index);

bool vs_tempo_global_tap(double now_sec, struct vs_tempo_snapshot *out_snap);
bool vs_tempo_global_set_bpm(float bpm, struct vs_tempo_snapshot *out_snap);
void vs_tempo_global_set_locked(bool locked, struct vs_tempo_snapshot *out_snap);
void vs_tempo_global_toggle_locked(struct vs_tempo_snapshot *out_snap);
void vs_tempo_global_resync_downbeat(struct vs_tempo_snapshot *out_snap);

bool vs_tempo_global_sync_instance(struct vs_tempo *t, bool *out_clear_ioi_history);

#ifdef __cplusplus
}
#endif
