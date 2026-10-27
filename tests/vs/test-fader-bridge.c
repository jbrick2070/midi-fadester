/*
 * Visual Sidechain - Fader Slot Modulation Source & In-Process Bridge Tests
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

	/* 1. Trigger "Visual Sidechain: Panic" */
	vs_fader_bridge_trigger_panic();
	assert(vs_fader_bridge_is_panic_active());
	vs_fader_bridge_apply_panic_to_slots(slots, 4u);

	/* Strobe/flash/modulation amounts immediately become 0.0; safety rate cap and threshold stay unchanged */
	assert(fabs(slots[0].current_value - 0.0) < 1e-6);
	assert(fabs(slots[1].current_value - 0.0) < 1e-6);
	assert(fabs(slots[2].current_value - 3.0) < 1e-6);
	assert(fabs(slots[3].current_value - 0.30) < 1e-6);

	/* Pressing Panic a second time while already panicked must not overwrite saved pre-panic values with 0.0 */
	vs_fader_bridge_trigger_panic();
	vs_fader_bridge_apply_panic_to_slots(slots, 4u);

	/* 2. Trigger "Visual Sidechain: Restore" */
	vs_fader_bridge_trigger_restore();
	assert(!vs_fader_bridge_is_panic_active());
	vs_fader_bridge_apply_restore_to_slots(slots, 4u);

	assert(fabs(slots[0].current_value - 0.55) < 1e-6);
	assert(fabs(slots[1].current_value - 100.0) < 1e-6);
	assert(fabs(slots[2].current_value - 3.0) < 1e-6);
	assert(fabs(slots[3].current_value - 0.30) < 1e-6);
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
	printf("test-fader-bridge: all assertions passed\n");
	return 0;
}
