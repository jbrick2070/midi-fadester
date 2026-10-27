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

#include "core/fft.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool float_near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void test_fft_4096_subbass_resolution(void)
{
	struct vs_fft_plan plan4096;
	assert(vs_fft_plan_init(&plan4096, 4096));

	/* At 48 kHz, N = 4096 has bin spacing 11.71875 Hz:
	 * - Sub-bass tone at bin 3: 3 * 11.71875 = 35.15625 Hz (amplitude 1.0)
	 * - Kick/bass tone at bin 7: 7 * 11.71875 = 82.03125 Hz (amplitude 0.8) */
	float samples[4096];
	float mag[2049];
	const double fs = 48000.0;
	const double f_sub = 3.0 * (fs / 4096.0);
	const double f_kick = 7.0 * (fs / 4096.0);

	for (size_t i = 0; i < 4096; i++) {
		double t = (double)i / fs;
		samples[i] = (float)(sin(2.0 * M_PI * f_sub * t) + 0.8 * sin(2.0 * M_PI * f_kick * t));
	}

	assert(vs_fft_compute(&plan4096, samples, mag));

	/* Distinct peaks at bin 3 (~1.0) and bin 7 (~0.8) */
	assert(float_near(mag[3], 1.0f, 1e-3f));
	assert(float_near(mag[7], 0.8f, 1e-3f));

	/* Deep valley (> 40 dB below peaks) at bin 5 (58.59 Hz) between sub-bass and kick */
	assert(mag[5] < 1e-3f);
	assert(mag[0] < 1e-4f);

	vs_fft_plan_free(&plan4096);
}

static void test_fft_2048_mid_high(void)
{
	struct vs_fft_plan plan2048;
	assert(vs_fft_plan_init(&plan2048, 2048));

	/* 1. Exact-bin high tone at bin 341 (7992.1875 Hz at 48 kHz) */
	float samples[2048];
	float mag[1025];
	const double fs = 48000.0;
	const double f_high = 341.0 * (fs / 2048.0);
	for (size_t i = 0; i < 2048; i++) {
		samples[i] = (float)sin(2.0 * M_PI * f_high * ((double)i / fs));
	}
	assert(vs_fft_compute(&plan2048, samples, mag));
	assert(float_near(mag[341], 1.0f, 1e-3f));
	assert(mag[338] < 1e-4f);
	assert(mag[344] < 1e-4f);

	/* 2. Worst-case half-bin tone at bin 42.5 (996.09375 Hz at 48 kHz):
	 * Hann window suppresses sidelobes > 40 dB (< 0.01) at >= 5 bins away,
	 * whereas an unwindowed rectangular FFT leaks > 0.06 at bin 48. */
	const double f_mid_half = 42.5 * (fs / 2048.0);
	for (size_t i = 0; i < 2048; i++) {
		samples[i] = (float)sin(2.0 * M_PI * f_mid_half * ((double)i / fs));
	}
	assert(vs_fft_compute(&plan2048, samples, mag));
	assert(mag[42] > 0.80f && mag[43] > 0.80f);
	for (size_t k = 0; k <= 36; k++) {
		assert(mag[k] < 0.008f);
	}
	for (size_t k = 49; k < 1025; k++) {
		assert(mag[k] < 0.008f);
	}

	vs_fft_plan_free(&plan2048);
}

static void test_fft_parseval_and_silence(void)
{
	struct vs_fft_plan plan1024;
	assert(vs_fft_plan_init(&plan1024, 1024));

	/* 1. Parseval energy conservation on unwindowed complex FFT:
	 * sum |x[n]|^2 == (1/N) * sum |X[k]|^2 */
	float re[1024];
	float im[1024];
	double time_energy = 0.0;
	for (size_t i = 0; i < 1024; i++) {
		re[i] = (float)(cos(0.13 * (double)i) - 0.5 * sin(0.71 * (double)i));
		im[i] = 0.0f;
		time_energy += (double)re[i] * (double)re[i];
	}
	assert(vs_fft_execute_complex(&plan1024, re, im));
	double freq_energy = 0.0;
	for (size_t k = 0; k < 1024; k++) {
		freq_energy += ((double)re[k] * (double)re[k] + (double)im[k] * (double)im[k]);
	}
	freq_energy /= 1024.0;
	assert(fabs(time_energy - freq_energy) < 1e-2);

	/* 2. Unit impulse on unwindowed FFT yields flat magnitude 1.0 across all bins */
	memset(re, 0, sizeof(re));
	memset(im, 0, sizeof(im));
	re[0] = 1.0f;
	assert(vs_fft_execute_complex(&plan1024, re, im));
	for (size_t k = 0; k < 1024; k++) {
		float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
		assert(float_near(m, 1.0f, 1e-4f));
	}

	/* 3. DC signal on windowed FFT peaks at bin 0 */
	float dc[1024];
	float mag[513];
	for (size_t i = 0; i < 1024; i++) {
		dc[i] = 0.75f;
	}
	assert(vs_fft_compute(&plan1024, dc, mag));
	assert(float_near(mag[0], 0.75f, 1e-3f));
	for (size_t k = 2; k < 513; k++) {
		assert(mag[k] < 1e-5f);
	}

	/* 4. Silence and NaN inputs yield all-zero magnitudes */
	memset(dc, 0, sizeof(dc));
	dc[10] = NAN;
	dc[20] = INFINITY;
	assert(vs_fft_compute(&plan1024, dc, mag));
	for (size_t k = 0; k < 513; k++) {
		assert(isfinite(mag[k]) && mag[k] == 0.0f);
	}

	vs_fft_plan_free(&plan1024);
}

int main(void)
{
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

	test_fft_4096_subbass_resolution();
	test_fft_2048_mid_high();
	test_fft_parseval_and_silence();

	printf("test-fft: all 3 tests passed\n");
	return 0;
}
