/*
 * Visual Sidechain - Dual-FFT Spectrum Unit Tests
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

#include "core/fft.h"
#include "core/spectrum.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define VS_TWO_PI 6.28318530717958647692f
#define SAMPLE_RATE 48000u

static void generate_sine_windows(float freq_hz, float amp, float *win_4096, float *win_2048)
{
	for (uint32_t i = 0; i < VS_FFT_LOW_SIZE; i++) {
		const float t = (float)i / (float)SAMPLE_RATE;
		win_4096[i] = amp * sinf(VS_TWO_PI * freq_hz * t);
	}
	/* 2048-sample window is the most recent half of the 4096-sample window. */
	for (uint32_t i = 0; i < VS_FFT_MID_SIZE; i++) {
		win_2048[i] = win_4096[VS_FFT_LOW_SIZE - VS_FFT_MID_SIZE + i];
	}
}

static void test_spectrum_subbass_vs_bass_4096(void)
{
	struct vs_fft_plan fft_low;
	struct vs_fft_plan fft_mid;
	struct vs_spectrum spec;
	assert(vs_fft_plan_init(&fft_low, VS_FFT_LOW_SIZE));
	assert(vs_fft_plan_init(&fft_mid, VS_FFT_MID_SIZE));
	assert(vs_spectrum_init(&spec, SAMPLE_RATE, VS_FFT_LOW_SIZE, VS_FFT_MID_SIZE));

	float *win_4096 = (float *)calloc(VS_FFT_LOW_SIZE, sizeof(float));
	float *win_2048 = (float *)calloc(VS_FFT_MID_SIZE, sizeof(float));
	float *mag_low = (float *)calloc(fft_low.bins, sizeof(float));
	float *mag_mid = (float *)calloc(fft_mid.bins, sizeof(float));
	struct vs_spectrum_frame frame;
	assert(win_4096 && win_2048 && mag_low && mag_mid);

	/* 1. 38 Hz sub-bass sine: excites sub_bass (> 0.7) and bands 1..2 while bands 5..10 stay < 0.05. */
	generate_sine_windows(38.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	vs_spectrum_compute(&spec, win_2048, mag_low, mag_mid, &frame);

	assert(frame.sub_bass > 0.7f);
	assert(frame.spectrum[1] > 0.45f);
	assert(frame.spectrum[2] > 0.7f);
	for (uint32_t b = 5; b <= 10; b++) {
		assert(frame.spectrum[b] < 0.05f);
	}
	assert(frame.mid < 0.02f);
	assert(frame.high < 0.02f);

	/* 2. 110 Hz bass sine: excites bass (> 0.7) and bands 6..8 while sub_bass stays < 0.05. */
	generate_sine_windows(110.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	vs_spectrum_compute(&spec, win_2048, mag_low, mag_mid, &frame);

	assert(frame.bass > 0.7f);
	assert(frame.sub_bass < 0.05f);
	const float band_6_8_max = fmaxf(frame.spectrum[6], fmaxf(frame.spectrum[7], frame.spectrum[8]));
	assert(band_6_8_max > 0.7f);
	assert(frame.spectrum[1] < 0.02f);
	assert(frame.spectrum[2] < 0.02f);

	free(win_4096);
	free(win_2048);
	free(mag_low);
	free(mag_mid);
	vs_fft_plan_free(&fft_low);
	vs_fft_plan_free(&fft_mid);
}

static void test_spectrum_mid_high_isolation(void)
{
	struct vs_fft_plan fft_low;
	struct vs_fft_plan fft_mid;
	struct vs_spectrum spec;
	assert(vs_fft_plan_init(&fft_low, VS_FFT_LOW_SIZE));
	assert(vs_fft_plan_init(&fft_mid, VS_FFT_MID_SIZE));
	assert(vs_spectrum_init(&spec, SAMPLE_RATE, VS_FFT_LOW_SIZE, VS_FFT_MID_SIZE));

	float *win_4096 = (float *)calloc(VS_FFT_LOW_SIZE, sizeof(float));
	float *win_2048 = (float *)calloc(VS_FFT_MID_SIZE, sizeof(float));
	float *mag_low = (float *)calloc(fft_low.bins, sizeof(float));
	float *mag_mid = (float *)calloc(fft_mid.bins, sizeof(float));
	struct vs_spectrum_frame frame;
	assert(win_4096 && win_2048 && mag_low && mag_mid);

	/* 1. 1 kHz sine excites mid and bands 14..18 while bass and high stay < 0.02. */
	generate_sine_windows(1000.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	vs_spectrum_compute(&spec, win_2048, mag_low, mag_mid, &frame);

	assert(frame.mid > 0.7f);
	assert(frame.bass < 0.02f);
	assert(frame.high < 0.02f);
	float mid_bands_peak = 0.0f;
	for (uint32_t b = 14; b <= 18; b++) {
		if (frame.spectrum[b] > mid_bands_peak)
			mid_bands_peak = frame.spectrum[b];
	}
	assert(mid_bands_peak > 0.7f);
	const float centroid_1k = frame.spectral_centroid;
	assert(centroid_1k > 0.45f && centroid_1k < 0.65f);

	/* 2. 8 kHz sine excites high and bands 25..29 while bass and mid stay < 0.02. */
	generate_sine_windows(8000.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	vs_spectrum_compute(&spec, win_2048, mag_low, mag_mid, &frame);

	assert(frame.high > 0.7f);
	assert(frame.bass < 0.02f);
	assert(frame.mid < 0.02f);
	float high_bands_peak = 0.0f;
	for (uint32_t b = 25; b <= 29; b++) {
		if (frame.spectrum[b] > high_bands_peak)
			high_bands_peak = frame.spectrum[b];
	}
	assert(high_bands_peak > 0.7f);
	assert(frame.spectral_centroid > centroid_1k + 0.2f);
	assert(frame.spectral_flux > 0.5f);

	free(win_4096);
	free(win_2048);
	free(mag_low);
	free(mag_mid);
	vs_fft_plan_free(&fft_low);
	vs_fft_plan_free(&fft_mid);
}

static void test_spectrum_custom_hz_window(void)
{
	struct vs_fft_plan fft_low;
	struct vs_fft_plan fft_mid;
	struct vs_spectrum spec;
	assert(vs_fft_plan_init(&fft_low, VS_FFT_LOW_SIZE));
	assert(vs_fft_plan_init(&fft_mid, VS_FFT_MID_SIZE));
	assert(vs_spectrum_init(&spec, SAMPLE_RATE, VS_FFT_LOW_SIZE, VS_FFT_MID_SIZE));

	float *win_4096 = (float *)calloc(VS_FFT_LOW_SIZE, sizeof(float));
	float *win_2048 = (float *)calloc(VS_FFT_MID_SIZE, sizeof(float));
	float *mag_low = (float *)calloc(fft_low.bins, sizeof(float));
	float *mag_mid = (float *)calloc(fft_mid.bins, sizeof(float));
	struct vs_spectrum_frame frame;
	assert(win_4096 && win_2048 && mag_low && mag_mid);

	/* Prime with silence so flux mode has a zero previous frame. */
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	vs_spectrum_compute(&spec, win_2048, mag_low, mag_mid, &frame);

	/* 440 Hz sine inside [420, 460] Hz -> > 0.8 in Average, Peak, and Flux modes. */
	generate_sine_windows(440.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));

	const float avg_440 = vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_AVERAGE);
	const float peak_440 = vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_PEAK);
	const float flux_440 = vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_FLUX);
	assert(avg_440 > 0.8f);
	assert(peak_440 > 0.8f);
	assert(flux_440 > 0.8f);

	/* 220 Hz sine -> rejected (< 0.05) by [420, 460] Hz window. */
	generate_sine_windows(220.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	assert(vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_AVERAGE) < 0.05f);
	assert(vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_PEAK) < 0.05f);

	/* 880 Hz sine -> rejected (< 0.05) by [420, 460] Hz window. */
	generate_sine_windows(880.0f, 1.0f, win_4096, win_2048);
	assert(vs_fft_compute(&fft_low, win_4096, mag_low));
	assert(vs_fft_compute(&fft_mid, win_2048, mag_mid));
	assert(vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_AVERAGE) < 0.05f);
	assert(vs_spectrum_extract_hz_band(&spec, mag_low, mag_mid, 420.0f, 460.0f, VS_HZ_MODE_PEAK) < 0.05f);

	free(win_4096);
	free(win_2048);
	free(mag_low);
	free(mag_mid);
	vs_fft_plan_free(&fft_low);
	vs_fft_plan_free(&fft_mid);
}

int main(void)
{
	test_spectrum_subbass_vs_bass_4096();
	test_spectrum_mid_high_isolation();
	test_spectrum_custom_hz_window();
	printf("test-spectrum: all assertions passed\n");
	return 0;
}
