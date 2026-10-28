/*
 * Visual Sidechain - End-to-End Analyzer & WAV Fixture Suite
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

#include "core/analyzer.h"
#include "wav-reader.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VS_TEST_FIXTURES_DIR
#define VS_TEST_FIXTURES_DIR "."
#endif

static void test_wav_kick_120bpm(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/kick-120bpm-4bars.wav", VS_TEST_FIXTURES_DIR);

	struct vs_wav_data wav;
	assert(vs_wav_read(path, &wav));
	assert(wav.sample_rate == 48000u);
	assert(wav.frames == 48000u * 4u);

	struct vs_analyzer_config cfg;
	struct vs_analyzer ana;
	vs_analyzer_default_config(&cfg);
	assert(vs_analyzer_init(&ana, &cfg));

	const size_t chunk = 800u; /* 60 fps at 48 kHz (16.6667 ms per video frame) */
	const float dt = (float)chunk / (float)wav.sample_rate;
	const size_t total_frames = wav.frames / chunk;

	float *kick_series = (float *)calloc(total_frames, sizeof(float));
	float *snare_series = (float *)calloc(total_frames, sizeof(float));
	assert(kick_series && snare_series);

	struct vs_audio_frame frame;
	for (size_t f = 0; f < total_frames; f++) {
		const uint64_t ts_ns = (uint64_t)((f + 1u) * chunk) * 1000000000ULL / wav.sample_rate;
		vs_analyzer_push_mono(&ana, &wav.mono_samples[f * chunk], chunk, ts_ns);
		vs_analyzer_step(&ana, dt, &frame);
		kick_series[f] = frame.kick;
		snare_series[f] = frame.snare;
	}

	/* Verify all 8 kicks (beats 0..7 at t = 0.0, 0.5, ..., 3.5 s, frame = beat * 30). */
	for (int beat = 0; beat < 8; beat++) {
		const size_t f0 = (size_t)beat * 30u;
		float kick_peak = 0.0f;
		float snare_peak = 0.0f;
		/* Check within <= 35 ms (frames f0 .. f0 + 2) of known kick/snare timestamp. */
		for (size_t df = 0; df <= 2u && (f0 + df) < total_frames; df++) {
			if (kick_series[f0 + df] > kick_peak)
				kick_peak = kick_series[f0 + df];
			if (snare_series[f0 + df] > snare_peak)
				snare_peak = snare_series[f0 + df];
		}

		assert(kick_peak > 0.7f);
		/* Between beats (f0 + 18 = +300 ms), kick envelope must decay < 0.2. */
		assert(kick_series[f0 + 18u] < 0.2f);

		if (beat % 2 == 1) {
			/* Beats 2 and 4 (t = 0.5, 1.5, 2.5, 3.5 s) have snare hits. */
			assert(snare_peak > 0.7f);
		} else {
			/* Kick-only beats 1 and 3 must not false-trigger snare (< 0.15). */
			assert(snare_peak < 0.15f);
		}
	}

	/* Beat tracker must lock to 120 BPM (+/- 2 BPM). */
	assert(fabsf(frame.estimated_bpm - 120.0f) <= 2.0f);

	free(kick_series);
	free(snare_series);
	vs_analyzer_free(&ana);
	vs_wav_free(&wav);
}

static void test_wav_subbass_35hz_vs_kick_95hz(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/subbass-35hz-vs-kick-95hz.wav", VS_TEST_FIXTURES_DIR);

	struct vs_wav_data wav;
	assert(vs_wav_read(path, &wav));
	assert(wav.sample_rate == 48000u);

	struct vs_analyzer_config cfg;
	struct vs_analyzer ana;
	vs_analyzer_default_config(&cfg);
	assert(vs_analyzer_init(&ana, &cfg));

	const size_t chunk = 800u;
	const float dt = (float)chunk / (float)wav.sample_rate;
	const size_t total_frames = wav.frames / chunk;

	float *subbass_series = (float *)calloc(total_frames, sizeof(float));
	float *kick_series = (float *)calloc(total_frames, sizeof(float));
	assert(subbass_series && kick_series);

	struct vs_audio_frame frame;
	for (size_t f = 0; f < total_frames; f++) {
		const uint64_t ts_ns = (uint64_t)((f + 1u) * chunk) * 1000000000ULL / wav.sample_rate;
		vs_analyzer_push_mono(&ana, &wav.mono_samples[f * chunk], chunk, ts_ns);
		vs_analyzer_step(&ana, dt, &frame);
		subbass_series[f] = frame.sub_bass;
		kick_series[f] = frame.kick;
	}

	/* Steady 35 Hz sub-bass drone keeps sub_bass > 0.7 after initial window fill. */
	assert(subbass_series[20] > 0.7f);
	assert(kick_series[20] < 0.2f);

	/* Each of the 4 95 Hz kicks (t = 0.5, 1.0, 1.5, 2.0 s -> frames 30, 60, 90, 120) triggers kick > 0.65. */
	for (int k = 1; k <= 4; k++) {
		const size_t f0 = (size_t)k * 30u;
		float kick_peak = 0.0f;
		for (size_t df = 0; df <= 2u && (f0 + df) < total_frames; df++) {
			if (kick_series[f0 + df] > kick_peak)
				kick_peak = kick_series[f0 + df];
		}
		assert(kick_peak > 0.65f);
		assert(subbass_series[f0] > 0.7f);
	}

	free(subbass_series);
	free(kick_series);
	vs_analyzer_free(&ana);
	vs_wav_free(&wav);
}

static void test_wav_dynamic_range_and_silence(void)
{
	char path_dyn[512];
	char path_sil[512];
	snprintf(path_dyn, sizeof(path_dyn), "%s/dynamic-range-40db.wav", VS_TEST_FIXTURES_DIR);
	snprintf(path_sil, sizeof(path_sil), "%s/silence-and-dc.wav", VS_TEST_FIXTURES_DIR);

	struct vs_wav_data wav_dyn;
	struct vs_wav_data wav_sil;
	assert(vs_wav_read(path_dyn, &wav_dyn));
	assert(vs_wav_read(path_sil, &wav_sil));

	struct vs_analyzer_config cfg;
	struct vs_analyzer ana;
	vs_analyzer_default_config(&cfg);
	assert(vs_analyzer_init(&ana, &cfg));

	const size_t chunk = 800u;
	const float dt = (float)chunk / 48000.0f;
	struct vs_audio_frame frame;

	/* 1. dynamic-range-40db.wav: first 1.0 s (-36 dBFS) and second 1.0 s (-6 dBFS) both normalize > 0.75. */
	float bass_end_quiet = 0.0f;
	float bass_end_loud = 0.0f;
	for (size_t f = 0; f < wav_dyn.frames / chunk; f++) {
		const uint64_t ts_ns = (uint64_t)((f + 1u) * chunk) * 1000000000ULL / wav_dyn.sample_rate;
		vs_analyzer_push_mono(&ana, &wav_dyn.mono_samples[f * chunk], chunk, ts_ns);
		vs_analyzer_step(&ana, dt, &frame);
		if (f == 58u)
			bass_end_quiet = frame.bass;
		if (f == 118u)
			bass_end_loud = frame.bass;
	}
	assert(bass_end_quiet > 0.75f);
	assert(bass_end_loud > 0.75f);

	/* 2. silence-and-dc.wav: silence, -70 dBFS noise, and +0.25 DC offset all stay 0.0. */
	vs_analyzer_reset(&ana);
	for (size_t f = 0; f < wav_sil.frames / chunk; f++) {
		if (f == 60u) {
			/* Reset at start of the steady +0.25 DC segment so we test steady DC, not a step click. */
			vs_analyzer_reset(&ana);
		}
		const uint64_t ts_ns = (uint64_t)((f + 1u) * chunk) * 1000000000ULL / wav_sil.sample_rate;
		vs_analyzer_push_mono(&ana, &wav_sil.mono_samples[f * chunk], chunk, ts_ns);
		vs_analyzer_step(&ana, dt, &frame);
		assert(frame.level == 0.0f);
		assert(frame.sub_bass == 0.0f);
		assert(frame.bass == 0.0f);
		assert(frame.kick == 0.0f);
		assert(frame.snare == 0.0f);
	}

	vs_analyzer_free(&ana);
	vs_wav_free(&wav_dyn);
	vs_wav_free(&wav_sil);
}

static void test_custom_hz_flux_after_analyzer_step(void)
{
	struct vs_analyzer_config cfg;
	struct vs_analyzer ana;
	struct vs_audio_frame frame;
	float samples[VS_FFT_LOW_SIZE] = {0};
	vs_analyzer_default_config(&cfg);
	assert(vs_analyzer_init(&ana, &cfg));
	vs_analyzer_push_mono(&ana, samples, VS_FFT_LOW_SIZE, 1u);
	vs_analyzer_step(&ana, 1.0f / 60.0f, &frame);
	for (size_t i = 0; i < VS_FFT_LOW_SIZE; i++)
		samples[i] = sinf(6.28318530717958647692f * 440.0f * (float)i / 48000.0f);
	vs_analyzer_push_mono(&ana, samples, VS_FFT_LOW_SIZE, 2u);
	vs_analyzer_step(&ana, 1.0f / 60.0f, &frame);
	assert(vs_analyzer_get_signal(&ana, &frame, VS_SIG_CUSTOM_HZ, 420.0f, 460.0f, VS_HZ_MODE_FLUX) > 0.8f);
	/* Repeated queries on one frame are stable, and the next unchanged FFT has no flux. */
	assert(vs_analyzer_get_signal(&ana, &frame, VS_SIG_CUSTOM_HZ, 420.0f, 460.0f, VS_HZ_MODE_FLUX) > 0.8f);
	vs_analyzer_step(&ana, 1.0f / 60.0f, &frame);
	assert(vs_analyzer_get_signal(&ana, &frame, VS_SIG_CUSTOM_HZ, 420.0f, 460.0f, VS_HZ_MODE_FLUX) == 0.0f);
	vs_analyzer_free(&ana);
}

static void test_aligned_windows_with_delayed_startup(void)
{
	struct vs_analyzer_config cfg;
	struct vs_analyzer ana;
	struct vs_audio_frame frame;
	float samples[100];
	for (size_t i = 0; i < 100; i++)
		samples[i] = 0.25f;
	vs_analyzer_default_config(&cfg);
	cfg.sync_offset_ms = 1.0f; /* 48 delayed samples, leaving 52 valid DC samples. */
	assert(vs_analyzer_init(&ana, &cfg));
	vs_analyzer_push_mono(&ana, samples, 100u, 1u);
	vs_analyzer_step(&ana, 1.0f / 60.0f, &frame);
	for (size_t i = 0; i < VS_FFT_LOW_SIZE; i++)
		assert(ana.win_low[i] == 0.0f);
	for (size_t i = 0; i < VS_FFT_MID_SIZE; i++)
		assert(ana.win_mid[i] == 0.0f);
	assert(frame.level == 0.0f);
	vs_analyzer_free(&ana);

	/* Configurable FFT sizes also support the mid window being the larger snapshot. */
	vs_analyzer_default_config(&cfg);
	cfg.fft_size_low = 1024u;
	assert(vs_analyzer_init(&ana, &cfg));
	vs_analyzer_push_mono(&ana, samples, 100u, 1u);
	vs_analyzer_step(&ana, 1.0f / 60.0f, &frame);
	assert(frame.level == 0.0f);
	vs_analyzer_free(&ana);
}

int main(void)
{
	assert(vs_wav_ensure_fixtures(VS_TEST_FIXTURES_DIR));
	test_wav_kick_120bpm();
	test_wav_subbass_35hz_vs_kick_95hz();
	test_wav_dynamic_range_and_silence();
	test_custom_hz_flux_after_analyzer_step();
	test_aligned_windows_with_delayed_startup();
	printf("test-analyzer: all WAV fixture assertions passed\n");
	return 0;
}
