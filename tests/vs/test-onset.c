/*
 * Visual Sidechain - Onset & Beat Phase Tracker Unit Tests
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/onset.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static void test_onset_kick_and_snare_separation(void)
{
	struct vs_onset_config cfg;
	struct vs_onset onset;
	vs_onset_default_config(&cfg);
	assert(vs_onset_init(&onset, &cfg));

	float *mag_low = (float *)calloc(VS_MAX_BINS_LOW, sizeof(float));
	float *mag_mid = (float *)calloc(VS_MAX_BINS_MID, sizeof(float));
	struct vs_onset_result res;
	assert(mag_low && mag_mid);

	/* 1. Kick burst in 4096-pt bins 7..8 (~82..94 Hz): triggers kick (> 0.7), not snare. */
	mag_low[7] = 0.85f;
	mag_low[8] = 0.85f;
	vs_onset_step(&onset, mag_low, mag_mid, 1.0f / 60.0f, &res);
	assert(res.kick_triggered);
	assert(!res.snare_triggered);
	assert(res.kick > 0.9f);
	assert(res.snare < 0.01f);

	/* Decay over 250 ms of silence. */
	mag_low[7] = 0.0f;
	mag_low[8] = 0.0f;
	for (int i = 0; i < 15; i++)
		vs_onset_step(&onset, mag_low, mag_mid, 1.0f / 60.0f, &res);
	assert(res.kick < 0.2f);

	/* 2. Snare burst in 2048-pt bins 40..60 (~937..1406 Hz): triggers snare (> 0.7), not kick. */
	for (uint32_t k = 40; k <= 60; k++)
		mag_mid[k] = 0.35f;
	vs_onset_step(&onset, mag_low, mag_mid, 1.0f / 60.0f, &res);
	assert(res.snare_triggered);
	assert(!res.kick_triggered);
	assert(res.snare > 0.9f);
	assert(res.kick < 0.05f);

	free(mag_low);
	free(mag_mid);
}

static void test_onset_refractory_cooldown(void)
{
	struct vs_onset_config cfg;
	struct vs_onset onset;
	vs_onset_default_config(&cfg);
	assert(vs_onset_init(&onset, &cfg));

	float *mag_low = (float *)calloc(VS_MAX_BINS_LOW, sizeof(float));
	float *mag_mid = (float *)calloc(VS_MAX_BINS_MID, sizeof(float));
	struct vs_onset_result res;
	assert(mag_low && mag_mid);

	const float dt = 1.0f / 60.0f;
	mag_low[8] = 0.9f;
	vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	assert(res.kick_triggered);

	/* Drop to zero on next frame, then re-hit 33 ms later (still inside 100 ms refractory window). */
	mag_low[8] = 0.0f;
	vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	mag_low[8] = 0.9f;
	vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	assert(!res.kick_triggered);

	free(mag_low);
	free(mag_mid);
}

static void test_onset_beat_tempo_and_sync_advance(void)
{
	struct vs_onset_config cfg;
	struct vs_onset onset;
	vs_onset_default_config(&cfg);
	assert(vs_onset_init(&onset, &cfg));

	float *mag_low = (float *)calloc(VS_MAX_BINS_LOW, sizeof(float));
	float *mag_mid = (float *)calloc(VS_MAX_BINS_MID, sizeof(float));
	struct vs_onset_result res;
	assert(mag_low && mag_mid);

	/* Step at 100 Hz (dt = 10 ms) with kicks every 50 frames (0.500 s = 120 BPM). */
	const float dt = 0.010f;
	for (int frame = 0; frame < 300; frame++) {
		mag_low[8] = (frame % 50 == 0) ? 0.9f : 0.0f;
		vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	}
	assert(fabsf(res.estimated_bpm - 120.0f) < 1.0f);

	/* At kick frame (frame 300), beat phase resets to 0.0; with -125 ms sync offset, advances by +0.25. */
	onset.cfg.sync_offset_ms = -125.0f;
	mag_low[8] = 0.9f;
	vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	assert(res.kick_triggered);
	assert(fabsf(res.beat - 0.25f) < 0.02f);

	free(mag_low);
	free(mag_mid);
}

int main(void)
{
	test_onset_kick_and_snare_separation();
	test_onset_refractory_cooldown();
	test_onset_beat_tempo_and_sync_advance();
	printf("test-onset: all assertions passed\n");
	return 0;
}
