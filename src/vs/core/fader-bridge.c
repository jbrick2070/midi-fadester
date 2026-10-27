/*
 * Visual Sidechain - Pure-C Fader Slot & In-Process Signal Bridge
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "fader-bridge.h"

#include <math.h>
#include <string.h>

static const char *const k_fader_slot_ids[VS_FADER_SLOT_COUNT] = {
	"crossfader", "fader_1", "fader_2", "fader_3", "fader_4", "fader_5", "fader_6", "fader_7", "fader_8", "master",
};

static proc_handler_t *g_ph = NULL;
static signal_handler_t *g_sh = NULL;
static bool g_midi_fadester_loaded = false;
static int g_poll_slot_idx = 0;

static float g_fader_values[VS_FADER_SLOT_COUNT];
static struct vs_audio_frame g_latest_signals;
static struct vs_audio_frame g_last_emitted_signals;
static float g_since_last_emit_sec = VS_SIGNALS_MIN_INTERVAL_SEC;
static bool g_has_emitted_once = false;

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
	calldata_set_float(cd, "level", (double)g_latest_signals.level);
	calldata_set_float(cd, "bass", (double)g_latest_signals.bass);
	calldata_set_float(cd, "mid", (double)g_latest_signals.mid);
	calldata_set_float(cd, "high", (double)g_latest_signals.high);
	calldata_set_float(cd, "kick", (double)g_latest_signals.kick);
	calldata_set_float(cd, "snare", (double)g_latest_signals.snare);
	calldata_set_float(cd, "beat_phase", (double)g_latest_signals.beat);
	calldata_set_float(cd, "bpm", (double)g_latest_signals.estimated_bpm);
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

	g_midi_fadester_loaded = true;
	g_fader_values[idx] = clamp01((float)calldata_float(params, "value"));
}

void vs_fader_bridge_detach(void)
{
	if (g_sh) {
		signal_handler_disconnect_global(g_sh, on_global_signal, NULL);
		g_sh = NULL;
	}
	g_ph = NULL;
	g_midi_fadester_loaded = false;
	memset(g_fader_values, 0, sizeof(g_fader_values));
}

void vs_fader_bridge_reset(void)
{
	vs_fader_bridge_detach();
	g_poll_slot_idx = 0;
	memset(&g_latest_signals, 0, sizeof(g_latest_signals));
	memset(&g_last_emitted_signals, 0, sizeof(g_last_emitted_signals));
	g_since_last_emit_sec = VS_SIGNALS_MIN_INTERVAL_SEC;
	g_has_emitted_once = false;
}

void vs_fader_bridge_attach(proc_handler_t *ph, signal_handler_t *sh)
{
	vs_fader_bridge_reset();
	g_ph = ph;
	g_sh = sh;

	if (g_ph) {
		proc_handler_add(
			g_ph,
			"void visual_sidechain_get_signals(out float level, out float bass, out float mid, out float high, out float kick, out float snare, out float beat_phase, out float bpm, out bool ok)",
			proc_visual_sidechain_get_signals, NULL);
	}

	if (g_sh) {
		signal_handler_add(
			g_sh,
			"void visual_sidechain_signals(float level, float bass, float mid, float high, float kick, float snare, float beat_phase, float bpm, bool ok)");
		signal_handler_connect_global(g_sh, on_global_signal, NULL);
	}

	vs_fader_bridge_poll_once();
}

bool vs_fader_bridge_is_midi_fadester_loaded(void)
{
	return g_midi_fadester_loaded;
}

bool vs_fader_bridge_poll_once(void)
{
	if (!g_ph) {
		g_midi_fadester_loaded = false;
		memset(g_fader_values, 0, sizeof(g_fader_values));
		return false;
	}

	const int idx = g_poll_slot_idx % VS_FADER_SLOT_COUNT;
	g_poll_slot_idx = (g_poll_slot_idx + 1) % VS_FADER_SLOT_COUNT;

	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", k_fader_slot_ids[idx]);
	const bool proc_exists = proc_handler_call(g_ph, "midi_fadester_get_slot_value", &cd);
	if (!proc_exists) {
		calldata_free(&cd);
		g_midi_fadester_loaded = false;
		memset(g_fader_values, 0, sizeof(g_fader_values));
		return false;
	}

	g_midi_fadester_loaded = true;
	if (calldata_bool(&cd, "ok")) {
		const float val = (float)calldata_float(&cd, "value");
		g_fader_values[idx] = clamp01(val);
	}
	calldata_free(&cd);
	return true;
}

bool vs_fader_bridge_set_slot(int slot_idx, float value)
{
	if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
		return false;
	g_midi_fadester_loaded = true;
	g_fader_values[slot_idx] = clamp01(value);
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
	if (!g_midi_fadester_loaded || slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
		return 0.0f;
	return g_fader_values[slot_idx];
}

void vs_fader_bridge_publish_signals(const struct vs_audio_frame *frame)
{
	if (!frame)
		return;
	g_latest_signals = *frame;
}

void vs_fader_bridge_get_signals(struct vs_audio_frame *out_frame)
{
	if (!out_frame)
		return;
	*out_frame = g_latest_signals;
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
	return false;
}

bool vs_fader_bridge_should_emit_signals(float dt_sec, const struct vs_audio_frame *frame)
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

bool vs_fader_bridge_on_tick(float dt_sec, const struct vs_audio_frame *frame)
{
	if (!frame)
		return false;

	vs_fader_bridge_publish_signals(frame);
	if (g_ph)
		vs_fader_bridge_poll_once();

	if (!vs_fader_bridge_should_emit_signals(dt_sec, frame))
		return false;

	if (g_sh) {
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
		signal_handler_signal(g_sh, "visual_sidechain_signals", &cd);
		calldata_free(&cd);
	}

	return true;
}
