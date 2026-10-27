/*
 * Visual Sidechain - Auto-Gain & Asymmetric Envelope Unit Tests
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/envelope.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static bool float_near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void test_envelope_noise_gate_silence(void)
{
	struct vs_envelope_config cfg;
	struct vs_envelope_channel ch;
	vs_envelope_default_config(&cfg);
	vs_envelope_channel_reset(&ch);

	/* 1. Pure silence (0.0f) stays 0.0f over 120 frames at 60 fps. */
	for (int i = 0; i < 120; i++) {
		const float out = vs_envelope_step(&ch, &cfg, 0.0f, 1.0f / 60.0f, 0.0f);
		assert(out == 0.0f);
	}

	/* 2. -70 dBFS background noise (below -55 dBFS gate) stays 0.0f under auto-gain. */
	const float noise_m70db = powf(10.0f, -70.0f / 20.0f);
	for (int i = 0; i < 240; i++) {
		const float out = vs_envelope_step(&ch, &cfg, noise_m70db, 1.0f / 60.0f, 0.0f);
		assert(out == 0.0f);
	}
}

static void test_envelope_autogain_40db(void)
{
	struct vs_envelope_config cfg;
	struct vs_envelope_channel ch_quiet;
	struct vs_envelope_channel ch_loud;
	vs_envelope_default_config(&cfg);
	vs_envelope_channel_reset(&ch_quiet);
	vs_envelope_channel_reset(&ch_loud);

	const float sig_m36db = powf(10.0f, -36.0f / 20.0f);
	const float sig_m6db = powf(10.0f, -6.0f / 20.0f);
	const float dt = 1.0f / 60.0f;

	float out_quiet = 0.0f;
	float out_loud = 0.0f;
	for (int i = 0; i < 60; i++) {
		out_quiet = vs_envelope_step(&ch_quiet, &cfg, sig_m36db, dt, 0.0f);
		out_loud = vs_envelope_step(&ch_loud, &cfg, sig_m6db, dt, 0.0f);
	}

	assert(out_quiet >= 0.75f && out_quiet <= 1.0f);
	assert(out_loud >= 0.75f && out_loud <= 1.0f);
	assert(float_near(out_quiet, out_loud, 0.05f));

	/* Also verify manual [floor_db, ceiling_db] dB mapping when auto_gain is false. */
	cfg.auto_gain = false;
	cfg.floor_db = -45.0f;
	cfg.ceiling_db = -6.0f;
	vs_envelope_channel_reset(&ch_quiet);
	const float manual_m36 = vs_envelope_normalize(&ch_quiet, &cfg, sig_m36db, dt);
	const float manual_m6 = vs_envelope_normalize(&ch_quiet, &cfg, sig_m6db, dt);
	assert(float_near(manual_m36, (-36.0f - (-45.0f)) / (-6.0f - (-45.0f)), 1e-4f));
	assert(float_near(manual_m6, 1.0f, 1e-4f));
}

static void test_envelope_attack_release_constants(void)
{
	struct vs_envelope_config cfg;
	struct vs_envelope_channel ch;
	vs_envelope_default_config(&cfg);
	cfg.attack_ms = 50.0f;
	cfg.release_ms = 200.0f;
	vs_envelope_channel_reset(&ch);

	/* Step 0 -> 1 for dt == attack_ms (50 ms): reaches 1 - e^-1 ~= 0.63212056. */
	const float expected_attack = 1.0f - expf(-1.0f);
	const float v_attack = vs_envelope_smooth(&ch, &cfg, 1.0f, 0.050f);
	assert(float_near(v_attack, expected_attack, 1e-5f));

	/* Frame-rate invariance check: 10 sub-steps of 5 ms produce the exact same value. */
	struct vs_envelope_channel ch_sub;
	vs_envelope_channel_reset(&ch_sub);
	for (int i = 0; i < 10; i++)
		vs_envelope_smooth(&ch_sub, &cfg, 1.0f, 0.005f);
	assert(float_near(ch_sub.value, expected_attack, 1e-5f));

	/* Step 1 -> 0 for dt == release_ms (200 ms): decays to e^-1 ~= 0.36787944. */
	ch.value = 1.0f;
	const float expected_release = expf(-1.0f);
	const float v_release = vs_envelope_smooth(&ch, &cfg, 0.0f, 0.200f);
	assert(float_near(v_release, expected_release, 1e-5f));
}

static void test_envelope_starvation_decay(void)
{
	struct vs_envelope_config cfg;
	struct vs_envelope_channel ch;
	vs_envelope_default_config(&cfg);
	vs_envelope_channel_reset(&ch);

	const float dt = 1.0f / 60.0f;
	for (int i = 0; i < 30; i++)
		vs_envelope_step(&ch, &cfg, 0.5f, dt, 0.0f);
	assert(ch.value > 0.9f);

	/* Simulate stopping audio callbacks (> 50 ms starvation) for 0.5 s. */
	float starved_sec = 0.060f;
	for (int i = 0; i < 30; i++) {
		starved_sec += dt;
		vs_envelope_step(&ch, &cfg, 0.5f, dt, starved_sec);
	}
	assert(ch.value < 1e-3f);
}

int main(void)
{
	test_envelope_noise_gate_silence();
	test_envelope_autogain_40db();
	test_envelope_attack_release_constants();
	test_envelope_starvation_decay();
	printf("test-envelope: all assertions passed\n");
	return 0;
}
