/*
 * Visual Sidechain - Modulation Slot Math & Rate/Epsilon Unit Tests
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

#include "core/modulation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

static void test_modulation_per_target_filter_batching(void)
{
	struct vs_mod_slot_config cfgs[VS_MOD_MAX_SLOTS];
	struct vs_mod_slot_state states[VS_MOD_MAX_SLOTS];
	float signals[VS_MOD_MAX_SLOTS];
	struct vs_mod_target_batch batches[VS_MOD_MAX_SLOTS];

	for (size_t i = 0; i < VS_MOD_MAX_SLOTS; i++) {
		vs_mod_slot_default_config(&cfgs[i]);
		vs_mod_slot_reset_state(&states[i]);
		signals[i] = 0.5f;
	}

	/* 3 active slots targeting "NVIDIA Background Removal", 1 targeting "Color Correction", 1 disabled */
	cfgs[0].enabled = true;
	snprintf(cfgs[0].target_filter, sizeof(cfgs[0].target_filter), "NVIDIA Background Removal");
	snprintf(cfgs[0].target_setting, sizeof(cfgs[0].target_setting), "threshold");
	cfgs[0].min_value = 0.0;
	cfgs[0].max_value = 1.0;

	cfgs[1].enabled = true;
	snprintf(cfgs[1].target_filter, sizeof(cfgs[1].target_filter), "Color Correction");
	snprintf(cfgs[1].target_setting, sizeof(cfgs[1].target_setting), "saturation");
	cfgs[1].min_value = 0.0;
	cfgs[1].max_value = 2.0;

	cfgs[2].enabled = true;
	snprintf(cfgs[2].target_filter, sizeof(cfgs[2].target_filter), "NVIDIA Background Removal");
	snprintf(cfgs[2].target_setting, sizeof(cfgs[2].target_setting), "feather");
	cfgs[2].min_value = 0.0;
	cfgs[2].max_value = 0.5;

	cfgs[3].enabled = false;
	snprintf(cfgs[3].target_filter, sizeof(cfgs[3].target_filter), "NVIDIA Background Removal");
	snprintf(cfgs[3].target_setting, sizeof(cfgs[3].target_setting), "disabled_param");

	cfgs[4].enabled = true;
	snprintf(cfgs[4].target_filter, sizeof(cfgs[4].target_filter), "NVIDIA Background Removal");
	snprintf(cfgs[4].target_setting, sizeof(cfgs[4].target_setting), "contour_filter");
	cfgs[4].prop_type = VS_PROP_INT;
	cfgs[4].min_value = 0.0;
	cfgs[4].max_value = 10.0;

	/* Must produce exactly 2 batches (1 per distinct target filter, R-MOD-4), NOT 4 separate updates */
	const size_t batch_count = vs_mod_eval_and_batch_slots(cfgs, states, signals, VS_MOD_MAX_SLOTS, batches);
	assert(batch_count == 2u);
	assert(strcmp(batches[0].target_filter, "NVIDIA Background Removal") == 0);
	assert(batches[0].write_count == 3u);
	assert(strcmp(batches[0].writes[0].target_setting, "threshold") == 0);
	assert(double_near(batches[0].writes[0].float_val, 0.5, 1e-6));
	assert(strcmp(batches[0].writes[1].target_setting, "feather") == 0);
	assert(double_near(batches[0].writes[1].float_val, 0.25, 1e-6));
	assert(strcmp(batches[0].writes[2].target_setting, "contour_filter") == 0);
	assert(batches[0].writes[2].int_val == 5);

	assert(strcmp(batches[1].target_filter, "Color Correction") == 0);
	assert(batches[1].write_count == 1u);
	assert(strcmp(batches[1].writes[0].target_setting, "saturation") == 0);
	assert(double_near(batches[1].writes[0].float_val, 1.0, 1e-6));

	/* Repeating identical signals must produce 0 dirty batches */
	assert(vs_mod_eval_and_batch_slots(cfgs, states, signals, VS_MOD_MAX_SLOTS, batches) == 0u);
}

int main(void)
{
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	test_modulation_math_and_epsilon();
	test_modulation_per_target_filter_batching();
	printf("test-modulation: all assertions passed\n");
	return 0;
}
