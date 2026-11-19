/*
visual-sidechain
Copyright (C) 2026 Jeffrey A. Brick
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_FFT_MIN_SIZE 64u
#define VS_FFT_MAX_SIZE 4096u
#define VS_FFT_LOW_SIZE 4096u
#define VS_FFT_MID_SIZE 2048u

struct vs_fft_plan {
	size_t size;
	size_t bins; /* size / 2 + 1 */
	float *window;
	uint32_t *bitrev;
	float *twiddle_cos;
	float *twiddle_sin;
	float *work_re;
	float *work_im;
};

/* Initializes precomputed Hann window, bit-reversal table, and twiddle factors
 * for a power-of-two FFT size in [VS_FFT_MIN_SIZE .. VS_FFT_MAX_SIZE] (R-ANA-4). */
bool vs_fft_plan_init(struct vs_fft_plan *plan, size_t size);
void vs_fft_plan_free(struct vs_fft_plan *plan);

/* In-place unwindowed radix-2 complex FFT on `re[0..size-1]` and `im[0..size-1]`. */
bool vs_fft_execute_complex(const struct vs_fft_plan *plan, float *re, float *im);

/* Applies the periodic Hann window to `in_samples[0..size-1]`, runs the FFT,
 * and writes normalized single-sided magnitudes (`0 .. size/2`) into `out_mag`.
 * Normalized by coherent Hann gain (2.0 / size for AC bins, 1.0 / size for DC/Nyquist
 * adjusted for Hann coherent gain 0.5) so a unit-amplitude sine at bin center yields
 * magnitude 1.0f. Non-finite inputs are sanitized to 0.0f. */
bool vs_fft_compute(struct vs_fft_plan *plan, const float *in_samples, float *out_mag);

#ifdef __cplusplus
}
#endif
