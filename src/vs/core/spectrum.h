/*
 * Visual Sidechain - Pure-C Dual-FFT Spectrum & Band Extractor
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_SPECTRUM_BANDS 32u
#define VS_FFT_TEX_BINS 512u
#define VS_FREQ_MIN_HZ 20.0f
#define VS_FREQ_MAX_HZ 20000.0f
#define VS_DUAL_FFT_CROSSOVER_HZ 250.0f
#define VS_MAX_BINS_LOW 2049u
#define VS_MAX_BINS_MID 1025u

enum vs_hz_band_mode {
	VS_HZ_MODE_AVERAGE = 0,
	VS_HZ_MODE_PEAK = 1,
	VS_HZ_MODE_FLUX = 2,
};

struct vs_spectrum_frame {
	float level;
	float sub_bass;
	float bass;
	float mid;
	float high;
	float spectral_centroid;
	float spectral_flux;
	float spectrum[VS_SPECTRUM_BANDS];
	float fft_bins[VS_FFT_TEX_BINS];
	float waveform[VS_FFT_TEX_BINS];
	float peak_raw;
	float magnitude_raw;
};

struct vs_spectrum {
	uint32_t sample_rate;
	uint32_t fft_size_low;
	uint32_t fft_size_mid;
	float band_edges_hz[VS_SPECTRUM_BANDS + 1u];
	float fft_tex_freq_hz[VS_FFT_TEX_BINS];
	float prev_mag_low[VS_MAX_BINS_LOW];
	float prev_mag_mid[VS_MAX_BINS_MID];
	bool has_prev;
};

bool vs_spectrum_init(struct vs_spectrum *spec, uint32_t sample_rate, uint32_t fft_size_low, uint32_t fft_size_mid);
void vs_spectrum_reset(struct vs_spectrum *spec);

/*
 * Merges the 4096-point low-band FFT (<= 250 Hz) and 2048-point mid/high FFT (> 250 Hz)
 * plus the 2048-sample mono time-domain window into out_frame.
 */
void vs_spectrum_compute(struct vs_spectrum *spec, const float *time_window_mid, const float *mag_low,
			 const float *mag_mid, struct vs_spectrum_frame *out_frame);

/*
 * Extracts energy in an arbitrary [freq_low_hz, freq_high_hz] window using the
 * dual-FFT magnitudes (4096-pt for <= 250 Hz, 2048-pt for > 250 Hz).
 */
float vs_spectrum_extract_hz_band(const struct vs_spectrum *spec, const float *mag_low, const float *mag_mid,
				  float freq_low_hz, float freq_high_hz, enum vs_hz_band_mode mode);

#ifdef __cplusplus
}
#endif
