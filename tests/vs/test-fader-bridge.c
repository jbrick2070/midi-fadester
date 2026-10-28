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
#include <float.h>
#include <limits.h>
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

	/* 1. Preserve the ten legacy indices and append all eight FX slots. */
	assert(VS_FADER_SLOT_COUNT == 18);
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
	for (int i = 0; i < 8; i++) {
		char id[16];
		snprintf(id, sizeof(id), "fx_%d", i + 1);
		assert(vs_fader_slot_from_name(id) == 10 + i);
		assert(strcmp(vs_fader_slot_id(10 + i), id) == 0);
		assert(vs_fader_bridge_set_slot_by_name(id, 0.125f * (float)(i + 1)));
		const enum vs_signal_id signal = (enum vs_signal_id)(210 + i);
		assert(float_near(vs_analyzer_get_signal(NULL, NULL, signal, 0.0f, 0.0f, VS_HZ_MODE_AVERAGE),
				  0.125f * (float)(i + 1), 1e-6f));
	}

	/* Reject invalid slot names */
	assert(vs_fader_slot_from_name("fader_9") == -1);
	assert(vs_fader_slot_from_name("fader_0") == -1);
	assert(vs_fader_slot_from_name("fx_0") == -1);
	assert(vs_fader_slot_from_name("fx_9") == -1);
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

static void test_complete_fader_snapshot(void)
{
	vs_fader_bridge_reset();
	float values[VS_FADER_SLOT_COUNT];
	memset(values, 0xff, sizeof(values));
	vs_fader_bridge_get_slots(values);
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++)
		assert(values[i] == 0.0f);
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++)
		vs_fader_bridge_set_slot(i, (float)i / 18.0f);
	vs_fader_bridge_get_slots(values);
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++)
		assert(float_near(values[i], (float)i / 18.0f, 1e-6f));
	vs_fader_bridge_detach();
	vs_fader_bridge_get_slots(values);
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++)
		assert(values[i] == 0.0f);
}

static void test_vendor_payload_fields_trigger_events(void)
{
	vs_fader_bridge_reset();
	struct vs_audio_frame frame = {0};
	assert(vs_fader_bridge_should_emit_signals(0.04f, &frame));
	frame.estimated_bpm = 120.0f;
	assert(vs_fader_bridge_should_emit_signals(0.04f, &frame));
	for (size_t i = 0; i < VS_SPECTRUM_BANDS; i++) {
		frame.spectrum[i] = 0.5f;
		assert(vs_fader_bridge_should_emit_signals(0.04f, &frame));
	}
	assert(!vs_fader_bridge_should_emit_signals(0.04f, &frame));
}

static void test_parameter_faders_saturate_before_numeric_conversion(void)
{
	vs_fader_bridge_reset();
	vs_fader_bridge_set_slot(VS_FADER_1, 1.0f);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_1, 0, 1e20, 1e30, 1, false, 0, false, 0) == INT_MAX);
	vs_fader_bridge_set_slot(VS_FADER_1, 0.0f);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_1, 0, -1e30, -1e20, 1, false, 0, false, 0) == INT_MIN);
	vs_fader_bridge_set_slot(VS_FADER_1, 1.0f);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_1, 0, -DBL_MAX, DBL_MAX, 1, false, 0, false, 0) == INT_MAX);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_1, 0, INT_MIN, INT_MAX, INT_MAX, false, 0, false, 0) ==
	       INT_MAX - 1);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_1, 0, 0, 100, 1, true, -DBL_MAX, true, DBL_MAX) == 100);
	const float value = vs_fader_bridge_eval_float_param(VS_FADER_1, 0, -DBL_MAX, DBL_MAX, false, 0, false, 0);
	assert(isfinite(value));
	assert(value == FLT_MAX);
}

struct fake_fadester_ctx {
	float slots[VS_FADER_SLOT_COUNT];
	int poll_calls;
	bool reject_fx;
	signal_handler_t *interleave_signal;
	bool emit_clean_during_poll;
	bool set_value_during_poll;
};

static void fake_midi_fadester_get_slot_value(void *data, calldata_t *cd)
{
	struct fake_fadester_ctx *ctx = (struct fake_fadester_ctx *)data;
	ctx->poll_calls++;

	const char *slot_name = calldata_string(cd, "slot");
	const int idx = vs_fader_slot_from_name(slot_name);
	if (idx < 0 || (ctx->reject_fx && idx >= VS_FADER_FX_1)) {
		calldata_set_float(cd, "value", 0.0);
		calldata_set_bool(cd, "ok", false);
		calldata_set_string(cd, "reason", "The requested slot is not available.");
		return;
	}

	const float snapshot = ctx->slots[idx];
	if (ctx->emit_clean_during_poll) {
		ctx->emit_clean_during_poll = false;
		struct calldata event;
		calldata_init(&event);
		calldata_set_string(&event, "slot", slot_name);
		calldata_set_float(&event, "value", 0.0);
		calldata_set_string(&event, "origin", "api");
		signal_handler_signal(ctx->interleave_signal, "midi_fadester_fader_changed", &event);
		calldata_free(&event);
	}
	if (ctx->set_value_during_poll) {
		ctx->set_value_during_poll = false;
		vs_fader_bridge_set_slot(idx, 0.25f);
	}
	calldata_set_float(cd, "value", (double)snapshot);
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

static void test_fx_bridge_poll_signal_and_legacy_provider(void)
{
	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *sh = signal_handler_create();
	assert(ph && sh);
	struct fake_fadester_ctx ctx = {0};
	for (int i = 0; i < 18; i++)
		ctx.slots[i] = (float)(i + 1) / 18.0f;
	proc_handler_add(
		ph,
		"void midi_fadester_get_slot_value(in string slot, out float value, out bool ok, out string reason)",
		fake_midi_fadester_get_slot_value, &ctx);
	signal_handler_add(sh, "void midi_fadester_fader_changed(string slot, float value, string origin)");
	vs_fader_bridge_attach(ph, sh);
	for (int i = 1; i < 18; i++)
		assert(vs_fader_bridge_poll_once());
	assert(ctx.poll_calls == 18);
	for (int i = 0; i < 18; i++)
		assert(float_near(vs_fader_bridge_get_slot(i), ctx.slots[i], 1e-6f));

	/* An API clean reaches the FX bridge immediately through the same event as MIDI input. */
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fx_8");
	calldata_set_float(&cd, "value", 0.0);
	calldata_set_string(&cd, "origin", "api");
	signal_handler_signal(sh, "midi_fadester_fader_changed", &cd);
	calldata_free(&cd);
	assert(vs_fader_bridge_get_slot(VS_FADER_FX_8) == 0.0f);
	assert(vs_fader_bridge_resolve_scale(VS_FADER_FX_8) == 0.0f);
	assert(vs_fader_bridge_eval_float_param(VS_FADER_FX_8, 1.0f, 0.0, 100.0, true, 0.0, true, 100.0) == 0.0f);

	/* A 0.3 provider still drives its ten legacy slots, and missing FX slots cannot retain stale values. */
	ctx.reject_fx = true;
	for (int i = 0; i < 18; i++)
		vs_fader_bridge_poll_once();
	assert(vs_fader_bridge_is_midi_fadester_loaded());
	for (int i = 0; i < 10; i++)
		assert(float_near(vs_fader_bridge_get_slot(i), ctx.slots[i], 1e-6f));
	for (int i = 10; i < 18; i++)
		assert(vs_fader_bridge_get_slot(i) == 0.0f);
	vs_fader_bridge_detach();
	signal_handler_destroy(sh);
	proc_handler_destroy(ph);
}

static void test_newer_events_win_over_in_flight_poll(void)
{
	proc_handler_t *ph = proc_handler_create();
	signal_handler_t *sh = signal_handler_create();
	assert(ph && sh);
	struct fake_fadester_ctx ctx = {0};
	ctx.slots[VS_FADER_FX_1] = 0.8f;
	ctx.slots[VS_FADER_FX_2] = 0.9f;
	ctx.interleave_signal = sh;
	proc_handler_add(
		ph,
		"void midi_fadester_get_slot_value(in string slot, out float value, out bool ok, out string reason)",
		fake_midi_fadester_get_slot_value, &ctx);
	signal_handler_add(sh, "void midi_fadester_fader_changed(string slot, float value, string origin)");
	vs_fader_bridge_attach(ph, sh);
	for (int i = 1; i < 10; i++)
		vs_fader_bridge_poll_once();

	/* The fake proc reads 0.8, then an API clean arrives before that read completes. */
	ctx.emit_clean_during_poll = true;
	assert(vs_fader_bridge_poll_once());
	assert(vs_fader_bridge_get_slot(VS_FADER_FX_1) == 0.0f);

	/* Direct setters also invalidate a preceding read of the same slot. */
	ctx.set_value_during_poll = true;
	assert(vs_fader_bridge_poll_once());
	assert(vs_fader_bridge_get_slot(VS_FADER_FX_2) == 0.25f);
	assert(vs_fader_bridge_is_midi_fadester_loaded());
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

static void test_shader_and_modulator_fader_parameter_controls(void)
{
	vs_fader_bridge_reset();

	/* 1. Unbound scale (VS_FADER_SLOT_NONE) defaults to 1.0f; unloaded bound fader returns 0.0f */
	assert(float_near(vs_fader_bridge_resolve_scale(VS_FADER_SLOT_NONE), 1.0f, 1e-6f));
	assert(float_near(vs_fader_bridge_resolve_scale(VS_FADER_1), 0.0f, 1e-6f));
	assert(float_near(vs_fader_bridge_resolve_scale(99), 0.0f, 1e-6f));

	/* Load fader values via bridge */
	vs_fader_bridge_set_slot(VS_FADER_1, 0.25f);
	vs_fader_bridge_set_slot(VS_FADER_2, 0.50f);
	vs_fader_bridge_set_slot(VS_FADER_MASTER, 1.00f);

	assert(float_near(vs_fader_bridge_resolve_scale(VS_FADER_1), 0.25f, 1e-6f));
	assert(float_near(vs_fader_bridge_resolve_scale(VS_FADER_2), 0.50f, 1e-6f));
	assert(float_near(vs_fader_bridge_resolve_scale(VS_FADER_MASTER), 1.00f, 1e-6f));

	/* 2. Audio reactivity frame scaling (scales reactive magnitudes & centers waveform around 0.5) */
	struct vs_audio_frame in_f;
	struct vs_audio_frame out_f;
	memset(&in_f, 0, sizeof(in_f));
	in_f.level = 0.80f;
	in_f.sub_bass = 0.60f;
	in_f.bass = 0.90f;
	in_f.kick = 1.00f;
	in_f.spectral_centroid = 0.44f;
	in_f.estimated_bpm = 124.0f;
	in_f.spectrum[0] = 0.80f;
	in_f.fft_bins[0] = 0.60f;
	in_f.waveform[0] = 0.90f; /* +0.40 excursion above 0.5 silence centerline */

	vs_fader_bridge_scale_audio_frame(&in_f, 0.50f, &out_f);
	assert(float_near(out_f.level, 0.40f, 1e-6f));
	assert(float_near(out_f.sub_bass, 0.30f, 1e-6f));
	assert(float_near(out_f.bass, 0.45f, 1e-6f));
	assert(float_near(out_f.kick, 0.50f, 1e-6f));
	assert(float_near(out_f.spectrum[0], 0.40f, 1e-6f));
	assert(float_near(out_f.fft_bins[0], 0.30f, 1e-6f));
	assert(float_near(out_f.waveform[0], 0.70f, 1e-6f));       /* 0.5 + 0.4 * 0.5 = 0.70 */
	assert(float_near(out_f.spectral_centroid, 0.44f, 1e-6f)); /* metadata preserved */
	assert(float_near(out_f.estimated_bpm, 124.0f, 1e-6f));    /* tempo preserved */

	/* 3. Direct Shader Filter float/int parameter fader evaluation & hard safety cap clamping */
	assert(float_near(vs_fader_bridge_eval_float_param(VS_FADER_SLOT_NONE, 1.40f, 0.0, 3.0, true, 0.0, true, 3.0),
			  1.40f, 1e-6f));

	/* Fader 2 at 0.50 across [0.20, 2.20] -> 1.20 */
	assert(float_near(vs_fader_bridge_eval_float_param(VS_FADER_2, 0.50f, 0.20, 2.20, true, 0.0, true, 3.0), 1.20f,
			  1e-6f));

	/* Inverted range [3.0, 1.0] at Fader 1 = 0.25 -> 2.50 */
	assert(float_near(vs_fader_bridge_eval_float_param(VS_FADER_1, 0.50f, 3.0, 1.0, true, 0.0, true, 3.0), 2.50f,
			  1e-6f));

	/* Hard photosensitivity / uniform ceiling clamp: even if user sets max_val = 10.0, hard_max = 3.0 caps at 3.0 */
	assert(float_near(vs_fader_bridge_eval_float_param(VS_FADER_MASTER, 1.0f, 0.5, 10.0, true, 0.5, true, 3.0),
			  3.0f, 1e-6f));

	/* Integer parameter fader evaluation with step quantization (e.g. symmetry_folds 2..16 step 2) */
	assert(vs_fader_bridge_eval_int_param(VS_FADER_SLOT_NONE, 6, 2.0, 16.0, 2, true, 2.0, true, 16.0) == 6);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_1, 6, 2.0, 18.0, 2, true, 2.0, true, 16.0) == 6);
	assert(vs_fader_bridge_eval_int_param(VS_FADER_MASTER, 6, 2.0, 24.0, 2, true, 2.0, true, 16.0) == 16);

	/* 4. End-to-end Audio Modulator Depth Fader scaling */
	struct vs_mod_slot_config cfg;
	struct vs_mod_slot_state st;
	struct vs_mod_eval_result res;
	vs_mod_slot_default_config(&cfg);
	vs_mod_slot_reset_state(&st);
	cfg.enabled = true;
	snprintf(cfg.target_filter, sizeof(cfg.target_filter), "Color Correction");
	snprintf(cfg.target_setting, sizeof(cfg.target_setting), "hue_shift");
	cfg.min_value = 0.0;
	cfg.max_value = 160.0;
	cfg.amount = 1.0;
	cfg.depth_fader_slot = VS_FADER_1; /* 0.25 */
	cfg.depth_scale = (double)vs_fader_bridge_resolve_scale(cfg.depth_fader_slot);

	vs_mod_slot_eval(&cfg, &st, 1.0f, &res);
	assert(res.should_write);
	assert(fabs(res.float_val - 40.0) < 1e-6); /* 160.0 * 1.0 * 0.25 = 40.0 */
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
	test_complete_fader_snapshot();
	test_parameter_faders_saturate_before_numeric_conversion();
	test_vendor_payload_fields_trigger_events();
	test_in_process_bridge_fake_proc_and_signal();
	test_fx_bridge_poll_signal_and_legacy_provider();
	test_newer_events_win_over_in_flight_poll();
	test_panic_and_restore_hotkeys_logic();
	test_multi_instance_frame_deduplication_and_track1_priority();
	test_shader_and_modulator_fader_parameter_controls();
	printf("test-fader-bridge: all assertions passed\n");
	return 0;
}
