/*
 * Visual Sidechain - Pure-C Fader Slot & In-Process Signal Bridge
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "fader-bridge.h"

#include <util/threading.h>

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static const char *const k_fader_slot_ids[VS_FADER_SLOT_COUNT] = {
	"crossfader", "fader_1", "fader_2", "fader_3", "fader_4", "fader_5", "fader_6", "fader_7", "fader_8", "master",
};

/*
 * Leaf data mutex protecting shared bridge snapshots and coalescing state
 * across video_tick, obs-websocket vendor request threads, and global signal
 * callbacks. Never held across proc_handler_call or signal_handler_signal, and
 * never acquired on the real-time audio capture callback thread.
 */
static pthread_mutex_t g_bridge_mutex = PTHREAD_MUTEX_INITIALIZER;

static proc_handler_t *g_ph = NULL;
static signal_handler_t *g_sh = NULL;
static bool g_midi_fadester_loaded = false;
static int g_poll_slot_idx = 0;
static volatile bool g_panic_active = false;

static float g_fader_values[VS_FADER_SLOT_COUNT];
static struct vs_audio_frame g_latest_signals;
static struct vs_audio_frame g_last_emitted_signals;
static float g_since_last_emit_sec = VS_SIGNALS_MIN_INTERVAL_SEC;
static bool g_has_emitted_once = false;

static uint64_t g_last_tick_frame_ts = 0;
static bool g_frame_has_primary_publisher = false;
static uint32_t g_primary_seen_frames_ago = 1000u;

static float clamp01(float x)
{
	if (!isfinite(x) || x <= 0.0f)
		return 0.0f;
	if (x >= 1.0f)
		return 1.0f;
	return x;
}

const char *vs_fader_slot_id(int slot_idx)
{
	if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
		return NULL;
	return k_fader_slot_ids[slot_idx];
}

int vs_fader_slot_from_name(const char *name)
{
	if (!name || name[0] == '\0')
		return -1;
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		if (strcmp(name, k_fader_slot_ids[i]) == 0)
			return i;
	}
	return -1;
}

static void proc_visual_sidechain_get_signals(void *data, calldata_t *cd)
{
	(void)data;
	struct vs_audio_frame snap;
	pthread_mutex_lock(&g_bridge_mutex);
	snap = g_latest_signals;
	pthread_mutex_unlock(&g_bridge_mutex);

	calldata_set_float(cd, "level", (double)snap.level);
	calldata_set_float(cd, "bass", (double)snap.bass);
	calldata_set_float(cd, "mid", (double)snap.mid);
	calldata_set_float(cd, "high", (double)snap.high);
	calldata_set_float(cd, "kick", (double)snap.kick);
	calldata_set_float(cd, "snare", (double)snap.snare);
	calldata_set_float(cd, "beat_phase", (double)snap.beat);
	calldata_set_float(cd, "bpm", (double)snap.estimated_bpm);
	calldata_set_bool(cd, "ok", true);
}

static void on_global_signal(void *data, const char *signal, calldata_t *params)
{
	(void)data;
	if (!signal || !params)
		return;
	if (strcmp(signal, "midi_fadester_fader_changed") != 0)
		return;

	const char *slot_name = calldata_string(params, "slot");
	const int idx = vs_fader_slot_from_name(slot_name);
	if (idx < 0)
		return;

	const float clamped = clamp01((float)calldata_float(params, "value"));
	pthread_mutex_lock(&g_bridge_mutex);
	g_midi_fadester_loaded = true;
	g_fader_values[idx] = clamped;
	pthread_mutex_unlock(&g_bridge_mutex);
}

void vs_fader_bridge_detach(void)
{
	signal_handler_t *old_sh = NULL;
	pthread_mutex_lock(&g_bridge_mutex);
	old_sh = g_sh;
	g_sh = NULL;
	g_ph = NULL;
	g_midi_fadester_loaded = false;
	memset(g_fader_values, 0, sizeof(g_fader_values));
	pthread_mutex_unlock(&g_bridge_mutex);

	if (old_sh)
		signal_handler_disconnect_global(old_sh, on_global_signal, NULL);
}

void vs_fader_bridge_reset(void)
{
	vs_fader_bridge_detach();
	os_atomic_set_bool(&g_panic_active, false);

	pthread_mutex_lock(&g_bridge_mutex);
	g_poll_slot_idx = 0;
	memset(&g_latest_signals, 0, sizeof(g_latest_signals));
	memset(&g_last_emitted_signals, 0, sizeof(g_last_emitted_signals));
	g_since_last_emit_sec = VS_SIGNALS_MIN_INTERVAL_SEC;
	g_has_emitted_once = false;
	g_last_tick_frame_ts = 0;
	g_frame_has_primary_publisher = false;
	g_primary_seen_frames_ago = 1000u;
	pthread_mutex_unlock(&g_bridge_mutex);
}

void vs_fader_bridge_attach(proc_handler_t *ph, signal_handler_t *sh)
{
	vs_fader_bridge_reset();

	pthread_mutex_lock(&g_bridge_mutex);
	g_ph = ph;
	g_sh = sh;
	pthread_mutex_unlock(&g_bridge_mutex);

	if (ph) {
		proc_handler_add(
			ph,
			"void visual_sidechain_get_signals(out float level, out float bass, out float mid, out float high, out float kick, out float snare, out float beat_phase, out float bpm, out bool ok)",
			proc_visual_sidechain_get_signals, NULL);
	}

	if (sh) {
		signal_handler_add(
			sh,
			"void visual_sidechain_signals(float level, float bass, float mid, float high, float kick, float snare, float beat_phase, float bpm, bool ok)");
		signal_handler_connect_global(sh, on_global_signal, NULL);
	}

	vs_fader_bridge_poll_once();
}

bool vs_fader_bridge_is_midi_fadester_loaded(void)
{
	pthread_mutex_lock(&g_bridge_mutex);
	const bool loaded = g_midi_fadester_loaded;
	pthread_mutex_unlock(&g_bridge_mutex);
	return loaded;
}

bool vs_fader_bridge_poll_once(void)
{
	pthread_mutex_lock(&g_bridge_mutex);
	proc_handler_t *ph = g_ph;
	if (!ph) {
		g_midi_fadester_loaded = false;
		memset(g_fader_values, 0, sizeof(g_fader_values));
		pthread_mutex_unlock(&g_bridge_mutex);
		return false;
	}
	const int idx = g_poll_slot_idx % VS_FADER_SLOT_COUNT;
	g_poll_slot_idx = (g_poll_slot_idx + 1) % VS_FADER_SLOT_COUNT;
	pthread_mutex_unlock(&g_bridge_mutex);

	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", k_fader_slot_ids[idx]);
	const bool proc_exists = proc_handler_call(ph, "midi_fadester_get_slot_value", &cd);
	if (!proc_exists) {
		calldata_free(&cd);
		pthread_mutex_lock(&g_bridge_mutex);
		if (g_ph == ph) {
			g_midi_fadester_loaded = false;
			memset(g_fader_values, 0, sizeof(g_fader_values));
		}
		pthread_mutex_unlock(&g_bridge_mutex);
		return false;
	}

	const bool ok = calldata_bool(&cd, "ok");
	const float val = ok ? clamp01((float)calldata_float(&cd, "value")) : 0.0f;
	calldata_free(&cd);

	pthread_mutex_lock(&g_bridge_mutex);
	if (g_ph == ph) {
		g_midi_fadester_loaded = true;
		if (ok)
			g_fader_values[idx] = val;
	}
	pthread_mutex_unlock(&g_bridge_mutex);
	return true;
}

bool vs_fader_bridge_set_slot(int slot_idx, float value)
{
	if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
		return false;
	const float clamped = clamp01(value);
	pthread_mutex_lock(&g_bridge_mutex);
	g_midi_fadester_loaded = true;
	g_fader_values[slot_idx] = clamped;
	pthread_mutex_unlock(&g_bridge_mutex);
	return true;
}

bool vs_fader_bridge_set_slot_by_name(const char *name, float value)
{
	const int idx = vs_fader_slot_from_name(name);
	if (idx < 0)
		return false;
	return vs_fader_bridge_set_slot(idx, value);
}

float vs_fader_bridge_get_slot(int slot_idx)
{
	if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
		return 0.0f;
	pthread_mutex_lock(&g_bridge_mutex);
	const float val = g_midi_fadester_loaded ? g_fader_values[slot_idx] : 0.0f;
	pthread_mutex_unlock(&g_bridge_mutex);
	return val;
}

void vs_fader_bridge_get_slots(float out_values[VS_FADER_SLOT_COUNT])
{
	if (!out_values)
		return;
	pthread_mutex_lock(&g_bridge_mutex);
	if (g_midi_fadester_loaded)
		memcpy(out_values, g_fader_values, sizeof(g_fader_values));
	else
		memset(out_values, 0, sizeof(g_fader_values));
	pthread_mutex_unlock(&g_bridge_mutex);
}

float vs_fader_bridge_resolve_scale(int slot_idx)
{
	if (slot_idx < 0)
		return 1.0f;
	if (slot_idx >= VS_FADER_SLOT_COUNT)
		return 0.0f;
	return vs_fader_bridge_get_slot(slot_idx);
}

void vs_fader_bridge_scale_audio_frame(const struct vs_audio_frame *in_frame, float reactivity_scale,
				       struct vs_audio_frame *out_frame)
{
	if (!out_frame)
		return;
	if (!in_frame) {
		memset(out_frame, 0, sizeof(*out_frame));
		return;
	}
	*out_frame = *in_frame;
	const float s = clamp01(reactivity_scale);
	if (s >= 0.999999f)
		return;

	out_frame->level *= s;
	out_frame->sub_bass *= s;
	out_frame->bass *= s;
	out_frame->mid *= s;
	out_frame->high *= s;
	out_frame->kick *= s;
	out_frame->snare *= s;
	out_frame->beat *= s;
	out_frame->spectral_flux *= s;
	for (size_t i = 0; i < VS_SPECTRUM_BANDS; i++)
		out_frame->spectrum[i] *= s;
	for (size_t i = 0; i < VS_FFT_TEX_BINS; i++) {
		out_frame->fft_bins[i] *= s;
		out_frame->waveform[i] = 0.5f + (out_frame->waveform[i] - 0.5f) * s;
	}
}

static double clamp_f64_finite(double v, double lo, double hi)
{
	if (!isfinite(v))
		return lo;
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

float vs_fader_bridge_map_float_param(float value, double min_val, double max_val, bool has_hard_min, double hard_min,
				      bool has_hard_max, double hard_max)
{
	const double t = (double)clamp01(value);
	/* Saturate before interpolation and conversion: a finite double can overflow a float. */
	const double safe_min = isfinite(min_val) ? clamp_f64_finite(min_val, -FLT_MAX, FLT_MAX) : 0.0;
	const double safe_max = isfinite(max_val) ? clamp_f64_finite(max_val, -FLT_MAX, FLT_MAX) : 1.0;
	const double lo = (safe_min <= safe_max) ? safe_min : safe_max;
	const double hi = (safe_min <= safe_max) ? safe_max : safe_min;

	double mapped = clamp_f64_finite(safe_min + t * (safe_max - safe_min), lo, hi);
	if (has_hard_min && isfinite(hard_min) && mapped < hard_min)
		mapped = clamp_f64_finite(hard_min, -FLT_MAX, FLT_MAX);
	if (has_hard_max && isfinite(hard_max) && mapped > hard_max)
		mapped = clamp_f64_finite(hard_max, -FLT_MAX, FLT_MAX);
	return (float)mapped;
}

float vs_fader_bridge_eval_float_param(int slot_idx, float slider_val, double min_val, double max_val,
				       bool has_hard_min, double hard_min, bool has_hard_max, double hard_max)
{
	if (slot_idx < 0)
		return slider_val;
	return vs_fader_bridge_map_float_param(vs_fader_bridge_get_slot(slot_idx), min_val, max_val, has_hard_min,
					       hard_min, has_hard_max, hard_max);
}

int vs_fader_bridge_map_int_param(float value, double min_val, double max_val, int step, bool has_hard_min,
				  double hard_min, bool has_hard_max, double hard_max)
{
	const double t = (double)clamp01(value);
	/* Every llround input and step arithmetic must already fit the uniform's int domain. */
	const double safe_min = isfinite(min_val) ? clamp_f64_finite(min_val, INT_MIN, INT_MAX) : 0.0;
	const double safe_max = isfinite(max_val) ? clamp_f64_finite(max_val, INT_MIN, INT_MAX) : 1.0;
	const double lo = (safe_min <= safe_max) ? safe_min : safe_max;
	const double hi = (safe_min <= safe_max) ? safe_max : safe_min;
	const double mapped = clamp_f64_finite(safe_min + t * (safe_max - safe_min), lo, hi);

	const int64_t q_step = (step > 1) ? (int64_t)step : 1;
	const int64_t i_lo = (int64_t)llround(lo);
	const int64_t i_hi = (int64_t)llround(hi);
	int64_t q = i_lo + llround((mapped - (double)i_lo) / (double)q_step) * q_step;
	if (q < i_lo)
		q = i_lo;
	if (q > i_hi)
		q = i_hi;
	if (has_hard_min && isfinite(hard_min)) {
		const int64_t h_lo = (int64_t)ceil(clamp_f64_finite(hard_min, INT_MIN, INT_MAX));
		if (q < h_lo)
			q = h_lo;
	}
	if (has_hard_max && isfinite(hard_max)) {
		const int64_t h_hi = (int64_t)floor(clamp_f64_finite(hard_max, INT_MIN, INT_MAX));
		if (q > h_hi)
			q = h_hi;
	}
	if (q < (int64_t)INT_MIN)
		q = (int64_t)INT_MIN;
	if (q > (int64_t)INT_MAX)
		q = (int64_t)INT_MAX;
	return (int)q;
}

int vs_fader_bridge_eval_int_param(int slot_idx, int slider_val, double min_val, double max_val, int step,
				   bool has_hard_min, double hard_min, bool has_hard_max, double hard_max)
{
	if (slot_idx < 0)
		return slider_val;
	return vs_fader_bridge_map_int_param(vs_fader_bridge_get_slot(slot_idx), min_val, max_val, step, has_hard_min,
					     hard_min, has_hard_max, hard_max);
}

void vs_fader_bridge_publish_signals(const struct vs_audio_frame *frame)
{
	if (!frame)
		return;
	pthread_mutex_lock(&g_bridge_mutex);
	g_latest_signals = *frame;
	pthread_mutex_unlock(&g_bridge_mutex);
}

void vs_fader_bridge_get_signals(struct vs_audio_frame *out_frame)
{
	if (!out_frame)
		return;
	pthread_mutex_lock(&g_bridge_mutex);
	*out_frame = g_latest_signals;
	pthread_mutex_unlock(&g_bridge_mutex);
}

static bool signals_differ_beyond_eps(const struct vs_audio_frame *a, const struct vs_audio_frame *b, float eps)
{
	if (fabsf(a->level - b->level) > eps)
		return true;
	if (fabsf(a->sub_bass - b->sub_bass) > eps)
		return true;
	if (fabsf(a->bass - b->bass) > eps)
		return true;
	if (fabsf(a->mid - b->mid) > eps)
		return true;
	if (fabsf(a->high - b->high) > eps)
		return true;
	if (fabsf(a->kick - b->kick) > eps)
		return true;
	if (fabsf(a->snare - b->snare) > eps)
		return true;
	if (fabsf(a->beat - b->beat) > eps)
		return true;
	if (fabsf(a->spectral_centroid - b->spectral_centroid) > eps)
		return true;
	if (fabsf(a->spectral_flux - b->spectral_flux) > eps)
		return true;
	if (fabsf(a->estimated_bpm - b->estimated_bpm) > eps)
		return true;
	for (size_t i = 0; i < VS_SPECTRUM_BANDS; i++) {
		if (fabsf(a->spectrum[i] - b->spectrum[i]) > eps)
			return true;
	}
	return false;
}

static bool should_emit_signals_locked(float dt_sec, const struct vs_audio_frame *frame)
{
	if (!frame)
		return false;
	if (isfinite(dt_sec) && dt_sec > 0.0f)
		g_since_last_emit_sec += dt_sec;

	if (g_since_last_emit_sec + 1e-6f < VS_SIGNALS_MIN_INTERVAL_SEC)
		return false;

	if (g_has_emitted_once && !signals_differ_beyond_eps(frame, &g_last_emitted_signals, 1e-3f))
		return false;

	g_last_emitted_signals = *frame;
	g_since_last_emit_sec = 0.0f;
	g_has_emitted_once = true;
	return true;
}

bool vs_fader_bridge_should_emit_signals(float dt_sec, const struct vs_audio_frame *frame)
{
	if (!frame)
		return false;
	pthread_mutex_lock(&g_bridge_mutex);
	const bool emit = should_emit_signals_locked(dt_sec, frame);
	pthread_mutex_unlock(&g_bridge_mutex);
	return emit;
}

static void emit_visual_sidechain_signal(signal_handler_t *sh, const struct vs_audio_frame *frame)
{
	if (!sh || !frame)
		return;
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_float(&cd, "level", (double)frame->level);
	calldata_set_float(&cd, "bass", (double)frame->bass);
	calldata_set_float(&cd, "mid", (double)frame->mid);
	calldata_set_float(&cd, "high", (double)frame->high);
	calldata_set_float(&cd, "kick", (double)frame->kick);
	calldata_set_float(&cd, "snare", (double)frame->snare);
	calldata_set_float(&cd, "beat_phase", (double)frame->beat);
	calldata_set_float(&cd, "bpm", (double)frame->estimated_bpm);
	calldata_set_bool(&cd, "ok", true);
	signal_handler_signal(sh, "visual_sidechain_signals", &cd);
	calldata_free(&cd);
}

bool vs_fader_bridge_on_frame_tick(uint64_t video_frame_ts, float dt_sec, const struct vs_audio_frame *frame,
				   bool is_primary_track)
{
	if (!frame)
		return false;

	if (video_frame_ts == 0) {
		pthread_mutex_lock(&g_bridge_mutex);
		g_latest_signals = *frame;
		const bool has_ph = (g_ph != NULL);
		pthread_mutex_unlock(&g_bridge_mutex);

		if (has_ph)
			vs_fader_bridge_poll_once();

		pthread_mutex_lock(&g_bridge_mutex);
		const bool emit = should_emit_signals_locked(dt_sec, frame);
		signal_handler_t *sh = emit ? g_sh : NULL;
		struct vs_audio_frame snap = *frame;
		pthread_mutex_unlock(&g_bridge_mutex);

		if (emit && sh)
			emit_visual_sidechain_signal(sh, &snap);
		return emit;
	}

	bool do_poll = false;
	bool check_emit = false;
	bool did_emit = false;
	signal_handler_t *sh = NULL;
	struct vs_audio_frame emit_snap;
	memset(&emit_snap, 0, sizeof(emit_snap));

	pthread_mutex_lock(&g_bridge_mutex);
	const bool new_frame = (video_frame_ts != g_last_tick_frame_ts);
	if (new_frame) {
		g_last_tick_frame_ts = video_frame_ts;
		g_frame_has_primary_publisher = false;
		if (g_primary_seen_frames_ago < 1000u)
			g_primary_seen_frames_ago++;
		do_poll = (g_ph != NULL);
		if (isfinite(dt_sec) && dt_sec > 0.0f)
			g_since_last_emit_sec += dt_sec;
	}

	if (is_primary_track) {
		g_primary_seen_frames_ago = 0u;
		if (!g_frame_has_primary_publisher) {
			g_latest_signals = *frame;
			g_frame_has_primary_publisher = true;
			check_emit = true;
		}
	} else if (new_frame && !g_frame_has_primary_publisher && g_primary_seen_frames_ago > 1u) {
		g_latest_signals = *frame;
		check_emit = true;
	}

	if (check_emit && should_emit_signals_locked(0.0f, &g_latest_signals)) {
		did_emit = true;
		sh = g_sh;
		emit_snap = g_latest_signals;
	}
	pthread_mutex_unlock(&g_bridge_mutex);

	if (do_poll)
		vs_fader_bridge_poll_once();
	if (did_emit && sh)
		emit_visual_sidechain_signal(sh, &emit_snap);

	return did_emit;
}

bool vs_fader_bridge_on_tick(float dt_sec, const struct vs_audio_frame *frame)
{
	return vs_fader_bridge_on_frame_tick(0, dt_sec, frame, true);
}

static bool contains_word_ci(const char *haystack, const char *needle)
{
	if (!haystack || !needle || needle[0] == '\0')
		return false;
	const size_t nlen = strlen(needle);
	for (const char *p = haystack; *p != '\0'; p++) {
		size_t i = 0;
		while (i < nlen && p[i] != '\0') {
			char c1 = p[i];
			char c2 = needle[i];
			if (c1 >= 'A' && c1 <= 'Z')
				c1 = (char)(c1 - 'A' + 'a');
			if (c2 >= 'A' && c2 <= 'Z')
				c2 = (char)(c2 - 'A' + 'a');
			if (c1 != c2)
				break;
			i++;
		}
		if (i == nlen)
			return true;
	}
	return false;
}

bool vs_fader_bridge_is_strobe_or_flash_param(const char *param_name)
{
	if (!param_name || param_name[0] == '\0')
		return false;

	/* Rate caps, thresholds, and frequency bounds are safety/filter limits, not strobe/flash amounts */
	if (contains_word_ci(param_name, "rate") || contains_word_ci(param_name, "threshold") ||
	    contains_word_ci(param_name, "_hz")) {
		return false;
	}

	return contains_word_ci(param_name, "flash") || contains_word_ci(param_name, "strobe") ||
	       contains_word_ci(param_name, "intensity") || contains_word_ci(param_name, "amount") ||
	       contains_word_ci(param_name, "strength") || contains_word_ci(param_name, "boost") ||
	       contains_word_ci(param_name, "reactivity");
}

void vs_fader_bridge_trigger_panic(void)
{
	os_atomic_set_bool(&g_panic_active, true);
}

void vs_fader_bridge_trigger_restore(void)
{
	os_atomic_set_bool(&g_panic_active, false);
}

bool vs_fader_bridge_is_panic_active(void)
{
	return os_atomic_load_bool(&g_panic_active);
}

bool vs_fader_bridge_should_shader_passthrough(bool filter_panic_passthrough, bool has_compiled_effect)
{
	if (!has_compiled_effect)
		return true;
	if (filter_panic_passthrough || os_atomic_load_bool(&g_panic_active))
		return true;
	return false;
}

void vs_fader_bridge_apply_panic_to_slots(struct vs_panic_param_slot *slots, size_t count)
{
	if (!slots)
		return;
	for (size_t i = 0; i < count; i++) {
		if (!vs_fader_bridge_is_strobe_or_flash_param(slots[i].name))
			continue;
		if (!slots[i].saved_valid) {
			slots[i].saved_value = slots[i].current_value;
			slots[i].saved_valid = true;
		}
		slots[i].current_value = 0.0;
	}
}

void vs_fader_bridge_apply_restore_to_slots(struct vs_panic_param_slot *slots, size_t count)
{
	if (!slots)
		return;
	for (size_t i = 0; i < count; i++) {
		if (!slots[i].saved_valid)
			continue;
		slots[i].current_value = slots[i].saved_value;
		slots[i].saved_valid = false;
	}
}

void vs_fader_bridge_apply_panic_to_shader(bool *filter_panic_passthrough, struct vs_panic_param_slot *slots,
					   size_t count)
{
	if (filter_panic_passthrough)
		*filter_panic_passthrough = true;
	vs_fader_bridge_apply_panic_to_slots(slots, count);
}

void vs_fader_bridge_apply_restore_to_shader(bool *filter_panic_passthrough, struct vs_panic_param_slot *slots,
					     size_t count)
{
	if (filter_panic_passthrough)
		*filter_panic_passthrough = false;
	vs_fader_bridge_apply_restore_to_slots(slots, count);
}
