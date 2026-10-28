/*
 * Visual Sidechain - Fader Slot Modulation Source & In-Process Bridge Tests
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

static void test_in_process_bridge_missing_midi_fadester(void)
{
	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *sh = signal_handler_create();
	assert(ph != NULL && sh != NULL);

	/* Attach a proc_handler without midi_fadester_get_slot_value */
	vs_fader_bridge_attach(ph, sh);
	assert(!vs_fader_bridge_is_midi_fadester_loaded());

	/* Seed a non-zero slot, then tick: because midi_fadester_get_slot_value is missing on ph,
	 * vs_fader_bridge_on_tick must mark Midi Fadester as not loaded and zero all fader slots. */
	vs_fader_bridge_set_slot(VS_FADER_CROSSFADER, 0.88f);
	struct vs_audio_frame frame;
	memset(&frame, 0, sizeof(frame));
	vs_fader_bridge_on_tick(0.01667f, &frame);

	assert(!vs_fader_bridge_is_midi_fadester_loaded());
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		assert(float_near(vs_fader_bridge_get_slot(i), 0.0f, 1e-6f));
		const float sig = vs_analyzer_get_signal(NULL, NULL,
							 (enum vs_signal_id)((int)VS_SIG_FADER_CROSSFADER + i), 60.0f,
							 150.0f, VS_HZ_MODE_AVERAGE);
		assert(float_near(sig, 0.0f, 1e-6f));
	}

	vs_fader_bridge_detach();
	signal_handler_destroy(sh);
	proc_handler_destroy(ph);
}

struct fake_fadester_ctx {
	float slots[VS_FADER_SLOT_COUNT];
	int poll_calls;
};

static void fake_midi_fadester_get_slot_value(void *data, calldata_t *cd)
{
	struct fake_fadester_ctx *ctx = (struct fake_fadester_ctx *)data;
	ctx->poll_calls++;

	const char *slot_name = calldata_string(cd, "slot");
	const int idx = vs_fader_slot_from_name(slot_name);
	if (idx < 0) {
		calldata_set_float(cd, "value", 0.0);
		calldata_set_bool(cd, "ok", false);
		calldata_set_string(cd, "reason", "Unknown slot");
		return;
	}

	calldata_set_float(cd, "value", (double)ctx->slots[idx]);
	calldata_set_bool(cd, "ok", true);
	calldata_set_string(cd, "reason", "");
}

struct signal_capture_ctx {
	int emit_count;
	float level;
	float bass;
	float mid;
	float high;
	float kick;
	float snare;
	float beat_phase;
	float bpm;
	bool ok;
};

static void on_visual_sidechain_signals_captured(void *data, calldata_t *cd)
{
	struct signal_capture_ctx *cap = (struct signal_capture_ctx *)data;
	cap->emit_count++;
	cap->level = (float)calldata_float(cd, "level");
	cap->bass = (float)calldata_float(cd, "bass");
	cap->mid = (float)calldata_float(cd, "mid");
	cap->high = (float)calldata_float(cd, "high");
	cap->kick = (float)calldata_float(cd, "kick");
	cap->snare = (float)calldata_float(cd, "snare");
	cap->beat_phase = (float)calldata_float(cd, "beat_phase");
	cap->bpm = (float)calldata_float(cd, "bpm");
	cap->ok = calldata_bool(cd, "ok");
}

static void test_in_process_bridge_fake_proc_and_signal(void)
{
	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *sh = signal_handler_create();
	assert(ph != NULL && sh != NULL);

	struct fake_fadester_ctx fake_fadester;
	memset(&fake_fadester, 0, sizeof(fake_fadester));
	fake_fadester.slots[VS_FADER_CROSSFADER] = 0.35f;
	fake_fadester.slots[VS_FADER_1] = 0.62f;

	/* Register fake Midi Fadester proc and signal per API-CONTRACT.md */
	proc_handler_add(
		ph,
		"void midi_fadester_get_slot_value(in string slot, out float value, out bool ok, out string reason)",
		fake_midi_fadester_get_slot_value, &fake_fadester);
	signal_handler_add(sh, "void midi_fadester_fader_changed(string slot, float value, string origin)");

	/* Attach Visual Sidechain bridge */
	vs_fader_bridge_attach(ph, sh);
	assert(vs_fader_bridge_is_midi_fadester_loaded());
	/* Attach performed exactly 1 initial probe */
	assert(fake_fadester.poll_calls == 1);
	assert(float_near(vs_fader_bridge_get_slot(VS_FADER_CROSSFADER), 0.35f, 1e-6f));

	/* Subscribe to visual_sidechain_signals on sh */
	struct signal_capture_ctx sig_cap;
	memset(&sig_cap, 0, sizeof(sig_cap));
	signal_handler_connect(sh, "visual_sidechain_signals", on_visual_sidechain_signals_captured, &sig_cap);

	/* Step 1 tick: verify at most 1 poll per tick and exact signal emission */
	struct vs_audio_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.level = 0.55f;
	frame.bass = 0.82f;
	frame.mid = 0.44f;
	frame.high = 0.21f;
	frame.kick = 0.93f;
	frame.snare = 0.12f;
	frame.beat = 0.75f;
	frame.estimated_bpm = 128.0f;

	assert(vs_fader_bridge_on_tick(0.01667f, &frame));
	assert(fake_fadester.poll_calls == 2); /* 1 at attach + 1 on this tick */
	assert(float_near(vs_fader_bridge_get_slot(VS_FADER_1), 0.62f, 1e-6f));

	assert(sig_cap.emit_count == 1);
	assert(sig_cap.ok);
	assert(float_near(sig_cap.level, 0.55f, 1e-6f));
	assert(float_near(sig_cap.bass, 0.82f, 1e-6f));
	assert(float_near(sig_cap.mid, 0.44f, 1e-6f));
	assert(float_near(sig_cap.high, 0.21f, 1e-6f));
	assert(float_near(sig_cap.kick, 0.93f, 1e-6f));
	assert(float_near(sig_cap.snare, 0.12f, 1e-6f));
	assert(float_near(sig_cap.beat_phase, 0.75f, 1e-6f));
	assert(float_near(sig_cap.bpm, 128.0f, 1e-5f));

	/* Verify visual_sidechain_get_signals proc returns the exact contract fields */
	struct calldata cd;
	calldata_init(&cd);
	assert(proc_handler_call(ph, "visual_sidechain_get_signals", &cd));
	assert(calldata_bool(&cd, "ok"));
	assert(float_near((float)calldata_float(&cd, "level"), 0.55f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "bass"), 0.82f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "mid"), 0.44f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "high"), 0.21f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "kick"), 0.93f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "snare"), 0.12f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "beat_phase"), 0.75f, 1e-6f));
	assert(float_near((float)calldata_float(&cd, "bpm"), 128.0f, 1e-5f));
	calldata_free(&cd);

	/* Emit midi_fadester_fader_changed signal and verify immediate fader slot update without polling */
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_5");
	calldata_set_float(&cd, "value", 0.91);
	calldata_set_string(&cd, "origin", "midi");
	signal_handler_signal(sh, "midi_fadester_fader_changed", &cd);
	calldata_free(&cd);

	assert(float_near(vs_fader_bridge_get_slot(VS_FADER_5), 0.91f, 1e-6f));
	assert(float_near(vs_analyzer_get_signal(NULL, NULL, VS_SIG_FADER_5, 60.0f, 150.0f, VS_HZ_MODE_AVERAGE), 0.91f,
			  1e-6f));

	vs_fader_bridge_detach();
	signal_handler_destroy(sh);
	proc_handler_destroy(ph);
}

static void test_panic_and_restore_hotkeys_logic(void)
{
	vs_fader_bridge_reset();
	assert(!vs_fader_bridge_is_panic_active());

	bool builtin_passthrough = false;
	struct vs_panic_param_slot slots[4];
	memset(slots, 0, sizeof(slots));
	snprintf(slots[0].name, sizeof(slots[0].name), "flash_intensity");
	slots[0].current_value = 0.55;
	snprintf(slots[1].name, sizeof(slots[1].name), "slot_1_amount_pct");
	slots[1].current_value = 100.0;
	snprintf(slots[2].name, sizeof(slots[2].name), "max_flash_rate_hz");
	slots[2].current_value = 3.0;
	snprintf(slots[3].name, sizeof(slots[3].name), "onset_threshold");
	slots[3].current_value = 0.30;

	/* User shader with non-matching parameter names and parameterless user shader */
	bool user_shader_passthrough = false;
	struct vs_panic_param_slot user_slots[2];
	memset(user_slots, 0, sizeof(user_slots));
	snprintf(user_slots[0].name, sizeof(user_slots[0].name), "custom_color_drive");
	user_slots[0].current_value = 0.85;
	snprintf(user_slots[1].name, sizeof(user_slots[1].name), "warp_phase_offset");
	user_slots[1].current_value = 1.25;

	bool zero_param_shader_passthrough = false;

	/* Before Panic: compiled shaders render normally (not pass-through); uncompiled shader is pass-through */
	assert(!vs_fader_bridge_should_shader_passthrough(builtin_passthrough, true));
	assert(!vs_fader_bridge_should_shader_passthrough(user_shader_passthrough, true));
	assert(!vs_fader_bridge_should_shader_passthrough(zero_param_shader_passthrough, true));
	assert(vs_fader_bridge_should_shader_passthrough(false, false));

	/* 1. Trigger "Visual Sidechain: Panic" */
	vs_fader_bridge_trigger_panic();
	assert(vs_fader_bridge_is_panic_active());
	vs_fader_bridge_apply_panic_to_shader(&builtin_passthrough, slots, 4u);
	vs_fader_bridge_apply_panic_to_shader(&user_shader_passthrough, user_slots, 2u);
	vs_fader_bridge_apply_panic_to_shader(&zero_param_shader_passthrough, NULL, 0u);

	/* Every Visual Sidechain: Shader Filter must enter pass-through regardless of parameter names */
	assert(builtin_passthrough);
	assert(user_shader_passthrough);
	assert(zero_param_shader_passthrough);
	assert(vs_fader_bridge_should_shader_passthrough(builtin_passthrough, true));
	assert(vs_fader_bridge_should_shader_passthrough(user_shader_passthrough, true));
	assert(vs_fader_bridge_should_shader_passthrough(zero_param_shader_passthrough, true));

	/* Strobe/flash/modulation amounts immediately become 0.0; safety rate cap, threshold, and custom params stay intact */
	assert(fabs(slots[0].current_value - 0.0) < 1e-6);
	assert(fabs(slots[1].current_value - 0.0) < 1e-6);
	assert(fabs(slots[2].current_value - 3.0) < 1e-6);
	assert(fabs(slots[3].current_value - 0.30) < 1e-6);
	assert(fabs(user_slots[0].current_value - 0.85) < 1e-6);
	assert(fabs(user_slots[1].current_value - 1.25) < 1e-6);

	/* Pressing Panic a second time while already panicked must keep pass-through active and preserve saved values */
	vs_fader_bridge_trigger_panic();
	vs_fader_bridge_apply_panic_to_shader(&builtin_passthrough, slots, 4u);
	vs_fader_bridge_apply_panic_to_shader(&user_shader_passthrough, user_slots, 2u);
	assert(vs_fader_bridge_should_shader_passthrough(user_shader_passthrough, true));

	/* 2. Trigger "Visual Sidechain: Restore" */
	vs_fader_bridge_trigger_restore();
	assert(!vs_fader_bridge_is_panic_active());
	vs_fader_bridge_apply_restore_to_shader(&builtin_passthrough, slots, 4u);
	vs_fader_bridge_apply_restore_to_shader(&user_shader_passthrough, user_slots, 2u);
	vs_fader_bridge_apply_restore_to_shader(&zero_param_shader_passthrough, NULL, 0u);

	/* Pass-through is cleared on Restore and pre-panic values are restored */
	assert(!builtin_passthrough);
	assert(!user_shader_passthrough);
	assert(!zero_param_shader_passthrough);
	assert(!vs_fader_bridge_should_shader_passthrough(builtin_passthrough, true));
	assert(!vs_fader_bridge_should_shader_passthrough(user_shader_passthrough, true));
	assert(!vs_fader_bridge_should_shader_passthrough(zero_param_shader_passthrough, true));

	assert(fabs(slots[0].current_value - 0.55) < 1e-6);
	assert(fabs(slots[1].current_value - 100.0) < 1e-6);
	assert(fabs(slots[2].current_value - 3.0) < 1e-6);
	assert(fabs(slots[3].current_value - 0.30) < 1e-6);
}

static void test_multi_instance_frame_deduplication_and_track1_priority(void)
{
	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *sh = signal_handler_create();
	assert(ph != NULL && sh != NULL);

	struct fake_fadester_ctx fake_fadester;
	memset(&fake_fadester, 0, sizeof(fake_fadester));
	fake_fadester.slots[VS_FADER_CROSSFADER] = 0.25f;
	fake_fadester.slots[VS_FADER_1] = 0.50f;
	fake_fadester.slots[VS_FADER_2] = 0.75f;

	proc_handler_add(
		ph,
		"void midi_fadester_get_slot_value(in string slot, out float value, out bool ok, out string reason)",
		fake_midi_fadester_get_slot_value, &fake_fadester);
	signal_handler_add(sh, "void midi_fadester_fader_changed(string slot, float value, string origin)");

	vs_fader_bridge_attach(ph, sh);
	assert(fake_fadester.poll_calls == 1); /* 1 initial probe on attach */

	struct signal_capture_ctx sig_cap;
	memset(&sig_cap, 0, sizeof(sig_cap));
	signal_handler_connect(sh, "visual_sidechain_signals", on_visual_sidechain_signals_captured, &sig_cap);

	struct vs_audio_frame secondary_a;
	struct vs_audio_frame primary_t1;
	struct vs_audio_frame secondary_c;
	memset(&secondary_a, 0, sizeof(secondary_a));
	memset(&primary_t1, 0, sizeof(primary_t1));
	memset(&secondary_c, 0, sizeof(secondary_c));
	secondary_a.kick = 0.11f;
	primary_t1.kick = 0.94f;
	secondary_c.kick = 0.22f;

	/* Video Frame 1 (ts = 100 ms): 3 filters tick in the same OBS frame (secondary, Track 1, secondary).
	 * Must poll midi_fadester_get_slot_value ONLY ONCE for the frame, and Track 1 must own g_latest_signals. */
	const uint64_t ts1 = 100000000ULL;
	vs_fader_bridge_on_frame_tick(ts1, 0.01667f, &secondary_a, false);
	vs_fader_bridge_on_frame_tick(ts1, 0.01667f, &primary_t1, true);
	vs_fader_bridge_on_frame_tick(ts1, 0.01667f, &secondary_c, false);

	assert(fake_fadester.poll_calls == 2); /* 1 attach + 1 for Frame 1 (NOT 1 + 3 = 4) */
	assert(sig_cap.emit_count == 1);

	struct vs_audio_frame snap;
	vs_fader_bridge_get_signals(&snap);
	assert(float_near(snap.kick, 0.94f, 1e-6f));

	/* Video Frame 2 (ts = 116.67 ms, +16.67 ms < 33.33 ms): all 3 filters tick again.
	 * dt_sec must NOT be summed 3x (which would prematurely cross 33.33 ms!), poll_calls must advance by 1,
	 * and secondary_a ticking before primary_t1 must NOT clobber g_latest_signals with 0.11f! */
	const uint64_t ts2 = 116666667ULL;
	primary_t1.kick = 0.85f;
	assert(!vs_fader_bridge_on_frame_tick(ts2, 0.01667f, &secondary_a, false));
	vs_fader_bridge_get_signals(&snap);
	assert(float_near(snap.kick, 0.94f, 1e-6f)); /* Not clobbered by secondary_a! */

	assert(!vs_fader_bridge_on_frame_tick(ts2, 0.01667f, &primary_t1, true));
	assert(!vs_fader_bridge_on_frame_tick(ts2, 0.01667f, &secondary_c, false));
	assert(fake_fadester.poll_calls == 3); /* +1 for Frame 2 */
	assert(sig_cap.emit_count == 1);       /* Still 1 because 16.67 ms < 33.33 ms */
	vs_fader_bridge_get_signals(&snap);
	assert(float_near(snap.kick, 0.85f, 1e-6f));

	/* Video Frame 3 (ts = 133.67 ms, +17.0 ms => 33.67 ms >= 33.33 ms): Track 1 emits cleanly at <= 30 Hz */
	const uint64_t ts3 = 133666667ULL;
	primary_t1.kick = 0.77f;
	assert(!vs_fader_bridge_on_frame_tick(ts3, 0.01700f, &secondary_a, false));
	assert(vs_fader_bridge_on_frame_tick(ts3, 0.01700f, &primary_t1, true));
	assert(!vs_fader_bridge_on_frame_tick(ts3, 0.01700f, &secondary_c, false));
	assert(fake_fadester.poll_calls == 4); /* +1 for Frame 3 across 9 total filter ticks */
	assert(sig_cap.emit_count == 2);
	assert(float_near(sig_cap.kick, 0.77f, 1e-6f));

	/* Fallback: if the Track 1 filter is removed/disabled, secondary_a takes over within 2 frames */
	vs_fader_bridge_on_frame_tick(150333334ULL, 0.01667f, &secondary_a, false);
	vs_fader_bridge_on_frame_tick(167333334ULL, 0.01700f, &secondary_a, false);
	vs_fader_bridge_get_signals(&snap);
	assert(float_near(snap.kick, 0.11f, 1e-6f));

	vs_fader_bridge_detach();
	signal_handler_destroy(sh);
	proc_handler_destroy(ph);
}

int main(void)
{
#ifdef _WIN32
	_set_error_mode(_OUT_TO_STDERR);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
	test_fader_slot_lookup_and_clamping();
	test_signals_event_coalescing_30hz();
	test_in_process_bridge_missing_midi_fadester();
	test_in_process_bridge_fake_proc_and_signal();
	test_panic_and_restore_hotkeys_logic();
	test_multi_instance_frame_deduplication_and_track1_priority();
	printf("test-fader-bridge: all assertions passed\n");
	return 0;
}
