/*
 * Visual Sidechain - Fader Slot Modulation Source & 30 Hz Signal Coalescer Tests
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

#include "core/fader-bridge.h"
#include "core/modulation.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool float_near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

static void test_fader_slot_lookup_and_clamping(void)
{
	vs_fader_bridge_reset();

	/* 1. Resolve all 10 canonical Fadester slot IDs */
	assert(vs_fader_slot_from_name("crossfader") == VS_FADER_CROSSFADER);
	assert(vs_fader_slot_from_name("fader_1") == VS_FADER_1);
	assert(vs_fader_slot_from_name("fader_2") == VS_FADER_2);
	assert(vs_fader_slot_from_name("fader_3") == VS_FADER_3);
	assert(vs_fader_slot_from_name("fader_4") == VS_FADER_4);
	assert(vs_fader_slot_from_name("fader_5") == VS_FADER_5);
	assert(vs_fader_slot_from_name("fader_6") == VS_FADER_6);
	assert(vs_fader_slot_from_name("fader_7") == VS_FADER_7);
	assert(vs_fader_slot_from_name("fader_8") == VS_FADER_8);
	assert(vs_fader_slot_from_name("master") == VS_FADER_MASTER);

	/* Reject invalid slot names */
	assert(vs_fader_slot_from_name("fader_9") == -1);
	assert(vs_fader_slot_from_name("fader_0") == -1);
	assert(vs_fader_slot_from_name("") == -1);
	assert(vs_fader_slot_from_name(NULL) == -1);

	/* 2. Clamping and retrieval */
	assert(vs_fader_bridge_set_slot_by_name("crossfader", 0.42f));
	assert(float_near(vs_fader_bridge_get_slot(VS_FADER_CROSSFADER), 0.42f, 1e-6f));

	assert(vs_fader_bridge_set_slot_by_name("fader_1", 1.75f));
	assert(float_near(vs_fader_bridge_get_slot(VS_FADER_1), 1.0f, 1e-6f));

	assert(vs_fader_bridge_set_slot_by_name("master", -0.50f));
	assert(float_near(vs_fader_bridge_get_slot(VS_FADER_MASTER), 0.0f, 1e-6f));

	/* 3. End-to-end: Fader slot as a modulation source via vs_analyzer_get_signal -> vs_mod_slot_eval */
	vs_fader_bridge_set_slot_by_name("fader_3", 0.75f);
	const float sig = vs_analyzer_get_signal(NULL, NULL, VS_SIG_FADER_3, 60.0f, 150.0f, VS_HZ_MODE_AVERAGE);
	assert(float_near(sig, 0.75f, 1e-6f));

	struct vs_mod_slot_config cfg;
	struct vs_mod_slot_state st;
	struct vs_mod_eval_result res;
	vs_mod_slot_default_config(&cfg);
	vs_mod_slot_reset_state(&st);
	cfg.enabled = true;
	strncpy(cfg.target_filter, "Color Correction", sizeof(cfg.target_filter) - 1u);
	strncpy(cfg.target_setting, "saturation", sizeof(cfg.target_setting) - 1u);
	cfg.prop_type = VS_PROP_FLOAT;
	cfg.min_value = 0.0;
	cfg.max_value = 2.0;
	cfg.amount = 1.0;

	vs_mod_slot_eval(&cfg, &st, sig, &res);
	assert(res.should_write);
	assert(fabs(res.float_val - 1.50) < 1e-6);
}

static void test_signals_event_coalescing_30hz(void)
{
	vs_fader_bridge_reset();

	struct vs_audio_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.kick = 0.80f;
	frame.bass = 0.65f;

	/* First emission is immediate because g_since_last_emit_sec starts at VS_SIGNALS_MIN_INTERVAL_SEC */
	assert(vs_fader_bridge_should_emit_signals(0.01667f, &frame));

	/* Next frame 16.67 ms later (60 fps) MUST be coalesced (suppressed) since 16.67 ms < 33.33 ms */
	frame.kick = 0.90f;
	assert(!vs_fader_bridge_should_emit_signals(0.01667f, &frame));

	/* Next frame 17.0 ms later (total 33.67 ms >= 33.33 ms) with changed kick emits */
	assert(vs_fader_bridge_should_emit_signals(0.01700f, &frame));

	/* Even after 50 ms, if the signal values are identical within 1e-3, do not emit redundant event */
	assert(!vs_fader_bridge_should_emit_signals(0.05000f, &frame));
}

int main(void)
{
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	test_fader_slot_lookup_and_clamping();
	test_signals_event_coalescing_30hz();
	printf("test-fader-bridge: all assertions passed\n");
	return 0;
}
