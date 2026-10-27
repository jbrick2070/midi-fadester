/*
 * Midi Fadester — test-smoother
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/smoother.h"
#include <math.h>
#include <stdio.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	struct mfd_smoother s60;
	struct mfd_smoother s120;

	/* 1. 30 ms exponential slew from 0.0 -> 1.0 is frame-rate independent (60 Hz vs 120 Hz) */
	mfd_smoother_init(&s60, 30.0f, 0.0f);
	mfd_smoother_init(&s120, 30.0f, 0.0f);
	mfd_smoother_set_target(&s60, 1.0f);
	mfd_smoother_set_target(&s120, 1.0f);

	REQUIRE(mfd_smoother_is_active(&s60));
	float v60 = mfd_smoother_tick(&s60, 1.0f / 60.0f);
	REQUIRE(v60 > 0.35f && v60 < 0.50f); /* 1 - exp(-16.67 / 30) = ~0.426 */

	mfd_smoother_tick(&s120, 1.0f / 120.0f);
	float v120 = mfd_smoother_tick(&s120, 1.0f / 120.0f);
	REQUIRE(fabsf(v60 - v120) < 1e-5f);

	/* 2. Snaps to exact 1.0 endpoint within ~250 ms */
	for (int i = 0; i < 20; ++i) {
		v60 = mfd_smoother_tick(&s60, 1.0f / 60.0f);
	}
	REQUIRE(v60 == 1.0f);
	REQUIRE(!mfd_smoother_is_active(&s60));

	/* 3. Snaps to exact 0.0 endpoint on downward fade */
	mfd_smoother_set_target(&s60, 0.0f);
	REQUIRE(mfd_smoother_is_active(&s60));
	for (int i = 0; i < 20; ++i) {
		v60 = mfd_smoother_tick(&s60, 1.0f / 60.0f);
	}
	REQUIRE(v60 == 0.0f);
	REQUIRE(!mfd_smoother_is_active(&s60));

	/* 4. Instant mode when tau_ms == 0 */
	mfd_smoother_set_tau_ms(&s60, 0.0f);
	mfd_smoother_set_target(&s60, 0.75f);
	REQUIRE(mfd_smoother_tick(&s60, 1.0f / 60.0f) == 0.75f);
	REQUIRE(!mfd_smoother_is_active(&s60));

	printf("test-smoother: PASS\n");
	return 0;
}
