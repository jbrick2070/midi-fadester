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

#include "tempo.h"

#include <util/threading.h>

#include <math.h>
#include <string.h>

static inline bool is_finite_f32(float v)
{
	uint32_t bits = 0;
	memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;
}

static inline bool is_finite_f64(double v)
{
	uint64_t bits = 0;
	memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
}

static float clamp01(float v)
{
	if (!is_finite_f32(v) || v <= 0.0f)
		return 0.0f;
	if (v >= 1.0f)
		return 1.0f;
	return v;
}

static float clamp_bpm(float bpm)
{
	if (!is_finite_f32(bpm))
		return VS_TEMPO_DEFAULT_BPM;
	if (bpm < VS_TEMPO_MIN_BPM)
		return VS_TEMPO_MIN_BPM;
	if (bpm > VS_TEMPO_MAX_BPM)
		return VS_TEMPO_MAX_BPM;
	return bpm;
}

static void normalize_bar_beats(struct vs_tempo *t)
{
	if (!is_finite_f64(t->bar_beats) || t->bar_beats < 0.0) {
		t->bar_beats = 0.0;
		return;
	}
	if (t->bar_beats >= 4.0) {
		const double wraps = floor(t->bar_beats / 4.0);
		if (wraps > 0.0) {
			t->bar_index += (uint64_t)wraps;
			t->bar_beats -= wraps * 4.0;
			if (t->bar_beats < 0.0 || t->bar_beats >= 4.0)
				t->bar_beats = 0.0;
		}
	}
}

void vs_tempo_init(struct vs_tempo *t, float initial_bpm)
{
	if (!t)
		return;
	memset(t, 0, sizeof(*t));
	t->bpm = clamp_bpm(initial_bpm);
}

void vs_tempo_snapshot(const struct vs_tempo *t, struct vs_tempo_snapshot *out)
{
	vs_tempo_snapshot_with_sync_offset(t, 0.0f, out);
}

void vs_tempo_snapshot_with_sync_offset(const struct vs_tempo *t, float sync_offset_ms, struct vs_tempo_snapshot *out)
{
	if (!out)
		return;
	memset(out, 0, sizeof(*out));
	if (!t) {
		out->bpm = VS_TEMPO_DEFAULT_BPM;
		return;
	}

	const float bpm = clamp_bpm(t->bpm);
	double shifted_beats = is_finite_f64(t->bar_beats) && t->bar_beats >= 0.0 ? t->bar_beats : 0.0;
	if (is_finite_f32(sync_offset_ms) && sync_offset_ms < 0.0f) {
		const double advance_sec = (double)(-sync_offset_ms) * 0.001;
		shifted_beats += advance_sec * ((double)bpm / 60.0);
	}

	uint64_t extra_bars = 0u;
	if (shifted_beats >= 4.0) {
		const double wraps = floor(shifted_beats / 4.0);
		extra_bars = (uint64_t)wraps;
		shifted_beats -= wraps * 4.0;
	}
	if (shifted_beats < 0.0 || shifted_beats >= 4.0)
		shifted_beats = 0.0;

	const float b = (float)shifted_beats;
	const uint32_t beat_idx = ((uint32_t)floorf(b)) % VS_TEMPO_BEATS_PER_BAR;
	const float beat_frac = b - floorf(b);

	out->bpm = bpm;
	out->bpm_locked = t->bpm_locked;
	out->beat_phase = clamp01(beat_frac);
	out->beat_index = beat_idx;
	out->bar_phase = clamp01(b * 0.25f);
	out->bar_index = t->bar_index + extra_bars;
	out->tap_count = t->tap_count;
}

void vs_tempo_advance(struct vs_tempo *t, float dt_sec)
{
	if (!t || !is_finite_f32(dt_sec) || dt_sec <= 0.0f)
		return;
	t->bpm = clamp_bpm(t->bpm);
	t->bar_beats += (double)dt_sec * ((double)t->bpm / 60.0);
	normalize_bar_beats(t);
}

void vs_tempo_on_kick(struct vs_tempo *t, float estimated_bpm_from_ioi, bool has_valid_ioi)
{
	if (!t)
		return;

	if (t->bpm_locked) {
		/* Locked mode: kicks never change BPM and only gently nudge phase near a beat boundary. */
		const double frac = t->bar_beats - floor(t->bar_beats);
		const double err = (frac <= 0.5) ? -frac : (1.0 - frac);
		if (fabs(err) <= (double)VS_TEMPO_LOCK_PHASE_WINDOW_BEATS) {
			t->bar_beats += (double)VS_TEMPO_LOCK_PHASE_GAIN * err;
			normalize_bar_beats(t);
		}
		return;
	}

	if (has_valid_ioi && is_finite_f32(estimated_bpm_from_ioi))
		t->bpm = clamp_bpm(estimated_bpm_from_ioi);

	if (!t->has_kick_aligned) {
		t->bar_beats = 0.0;
		t->has_kick_aligned = true;
		return;
	}

	double snapped = floor(t->bar_beats + 0.5);
	if (snapped < 0.0)
		snapped = 0.0;
	t->bar_beats = snapped;
	normalize_bar_beats(t);
}

void vs_tempo_freeze(struct vs_tempo *t)
{
	if (!t || t->bpm_locked)
		return;
	t->bar_beats = 0.0;
	t->has_kick_aligned = false;
	t->tap_count = 0u;
	t->total_chain_taps = 0u;
}

bool vs_tempo_tap(struct vs_tempo *t, double now_sec)
{
	if (!t || !is_finite_f64(now_sec))
		return false;

	if (t->tap_count > 0u) {
		const double dt = now_sec - t->tap_times_sec[t->tap_count - 1u];
		if (dt >= 0.0 && dt < VS_TEMPO_TAP_MIN_SEC)
			return false;
		if (dt < 0.0 || dt > VS_TEMPO_TAP_RESET_SEC) {
			t->tap_count = 0u;
			t->total_chain_taps = 0u;
		}
	}

	if (t->tap_count == 0u) {
		if (t->bar_beats > 1e-4)
			t->bar_index++;
		t->tap_chain_start_bar = t->bar_index;
		t->total_chain_taps = 1u;
		t->tap_times_sec[0] = now_sec;
		t->tap_count = 1u;
		t->bar_beats = 0.0;
		t->has_kick_aligned = true;
		return true;
	}

	if (t->tap_count < VS_TEMPO_TAP_HISTORY) {
		t->tap_times_sec[t->tap_count++] = now_sec;
	} else {
		memmove(&t->tap_times_sec[0], &t->tap_times_sec[1], (VS_TEMPO_TAP_HISTORY - 1u) * sizeof(double));
		t->tap_times_sec[VS_TEMPO_TAP_HISTORY - 1u] = now_sec;
	}
	t->total_chain_taps++;

	const double span = t->tap_times_sec[t->tap_count - 1u] - t->tap_times_sec[0];
	const uint32_t intervals = t->tap_count - 1u;
	if (intervals > 0u && span > 0.0) {
		const double mean_ioi = span / (double)intervals;
		t->bpm = clamp_bpm((float)(60.0 / mean_ioi));
	}

	const uint32_t zero_based = t->total_chain_taps - 1u;
	t->bar_index = t->tap_chain_start_bar + (uint64_t)(zero_based / VS_TEMPO_BEATS_PER_BAR);
	t->bar_beats = (double)(zero_based % VS_TEMPO_BEATS_PER_BAR);
	t->has_kick_aligned = true;
	return true;
}

bool vs_tempo_set_bpm(struct vs_tempo *t, float bpm)
{
	if (!t || !is_finite_f32(bpm) || bpm < VS_TEMPO_MIN_BPM || bpm > VS_TEMPO_MAX_BPM)
		return false;
	t->bpm = bpm;
	t->tap_count = 0u;
	t->total_chain_taps = 0u;
	return true;
}

void vs_tempo_set_locked(struct vs_tempo *t, bool locked)
{
	if (!t)
		return;
	t->bpm_locked = locked;
}

void vs_tempo_resync_downbeat(struct vs_tempo *t)
{
	if (!t)
		return;
	if (t->bar_beats > 1e-4)
		t->bar_index++;
	t->bar_beats = 0.0;
	t->tap_count = 0u;
	t->total_chain_taps = 0u;
	t->has_kick_aligned = true;
}

/* ---------------- Process-wide operator command queue ---------------- */

enum vs_tempo_cmd_kind {
	VS_TEMPO_CMD_TAP = 1,
	VS_TEMPO_CMD_SET_BPM = 2,
	VS_TEMPO_CMD_LOCK = 3,
	VS_TEMPO_CMD_RESYNC = 4,
};

struct vs_tempo_cmd {
	uint64_t seq;
	enum vs_tempo_cmd_kind kind;
	float bpm;
	bool locked;
	uint32_t beat_index;
	uint64_t bar_index;
};

#define VS_TEMPO_CMD_RING_CAP 32u

static pthread_mutex_t g_tempo_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_global_inited = false;
static struct vs_tempo g_global_tempo;
static uint64_t g_cmd_seq = 0u;
static struct vs_tempo_cmd g_cmds[VS_TEMPO_CMD_RING_CAP];

static void ensure_global_inited_locked(void)
{
	if (!g_global_inited) {
		vs_tempo_init(&g_global_tempo, VS_TEMPO_DEFAULT_BPM);
		g_global_inited = true;
	}
}

static void record_cmd_locked(enum vs_tempo_cmd_kind kind)
{
	struct vs_tempo_snapshot snap;
	vs_tempo_snapshot(&g_global_tempo, &snap);

	const uint64_t seq = ++g_cmd_seq;
	struct vs_tempo_cmd *slot = &g_cmds[(seq - 1u) % VS_TEMPO_CMD_RING_CAP];
	slot->seq = seq;
	slot->kind = kind;
	slot->bpm = snap.bpm;
	slot->locked = snap.bpm_locked;
	slot->beat_index = snap.beat_index;
	slot->bar_index = snap.bar_index;
	g_global_tempo.synced_global_seq = seq;
}

void vs_tempo_global_reset(void)
{
	pthread_mutex_lock(&g_tempo_mutex);
	vs_tempo_init(&g_global_tempo, VS_TEMPO_DEFAULT_BPM);
	g_global_inited = true;
	g_cmd_seq = 0u;
	memset(g_cmds, 0, sizeof(g_cmds));
	pthread_mutex_unlock(&g_tempo_mutex);
}

void vs_tempo_global_get_snapshot(struct vs_tempo_snapshot *out_snap)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	vs_tempo_snapshot(&g_global_tempo, out_snap);
	pthread_mutex_unlock(&g_tempo_mutex);
}

void vs_tempo_global_observe_live(float bpm, uint32_t beat_index, float bar_phase, uint64_t bar_index)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	if (!g_global_tempo.bpm_locked && g_global_tempo.tap_count == 0u && is_finite_f32(bpm))
		g_global_tempo.bpm = clamp_bpm(bpm);
	if (g_global_tempo.tap_count == 0u) {
		if (is_finite_f32(bar_phase) && bar_phase >= 0.0f && bar_phase < 1.0f)
			g_global_tempo.bar_beats = (double)bar_phase * 4.0;
		else
			g_global_tempo.bar_beats = (double)(beat_index % VS_TEMPO_BEATS_PER_BAR);
		if (bar_index >= g_global_tempo.bar_index)
			g_global_tempo.bar_index = bar_index;
	}
	pthread_mutex_unlock(&g_tempo_mutex);
}

bool vs_tempo_global_tap(double now_sec, struct vs_tempo_snapshot *out_snap)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	const bool accepted = vs_tempo_tap(&g_global_tempo, now_sec);
	if (accepted)
		record_cmd_locked(VS_TEMPO_CMD_TAP);
	vs_tempo_snapshot(&g_global_tempo, out_snap);
	pthread_mutex_unlock(&g_tempo_mutex);
	return accepted;
}

bool vs_tempo_global_set_bpm(float bpm, struct vs_tempo_snapshot *out_snap)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	const bool ok = vs_tempo_set_bpm(&g_global_tempo, bpm);
	if (ok)
		record_cmd_locked(VS_TEMPO_CMD_SET_BPM);
	vs_tempo_snapshot(&g_global_tempo, out_snap);
	pthread_mutex_unlock(&g_tempo_mutex);
	return ok;
}

void vs_tempo_global_set_locked(bool locked, struct vs_tempo_snapshot *out_snap)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	vs_tempo_set_locked(&g_global_tempo, locked);
	record_cmd_locked(VS_TEMPO_CMD_LOCK);
	vs_tempo_snapshot(&g_global_tempo, out_snap);
	pthread_mutex_unlock(&g_tempo_mutex);
}

void vs_tempo_global_toggle_locked(struct vs_tempo_snapshot *out_snap)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	vs_tempo_set_locked(&g_global_tempo, !g_global_tempo.bpm_locked);
	record_cmd_locked(VS_TEMPO_CMD_LOCK);
	vs_tempo_snapshot(&g_global_tempo, out_snap);
	pthread_mutex_unlock(&g_tempo_mutex);
}

void vs_tempo_global_resync_downbeat(struct vs_tempo_snapshot *out_snap)
{
	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	vs_tempo_resync_downbeat(&g_global_tempo);
	record_cmd_locked(VS_TEMPO_CMD_RESYNC);
	vs_tempo_snapshot(&g_global_tempo, out_snap);
	pthread_mutex_unlock(&g_tempo_mutex);
}

bool vs_tempo_global_sync_instance(struct vs_tempo *t, bool *out_clear_ioi_history)
{
	if (out_clear_ioi_history)
		*out_clear_ioi_history = false;
	if (!t)
		return false;

	pthread_mutex_lock(&g_tempo_mutex);
	ensure_global_inited_locked();
	if (g_cmd_seq == 0u || t->synced_global_seq >= g_cmd_seq) {
		pthread_mutex_unlock(&g_tempo_mutex);
		return false;
	}

	bool clear_ioi = false;
	const uint64_t oldest_seq = (g_cmd_seq > VS_TEMPO_CMD_RING_CAP) ? (g_cmd_seq - VS_TEMPO_CMD_RING_CAP + 1u) : 1u;
	uint64_t start_seq = t->synced_global_seq + 1u;
	if (start_seq < oldest_seq) {
		/* Instance fell more than 32 commands behind (or was newly created): adopt current global state. */
		t->bpm = g_global_tempo.bpm;
		t->bpm_locked = g_global_tempo.bpm_locked;
		t->bar_beats = g_global_tempo.bar_beats;
		t->bar_index = g_global_tempo.bar_index;
		t->has_kick_aligned = g_global_tempo.has_kick_aligned;
		clear_ioi = true;
	} else {
		for (uint64_t s = start_seq; s <= g_cmd_seq; s++) {
			const struct vs_tempo_cmd *c = &g_cmds[(s - 1u) % VS_TEMPO_CMD_RING_CAP];
			if (c->seq != s)
				continue;
			switch (c->kind) {
			case VS_TEMPO_CMD_TAP:
				t->bpm = c->bpm;
				t->bar_beats = (double)c->beat_index;
				t->bar_index = c->bar_index;
				t->has_kick_aligned = true;
				clear_ioi = true;
				break;
			case VS_TEMPO_CMD_SET_BPM:
				t->bpm = c->bpm;
				clear_ioi = true;
				break;
			case VS_TEMPO_CMD_LOCK:
				t->bpm_locked = c->locked;
				if (c->locked)
					t->bpm = c->bpm;
				break;
			case VS_TEMPO_CMD_RESYNC:
				if (t->bar_beats > 1e-4 && c->bar_index > t->bar_index)
					t->bar_index = c->bar_index;
				else if (t->bar_beats > 1e-4)
					t->bar_index++;
				t->bar_beats = 0.0;
				t->has_kick_aligned = true;
				break;
			}
		}
	}

	t->synced_global_seq = g_cmd_seq;
	pthread_mutex_unlock(&g_tempo_mutex);

	if (out_clear_ioi_history)
		*out_clear_ioi_history = clear_ioi;
	return true;
}
