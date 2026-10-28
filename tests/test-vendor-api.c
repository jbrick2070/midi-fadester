/*
 * Midi Fadester — test-vendor-api
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include <obs-module.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("midi-fadester", "en-US")

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static int g_mock_tbar_calls = 0;
static int g_mock_tbar_completions = 0;
static float g_mock_tbar_last_val = -1.0f;

static bool mock_tbar_driver(float t_norm, bool *out_completed_throw, char *out_err, size_t err_size)
{
	if (out_err && err_size > 0U) {
		out_err[0] = '\0';
	}
	++g_mock_tbar_calls;
	g_mock_tbar_last_val = t_norm;
	bool completed = (t_norm >= MFD_THROW_COMPLETE_THRESHOLD);
	if (completed) {
		++g_mock_tbar_completions;
	}
	if (out_completed_throw) {
		*out_completed_throw = completed;
	}
	return true;
}

static void push_midi_cc(struct mfd_engine *eng, uint8_t ch, uint8_t cc, uint8_t val)
{
	uint32_t raw_msg = (uint32_t)(0xB0U | (ch & 0x0FU)) | ((uint32_t)(cc & 0x7FU) << 8) |
			   ((uint32_t)(val & 0x7FU) << 16);
	mfd_midi_queue_push(&eng->midi_queue, raw_msg, 0U);
}

static bool get_slot_parked_via_get_state(struct mfd_engine *eng, int slot_idx)
{
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, eng);
	obs_data_array_t *arr = obs_data_get_array(res, "slots");
	obs_data_t *item = obs_data_array_item(arr, (size_t)slot_idx);
	bool parked = obs_data_get_bool(item, "parked");
	obs_data_release(item);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);
	return parked;
}

static bool mock_source_exists(const char *source_name, void *user_data)
{
	UNUSED_PARAMETER(user_data);
	if (!source_name) {
		return false;
	}
	return strcmp(source_name, "Cam 1") == 0 || strcmp(source_name, "Loop 3") == 0;
}

struct fake_ws_log {
	int fader_changed_count;
	int state_changed_count;
	char last_fader_slot[32];
	double last_fader_value;
	char last_fader_origin[32];
};

static void on_fake_ws_event(const char *event_name, obs_data_t *event_data, void *user_data)
{
	struct fake_ws_log *log = (struct fake_ws_log *)user_data;
	if (!log || !event_name || !event_data) {
		return;
	}
	if (strcmp(event_name, "fader_changed") == 0) {
		++log->fader_changed_count;
		const char *s = obs_data_get_string(event_data, "slot");
		const char *o = obs_data_get_string(event_data, "origin");
		snprintf(log->last_fader_slot, sizeof(log->last_fader_slot), "%s", s ? s : "");
		snprintf(log->last_fader_origin, sizeof(log->last_fader_origin), "%s", o ? o : "");
		log->last_fader_value = obs_data_get_double(event_data, "value");
	} else if (strcmp(event_name, "state_changed") == 0) {
		++log->state_changed_count;
	}
}

struct fake_signal_log {
	int count;
	char slot[32];
	float value;
	char origin[32];
};

static void on_fake_signal(void *data, calldata_t *cd)
{
	struct fake_signal_log *sig = (struct fake_signal_log *)data;
	if (!sig || !cd) {
		return;
	}
	++sig->count;
	const char *s = calldata_string(cd, "slot");
	const char *o = calldata_string(cd, "origin");
	snprintf(sig->slot, sizeof(sig->slot), "%s", s ? s : "");
	snprintf(sig->origin, sizeof(sig->origin), "%s", o ? o : "");
	sig->value = (float)calldata_float(cd, "value");
}

int main(void)
{
	struct mfd_engine eng;
	memset(&eng, 0, sizeof(eng));
	eng.smooth_tau_ms = 12.0f;
	eng.tbar_bidirectional_flip = true;
	eng.tbar_driver = mock_tbar_driver;
	eng.source_exists_fn = mock_source_exists;
	eng.studio_mode_override_set = true;
	eng.studio_mode_override = false;

	struct fake_ws_log ws_log;
	memset(&ws_log, 0, sizeof(ws_log));
	mfd_vendor_set_event_sink(on_fake_ws_event, &ws_log);

	/* 1. load_profile { name: "apc40mk2" } and error check on unknown profile */
	obs_data_t *req = obs_data_create();
	obs_data_t *res = obs_data_create();
	obs_data_set_string(req, "name", "nonexistent_desk");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Unknown profile 'nonexistent_desk'. Valid profiles: apc40mk2.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "name", "apc40mk2");
	mfd_vendor_handle_load_profile(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "loaded"), "apc40mk2") == 0);
	REQUIRE(ws_log.state_changed_count == 1);
	REQUIRE(strcmp(eng.fader_slots[0].id, "crossfader") == 0);
	REQUIRE(eng.fader_slots[0].target_type == MFD_TARGET_TBAR);
	REQUIRE(strcmp(eng.fader_slots[1].id, "fader_1") == 0);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	REQUIRE(strcmp(eng.fader_slots[9].id, "master") == 0);
	obs_data_release(req);
	obs_data_release(res);

	/* 2. get_state: verify studio_mode, device, and slot availability/reason when Studio Mode is OFF */
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	REQUIRE(obs_data_get_int(res, "api_version") == 1);
	REQUIRE(!obs_data_get_bool(res, "studio_mode"));
	obs_data_array_t *arr = obs_data_get_array(res, "slots");
	REQUIRE(arr != NULL);
	REQUIRE(obs_data_array_count(arr) == MFD_MAX_FADER_SLOTS);
	obs_data_t *s0 = obs_data_array_item(arr, 0);
	REQUIRE(strcmp(obs_data_get_string(s0, "id"), "crossfader") == 0);
	REQUIRE(strcmp(obs_data_get_string(s0, "target_mode"), "tbar") == 0);
	REQUIRE(!obs_data_get_bool(s0, "available"));
	REQUIRE(strcmp(obs_data_get_string(s0, "reason"), "Studio Mode is off") == 0);
	obs_data_release(s0);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);

	/* Turn Studio Mode ON -> crossfader becomes available */
	eng.studio_mode_override = true;
	req = obs_data_create();
	res = obs_data_create();
	mfd_vendor_handle_get_state(req, res, &eng);
	REQUIRE(obs_data_get_bool(res, "studio_mode"));
	arr = obs_data_get_array(res, "slots");
	s0 = obs_data_array_item(arr, 0);
	REQUIRE(obs_data_get_bool(s0, "available"));
	REQUIRE(strcmp(obs_data_get_string(s0, "reason"), "") == 0);
	obs_data_release(s0);
	obs_data_array_release(arr);
	obs_data_release(req);
	obs_data_release(res);

	/* 3. set_slot error sentences for invalid slot id and unknown source names */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_9");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.") == 0);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "NonExistentLoop");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "comment"),
		       "Source 'NonExistentLoop' not found. Create the source in OBS first or check its spelling.") ==
		0);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);
	obs_data_release(req);
	obs_data_release(res);

	/* Valid set_slot on fader_1: returns updated slot object and emits state_changed */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "curve", "dip_free");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "Loop 3");
	obs_data_set_bool(req, "afv", true);
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "id"), "fader_1") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "target_mode"), "crossfade_ab") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "curve"), "dip_free") == 0);
	REQUIRE(strcmp(obs_data_get_string(res, "label"), "Cam 1 -> Loop 3") == 0);
	REQUIRE(obs_data_get_bool(res, "available"));
	REQUIRE(strcmp(obs_data_get_string(res, "reason"), "") == 0);
	REQUIRE(ws_log.state_changed_count == 2);
	obs_data_release(req);
	obs_data_release(res);

	/* 4. set_fader_value on fader_1: Dip-Free holds alpha_a=1.0 across entire range (including t=0.75) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.75);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "slot"), "fader_1") == 0);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.75) < 1e-5);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_a - 1.0f) < 1e-5f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 0.75f) < 1e-5f);
	REQUIRE(ws_log.fader_changed_count == 1);
	REQUIRE(strcmp(ws_log.last_fader_slot, "fader_1") == 0);
	REQUIRE(strcmp(ws_log.last_fader_origin, "api") == 0);
	REQUIRE(fabs(ws_log.last_fader_value - 0.75) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);

	/* 5. nudge_fader and 30 Hz per-slot fader_changed coalescing */
	/* Advance time by 0.05s (> 1/30s) so the first nudge emits immediately */
	mfd_engine_tick(&eng, 0.05f);
	int before_burst = ws_log.fader_changed_count;

	for (int i = 0; i < 5; ++i) {
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "fader_1");
		obs_data_set_double(req, "delta", -0.05);
		mfd_vendor_handle_nudge_fader(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
	}
	/* 0.75 - 5 * 0.05 = 0.50; only 1 immediate event emitted during the 0ms burst */
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.50f) < 1e-5f);
	REQUIRE(ws_log.fader_changed_count == before_burst + 1);
	REQUIRE(eng.fader_slots[1].fader_event_pending);

	/* Advance 0.04s (> 33.3ms) -> coalesced pending event flushes with final value 0.50 */
	mfd_engine_tick(&eng, 0.04f);
	REQUIRE(!eng.fader_slots[1].fader_event_pending);
	REQUIRE(ws_log.fader_changed_count == before_burst + 2);
	REQUIRE(fabs(ws_log.last_fader_value - 0.50) < 1e-5);

	/* 6. auto_fade: timed ramp from 0.50 to 1.00 over 100 ms */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 10); /* invalid (< 50 ms) */
	mfd_vendor_handle_auto_fade(req, res, &eng);
	REQUIRE(obs_data_get_string(res, "comment")[0] != '\0');
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 100);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	REQUIRE(strcmp(obs_data_get_string(res, "slot"), "fader_1") == 0);
	REQUIRE(eng.fader_slots[1].auto_fade_active);
	obs_data_release(req);
	obs_data_release(res);

	mfd_engine_tick(&eng, 0.05f); /* halfway through 100ms fade: 0.50 -> 0.75 */
	REQUIRE(eng.fader_slots[1].auto_fade_active);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.75f) < 1e-5f);
	REQUIRE(strcmp(ws_log.last_fader_origin, "auto_fade") == 0);

	mfd_engine_tick(&eng, 0.05f); /* completed 100ms fade: 1.00 */
	REQUIRE(!eng.fader_slots[1].auto_fade_active);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.00f) < 1e-5f);

	/* 7. set_fader_value on crossfader (T-Bar) with release: true finishes transition */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.4);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(g_mock_tbar_calls == 1);
	REQUIRE(fabsf(g_mock_tbar_last_val - 0.4f) < 1e-5f);
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.4);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(g_mock_tbar_calls == 2);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.0) < 1e-5);
	REQUIRE(eng.fader_slots[0].takeover.state == MFD_TAKEOVER_PARKED);
	obs_data_release(req);
	obs_data_release(res);

	/* 8. JSON config Export and Import round-trip (Item 7: D-5 Import/Export without file watcher) */
	const char *tmp_json = "test-mfd-config-roundtrip.json";
	snprintf(eng.configured_device_name, sizeof(eng.configured_device_name), "APC40 mkII");
	eng.smooth_tau_ms = 24.5f;
	eng.tbar_bidirectional_flip = false;
	REQUIRE(mfd_engine_export_config_file(&eng, tmp_json));

	mfd_engine_load_apc40_defaults(&eng);
	eng.configured_device_name[0] = '\0';
	eng.smooth_tau_ms = 12.0f;
	eng.tbar_bidirectional_flip = true;
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_UNASSIGNED);

	REQUIRE(mfd_engine_import_config_file(&eng, tmp_json));
	REQUIRE(strcmp(eng.configured_device_name, "APC40 mkII") == 0);
	REQUIRE(fabsf(eng.smooth_tau_ms - 24.5f) < 1e-4f);
	REQUIRE(!eng.tbar_bidirectional_flip);
	REQUIRE(eng.fader_slots[1].target_type == MFD_TARGET_TWO_SOURCE_MIX);
	REQUIRE(strcmp(eng.fader_slots[1].source_a_name, "Cam 1") == 0);
	REQUIRE(strcmp(eng.fader_slots[1].source_b_name, "Loop 3") == 0);
	remove(tmp_json);

	/* 9. In-process bridge: midi_fadester_get_slot_value, midi_fadester_set_fader_value, midi_fadester_fader_changed */
	proc_handler_t *fake_ph = proc_handler_create();
	signal_handler_t *fake_sh = signal_handler_create();
	REQUIRE(fake_ph != NULL && fake_sh != NULL);
	REQUIRE(mfd_bridge_register_with_handlers(fake_ph, fake_sh, &eng));

	struct fake_signal_log sig_log;
	memset(&sig_log, 0, sizeof(sig_log));
	signal_handler_connect(fake_sh, "midi_fadester_fader_changed", on_fake_signal, &sig_log);

	/* Advance tick so slot 1 is ready to emit immediately */
	mfd_engine_tick(&eng, 0.05f);

	/* 9a. Invalid slot over proc_handler returns ok=false and reason sentence */
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_9");
	calldata_set_float(&cd, "value", 0.5);
	REQUIRE(proc_handler_call(fake_ph, "midi_fadester_set_fader_value", &cd));
	REQUIRE(!calldata_bool(&cd, "ok"));
	REQUIRE(strcmp(calldata_string(&cd, "reason"),
		       "Slot 'fader_9' does not exist. Valid slots: crossfader, fader_1 .. fader_8, master.") == 0);
	calldata_free(&cd);

	/* 9b. Valid midi_fadester_set_fader_value emits midi_fadester_fader_changed signal */
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_1");
	calldata_set_float(&cd, "value", 0.625);
	REQUIRE(proc_handler_call(fake_ph, "midi_fadester_set_fader_value", &cd));
	REQUIRE(calldata_bool(&cd, "ok"));
	REQUIRE(strcmp(calldata_string(&cd, "reason"), "") == 0);
	calldata_free(&cd);

	REQUIRE(sig_log.count == 1);
	REQUIRE(strcmp(sig_log.slot, "fader_1") == 0);
	REQUIRE(fabsf(sig_log.value - 0.625f) < 1e-5f);
	REQUIRE(strcmp(sig_log.origin, "api") == 0);

	/* 9c. midi_fadester_get_slot_value reads back 0.625 with ok=true */
	calldata_init(&cd);
	calldata_set_string(&cd, "slot", "fader_1");
	REQUIRE(proc_handler_call(fake_ph, "midi_fadester_get_slot_value", &cd));
	REQUIRE(calldata_bool(&cd, "ok"));
	REQUIRE(fabs(calldata_float(&cd, "value") - 0.625) < 1e-5);
	REQUIRE(strcmp(calldata_string(&cd, "reason"), "") == 0);
	calldata_free(&cd);

	signal_handler_disconnect(fake_sh, "midi_fadester_fader_changed", on_fake_signal, &sig_log);
	mfd_bridge_register_with_handlers(NULL, NULL, NULL);
	signal_handler_destroy(fake_sh);
	proc_handler_destroy(fake_ph);

	/*
	 * 10. Rail Behavior Audit (R-FEEL-4):
	 *     - 10a: Two-source crossfade (crossfade_ab) and single-layer opacity (layer_opacity) at 0 and 100
	 *            (fader jitter at a rail must not flicker the picture; 0 extra output_write_count).
	 *     - 10b: Audio volume (audio_volume) at the bottom rail (no flicker between silent -inf dB and -60 dB).
	 *     - 10c: set_fader_value with release and auto_fade reaching a rail over the API (one-shot finish + parked).
	 *     - 10d: nudge_fader right after a completed throw (Stream Deck dial on T-bar and two-rail slots).
	 *     - 10e: "Finish T-bar" press (one-shot finish when active, no-op when already parked).
	 *     - get_state exposes `"parked": true | false` per API-CONTRACT.md.
	 */
	mfd_midi_queue_init(&eng.midi_queue);
	mfd_midi_parser_init(&eng.midi_parser);
	eng.learning_slot_index = -1;
	eng.tbar_bidirectional_flip = true;
	mfd_engine_load_apc40_defaults(&eng);
	mfd_engine_set_smooth_tau_ms(&eng, 0.0f);

	/* Configure fader_1 (Ch 1, CC 7) as crossfade_ab and fader_2 (Ch 2, CC 7) as layer_opacity */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_string(req, "target_mode", "crossfade_ab");
	obs_data_set_string(req, "source_a", "Cam 1");
	obs_data_set_string(req, "source_b", "Loop 3");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "parked"));
	obs_data_release(req);
	obs_data_release(res);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_2");
	obs_data_set_string(req, "target_mode", "layer_opacity");
	obs_data_set_string(req, "source_a", "Cam 1");
	mfd_vendor_handle_set_slot(req, res, &eng);
	REQUIRE(!obs_data_get_bool(res, "parked"));
	obs_data_release(req);
	obs_data_release(res);

	/* 10a-1: Two-source crossfade (fader_1, Ch 0 CC 7) at 100 and 0 */
	push_midi_cc(&eng, 0, 7, 0);
	push_midi_cc(&eng, 0, 7, 64);
	push_midi_cc(&eng, 0, 7, 115); /* 115/127 = 0.9055 >= 0.90 -> snaps one-shot to 1.0 and parks */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_a - 1.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 1.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	uint32_t writes_xfade_top = eng.fader_slots[1].output_write_count;

	/* Rail travel + jitter at 100% rail (CC 120, 127, 122, 126, 118, 127 — all within 8% of peak 127) */
	push_midi_cc(&eng, 0, 7, 120);
	push_midi_cc(&eng, 0, 7, 127);
	push_midi_cc(&eng, 0, 7, 122);
	push_midi_cc(&eng, 0, 7, 126);
	push_midi_cc(&eng, 0, 7, 118);
	push_midi_cc(&eng, 0, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[1].output_write_count == writes_xfade_top);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 1.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	/* Retreat > 8% (CC 114 = 0.8976 < 1.0 - 0.08) unparks with zero jump, then sweep down to <= 10% (CC 11) */
	push_midi_cc(&eng, 0, 7, 114);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.0f) < 1e-6f);

	push_midi_cc(&eng, 0, 7, 57);
	push_midi_cc(&eng, 0, 7, 11); /* 11/114 = 0.0965 <= 0.10 -> snaps one-shot to 0.0 and parks */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	uint32_t writes_xfade_bot = eng.fader_slots[1].output_write_count;

	/* Rail travel + jitter at 0% rail (CC 5, 0, 4, 1, 9, 0 — all within 8% of peak 0) -> 0 writes */
	push_midi_cc(&eng, 0, 7, 5);
	push_midi_cc(&eng, 0, 7, 0);
	push_midi_cc(&eng, 0, 7, 4);
	push_midi_cc(&eng, 0, 7, 1);
	push_midi_cc(&eng, 0, 7, 9);
	push_midi_cc(&eng, 0, 7, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[1].output_write_count == writes_xfade_bot);
	REQUIRE(fabsf(eng.fader_slots[1].last_alpha_b - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	/* 10a-2: Single-layer opacity (fader_2, Ch 1 CC 7, starts at 1.0) at 0 and 100 */
	push_midi_cc(&eng, 1, 7, 127);
	push_midi_cc(&eng, 1, 7, 64);
	push_midi_cc(&eng, 1, 7, 12); /* 12/127 = 0.0945 <= 0.10 -> snaps one-shot to 0.0 and parks */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[2].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[2].last_alpha_a - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 2));
	uint32_t writes_layer_bot = eng.fader_slots[2].output_write_count;

	/* Bottom rail jitter on layer_opacity -> 0 writes */
	push_midi_cc(&eng, 1, 7, 4);
	push_midi_cc(&eng, 1, 7, 0);
	push_midi_cc(&eng, 1, 7, 7);
	push_midi_cc(&eng, 1, 7, 2);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[2].output_write_count == writes_layer_bot);

	/* Unpark upward (> 8% from 0: CC 13) and sweep to top rail (CC 116) -> snaps one-shot to 1.0 and parks */
	push_midi_cc(&eng, 1, 7, 13);
	push_midi_cc(&eng, 1, 7, 64);
	push_midi_cc(&eng, 1, 7, 116);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[2].last_applied_val - 1.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[2].last_alpha_a - 1.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 2));
	uint32_t writes_layer_top = eng.fader_slots[2].output_write_count;

	/* Top rail jitter on layer_opacity -> 0 writes (never toggles >= 0.999 shader bypass) */
	push_midi_cc(&eng, 1, 7, 127);
	push_midi_cc(&eng, 1, 7, 121);
	push_midi_cc(&eng, 1, 7, 126);
	push_midi_cc(&eng, 1, 7, 118);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[2].output_write_count == writes_layer_top);

	/* 10b: Audio volume (fader_3, Ch 2 CC 7) at the bottom rail (no flicker between silent and -60 dB) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_3");
	obs_data_set_string(req, "target_mode", "audio_volume");
	obs_data_set_string(req, "source_a", "Cam 1");
	mfd_vendor_handle_set_slot(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);

	push_midi_cc(&eng, 2, 7, 127);
	push_midi_cc(&eng, 2, 7, 64);
	push_midi_cc(&eng, 2, 7, 12); /* 12/127 = 0.0945 <= 0.10 (-60 dB threshold) -> snaps one-shot to silent 0.0 */
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(fabsf(eng.fader_slots[3].last_audio_mul - 0.0f) < 1e-6f);
	REQUIRE(!isfinite(eng.fader_slots[3].last_audio_db)); /* -inf dB (silent), not -60 dB */
	REQUIRE(get_slot_parked_via_get_state(&eng, 3));
	uint32_t writes_audio_bot = eng.fader_slots[3].output_write_count;

	/* Jitter at bottom rail (CC 6, 0, 9, 2, 10, 0) produces 0 volume writes and stays silent (-inf dB) */
	push_midi_cc(&eng, 2, 7, 6);
	push_midi_cc(&eng, 2, 7, 0);
	push_midi_cc(&eng, 2, 7, 9);
	push_midi_cc(&eng, 2, 7, 2);
	push_midi_cc(&eng, 2, 7, 10);
	push_midi_cc(&eng, 2, 7, 0);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(eng.fader_slots[3].output_write_count == writes_audio_bot);
	REQUIRE(fabsf(eng.fader_slots[3].last_audio_mul - 0.0f) < 1e-6f);
	REQUIRE(!isfinite(eng.fader_slots[3].last_audio_db));
	REQUIRE(get_slot_parked_via_get_state(&eng, 3));

	/* Advance > 8% (CC 13 = 0.1024 > 0.08) unparks smoothly from silent (0.0f) */
	push_midi_cc(&eng, 2, 7, 13);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 3));
	REQUIRE(fabsf(eng.fader_slots[3].last_applied_val - 0.0f) < 1e-6f);

	/* 10c: set_fader_value with release and auto_fade reaching a rail over the API */
	/* 10c-1: set_fader_value with release on two-rail target (fader_1) snaps one-shot to rail and parks */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.50);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 0.80);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 1.0) < 1e-6);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	uint32_t writes_after_api_rel = eng.fader_slots[1].output_write_count;

	/* Repeating set_fader_value at the same rail while parked is a one-shot no-op */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(eng.fader_slots[1].output_write_count == writes_after_api_rel);

	/* 10c-2: auto_fade to 0.0 on fader_1 parks one-shot on completion */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "to", 0.0);
	obs_data_set_int(req, "ms", 100);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));
	mfd_engine_tick(&eng, 0.05f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));
	mfd_engine_tick(&eng, 0.05f);
	REQUIRE(!eng.fader_slots[1].auto_fade_active);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.0f) < 1e-6f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));

	/* 10c-3: auto_fade 0.0 -> 1.0 on T-bar (crossfader) over 10 ticks completes ONE-SHOT at 90% and stops */
	g_mock_tbar_calls = 0;
	g_mock_tbar_completions = 0;
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "to", 1.0);
	obs_data_set_int(req, "ms", 100);
	mfd_vendor_handle_auto_fade(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);

	for (int tick = 0; tick < 11; ++tick) {
		mfd_engine_tick(&eng, 0.01f);
	}
	REQUIRE(g_mock_tbar_completions == 1);
	REQUIRE(!eng.fader_slots[0].auto_fade_active);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));
	REQUIRE(fabsf(eng.fader_slots[0].last_applied_val - 0.0f) < 1e-6f);

	/* 10d: nudge_fader right after a completed throw (Stream Deck dial) */
	/* 10d-1: Physical MIDI throw 0 -> 127 on crossfader (T-bar), then negative Stream Deck dial nudge */
	eng.fader_slots[0].takeover.state = MFD_TAKEOVER_ENGAGED;
	eng.fader_slots[0].takeover.polarity_reversed = false;
	eng.fader_slots[0].takeover.rescale_origin = 0.0f;
	push_midi_cc(&eng, 0, 15, 0);
	push_midi_cc(&eng, 0, 15, 115);
	push_midi_cc(&eng, 0, 15, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* Turn Stream Deck dial counter-clockwise (delta: -0.05) right after completed throw */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "delta", -0.05);
	mfd_vendor_handle_nudge_fader(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.05) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 0));

	/* Second dial click in the same direction (-0.05) continues to 0.10 */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "delta", -0.05);
	mfd_vendor_handle_nudge_fader(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.10) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);

	/* 1-LSB physical MIDI jitter at top rail (CC 126) does NOT jump T-bar away from 0.10 */
	push_midi_cc(&eng, 0, 15, 126);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[0].last_applied_val - 0.10f) < 1e-5f);

	/* 10d-2: Physical MIDI throw 0 -> 127 on fader_1 (crossfade_ab), then small -0.02 dial nudge */
	eng.fader_slots[1].takeover.state = MFD_TAKEOVER_ENGAGED;
	eng.fader_slots[1].takeover.lower_rescale_origin = 0.0f;
	eng.fader_slots[1].takeover.upper_rescale_origin = 1.0f;
	eng.fader_slots[1].takeover.armed_upper_rail = true;
	push_midi_cc(&eng, 0, 7, 0);
	push_midi_cc(&eng, 0, 7, 115);
	push_midi_cc(&eng, 0, 7, 127);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(get_slot_parked_via_get_state(&eng, 1));
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 1.0f) < 1e-6f);

	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "fader_1");
	obs_data_set_double(req, "delta", -0.02);
	mfd_vendor_handle_nudge_fader(req, res, &eng);
	REQUIRE(fabs(obs_data_get_double(res, "value") - 0.98) < 1e-5);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	/* 1-LSB MIDI jitter at top rail (CC 126) does NOT re-snap fader_1 to 1.0 or re-park */
	push_midi_cc(&eng, 0, 7, 126);
	mfd_engine_tick(&eng, 0.016f);
	REQUIRE(fabsf(eng.fader_slots[1].last_applied_val - 0.98f) < 1e-5f);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 1));

	/* 10e: "Finish T-bar" press (one-shot finish when active, no-op when already parked) */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 0.45);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(!get_slot_parked_via_get_state(&eng, 0));

	int calls_before_finish = g_mock_tbar_calls;
	int completions_before_finish = g_mock_tbar_completions;

	/* First "Finish T-bar" press -> completes throw once and parks */
	req = obs_data_create();
	res = obs_data_create();
	obs_data_set_string(req, "slot", "crossfader");
	obs_data_set_double(req, "value", 1.0);
	obs_data_set_bool(req, "release", true);
	mfd_vendor_handle_set_fader_value(req, res, &eng);
	obs_data_release(req);
	obs_data_release(res);
	REQUIRE(g_mock_tbar_calls == calls_before_finish + 1);
	REQUIRE(g_mock_tbar_completions == completions_before_finish + 1);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	/* Second and third "Finish T-bar" presses while already parked -> strict no-op (0 calls) */
	for (int press = 0; press < 2; ++press) {
		req = obs_data_create();
		res = obs_data_create();
		obs_data_set_string(req, "slot", "crossfader");
		obs_data_set_double(req, "value", 1.0);
		obs_data_set_bool(req, "release", true);
		mfd_vendor_handle_set_fader_value(req, res, &eng);
		obs_data_release(req);
		obs_data_release(res);
	}
	REQUIRE(g_mock_tbar_calls == calls_before_finish + 1);
	REQUIRE(g_mock_tbar_completions == completions_before_finish + 1);
	REQUIRE(get_slot_parked_via_get_state(&eng, 0));

	mfd_vendor_set_event_sink(NULL, NULL);
	printf("test-vendor-api: PASS\n");
	return 0;
}
