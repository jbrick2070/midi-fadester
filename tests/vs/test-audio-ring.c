/*
visual-sidechain
Copyright (C) 2026 Jeffrey A. Brick
SPDX-License-Identifier: GPL-2.0-or-later
*/

#undef NDEBUG
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "core/audio-ring.h"

static bool float_near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void test_ring_wrap_around(void)
{
	struct vs_audio_ring ring;
	assert(vs_audio_ring_init(&ring));

	/* Push 70,000 ramp samples across more than two 32,768-sample wrap-arounds */
	float chunk[1024];
	const size_t total_samples = 70000;
	size_t pushed = 0;
	while (pushed < total_samples) {
		size_t n = (total_samples - pushed > 1024) ? 1024 : (total_samples - pushed);
		for (size_t i = 0; i < n; i++) {
			chunk[i] = (float)(pushed + i + 1);
		}
		vs_audio_ring_push_mono(&ring, chunk, n, (uint64_t)(pushed + n) * 1000ULL);
		pushed += n;
	}

	float win4096[4096];
	uint64_t written = 0;
	uint64_t ts = 0;
	assert(vs_audio_ring_read_window(&ring, 4096, 0, win4096, &written, &ts));
	assert(written == total_samples);
	assert(ts == (uint64_t)total_samples * 1000ULL);

	for (size_t i = 0; i < 4096; i++) {
		float expected = (float)(total_samples - 4096 + i + 1);
		assert(float_near(win4096[i], expected, 1e-4f));
	}

	float win2048[2048];
	assert(vs_audio_ring_read_window(&ring, 2048, 0, win2048, NULL, NULL));
	for (size_t i = 0; i < 2048; i++) {
		float expected = (float)(total_samples - 2048 + i + 1);
		assert(float_near(win2048[i], expected, 1e-4f));
	}

	vs_audio_ring_free(&ring);
}

static void test_ring_sync_offset_delay(void)
{
	struct vs_audio_ring ring;
	assert(vs_audio_ring_init(&ring));

	/* Push 30,000 samples with a unit impulse at global sample index 5,999
	 * (which is 24,001 samples before the end of the 30,000-sample stream). */
	float block[1000];
	for (size_t b = 0; b < 30; b++) {
		memset(block, 0, sizeof(block));
		if (b == 5) {
			block[999] = 1.0f; /* global index 5999 */
		}
		if (b == 24) {
			block[999] = 0.75f; /* global index 24999 */
		}
		if (b == 29) {
			block[999] = 0.5f; /* global index 29999 (latest sample) */
		}
		vs_audio_ring_push_mono(&ring, block, 1000, (uint64_t)(b + 1) * 1000000ULL);
	}

	float win[2048];

	/* 1. offset_samples = 0: latest sample (29999 -> 0.5f) is at win[2047] */
	assert(vs_audio_ring_read_window(&ring, 2048, 0, win, NULL, NULL));
	assert(float_near(win[2047], 0.5f, 1e-6f));
	assert(float_near(win[2046], 0.0f, 1e-6f));

	/* 2. offset_samples = 5000 (~104.2 ms at 48 kHz): end of window is sample 24999 (0.75f) */
	assert(vs_audio_ring_read_window(&ring, 2048, 5000, win, NULL, NULL));
	assert(float_near(win[2047], 0.75f, 1e-6f));

	/* 3. offset_samples = 24000 (+500 ms at 48 kHz): end of window is sample 5999 (1.0f) */
	float win4096[4096];
	assert(vs_audio_ring_read_window(&ring, 4096, 24000, win4096, NULL, NULL));
	assert(float_near(win4096[4095], 1.0f, 1e-6f));
	assert(float_near(win4096[4094], 0.0f, 1e-6f));

	/* Out-of-bounds window + offset (> 32768) must be rejected cleanly */
	assert(!vs_audio_ring_read_window(&ring, 4096, 30000, win4096, NULL, NULL));
	assert(!vs_audio_ring_read_window(&ring, SIZE_MAX, 1, win4096, NULL, NULL));
	assert(!vs_audio_ring_read_window(&ring, 4096, SIZE_MAX, win4096, NULL, NULL));
	assert(float_near(win4096[4095], 1.0f, 1e-6f));

	vs_audio_ring_free(&ring);
}

static void test_ring_nan_inf_sanitization(void)
{
	struct vs_audio_ring ring;
	assert(vs_audio_ring_init(&ring));

	float left[4] = {NAN, INFINITY, -INFINITY, 0.5f};
	float right[4] = {0.4f, -0.2f, NAN, 0.25f};
	const float *planes[2] = {left, right};

	vs_audio_ring_push_planar(&ring, planes, 2, 4, false, false, 100ULL);

	float out[4];
	assert(vs_audio_ring_read_window(&ring, 4, 0, out, NULL, NULL));
	/* (0.0 + 0.4)/2 = 0.2, (0.0 + -0.2)/2 = -0.1, (0.0 + 0.0)/2 = 0.0, (0.5 + 0.25)/2 = 0.375 */
	assert(isfinite(out[0]) && float_near(out[0], 0.2f, 1e-6f));
	assert(isfinite(out[1]) && float_near(out[1], -0.1f, 1e-6f));
	assert(isfinite(out[2]) && float_near(out[2], 0.0f, 1e-6f));
	assert(isfinite(out[3]) && float_near(out[3], 0.375f, 1e-6f));

	/* Muted source without ghost-sidechain pushes 0.0f */
	vs_audio_ring_push_planar(&ring, planes, 2, 4, true, false, 200ULL);
	assert(vs_audio_ring_read_window(&ring, 4, 0, out, NULL, NULL));
	for (size_t i = 0; i < 4; i++) {
		assert(float_near(out[i], 0.0f, 1e-6f));
	}

	/* Muted source WITH react_when_muted (ghost sidechain) preserves signal */
	vs_audio_ring_push_planar(&ring, planes, 2, 4, true, true, 300ULL);
	assert(vs_audio_ring_read_window(&ring, 4, 0, out, NULL, NULL));
	assert(float_near(out[3], 0.375f, 1e-6f));

	vs_audio_ring_free(&ring);
}

/* OBS set to 5.1 or 7.1: a stereo source fills front left and right only, and OBS spreads a mono source over every
 * plane (LFE excepted on 7.1). Both reach the analyser at the level a stereo layout gives them. */
static void test_ring_surround_layouts_keep_stereo_level(void)
{
	struct vs_audio_ring ring;
	assert(vs_audio_ring_init(&ring));

	float front_l[4] = {0.5f, 0.5f, -0.5f, 0.25f};
	float front_r[4] = {0.5f, 0.3f, -0.5f, 0.25f};
	float silent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	float mono[4] = {0.5f, 0.4f, -0.5f, 0.25f};
	const float expect[4] = {0.5f, 0.4f, -0.5f, 0.25f};
	float out[4];

	for (size_t channels = 6; channels <= 8; channels += 2) {
		const float *stereo_planes[8] = {front_l, front_r, silent, silent, silent, silent, silent, silent};
		vs_audio_ring_push_planar(&ring, stereo_planes, channels, 4, false, false, 100ULL);
		assert(vs_audio_ring_read_window(&ring, 4, 0, out, NULL, NULL));
		for (size_t i = 0; i < 4; i++)
			assert(float_near(out[i], expect[i], 1e-6f));

		const float *mono_planes[8] = {mono, mono, mono, channels == 8 ? silent : mono, mono, mono, mono, mono};
		vs_audio_ring_push_planar(&ring, mono_planes, channels, 4, false, false, 200ULL);
		assert(vs_audio_ring_read_window(&ring, 4, 0, out, NULL, NULL));
		for (size_t i = 0; i < 4; i++)
			assert(float_near(out[i], expect[i], 1e-6f));

		/* every plane silent: silence */
		const float *quiet_planes[8] = {silent, silent, silent, silent, silent, silent, silent, silent};
		vs_audio_ring_push_planar(&ring, quiet_planes, channels, 4, false, false, 400ULL);
		assert(vs_audio_ring_read_window(&ring, 4, 0, out, NULL, NULL));
		for (size_t i = 0; i < 4; i++)
			assert(float_near(out[i], 0.0f, 1e-9f));
	}

	vs_audio_ring_free(&ring);
}

/* The last sample the ring holds. */
static float ring_last(struct vs_audio_ring *ring)
{
	float out[1];
	assert(vs_audio_ring_read_window(ring, 1, 0, out, NULL, NULL));
	return out[0];
}

/* 5.1 and 7.1 with the front silent and sound only in the centre (dialogue): after the front has been silent for
 * VS_AUDIO_RING_FALLBACK_HOLD blocks, the centre arrives at its own level (the silent rear planes and the LFE are not
 * averaged in). Front noise between -70 and -50 dBFS does not flip the choice back; clear front sound does, at once. */
static void test_ring_surround_centre_fallback(void)
{
	float centre[4] = {0.5f, 0.5f, 0.5f, 0.5f};
	float lfe[4] = {0.9f, 0.9f, 0.9f, 0.9f};
	float dither[4] = {1e-4f, -1e-4f, 1e-4f, -1e-4f}; /* -80 dBFS: silent */
	float hiss[4] = {1e-3f, -1e-3f, 1e-3f, -1e-3f};   /* -60 dBFS: quiet, not silent, not clear sound */
	float music[4] = {0.5f, 0.5f, 0.5f, 0.5f};
	float silent[4] = {0.0f, 0.0f, 0.0f, 0.0f};

	for (size_t channels = 6; channels <= 8; channels += 2) {
		struct vs_audio_ring ring;
		assert(vs_audio_ring_init(&ring));
		/* LFE is plane 3 on 5.1 and 7.1: loud here, and never averaged in */
		const float *dialogue[8] = {dither, dither, centre, lfe, silent, silent, silent, silent};
		for (unsigned b = 1; b < VS_AUDIO_RING_FALLBACK_HOLD; b++) {
			vs_audio_ring_push_planar(&ring, dialogue, channels, 4, false, false, b);
			assert(fabsf(ring_last(&ring)) < 2e-4f); /* held: still the front */
		}
		vs_audio_ring_push_planar(&ring, dialogue, channels, 4, false, false, 10ULL);
		assert(float_near(ring_last(&ring), 0.5f, 1e-6f)); /* the centre, at its own level */

		/* no chatter: front noise below the switch-back level, block after block, keeps the centre */
		const float *noisy[8] = {hiss, dither, centre, lfe, silent, silent, silent, silent};
		for (int b = 0; b < 6; b++) {
			vs_audio_ring_push_planar(&ring, (b & 1) ? dialogue : noisy, channels, 4, false, false,
						  20ULL + b);
			assert(float_near(ring_last(&ring), 0.5f, 1e-6f));
		}

		/* clear front sound takes the front back at once */
		const float *front_back[8] = {music, music, centre, lfe, silent, silent, silent, silent};
		vs_audio_ring_push_planar(&ring, front_back, channels, 4, false, false, 30ULL);
		assert(float_near(ring_last(&ring), 0.5f, 1e-6f));
		const float *front_only[8] = {music, silent, silent, silent, silent, silent, silent, silent};
		vs_audio_ring_push_planar(&ring, front_only, channels, 4, false, false, 31ULL);
		assert(float_near(ring_last(&ring), 0.25f, 1e-6f)); /* (music + 0) / 2: the front pair again */

		/* and the hold starts over: one silent-front block does not switch */
		vs_audio_ring_push_planar(&ring, dialogue, channels, 4, false, false, 32ULL);
		assert(fabsf(ring_last(&ring)) < 2e-4f);
		vs_audio_ring_free(&ring);
	}
}

static void test_ring_underrun_zero_pad(void)
{
	struct vs_audio_ring ring;
	assert(vs_audio_ring_init(&ring));

	float short_input[10] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
	vs_audio_ring_push_mono(&ring, short_input, 10, 50ULL);

	float win[4096];
	for (size_t i = 0; i < 4096; i++) {
		win[i] = -99.0f;
	}
	assert(vs_audio_ring_read_window(&ring, 4096, 0, win, NULL, NULL));
	for (size_t i = 0; i < 4086; i++) {
		assert(float_near(win[i], 0.0f, 1e-6f));
	}
	for (size_t i = 0; i < 10; i++) {
		assert(float_near(win[4086 + i], (float)(i + 1), 1e-6f));
	}

	vs_audio_ring_free(&ring);
}

int main(void)
{
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif

	test_ring_wrap_around();
	test_ring_sync_offset_delay();
	test_ring_nan_inf_sanitization();
	test_ring_surround_layouts_keep_stereo_level();
	test_ring_surround_centre_fallback();
	test_ring_underrun_zero_pad();

	printf("test-audio-ring: all 6 tests passed\n");
	return 0;
}
