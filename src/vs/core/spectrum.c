/*
 * Visual Sidechain - Pure-C Dual-FFT Spectrum & Band Extractor
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "spectrum.h"

#include <math.h>
#include <string.h>

static float clamp01(float v)
{
	if (!isfinite(v) || v <= 0.0f)
		return 0.0f;
	if (v >= 1.0f)
		return 1.0f;
	return v;
}

static float sample_fft_interp(const float *mag, uint32_t fft_size, uint32_t sample_rate, float freq_hz)
{
	if (!mag || fft_size == 0 || sample_rate == 0 || freq_hz <= 0.0f)
		return 0.0f;

	const uint32_t max_bin = fft_size / 2u;
	const float bin_pos = (freq_hz * (float)fft_size) / (float)sample_rate;
	if (bin_pos <= 0.0f)
		return mag[0];
	if (bin_pos >= (float)max_bin)
		return mag[max_bin];

	const uint32_t k0 = (uint32_t)bin_pos;
	const uint32_t k1 = (k0 < max_bin) ? (k0 + 1u) : max_bin;
	const float alpha = bin_pos - (float)k0;
	return (1.0f - alpha) * mag[k0] + alpha * mag[k1];
}

static float extract_band_from_fft(const float *mag, uint32_t fft_size, uint32_t sample_rate, float freq_low_hz,
				   float freq_high_hz, bool peak_mode)
{
	if (!mag || fft_size == 0 || sample_rate == 0 || freq_high_hz <= freq_low_hz)
		return 0.0f;

	const uint32_t max_bin = fft_size / 2u;
	float b_low = (freq_low_hz * (float)fft_size) / (float)sample_rate;
	float b_high = (freq_high_hz * (float)fft_size) / (float)sample_rate;

	if (b_low < 0.0f)
		b_low = 0.0f;
	if (b_high > (float)max_bin)
		b_high = (float)max_bin;
	if (b_high <= b_low)
		return sample_fft_interp(mag, fft_size, sample_rate, 0.5f * (freq_low_hz + freq_high_hz));

	const float span = b_high - b_low;

	if (!peak_mode && span <= 1.5f) {
		/* Fractional bin sub-sampling across narrow bands (e.g. low spectrum bands on 4096-pt FFT). */
		const int steps = 5;
		float sum = 0.0f;
		for (int i = 0; i < steps; i++) {
			const float t = (float)i / (float)(steps - 1);
			const float f = freq_low_hz + t * (freq_high_hz - freq_low_hz);
			sum += sample_fft_interp(mag, fft_size, sample_rate, f);
		}
		return clamp01(sum / (float)steps);
	}

	/*
	 * For wider bands or explicit peak mode, find the dominant spectral peak inside
	 * [b_low, b_high] and reconstruct its Hann main-lobe energy so tones landing
	 * between integer bins still read full amplitude without out-of-band leakage.
	 */
	float best = sample_fft_interp(mag, fft_size, sample_rate, freq_low_hz);
	const float mid_val = sample_fft_interp(mag, fft_size, sample_rate, 0.5f * (freq_low_hz + freq_high_hz));
	const float high_val = sample_fft_interp(mag, fft_size, sample_rate, freq_high_hz);
	if (mid_val > best)
		best = mid_val;
	if (high_val > best)
		best = high_val;

	uint32_t k_start = (uint32_t)ceilf(b_low);
	uint32_t k_end = (uint32_t)floorf(b_high);
	if (k_end > max_bin)
		k_end = max_bin;

	uint32_t peak_k = k_start;
	float peak_bin_val = 0.0f;
	if (k_start <= k_end) {
		for (uint32_t k = k_start; k <= k_end; k++) {
			if (mag[k] > peak_bin_val) {
				peak_bin_val = mag[k];
				peak_k = k;
			}
			if (mag[k] > best)
				best = mag[k];
		}

		if (peak_bin_val > 0.0f) {
			float e2 = peak_bin_val * peak_bin_val;
			if (peak_k > 0u && (float)(peak_k - 1u) >= b_low - 0.5f)
				e2 += mag[peak_k - 1u] * mag[peak_k - 1u];
			if (peak_k < max_bin && (float)(peak_k + 1u) <= b_high + 0.5f)
				e2 += mag[peak_k + 1u] * mag[peak_k + 1u];
			const float reconstructed = sqrtf(e2 / 1.5f);
			if (reconstructed > best)
				best = reconstructed;
		}
	}

	return clamp01(best);
}

bool vs_spectrum_init(struct vs_spectrum *spec, uint32_t sample_rate, uint32_t fft_size_low, uint32_t fft_size_mid)
{
	if (!spec || sample_rate == 0)
		return false;
	if (fft_size_low == 0 || fft_size_low / 2u + 1u > VS_MAX_BINS_LOW)
		return false;
	if (fft_size_mid == 0 || fft_size_mid / 2u + 1u > VS_MAX_BINS_MID)
		return false;

	memset(spec, 0, sizeof(*spec));
	spec->sample_rate = sample_rate;
	spec->fft_size_low = fft_size_low;
	spec->fft_size_mid = fft_size_mid;

	const float ratio = VS_FREQ_MAX_HZ / VS_FREQ_MIN_HZ; /* 1000.0f */
	for (uint32_t b = 0; b <= VS_SPECTRUM_BANDS; b++) {
		const float exp_t = (float)b / (float)VS_SPECTRUM_BANDS;
		spec->band_edges_hz[b] = VS_FREQ_MIN_HZ * powf(ratio, exp_t);
	}

	for (uint32_t i = 0; i < VS_FFT_TEX_BINS; i++) {
		const float exp_t = (float)i / (float)(VS_FFT_TEX_BINS - 1u);
		spec->fft_tex_freq_hz[i] = VS_FREQ_MIN_HZ * powf(ratio, exp_t);
	}

	return true;
}

void vs_spectrum_reset(struct vs_spectrum *spec)
{
	if (!spec)
		return;
	memset(spec->prev_mag_low, 0, sizeof(spec->prev_mag_low));
	memset(spec->prev_mag_mid, 0, sizeof(spec->prev_mag_mid));
	spec->has_prev = false;
}

float vs_spectrum_extract_hz_band(const struct vs_spectrum *spec, const float *mag_low, const float *mag_mid,
				  float freq_low_hz, float freq_high_hz, enum vs_hz_band_mode mode)
{
	if (!spec || !mag_low || !mag_mid)
		return 0.0f;

	if (freq_low_hz < 1.0f)
		freq_low_hz = 1.0f;
	if (freq_high_hz <= freq_low_hz)
		freq_high_hz = freq_low_hz + 1.0f;

	const float center_hz = 0.5f * (freq_low_hz + freq_high_hz);
	const bool use_low = (center_hz <= VS_DUAL_FFT_CROSSOVER_HZ);
	const float *mag = use_low ? mag_low : mag_mid;
	const float *prev_mag = use_low ? spec->prev_mag_low : spec->prev_mag_mid;
	const uint32_t fft_size = use_low ? spec->fft_size_low : spec->fft_size_mid;

	if (mode == VS_HZ_MODE_FLUX) {
		if (!spec->has_prev)
			return 0.0f;
		const uint32_t max_bin = fft_size / 2u;
		uint32_t k0 = (uint32_t)floorf((freq_low_hz * (float)fft_size) / (float)spec->sample_rate);
		uint32_t k1 = (uint32_t)ceilf((freq_high_hz * (float)fft_size) / (float)spec->sample_rate);
		if (k0 < 1u)
			k0 = 1u;
		if (k1 > max_bin)
			k1 = max_bin;
		if (k1 < k0)
			k1 = k0;

		float flux_sum = 0.0f;
		for (uint32_t k = k0; k <= k1; k++) {
			const float diff = mag[k] - prev_mag[k];
			if (diff > 0.0f)
				flux_sum += diff;
		}
		return clamp01(flux_sum * 0.5f);
	}

	if (mode == VS_HZ_MODE_AVERAGE) {
		const float span_bins = ((freq_high_hz - freq_low_hz) * (float)fft_size) / (float)spec->sample_rate;
		if (span_bins <= 3.0f) {
			/* Narrow custom window: integrate Hann energy across the window. */
			return extract_band_from_fft(mag, fft_size, spec->sample_rate, freq_low_hz, freq_high_hz, true);
		}
		const int steps = 9;
		float sum_sq = 0.0f;
		for (int i = 0; i < steps; i++) {
			const float t = (float)i / (float)(steps - 1);
			const float f = freq_low_hz + t * (freq_high_hz - freq_low_hz);
			const float v = sample_fft_interp(mag, fft_size, spec->sample_rate, f);
			sum_sq += v * v;
		}
		return clamp01(sqrtf(sum_sq / (float)steps) * 1.41421356f);
	}

	return extract_band_from_fft(mag, fft_size, spec->sample_rate, freq_low_hz, freq_high_hz, true);
}

void vs_spectrum_compute(struct vs_spectrum *spec, const float *time_window_mid, const float *mag_low,
			 const float *mag_mid, struct vs_spectrum_frame *out_frame)
{
	if (!spec || !mag_low || !mag_mid || !out_frame)
		return;

	memset(out_frame, 0, sizeof(*out_frame));

	/* 1. Time-domain RMS level, raw peak, raw magnitude, and 512-sample waveform row. */
	if (time_window_mid && spec->fft_size_mid > 0) {
		const uint32_t n = spec->fft_size_mid;
		float sum_sq = 0.0f;
		float sum_abs = 0.0f;
		float peak = 0.0f;

		for (uint32_t i = 0; i < n; i++) {
			const float s = isfinite(time_window_mid[i]) ? time_window_mid[i] : 0.0f;
			const float a = fabsf(s);
			sum_sq += s * s;
			sum_abs += a;
			if (a > peak)
				peak = a;
		}

		const float rms = sqrtf(sum_sq / (float)n);
		out_frame->peak_raw = peak;
		out_frame->magnitude_raw = sum_abs / (float)n;
		out_frame->level = clamp01(rms * 1.41421356f);

		for (uint32_t i = 0; i < VS_FFT_TEX_BINS; i++) {
			const uint32_t idx = (i * n) / VS_FFT_TEX_BINS;
			const float s = isfinite(time_window_mid[idx]) ? time_window_mid[idx] : 0.0f;
			out_frame->waveform[i] = clamp01(0.5f + 0.5f * s);
		}
	} else {
		for (uint32_t i = 0; i < VS_FFT_TEX_BINS; i++)
			out_frame->waveform[i] = 0.5f;
	}

	/* 2. 32 log-spaced spectrum bands (4096-pt FFT for <= 250 Hz, 2048-pt FFT for > 250 Hz). */
	for (uint32_t b = 0; b < VS_SPECTRUM_BANDS; b++) {
		const float f0 = spec->band_edges_hz[b];
		const float f1 = spec->band_edges_hz[b + 1u];
		const float f_center = sqrtf(f0 * f1);
		if (f_center <= VS_DUAL_FFT_CROSSOVER_HZ) {
			out_frame->spectrum[b] =
				extract_band_from_fft(mag_low, spec->fft_size_low, spec->sample_rate, f0, f1, false);
		} else {
			out_frame->spectrum[b] =
				extract_band_from_fft(mag_mid, spec->fft_size_mid, spec->sample_rate, f0, f1, false);
		}
	}

	/* 3. 512 log-frequency bins for GPU texture Row 0 (audio_fft). */
	for (uint32_t i = 0; i < VS_FFT_TEX_BINS; i++) {
		const float f = spec->fft_tex_freq_hz[i];
		if (f <= VS_DUAL_FFT_CROSSOVER_HZ) {
			out_frame->fft_bins[i] =
				clamp01(sample_fft_interp(mag_low, spec->fft_size_low, spec->sample_rate, f));
		} else {
			out_frame->fft_bins[i] =
				clamp01(sample_fft_interp(mag_mid, spec->fft_size_mid, spec->sample_rate, f));
		}
	}

	/* 4. Summary bands: sub_bass & bass from 4096-pt FFT; mid & high from 2048-pt FFT. */
	out_frame->sub_bass = extract_band_from_fft(mag_low, spec->fft_size_low, spec->sample_rate, 20.0f, 60.0f, true);
	out_frame->bass = extract_band_from_fft(mag_low, spec->fft_size_low, spec->sample_rate, 20.0f, 250.0f, true);
	out_frame->mid = extract_band_from_fft(mag_mid, spec->fft_size_mid, spec->sample_rate, 250.0f, 4000.0f, true);
	out_frame->high =
		extract_band_from_fft(mag_mid, spec->fft_size_mid, spec->sample_rate, 4000.0f, 20000.0f, true);

	/* 5. Spectral centroid and wideband positive spectral flux. */
	const uint32_t max_mid_bin = spec->fft_size_mid / 2u;
	const float bin_hz_mid = (float)spec->sample_rate / (float)spec->fft_size_mid;
	float weighted_freq_sum = 0.0f;
	float mag_sum = 0.0f;
	float flux_sum = 0.0f;

	for (uint32_t k = 1u; k <= max_mid_bin; k++) {
		const float m = mag_mid[k];
		const float f = (float)k * bin_hz_mid;
		if (f >= VS_FREQ_MIN_HZ && f <= VS_FREQ_MAX_HZ) {
			weighted_freq_sum += f * m;
			mag_sum += m;
		}
		if (spec->has_prev) {
			const float diff = m - spec->prev_mag_mid[k];
			if (diff > 0.0f)
				flux_sum += diff;
		}
	}

	if (mag_sum > 1e-4f) {
		const float centroid_hz = weighted_freq_sum / mag_sum;
		const float norm = log10f(centroid_hz / VS_FREQ_MIN_HZ) / 3.0f; /* log10(20000/20) = 3.0 */
		out_frame->spectral_centroid = clamp01(norm);
	} else {
		out_frame->spectral_centroid = 0.0f;
	}

	out_frame->spectral_flux = spec->has_prev ? clamp01(flux_sum * 0.5f) : 0.0f;

	/* Save magnitudes for next frame's spectral flux. */
	memcpy(spec->prev_mag_low, mag_low, (spec->fft_size_low / 2u + 1u) * sizeof(float));
	memcpy(spec->prev_mag_mid, mag_mid, (spec->fft_size_mid / 2u + 1u) * sizeof(float));
	spec->has_prev = true;
}
