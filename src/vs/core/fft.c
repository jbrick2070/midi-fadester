/*
visual-sidechain
Copyright (C) 2026 Jeffrey A. Brick
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/fft.h"

#include <math.h>
#include <string.h>
#include <util/bmem.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool is_power_of_two(size_t n)
{
	return n >= VS_FFT_MIN_SIZE && n <= VS_FFT_MAX_SIZE && ((n & (n - 1u)) == 0u);
}

bool vs_fft_plan_init(struct vs_fft_plan *plan, size_t size)
{
	if (!plan || !is_power_of_two(size))
		return false;

	memset(plan, 0, sizeof(*plan));
	plan->size = size;
	plan->bins = (size / 2u) + 1u;

	size_t half = size / 2u;
	plan->window = bzalloc(size * sizeof(float));
	plan->bitrev = bzalloc(size * sizeof(uint32_t));
	plan->twiddle_cos = bzalloc(half * sizeof(float));
	plan->twiddle_sin = bzalloc(half * sizeof(float));
	plan->work_re = bzalloc(size * sizeof(float));
	plan->work_im = bzalloc(size * sizeof(float));

	unsigned int log2n = 0;
	for (size_t tmp = size; tmp > 1u; tmp >>= 1u) {
		log2n++;
	}

	/* Periodic Hann window: w[n] = 0.5 * (1 - cos(2*pi*n / N)) */
	const double two_pi_over_n = (2.0 * M_PI) / (double)size;
	for (size_t i = 0; i < size; i++) {
		plan->window[i] = (float)(0.5 * (1.0 - cos(two_pi_over_n * (double)i)));

		uint32_t x = (uint32_t)i;
		uint32_t rev = 0;
		for (unsigned int b = 0; b < log2n; b++) {
			rev = (rev << 1u) | (x & 1u);
			x >>= 1u;
		}
		plan->bitrev[i] = rev;
	}

	for (size_t k = 0; k < half; k++) {
		double angle = -two_pi_over_n * (double)k;
		plan->twiddle_cos[k] = (float)cos(angle);
		plan->twiddle_sin[k] = (float)sin(angle);
	}

	return true;
}

void vs_fft_plan_free(struct vs_fft_plan *plan)
{
	if (!plan)
		return;

	bfree(plan->window);
	bfree(plan->bitrev);
	bfree(plan->twiddle_cos);
	bfree(plan->twiddle_sin);
	bfree(plan->work_re);
	bfree(plan->work_im);
	memset(plan, 0, sizeof(*plan));
}

bool vs_fft_execute_complex(const struct vs_fft_plan *plan, float *re, float *im)
{
	if (!plan || !plan->bitrev || !re || !im || plan->size == 0)
		return false;

	const size_t n = plan->size;
	for (size_t i = 0; i < n; i++) {
		uint32_t j = plan->bitrev[i];
		if (j > i) {
			float tr = re[i];
			float ti = im[i];
			re[i] = re[j];
			im[i] = im[j];
			re[j] = tr;
			im[j] = ti;
		}
	}

	for (size_t len = 2u; len <= n; len <<= 1u) {
		size_t half_len = len >> 1u;
		size_t step = n / len;
		for (size_t i = 0; i < n; i += len) {
			size_t tw_idx = 0;
			for (size_t j = 0; j < half_len; j++) {
				float wr = plan->twiddle_cos[tw_idx];
				float wi = plan->twiddle_sin[tw_idx];
				tw_idx += step;

				size_t u = i + j;
				size_t v = u + half_len;
				float tr = wr * re[v] - wi * im[v];
				float ti = wr * im[v] + wi * re[v];

				re[v] = re[u] - tr;
				im[v] = im[u] - ti;
				re[u] += tr;
				im[u] += ti;
			}
		}
	}

	return true;
}

bool vs_fft_compute(struct vs_fft_plan *plan, const float *in_samples, float *out_mag)
{
	if (!plan || !plan->window || !in_samples || !out_mag)
		return false;

	const size_t n = plan->size;
	for (size_t i = 0; i < n; i++) {
		float s = isfinite(in_samples[i]) ? in_samples[i] : 0.0f;
		plan->work_re[i] = s * plan->window[i];
		plan->work_im[i] = 0.0f;
	}

	if (!vs_fft_execute_complex(plan, plan->work_re, plan->work_im))
		return false;

	/* Coherent gain of periodic Hann window is 0.5; single-sided AC bin peak
	 * for a unit-amplitude sine is N * 0.5 * 0.5 = N / 4, so scale by 4.0 / N
	 * for AC bins and 2.0 / N for DC and Nyquist. */
	const float ac_scale = 4.0f / (float)n;
	const float dc_scale = 2.0f / (float)n;
	const size_t bins = plan->bins;

	for (size_t k = 0; k < bins; k++) {
		float re = plan->work_re[k];
		float im = plan->work_im[k];
		float mag = sqrtf(re * re + im * im);
		float scale = (k == 0 || k == bins - 1u) ? dc_scale : ac_scale;
		float norm = mag * scale;
		out_mag[k] = isfinite(norm) ? norm : 0.0f;
	}

	return true;
}
