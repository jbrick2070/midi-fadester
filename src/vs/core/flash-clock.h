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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Global flash-rate ceiling (3.0 Hz) and personal lower limit (0.5..3.0 Hz). Never configurable upward. */
#define VS_FLASH_RATE_MIN_HZ 0.5
#define VS_FLASH_RATE_MAX_HZ 3.0
#define VS_FLASH_RATE_DEFAULT_HZ 3.0
#define VS_FLASH_FALL_MAX_SEC 0.15
#define VS_FLASH_DEFAULT_THRESHOLD 0.30f

/* Per-client envelope state. When shared across a vs_flash_clock, personal max_rate_hz may lower the rate, while the
 * shared clock enforces <= 3 flashes/s across all clients combined in arrival order. */
struct vs_flash_limiter {
	double since_admit_sec;
	float peak;
	bool has_admitted;
};

/* Shared flash budget clock. frame_token deduplicates dt_sec across multiple clients stepping in the same video tick. */
struct vs_flash_clock {
	double since_admit_sec;
	uint64_t last_frame_token;
	bool has_frame_token;
	bool has_admitted;
};

void vs_flash_clock_reset(struct vs_flash_clock *clock);
void vs_flash_limiter_reset(struct vs_flash_limiter *lim);

/* Normalized amount [0..1] by which signal_01 exceeds threshold_01 (clamped to [0.05..0.95], default 0.30). */
float vs_flash_trigger_over(float signal_01, float threshold_01);

/* Steps one client against an explicit shared clock (or NULL for standalone single-client limiting). When frame_token
 * is non-zero, clock->since_admit_sec advances at most once per distinct frame_token. Returns envelope in [0..1]. */
float vs_flash_clock_step(struct vs_flash_clock *clock, struct vs_flash_limiter *client, uint64_t frame_token,
			  float over_01, float max_rate_hz, float dt_sec);

/* Process-wide shared flash clock used by all Visual Sidechain Shader Filters and Audio Modulator slots. */
void vs_flash_clock_global_reset(void);
uint64_t vs_flash_clock_next_fallback_token(void);
float vs_flash_clock_global_step(struct vs_flash_limiter *client, uint64_t frame_token, float over_01,
				 float max_rate_hz, float dt_sec);

#ifdef __cplusplus
}
#endif
