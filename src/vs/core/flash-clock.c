/*
 * Visual Sidechain - Shared Process-Wide Photosensitivity Flash Clock
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "flash-clock.h"

#include <util/threading.h>

#include <math.h>
#include <string.h>

/* Bit-level finiteness check: safe even if a translation unit enables fast-math. */
static inline bool is_finite_f32(float v)
{
	uint32_t bits = 0;
	memcpy(&bits, &v, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;
}

static pthread_mutex_t g_flash_clock_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct vs_flash_clock g_process_flash_clock;
static uint64_t g_fallback_frame_seq = 0;

void vs_flash_clock_reset(struct vs_flash_clock *clock)
{
	if (clock)
		memset(clock, 0, sizeof(*clock));
}

void vs_flash_limiter_reset(struct vs_flash_limiter *lim)
{
	if (lim)
		memset(lim, 0, sizeof(*lim));
}

float vs_flash_trigger_over(float signal_01, float threshold_01)
{
	if (!is_finite_f32(signal_01) || signal_01 <= 0.0f)
		return 0.0f;
	float thr = is_finite_f32(threshold_01) ? threshold_01 : VS_FLASH_DEFAULT_THRESHOLD;
	if (thr < 0.05f)
		thr = 0.05f;
	if (thr > 0.95f)
		thr = 0.95f;
	if (signal_01 <= thr)
		return 0.0f;
	const float over = (signal_01 - thr) / fmaxf(1.0f - thr, 1e-4f);
	if (over < 0.0f)
		return 0.0f;
	if (over > 1.0f)
		return 1.0f;
	return over;
}

float vs_flash_clock_step(struct vs_flash_clock *clock, struct vs_flash_limiter *client, uint64_t frame_token,
			  float over_01, float max_rate_hz, float dt_sec)
{
	if (!client)
		return 0.0f;

	double rate = is_finite_f32(max_rate_hz) ? (double)max_rate_hz : VS_FLASH_RATE_DEFAULT_HZ;
	if (rate < VS_FLASH_RATE_MIN_HZ)
		rate = VS_FLASH_RATE_MIN_HZ;
	if (rate > VS_FLASH_RATE_MAX_HZ)
		rate = VS_FLASH_RATE_MAX_HZ;
	const double personal_interval = 1.0 / rate;
	const double global_interval = 1.0 / VS_FLASH_RATE_MAX_HZ;
	/* At most 0.15 s: at 3 Hz or less every flash is dark again well before the next may start. */
	const double fall = fmin(0.5 * personal_interval, VS_FLASH_FALL_MAX_SEC);

	const double dt = (is_finite_f32(dt_sec) && dt_sec > 0.0f) ? (double)dt_sec : 0.0;
	client->since_admit_sec += dt;

	if (clock) {
		if (frame_token == 0 || !clock->has_frame_token || clock->last_frame_token != frame_token) {
			clock->since_admit_sec += dt;
			if (frame_token != 0) {
				clock->last_frame_token = frame_token;
				clock->has_frame_token = true;
			}
		}
	}

	float over = isnan(over_01) ? 0.0f : over_01;
	if (over < 0.0f)
		over = 0.0f;
	if (over > 1.0f)
		over = 1.0f;

	/* The 1e-9 s slack absorbs rounding when the interval is a whole number of frames. */
	const bool personal_ready = (!client->has_admitted || client->since_admit_sec + 1e-9 >= personal_interval);
	const bool global_ready = (!clock || !clock->has_admitted || clock->since_admit_sec + 1e-9 >= global_interval);
	if (over > 0.0f && personal_ready && global_ready) {
		client->peak = over;
		client->since_admit_sec = 0.0;
		client->has_admitted = true;
		if (clock) {
			clock->since_admit_sec = 0.0;
			clock->has_admitted = true;
		}
	}

	/* Within a flash the envelope only falls: each admitted flash is exactly one rise and one fall. */
	if (!client->has_admitted || client->since_admit_sec >= fall)
		return 0.0f;
	return (float)((double)client->peak * (1.0 - client->since_admit_sec / fall));
}

void vs_flash_clock_global_reset(void)
{
	pthread_mutex_lock(&g_flash_clock_mutex);
	vs_flash_clock_reset(&g_process_flash_clock);
	g_fallback_frame_seq = 0;
	pthread_mutex_unlock(&g_flash_clock_mutex);
}

uint64_t vs_flash_clock_next_fallback_token(void)
{
	pthread_mutex_lock(&g_flash_clock_mutex);
	/* High bit distinguishes synthesized headless tokens from OBS video timestamps. */
	const uint64_t token = (++g_fallback_frame_seq) | (1ULL << 63);
	pthread_mutex_unlock(&g_flash_clock_mutex);
	return token;
}

float vs_flash_clock_global_step(struct vs_flash_limiter *client, uint64_t frame_token, float over_01,
				 float max_rate_hz, float dt_sec)
{
	pthread_mutex_lock(&g_flash_clock_mutex);
	const float env =
		vs_flash_clock_step(&g_process_flash_clock, client, frame_token, over_01, max_rate_hz, dt_sec);
	pthread_mutex_unlock(&g_flash_clock_mutex);
	return env;
}
