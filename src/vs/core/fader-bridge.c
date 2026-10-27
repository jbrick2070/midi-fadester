/*
 * Visual Sidechain - Pure-C Fader Slot & Signal Bridge
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

void vs_fader_bridge_reset(void)
{
	memset(g_fader_values, 0, sizeof(g_fader_values));
	memset(&g_latest_signals, 0, sizeof(g_latest_signals));
	memset(&g_last_emitted_signals, 0, sizeof(g_last_emitted_signals));
	g_since_last_emit_sec = VS_SIGNALS_MIN_INTERVAL_SEC;
	g_has_emitted_once = false;
}

bool vs_fader_bridge_set_slot(int slot_idx, float value)
{
	if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
		return false;
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
	if (slot_idx < 0 || slot_idx >= VS_FADER_SLOT_COUNT)
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
