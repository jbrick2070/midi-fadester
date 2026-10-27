/*
 * Visual Sidechain - Modulation Slot Math & Rate/Epsilon Unit Tests
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "core/modulation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool double_near(double a, double b, double eps)
{
	return fabs(a - b) <= eps;
}

static void test_modulation_math_and_epsilon(void)
{
	struct vs_mod_slot_config cfg;
	struct vs_mod_slot_state state;
	struct vs_mod_eval_result res;
	vs_mod_slot_default_config(&cfg);
	vs_mod_slot_reset_state(&state);

	cfg.enabled = true;
	strncpy(cfg.target_filter, "NVIDIA Blur", sizeof(cfg.target_filter) - 1u);
	strncpy(cfg.target_setting, "intensity", sizeof(cfg.target_setting) - 1u);
	cfg.prop_type = VS_PROP_FLOAT;
	cfg.min_value = 0.20;
	cfg.max_value = 0.90;
	cfg.amount = 1.0;

	/* 1. Positive amount (+1.0): signal 0.0 -> min (0.20), signal 1.0 -> max (0.90). */
	vs_mod_slot_eval(&cfg, &state, 0.0f, &res);
	assert(res.should_write);
	assert(double_near(res.float_val, 0.20, 1e-6));

	/* Epsilon gate: repeating the exact same signal (0.0f) returns should_write == false. */
	vs_mod_slot_eval(&cfg, &state, 0.0f, &res);
	assert(!res.should_write);

	vs_mod_slot_eval(&cfg, &state, 1.0f, &res);
	assert(res.should_write);
	assert(double_near(res.float_val, 0.90, 1e-6));

	/* 2. Negative amount (-1.0, ducking): signal 0.0 -> max (0.90), signal 1.0 -> min (0.20). */
	cfg.amount = -1.0;
	vs_mod_slot_reset_state(&state);
	vs_mod_slot_eval(&cfg, &state, 0.0f, &res);
	assert(res.should_write);
	assert(double_near(res.float_val, 0.90, 1e-6));

	vs_mod_slot_eval(&cfg, &state, 1.0f, &res);
	assert(res.should_write);
	assert(double_near(res.float_val, 0.20, 1e-6));

	/* 3. Integer step rounding (e.g. step = 5 across [0, 100]). */
	cfg.prop_type = VS_PROP_INT;
	cfg.amount = 1.0;
	cfg.min_value = 0.0;
	cfg.max_value = 100.0;
	cfg.int_step = 5;
	vs_mod_slot_reset_state(&state);

	vs_mod_slot_eval(&cfg, &state, 0.13f, &res); /* 13.0 -> rounds to 15 */
	assert(res.should_write);
	assert(res.int_val == 15);

	vs_mod_slot_eval(&cfg, &state, 0.14f, &res); /* 14.0 -> still 15 -> gated */
	assert(!res.should_write);

	/* 4. Rate divider (div = 4): only ticks 0, 4, 8 write. */
	cfg.prop_type = VS_PROP_FLOAT;
	cfg.min_value = 0.0;
	cfg.max_value = 1.0;
	cfg.rate_divider = 4u;
	vs_mod_slot_reset_state(&state);

	for (int tick = 0; tick < 8; tick++) {
		const float sig = 0.1f * (float)(tick + 1);
		vs_mod_slot_eval(&cfg, &state, sig, &res);
		if (tick % 4 == 0) {
			assert(res.should_write);
		} else {
			assert(!res.should_write);
		}
	}
}

int main(void)
{
	test_modulation_math_and_epsilon();
	printf("test-modulation: all assertions passed\n");
	return 0;
}
