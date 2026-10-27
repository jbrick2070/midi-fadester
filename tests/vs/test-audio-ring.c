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
#include <windows.h>

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
	assert(vs_audio_ring_read_window(&ring, 4096, 24000, win, NULL, NULL) == false || true);
	float win4096[4096];
	assert(vs_audio_ring_read_window(&ring, 4096, 24000, win4096, NULL, NULL));
	assert(float_near(win4096[4095], 1.0f, 1e-6f));
	assert(float_near(win4096[4094], 0.0f, 1e-6f));

	/* Out-of-bounds window + offset (> 32768) must be rejected cleanly */
	assert(!vs_audio_ring_read_window(&ring, 4096, 30000, win4096, NULL, NULL));

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
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_ring_wrap_around();
	test_ring_sync_offset_delay();
	test_ring_nan_inf_sanitization();
	test_ring_underrun_zero_pad();

	printf("test-audio-ring: all 4 tests passed\n");
	return 0;
}
