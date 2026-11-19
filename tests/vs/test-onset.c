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

/* Photosensitivity flash limiter (R-SHD-3 (8)), stepped at 60 fps. */
#define FLASH_TEST_FRAMES 300

static const float k_flash_dt = 1.0f / 60.0f;

static void run_flash_limiter(const float *over, int frames, float rate_hz, float *env)
{
	struct vs_flash_limiter lim;
	vs_flash_limiter_reset(&lim);
	for (int i = 0; i < frames; i++)
		env[i] = vs_flash_limiter_step(&lim, over[i], rate_hz, k_flash_dt);
}

/* A kick as the onset detector shapes it (a jump to 1, then a 65 ms decay) through the default 0.30 threshold: how
 * far it is over, 0..1, `age` frames after its onset. Over for 5 frames, then 0. */
static float kick_over(int age)
{
	const double kick = exp(-(double)age * (double)k_flash_dt / 0.065);
	const double over = (kick - 0.30) / 0.70;
	return over > 0.0 ? (float)over : 0.0f;
}

/* The photosensitivity contract on one run; returns the number of flashes. Any frame brighter than the one before is
 * a rise, and it must start from exactly 0: a flash never brightens again before it has gone dark. From `fall`
 * seconds after its rise a flash is exactly 0; rises are at least 1 / rate apart; and any half-open 1 s window holds
 * at most max_per_second rises (a window that starts at a rise is the fullest). */
static int check_flash_contract(const float *env, int frames, double rate_hz, int max_per_second)
{
	assert(frames <= FLASH_TEST_FRAMES);
	const double dt = (double)k_flash_dt;
	const double interval = 1.0 / rate_hz;
	const double fall = fmin(0.5 * interval, 0.15);
	int rise[FLASH_TEST_FRAMES];
	int rises = 0;
	float prev = 0.0f;
	for (int i = 0; i < frames; i++) {
		assert(env[i] >= 0.0f && env[i] <= 1.0f);
		if (env[i] > prev) {
			assert(prev == 0.0f);
			rise[rises++] = i;
		}
		prev = env[i];
	}
	for (int r = 0; r < rises; r++) {
		const int end = r + 1 < rises ? rise[r + 1] : frames;
		for (int i = rise[r] + 1; i < end; i++) {
			if ((double)(i - rise[r]) * dt >= fall + 1e-9)
				assert(env[i] == 0.0f);
		}
		if (r > 0)
			assert((double)(rise[r] - rise[r - 1]) * dt >= interval - 1e-6);
		int in_window = 0;
		for (int q = r; q < rises && (double)(rise[q] - rise[r]) * dt < 1.0; q++)
			in_window++;
		assert(in_window <= max_per_second);
	}
	return rises;
}

static void test_flash_limiter_caps_flash_rate(void)
{
	float over[FLASH_TEST_FRAMES];
	float env[FLASH_TEST_FRAMES];
	float ref[FLASH_TEST_FRAMES];

	/* (a) A trigger over its threshold on every frame for 3 s at the default 3 Hz: exactly 9 flashes, 1/3 s apart,
	 * each from full intensity down to exactly 0 within 0.15 s. */
	for (int i = 0; i < 180; i++)
		over[i] = 1.0f;
	run_flash_limiter(over, 180, 3.0f, env);
	assert(env[0] == 1.0f);
	assert(check_flash_contract(env, 180, 3.0, 3) == 9);

	/* The same trigger at lower settings: the cap follows the slider. */
	const struct {
		float rate;
		int max_per_second;
		int flashes;
	} k_settings[] = {{2.5f, 3, 8}, {2.0f, 2, 6}, {1.0f, 1, 3}, {0.5f, 1, 2}};
	for (size_t s = 0; s < sizeof(k_settings) / sizeof(k_settings[0]); s++) {
		run_flash_limiter(over, 180, k_settings[s].rate, env);
		assert(check_flash_contract(env, 180, k_settings[s].rate, k_settings[s].max_per_second) ==
		       k_settings[s].flashes);
	}

	/* (b) A 10 Hz trigger train (every 6 frames) at 3 Hz, where the old stateless gate let 7 flashes a second
	 * through. Single-frame pulses: 8 flashes in 3 s, 0.4 s apart. Kick-shaped pulses, over the threshold for 5
	 * frames each: exactly at the cap, 9 flashes 1/3 s apart. */
	for (int i = 0; i < 180; i++)
		over[i] = (i % 6 == 0) ? 1.0f : 0.0f;
	run_flash_limiter(over, 180, 3.0f, env);
	assert(check_flash_contract(env, 180, 3.0, 3) == 8);
	for (int i = 0; i < 180; i++)
		over[i] = kick_over(i % 6);
	run_flash_limiter(over, 180, 3.0f, env);
	assert(check_flash_contract(env, 180, 3.0, 3) == 9);

	/* (c) At 0.5 Hz, kicks at 1.9 s and 2.1 s (frames 114 and 126) give one flash, from the first kick. */
	for (int i = 0; i < 300; i++) {
		if (i >= 126)
			over[i] = kick_over(i - 126);
		else if (i >= 114)
			over[i] = kick_over(i - 114);
		else
			over[i] = 0.0f;
	}
	run_flash_limiter(over, 300, 0.5f, env);
	assert(check_flash_contract(env, 300, 0.5, 1) == 1);
	assert(env[113] == 0.0f && env[114] > 0.99f);

	/* (d) A trigger at or below its threshold, or NaN, never starts a flash. The limiter is live all along: the
	 * first trigger over the threshold flashes at once. */
	const float not_over[] = {0.0f, -0.0f, -1e-6f, -1.0f, -INFINITY, NAN};
	const float some_rates[] = {0.5f, 3.0f};
	for (size_t r = 0; r < sizeof(some_rates) / sizeof(some_rates[0]); r++) {
		for (size_t k = 0; k < sizeof(not_over) / sizeof(not_over[0]); k++) {
			struct vs_flash_limiter lim;
			vs_flash_limiter_reset(&lim);
			for (int i = 0; i < 240; i++)
				assert(vs_flash_limiter_step(&lim, not_over[k], some_rates[r], k_flash_dt) == 0.0f);
			assert(vs_flash_limiter_step(&lim, 0.25f, some_rates[r], k_flash_dt) == 0.25f);
		}
	}

	/* (e) A NaN or infinite rate behaves exactly like 3 Hz, and so do 3.25, 8 and 100 Hz (the cap has no unlock);
	 * rates under 0.5 Hz behave exactly like 0.5 Hz. Checked on the steady trigger and on the kick train. */
	const float like_3[] = {NAN, INFINITY, -INFINITY, 3.25f, 8.0f, 100.0f};
	const float like_half[] = {0.25f, 0.0f, -5.0f};
	for (int pattern = 0; pattern < 2; pattern++) {
		for (int i = 0; i < 180; i++)
			over[i] = pattern == 0 ? 1.0f : kick_over(i % 6);
		run_flash_limiter(over, 180, 3.0f, ref);
		for (size_t k = 0; k < sizeof(like_3) / sizeof(like_3[0]); k++) {
			run_flash_limiter(over, 180, like_3[k], env);
			for (int i = 0; i < 180; i++)
				assert(env[i] == ref[i]);
			assert(check_flash_contract(env, 180, 3.0, 3) == 9);
		}
		run_flash_limiter(over, 180, 0.5f, ref);
		for (size_t k = 0; k < sizeof(like_half) / sizeof(like_half[0]); k++) {
			run_flash_limiter(over, 180, like_half[k], env);
			for (int i = 0; i < 180; i++)
				assert(env[i] == ref[i]);
		}
	}

	/* (f) A NaN, negative, zero or infinite frame time does not advance time: a flash neither fades nor restarts
	 * through such frames, and once real frames resume the next flash comes exactly 20 real frames (1/3 s) after
	 * the first. */
	struct vs_flash_limiter lim;
	vs_flash_limiter_reset(&lim);
	assert(vs_flash_limiter_step(&lim, 0.6f, 3.0f, k_flash_dt) == 0.6f);
	float held = 0.0f;
	for (int i = 0; i < 3; i++)
		held = vs_flash_limiter_step(&lim, 0.0f, 3.0f, k_flash_dt);
	assert(held > 0.0f && held < 0.6f);
	const float bad_dt[] = {NAN, -k_flash_dt, -1.0f, -INFINITY, INFINITY, 0.0f, -0.0f};
	for (size_t k = 0; k < sizeof(bad_dt) / sizeof(bad_dt[0]); k++) {
		for (int i = 0; i < 40; i++)
			assert(vs_flash_limiter_step(&lim, 1.0f, 3.0f, bad_dt[k]) == held);
	}
	int real_frames = 3;
	float e = held;
	while (e != 1.0f) {
		e = vs_flash_limiter_step(&lim, 1.0f, 3.0f, k_flash_dt);
		real_frames++;
		assert(real_frames <= 20);
		if (real_frames >= 9 && real_frames < 20)
			assert(e == 0.0f);
	}
	assert(real_frames == 20);
}

/* Panic zeroes the peak of a flash in flight (shader-filter.c) and keeps the time since it was admitted: the envelope
 * stays exactly 0 until the next admitted flash, which still comes no sooner than 1/3 s after the last. */
static void test_flash_limiter_peak_zeroed_mid_fall(void)
{
	struct vs_flash_limiter lim;
	vs_flash_limiter_reset(&lim);
	assert(vs_flash_limiter_step(&lim, 1.0f, 3.0f, k_flash_dt) == 1.0f);
	assert(vs_flash_limiter_step(&lim, 1.0f, 3.0f, k_flash_dt) > 0.0f);
	lim.peak = 0.0f;
	for (int frame = 2; frame < 20; frame++)
		assert(vs_flash_limiter_step(&lim, 1.0f, 3.0f, k_flash_dt) == 0.0f);
	assert(vs_flash_limiter_step(&lim, 0.7f, 3.0f, k_flash_dt) == 0.7f);
}

static void assert_combined_window_cap(const float *env_a, const float *env_b, int frames, int max_per_second)
{
	int rises[FLASH_TEST_FRAMES * 2];
	int rise_count = 0;
	float prev_a = 0.0f;
	float prev_b = 0.0f;
	for (int i = 0; i < frames; i++) {
		if (env_a[i] > prev_a) {
			assert(prev_a == 0.0f);
			rises[rise_count++] = i;
		}
		if (env_b[i] > prev_b) {
			assert(prev_b == 0.0f);
			rises[rise_count++] = i;
		}
		prev_a = env_a[i];
		prev_b = env_b[i];
	}
	for (int r = 0; r < rise_count; r++) {
		int in_win = 0;
		for (int q = r; q < rise_count && (double)(rises[q] - rises[r]) * (double)k_flash_dt < 1.0; q++)
			in_win++;
		assert(in_win <= max_per_second);
	}
}

/* Two limiter clients sharing one flash clock (explicit or process-wide) fed a 10 Hz transient admit a combined total
 * of at most 3 flashes in any 1 s window, whether aligned, interleaved by 50 ms, or running with different personal
 * rate limits (P0-1). */
static void test_shared_flash_clock_caps_two_clients(void)
{
	float env_a[FLASH_TEST_FRAMES];
	float env_b[FLASH_TEST_FRAMES];
	struct vs_flash_clock clock;
	struct vs_flash_limiter lim_a;
	struct vs_flash_limiter lim_b;

	/* 1. Interleaved 10 Hz pulses (50 ms offset, where independent per-filter limiters interleaved to 6 Hz). */
	vs_flash_clock_reset(&clock);
	vs_flash_limiter_reset(&lim_a);
	vs_flash_limiter_reset(&lim_b);
	for (int i = 0; i < 180; i++) {
		const uint64_t tok = (uint64_t)(i + 1);
		const float oa = (i % 6 == 0) ? 1.0f : 0.0f;
		const float ob = (i % 6 == 3) ? 1.0f : 0.0f;
		env_a[i] = vs_flash_clock_step(&clock, &lim_a, tok, oa, 3.0f, k_flash_dt);
		env_b[i] = vs_flash_clock_step(&clock, &lim_b, tok, ob, 3.0f, k_flash_dt);
	}
	const int inter_a = check_flash_contract(env_a, 180, 3.0, 3);
	const int inter_b = check_flash_contract(env_b, 180, 3.0, 3);
	assert(inter_a > 0 && inter_b > 0);
	assert(inter_a + inter_b <= 9);
	assert_combined_window_cap(env_a, env_b, 180, 3);

	/* 2. Simultaneous 10 Hz kick-shaped pulses on the process-wide global clock (with B asking for 100 Hz). */
	vs_flash_clock_global_reset();
	vs_flash_limiter_reset(&lim_a);
	vs_flash_limiter_reset(&lim_b);
	for (int i = 0; i < 180; i++) {
		const uint64_t tok = (uint64_t)(i + 1);
		const float okick = kick_over(i % 6);
		env_a[i] = vs_flash_clock_global_step(&lim_a, tok, okick, 3.0f, k_flash_dt);
		env_b[i] = vs_flash_clock_global_step(&lim_b, tok, okick, 100.0f, k_flash_dt);
	}
	assert(check_flash_contract(env_a, 180, 3.0, 3) + check_flash_contract(env_b, 180, 3.0, 3) == 9);
	assert_combined_window_cap(env_a, env_b, 180, 3);

	/* 3. Client A lowered to 1.0 Hz personal rate while Client B runs at 3.0 Hz on the same global clock. */
	vs_flash_clock_global_reset();
	vs_flash_limiter_reset(&lim_a);
	vs_flash_limiter_reset(&lim_b);
	for (int i = 0; i < 180; i++) {
		const uint64_t tok = (uint64_t)(i + 1);
		const float okick = kick_over(i % 6);
		env_a[i] = vs_flash_clock_global_step(&lim_a, tok, okick, 1.0f, k_flash_dt);
		env_b[i] = vs_flash_clock_global_step(&lim_b, tok, okick, 3.0f, k_flash_dt);
	}
	assert(check_flash_contract(env_a, 180, 1.0, 1) <= 3);
	assert(check_flash_contract(env_b, 180, 3.0, 3) <= 9);
	assert_combined_window_cap(env_a, env_b, 180, 3);
}

static void test_tempo_tap_lock_gap_and_resync(void)
{
	vs_tempo_global_reset();

	struct vs_onset_config cfg;
	struct vs_onset onset;
	vs_onset_default_config(&cfg);
	assert(vs_onset_init(&onset, &cfg));

	float *mag_low = (float *)calloc(VS_MAX_BINS_LOW, sizeof(float));
	float *mag_mid = (float *)calloc(VS_MAX_BINS_MID, sizeof(float));
	struct vs_onset_result res;
	assert(mag_low && mag_mid);

	/* 1. Four taps at 500 ms intervals give 120 BPM within 1 and step beat_index 0..3. */
	assert(vs_onset_tap_tempo(&onset, 10.000));
	assert(onset.tempo.tap_count == 1u);
	assert(vs_onset_tap_tempo(&onset, 10.500));
	assert(vs_onset_tap_tempo(&onset, 11.000));
	assert(vs_onset_tap_tempo(&onset, 11.500));
	assert(onset.tempo.tap_count == 4u);
	assert(fabsf(onset.estimated_bpm - 120.0f) <= 1.0f);

	/* Switch bounce < 200 ms is ignored; a tap > 2.5 s later starts a fresh chain on beat 0. */
	assert(!vs_onset_tap_tempo(&onset, 11.600));
	assert(onset.tempo.tap_count == 4u);
	assert(vs_onset_tap_tempo(&onset, 15.000));
	assert(onset.tempo.tap_count == 1u);
	assert(onset.tempo.bar_beats == 0.0);

	/* 2. Lock BPM at 120: a 4 s kickless gap leaves bar_phase continuous (even across vs_onset_freeze_beat). */
	assert(vs_onset_set_bpm(&onset, 120.0f));
	vs_onset_set_bpm_locked(&onset, true);
	vs_onset_resync_downbeat(&onset);

	const float gap_dt = 1.0f / 64.0f; /* exact IEEE-754 power-of-2 step: 256 frames == 4.0 s == 2 full bars */
	float prev_bar_phase = 0.0f;
	uint64_t start_bar = onset.tempo.bar_index;
	for (int frame = 0; frame < 256; frame++) {
		if (frame == 160) {
			/* Even if silence hold attempts to freeze at > 2.0 s, locked mode keeps bar_phase running. */
			vs_onset_freeze_beat(&onset);
		}
		vs_onset_step(&onset, mag_low, mag_mid, gap_dt, &res);
		assert(res.bpm_locked);
		assert(fabsf(res.estimated_bpm - 120.0f) < 1e-4f);

		float delta = res.bar_phase - prev_bar_phase;
		if (delta < 0.0f)
			delta += 1.0f;
		assert(fabsf(delta - (1.0f / 128.0f)) < 1e-5f);
		prev_bar_phase = res.bar_phase;
	}
	/* 4.0 s at 120 BPM is 8 beats == 2 full 4-beat bars. */
	assert(res.bar_index == start_bar + 2u);

	/* Off-beat kicks at 0.40 s intervals (150 BPM) while locked never change BPM and ignore mid-beat hits. */
	const float dt = 0.010f;
	for (int frame = 0; frame < 200; frame++) {
		mag_low[8] = (frame % 40 == 25) ? 0.9f : 0.0f;
		vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
		assert(fabsf(res.estimated_bpm - 120.0f) < 1e-4f);
	}
	mag_low[8] = 0.0f;

	/* 3. resync_downbeat from mid-bar sets beat_index to 0 on the next event without decreasing bar_index. */
	assert(vs_onset_set_bpm(&onset, 120.0f));
	vs_onset_resync_downbeat(&onset);
	/* Step 1.25 s (2.5 beats -> beat_index == 2). */
	for (int frame = 0; frame < 125; frame++)
		vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	assert(res.beat_index == 2u);
	const uint64_t bar_before_resync = res.bar_index;

	vs_onset_resync_downbeat(&onset);
	vs_onset_step(&onset, mag_low, mag_mid, dt, &res);
	assert(res.beat_index == 0u);
	assert(res.bar_phase < 0.02f);
	assert(res.bar_index == bar_before_resync + 1u);

	/* 4. bar_phase and beat_phase stay strictly in [0, 1) even at double values just below 4.0. */
	struct vs_tempo near_wrap;
	struct vs_tempo_snapshot near_snap;
	vs_tempo_init(&near_wrap, 120.0f);
	near_wrap.bar_beats = nextafter(4.0, 0.0);
	vs_tempo_snapshot(&near_wrap, &near_snap);
	assert(near_snap.beat_index == 3u);
	assert(near_snap.beat_phase >= 0.0f && near_snap.beat_phase < 1.0f);
	assert(near_snap.bar_phase >= 0.0f && near_snap.bar_phase < 1.0f);

	free(mag_low);
	free(mag_mid);
}

int main(void)
{
	test_onset_kick_and_snare_separation();
	test_onset_refractory_cooldown();
	test_onset_beat_tempo_and_sync_advance();
	test_flash_limiter_caps_flash_rate();
	test_flash_limiter_peak_zeroed_mid_fall();
	test_shared_flash_clock_caps_two_clients();
	test_tempo_tap_lock_gap_and_resync();
	printf("test-onset: all assertions passed\n");
	return 0;
}
