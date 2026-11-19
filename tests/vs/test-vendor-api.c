/*
 * Visual Sidechain - Headless obs-websocket 5 Vendor API & Panic Truth Channel Tests (P0-3)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "obs/vendor-api.h"
#include "tubester/command.h"
#include "tubester/deck-filter.h"
#include "tubester/speed.h"

#include <util/bmem.h>
#include <util/platform.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef void (*ws_req_fn)(obs_data_t *request_data, obs_data_t *response_data, void *priv_data);

struct ws_req_cb_view {
	ws_req_fn callback;
	void *priv_data;
};

struct registered_req {
	char name[64];
	ws_req_fn fn;
	void *priv_data;
};

struct emitted_event {
	char type[64];
	bool panic;
	bool bpm_locked;
	bool success;
	long long since_ms;
	long long api_version;
	long long beat_index;
	long long bar_index;
	double bpm;
	double bar_phase;
	size_t deck_count;
	char first_deck_source[64];
	long long first_deck_speed_pct;
	long long first_deck_effective_pct;
	bool first_deck_playing;
};

struct fake_deck {
	char source[64];
	char deck_id[32];
	char role[32];
	char state[32];
	char tape[128];
	bool has_tape;
	bool playing;
	bool ended;
	bool tape_pitch;
	int speed_pct;
	int want_pct;
	bool fader_live;
	int fader_pct;
	double start_seconds;
	int cue_count;
	struct tub_coalesce coalesce;
	int offered_pct;
	int page_rate_cmds_sent;
	char last_rate_json[256];
};

static int g_vendor_token = 42;
static proc_handler_t *g_fake_ws_ph = NULL;
static struct registered_req g_reqs[16];
static size_t g_req_count = 0;
static struct emitted_event g_events[64];
static size_t g_event_count = 0;
static int g_panic_walks = 0;
static int g_restore_walks = 0;
static struct fake_deck g_fake_decks[8];
static size_t g_fake_deck_count = 0;

void tub_decks_describe(obs_data_t *out)
{
	obs_data_set_bool(out, "success", true);
	obs_data_set_string(out, "version", "0.6.0");
	obs_data_set_int(out, "api_version", 1);
	obs_data_array_t *arr = obs_data_array_create();
	for (size_t i = 0; i < g_fake_deck_count; i++) {
		const struct fake_deck *d = &g_fake_decks[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "source", d->source);
		obs_data_set_string(item, "filter", "Visual Sidechain: Tape Deck");
		obs_data_set_string(item, "deck_id", d->deck_id);
		obs_data_set_string(item, "role", d->role);
		obs_data_set_string(item, "state", d->state);
		obs_data_set_string(item, "tape", d->tape);
		obs_data_set_int(item, "speed_percent", d->speed_pct);
		obs_data_set_int(item, "effective_speed_percent", d->want_pct);
		obs_data_set_bool(item, "playing", d->playing);
		obs_data_t *fader = obs_data_create();
		obs_data_set_int(fader, "slot", d->fader_live ? 0 : -1);
		obs_data_set_string(fader, "slot_id", d->fader_live ? "crossfader" : "");
		obs_data_set_int(fader, "min_percent", 50);
		obs_data_set_int(fader, "max_percent", 200);
		obs_data_set_bool(fader, "live", d->fader_live);
		obs_data_set_obj(item, "speed_fader", fader);
		obs_data_release(fader);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(out, "decks", arr);
	obs_data_array_release(arr);
}

enum tub_deck_cmd_status tub_deck_command_apply(const char *deck_target, enum tub_deck_op op, int arg,
						struct tub_deck_command_result *out)
{
	size_t chosen = g_fake_deck_count;
	int best_rank = 0;
	for (size_t i = 0; i < g_fake_deck_count; i++) {
		int rank = 0;
		if (g_fake_decks[i].source[0] != '\0' && strcmp(g_fake_decks[i].source, deck_target) == 0)
			rank = strcmp(g_fake_decks[i].role, "owner") == 0 ? 3 : 2;
		else if (strcmp(g_fake_decks[i].deck_id, deck_target) == 0)
			rank = 1;
		if (rank > best_rank) {
			best_rank = rank;
			chosen = i;
		}
	}
	if (chosen >= g_fake_deck_count)
		return TUB_DECK_CMD_NOT_FOUND;

	struct fake_deck *d = &g_fake_decks[chosen];
	if (op == TUB_DECK_OP_PLAY_PAUSE) {
		if (d->ended) {
			d->ended = false;
			d->cue_count++;
			d->playing = true;
			snprintf(d->state, sizeof(d->state), "%s", "playing");
		} else {
			d->playing = !d->playing;
			snprintf(d->state, sizeof(d->state), "%s", d->playing ? "playing" : "paused");
		}
	} else if (op == TUB_DECK_OP_CUE_TO_START) {
		if (!d->has_tape)
			return TUB_DECK_CMD_NO_TAPE;
		d->cue_count++;
	} else if (op == TUB_DECK_OP_SPEED_SET) {
		d->speed_pct = tub_speed_clamp_pct(arg);
		d->want_pct = d->fader_live ? d->fader_pct : d->speed_pct;
	} else if (op == TUB_DECK_OP_SPEED_STEP) {
		d->speed_pct = tub_speed_step_n_pct(d->speed_pct, arg);
		d->want_pct = d->fader_live ? d->fader_pct : d->speed_pct;
	} else {
		d->speed_pct = TUB_SPEED_NORMAL_PCT;
		d->want_pct = d->fader_live ? d->fader_pct : d->speed_pct;
	}

	if (out) {
		snprintf(out->source, sizeof(out->source), "%s", d->source);
		snprintf(out->deck_id, sizeof(out->deck_id), "%s", d->deck_id);
		snprintf(out->state, sizeof(out->state), "%s", d->state);
		out->playing = d->playing;
		out->speed_percent = d->speed_pct;
		out->effective_speed_percent = d->want_pct;
		out->speed_fader_live = d->fader_live;
	}
	return TUB_DECK_CMD_OK;
}

/* Mirrors deck_video_tick's per-frame speed coalescer (deck-filter.c:1041-1064): all speed_set calls inside one frame
 * collapse to a single rate command sent on the video frame tick. */
static void fake_deck_video_tick(struct fake_deck *d, uint64_t now_ns)
{
	int want = d->want_pct;
	bool send_speed = false;
	if (want != d->offered_pct) {
		send_speed = tub_coalesce_offer(&d->coalesce, want, now_ns);
	} else {
		int held = 0;
		send_speed = tub_coalesce_flush(&d->coalesce, now_ns, &held);
	}
	d->offered_pct = want;
	if (send_speed) {
		char *json = tub_command_rate(tub_speed_pct_to_rate(want), d->tape_pitch);
		assert(json != NULL);
		snprintf(d->last_rate_json, sizeof(d->last_rate_json), "%s", json);
		bfree(json);
		d->page_rate_cmds_sent++;
	}
}

static void record_walk(bool panic)
{
	if (panic)
		g_panic_walks++;
	else
		g_restore_walks++;
}

static void proc_obs_websocket_api_get_ph(void *unused, calldata_t *cd)
{
	(void)unused;
	calldata_set_ptr(cd, "ph", g_fake_ws_ph);
}

static void proc_vendor_register(void *unused, calldata_t *cd)
{
	(void)unused;
	const char *name = calldata_string(cd, "name");
	assert(name && strcmp(name, VS_VENDOR_NAME) == 0);
	calldata_set_ptr(cd, "vendor", &g_vendor_token);
}

static void proc_vendor_request_register(void *unused, calldata_t *cd)
{
	(void)unused;
	assert(calldata_ptr(cd, "vendor") == &g_vendor_token);
	const char *type = calldata_string(cd, "type");
	const struct ws_req_cb_view *cb = (const struct ws_req_cb_view *)calldata_ptr(cd, "callback");
	assert(type && cb && cb->callback);
	assert(g_req_count < sizeof(g_reqs) / sizeof(g_reqs[0]));
	snprintf(g_reqs[g_req_count].name, sizeof(g_reqs[g_req_count].name), "%s", type);
	g_reqs[g_req_count].fn = cb->callback;
	g_reqs[g_req_count].priv_data = cb->priv_data;
	g_req_count++;
	calldata_set_bool(cd, "success", true);
}

static void proc_vendor_event_emit(void *unused, calldata_t *cd)
{
	(void)unused;
	assert(calldata_ptr(cd, "vendor") == &g_vendor_token);
	const char *type = calldata_string(cd, "type");
	obs_data_t *data = (obs_data_t *)calldata_ptr(cd, "data");
	assert(type && data);
	assert(g_event_count < sizeof(g_events) / sizeof(g_events[0]));
	struct emitted_event *ev = &g_events[g_event_count++];
	memset(ev, 0, sizeof(*ev));
	snprintf(ev->type, sizeof(ev->type), "%s", type);
	ev->success = obs_data_get_bool(data, "success");
	ev->panic = obs_data_get_bool(data, "panic");
	ev->bpm_locked = obs_data_get_bool(data, "bpm_locked");
	ev->since_ms = obs_data_get_int(data, "since_ms");
	ev->api_version = obs_data_get_int(data, "api_version");
	ev->beat_index = obs_data_get_int(data, "beat_index");
	ev->bar_index = obs_data_get_int(data, "bar_index");
	ev->bpm = obs_data_get_double(data, "bpm");
	ev->bar_phase = obs_data_get_double(data, "bar_phase");
	obs_data_array_t *decks = obs_data_get_array(data, "decks");
	if (decks) {
		ev->deck_count = obs_data_array_count(decks);
		if (ev->deck_count > 0) {
			obs_data_t *first = obs_data_array_item(decks, 0);
			snprintf(ev->first_deck_source, sizeof(ev->first_deck_source), "%s",
				 obs_data_get_string(first, "source"));
			ev->first_deck_speed_pct = obs_data_get_int(first, "speed_percent");
			ev->first_deck_effective_pct = obs_data_get_int(first, "effective_speed_percent");
			ev->first_deck_playing = obs_data_get_bool(first, "playing");
			obs_data_release(first);
		}
		obs_data_array_release(decks);
	}
}

static const struct registered_req *find_req(const char *name)
{
	for (size_t i = 0; i < g_req_count; i++) {
		if (strcmp(g_reqs[i].name, name) == 0)
			return &g_reqs[i];
	}
	return NULL;
}

static obs_data_t *call_req(const char *name)
{
	const struct registered_req *r = find_req(name);
	assert(r != NULL);
	obs_data_t *req = obs_data_create();
	obs_data_t *resp = obs_data_create();
	r->fn(req, resp, r->priv_data);
	obs_data_release(req);
	return resp;
}

static void test_vendor_safety_requests_and_events(void)
{
	g_req_count = 0;
	g_event_count = 0;
	g_panic_walks = 0;
	g_restore_walks = 0;
	vs_vendor_api_reset();
	vs_fader_bridge_reset();

	proc_handler_t *global_ph = proc_handler_create();
	signal_handler_t *global_sh = signal_handler_create();
	g_fake_ws_ph = proc_handler_create();

	proc_handler_add(global_ph, "void obs_websocket_api_get_ph(out ptr ph)", proc_obs_websocket_api_get_ph, NULL);
	proc_handler_add(g_fake_ws_ph, "void vendor_register(in string name, out ptr vendor)", proc_vendor_register,
			 NULL);
	proc_handler_add(
		g_fake_ws_ph,
		"void vendor_request_register(in ptr vendor, in string type, in ptr callback, out bool success)",
		proc_vendor_request_register, NULL);
	proc_handler_add(g_fake_ws_ph, "void vendor_event_emit(in ptr vendor, in string type, in ptr data)",
			 proc_vendor_event_emit, NULL);

	vs_vendor_api_set_safety_walk_scheduler(record_walk);
	assert(vs_vendor_api_register_with_ph(global_ph, global_sh));
	assert(g_req_count == 10u);
	assert(find_req("get_signals") != NULL);
	assert(find_req("get_decks") != NULL);
	assert(find_req("deck_command") != NULL);
	assert(find_req("get_safety") != NULL);
	assert(find_req("panic") != NULL);
	assert(find_req("restore") != NULL);
	assert(find_req("tap_tempo") != NULL);
	assert(find_req("set_bpm") != NULL);
	assert(find_req("lock_bpm") != NULL);
	assert(find_req("resync_downbeat") != NULL);

	/* 1. Initial get_safety and get_signals report panic == false, since_ms == 0, api_version == 1. */
	obs_data_t *s0 = call_req("get_safety");
	assert(obs_data_get_bool(s0, "success"));
	assert(obs_data_get_int(s0, "api_version") == 1);
	assert(!obs_data_get_bool(s0, "panic"));
	assert(obs_data_get_int(s0, "since_ms") == 0);
	obs_data_release(s0);

	obs_data_t *sig0 = call_req("get_signals");
	assert(obs_data_get_bool(sig0, "success"));
	assert(obs_data_get_int(sig0, "api_version") == 1);
	assert(!obs_data_get_bool(sig0, "panic"));
	obs_data_release(sig0);

	obs_data_t *decks = call_req("get_decks");
	assert(obs_data_get_bool(decks, "success"));
	assert(obs_data_get_int(decks, "api_version") == 1);
	obs_data_release(decks);

	/* 2. Vendor `panic` enters Panic, sets since_ms > 0, schedules a walk, and emits `safety_changed`. */
	obs_data_t *p1 = call_req("panic");
	assert(obs_data_get_bool(p1, "success"));
	assert(obs_data_get_int(p1, "api_version") == 1);
	assert(obs_data_get_bool(p1, "panic"));
	const long long first_since = obs_data_get_int(p1, "since_ms");
	assert(first_since > 0);
	obs_data_release(p1);
	assert(g_panic_walks == 1);
	assert(g_event_count == 1u);
	assert(strcmp(g_events[0].type, "safety_changed") == 0);
	assert(g_events[0].panic);
	assert(g_events[0].since_ms == first_since);
	assert(g_events[0].api_version == 1);

	/* 3. get_safety and get_signals now report panic == true; a repeat `panic` preserves since_ms and walks again. */
	os_sleep_ms(5);
	obs_data_t *p2 = call_req("panic");
	assert(obs_data_get_bool(p2, "panic"));
	assert(obs_data_get_int(p2, "since_ms") == first_since);
	obs_data_release(p2);
	assert(g_panic_walks == 2);
	assert(g_event_count == 2u);

	obs_data_t *sig1 = call_req("get_signals");
	assert(obs_data_get_bool(sig1, "panic"));
	obs_data_release(sig1);

	obs_data_t *s1 = call_req("get_safety");
	assert(obs_data_get_bool(s1, "panic"));
	assert(obs_data_get_int(s1, "since_ms") == first_since);
	obs_data_release(s1);

	/* 4. Vendor `restore` clears Panic, zeroes since_ms, schedules restore walk, and emits `safety_changed`. */
	obs_data_t *r1 = call_req("restore");
	assert(obs_data_get_bool(r1, "success"));
	assert(obs_data_get_int(r1, "api_version") == 1);
	assert(!obs_data_get_bool(r1, "panic"));
	assert(obs_data_get_int(r1, "since_ms") == 0);
	obs_data_release(r1);
	assert(g_restore_walks == 1);
	assert(g_event_count == 3u);
	assert(strcmp(g_events[2].type, "safety_changed") == 0);
	assert(!g_events[2].panic);
	assert(g_events[2].since_ms == 0);

	/* 5. Keyboard hotkey path (`vs_fader_bridge_trigger_panic` + `vs_vendor_api_emit_safety_changed`) emits event. */
	vs_fader_bridge_trigger_panic();
	vs_vendor_api_emit_safety_changed();
	assert(g_event_count == 4u);
	assert(strcmp(g_events[3].type, "safety_changed") == 0);
	assert(g_events[3].panic);
	assert(g_events[3].since_ms > 0);

	vs_vendor_api_reset();
	vs_fader_bridge_reset();
	proc_handler_destroy(g_fake_ws_ph);
	g_fake_ws_ph = NULL;
	signal_handler_destroy(global_sh);
	proc_handler_destroy(global_ph);
}

static obs_data_t *call_req_with_data(const char *name, obs_data_t *req)
{
	const struct registered_req *r = find_req(name);
	assert(r != NULL);
	obs_data_t *resp = obs_data_create();
	r->fn(req, resp, r->priv_data);
	return resp;
}

static void test_vendor_tempo_requests_and_events(void)
{
	g_req_count = 0;
	g_event_count = 0;
	vs_vendor_api_reset();
	vs_fader_bridge_reset();

	proc_handler_t *global_ph = proc_handler_create();
	signal_handler_t *global_sh = signal_handler_create();
	g_fake_ws_ph = proc_handler_create();

	proc_handler_add(global_ph, "void obs_websocket_api_get_ph(out ptr ph)", proc_obs_websocket_api_get_ph, NULL);
	proc_handler_add(g_fake_ws_ph, "void vendor_register(in string name, out ptr vendor)", proc_vendor_register,
			 NULL);
	proc_handler_add(
		g_fake_ws_ph,
		"void vendor_request_register(in ptr vendor, in string type, in ptr callback, out bool success)",
		proc_vendor_request_register, NULL);
	proc_handler_add(g_fake_ws_ph, "void vendor_event_emit(in ptr vendor, in string type, in ptr data)",
			 proc_vendor_event_emit, NULL);

	assert(vs_vendor_api_register_with_ph(global_ph, global_sh));

	/* 1. Four taps at 500 ms via tap_tempo give 120 BPM within 1 and step beat_index 0..3. */
	for (int i = 0; i < 4; i++) {
		obs_data_t *resp = obs_data_create();
		vs_vendor_tap_tempo_at(50.0 + (double)i * 0.5, resp);
		assert(obs_data_get_bool(resp, "success"));
		assert(obs_data_get_int(resp, "api_version") == 1);
		assert(obs_data_get_int(resp, "beat_index") == i);
		if (i == 3)
			assert(fabs(obs_data_get_double(resp, "bpm") - 120.0) <= 1.0);
		obs_data_release(resp);
	}
	assert(g_event_count == 4u);
	assert(strcmp(g_events[3].type, "signals_changed") == 0);
	assert(g_events[3].beat_index == 3);
	assert(fabs(g_events[3].bpm - 120.0) <= 1.0);

	/* 2. set_bpm validates bounds in one plain sentence and updates BPM on valid input. */
	obs_data_t *bad_req = obs_data_create();
	obs_data_set_double(bad_req, "bpm", 420.0);
	obs_data_t *bad_resp = call_req_with_data("set_bpm", bad_req);
	assert(!obs_data_get_bool(bad_resp, "success"));
	assert(strlen(obs_data_get_string(bad_resp, "error")) > 0);
	obs_data_release(bad_resp);
	obs_data_release(bad_req);
	assert(g_event_count == 4u);

	obs_data_t *set_req = obs_data_create();
	obs_data_set_double(set_req, "bpm", 96.0);
	obs_data_t *set_resp = call_req_with_data("set_bpm", set_req);
	assert(obs_data_get_bool(set_resp, "success"));
	assert(fabs(obs_data_get_double(set_resp, "bpm") - 96.0) < 1e-4);
	obs_data_release(set_resp);
	obs_data_release(set_req);
	assert(g_event_count == 5u);
	assert(fabs(g_events[4].bpm - 96.0) < 1e-4);

	/* 3. lock_bpm validates boolean field and sets bpm_locked. */
	obs_data_t *bad_lock = obs_data_create();
	obs_data_t *bad_lock_resp = call_req_with_data("lock_bpm", bad_lock);
	assert(!obs_data_get_bool(bad_lock_resp, "success"));
	assert(strlen(obs_data_get_string(bad_lock_resp, "error")) > 0);
	obs_data_release(bad_lock_resp);
	obs_data_release(bad_lock);

	obs_data_t *lock_req = obs_data_create();
	obs_data_set_bool(lock_req, "locked", true);
	obs_data_t *lock_resp = call_req_with_data("lock_bpm", lock_req);
	assert(obs_data_get_bool(lock_resp, "success"));
	assert(obs_data_get_bool(lock_resp, "bpm_locked"));
	obs_data_release(lock_resp);
	obs_data_release(lock_req);
	assert(g_event_count == 6u);
	assert(g_events[5].bpm_locked);

	/* 4. resync_downbeat from beat_index == 2 sets beat_index to 0 on response and next event. */
	struct vs_audio_frame mid_bar;
	memset(&mid_bar, 0, sizeof(mid_bar));
	mid_bar.estimated_bpm = 96.0f;
	mid_bar.bpm_locked = true;
	mid_bar.beat = 0.5f;
	mid_bar.beat_index = 2u;
	mid_bar.bar_phase = 0.625f;
	mid_bar.bar_index = 5u;
	mid_bar.audio_present = true;
	vs_fader_bridge_publish_signals(&mid_bar);

	obs_data_t *resync_resp = call_req("resync_downbeat");
	assert(obs_data_get_bool(resync_resp, "success"));
	assert(obs_data_get_int(resync_resp, "beat_index") == 0);
	assert(obs_data_get_double(resync_resp, "bar_phase") == 0.0);
	assert(obs_data_get_int(resync_resp, "bar_index") == 6);
	obs_data_release(resync_resp);

	assert(g_event_count == 7u);
	assert(strcmp(g_events[6].type, "signals_changed") == 0);
	assert(g_events[6].beat_index == 0);
	assert(g_events[6].bar_phase == 0.0);
	assert(g_events[6].bar_index == 6);

	obs_data_t *sig = call_req("get_signals");
	assert(obs_data_get_int(sig, "beat_index") == 0);
	assert(obs_data_get_bool(sig, "bpm_locked"));
	assert(obs_data_get_int(sig, "bar_index") == 6);
	obs_data_release(sig);

	struct calldata cd;
	calldata_init(&cd);
	assert(proc_handler_call(global_ph, "visual_sidechain_get_signals", &cd));
	assert(calldata_int(&cd, "beat_index") == 0);
	assert(calldata_int(&cd, "bar_index") == 6);
	assert(calldata_bool(&cd, "bpm_locked"));
	calldata_free(&cd);

	/* 5. After a tap chain while unlocked, live analyzer frames advance global BPM & bar_index so a later
	 * lock_bpm or new tap chain never publishes stale tap values or rewinds bar_index. */
	obs_data_t *unlock_req = obs_data_create();
	obs_data_set_bool(unlock_req, "locked", false);
	obs_data_t *unlock_resp = call_req_with_data("lock_bpm", unlock_req);
	assert(obs_data_get_bool(unlock_resp, "success"));
	obs_data_release(unlock_resp);
	obs_data_release(unlock_req);

	for (int i = 0; i < 4; i++) {
		obs_data_t *resp = obs_data_create();
		vs_vendor_tap_tempo_at(100.0 + (double)i * 0.5, resp);
		obs_data_release(resp);
	}

	struct vs_audio_frame live_later;
	memset(&live_later, 0, sizeof(live_later));
	live_later.estimated_bpm = 134.0f;
	live_later.bpm_locked = false;
	live_later.beat = 0.25f;
	live_later.beat_index = 2u;
	live_later.bar_phase = 0.5625f;
	live_later.bar_index = 25u;
	live_later.audio_present = true;
	vs_fader_bridge_publish_signals(&live_later);

	obs_data_t *lock_live_req = obs_data_create();
	obs_data_set_bool(lock_live_req, "locked", true);
	obs_data_t *lock_live_resp = call_req_with_data("lock_bpm", lock_live_req);
	assert(obs_data_get_bool(lock_live_resp, "success"));
	assert(fabs(obs_data_get_double(lock_live_resp, "bpm") - 134.0) < 1e-4);
	assert(obs_data_get_int(lock_live_resp, "beat_index") == 2);
	assert(obs_data_get_int(lock_live_resp, "bar_index") == 25);
	obs_data_release(lock_live_resp);
	obs_data_release(lock_live_req);

	obs_data_t *new_tap_resp = obs_data_create();
	vs_vendor_tap_tempo_at(200.0, new_tap_resp);
	assert(obs_data_get_bool(new_tap_resp, "success"));
	assert(obs_data_get_int(new_tap_resp, "bar_index") >= 26);
	obs_data_release(new_tap_resp);

	vs_vendor_api_reset();
	vs_fader_bridge_reset();
	proc_handler_destroy(g_fake_ws_ph);
	g_fake_ws_ph = NULL;
	signal_handler_destroy(global_sh);
	proc_handler_destroy(global_ph);
}

static void assert_deck_cmd_error(obs_data_t *req, const char *expected_sentence)
{
	const size_t before_events = g_event_count;
	obs_data_t *resp = call_req_with_data("deck_command", req);
	assert(!obs_data_get_bool(resp, "success"));
	assert(obs_data_get_int(resp, "api_version") == 1);
	assert(strcmp(obs_data_get_string(resp, "error"), expected_sentence) == 0);
	assert(strcmp(obs_data_get_string(resp, "comment"), expected_sentence) == 0);
	assert(g_event_count == before_events);
	obs_data_release(resp);
}

static void test_vendor_deck_commands_and_events(void)
{
	g_req_count = 0;
	g_event_count = 0;
	g_fake_deck_count = 0;
	vs_vendor_api_reset();
	vs_fader_bridge_reset();

	proc_handler_t *global_ph = proc_handler_create();
	signal_handler_t *global_sh = signal_handler_create();
	g_fake_ws_ph = proc_handler_create();

	proc_handler_add(global_ph, "void obs_websocket_api_get_ph(out ptr ph)", proc_obs_websocket_api_get_ph, NULL);
	proc_handler_add(g_fake_ws_ph, "void vendor_register(in string name, out ptr vendor)", proc_vendor_register,
			 NULL);
	proc_handler_add(
		g_fake_ws_ph,
		"void vendor_request_register(in ptr vendor, in string type, in ptr callback, out bool success)",
		proc_vendor_request_register, NULL);
	proc_handler_add(g_fake_ws_ph, "void vendor_event_emit(in ptr vendor, in string type, in ptr data)",
			 proc_vendor_event_emit, NULL);

	assert(vs_vendor_api_register_with_ph(global_ph, global_sh));

	/* Register two fake decks on "Reel A" (one waiting, one owner) plus a tapeless deck "Empty Reel". */
	memset(g_fake_decks, 0, sizeof(g_fake_decks));
	g_fake_deck_count = 3;

	snprintf(g_fake_decks[0].source, sizeof(g_fake_decks[0].source), "%s", "Reel A");
	snprintf(g_fake_decks[0].deck_id, sizeof(g_fake_decks[0].deck_id), "%s", "d_waiting");
	snprintf(g_fake_decks[0].role, sizeof(g_fake_decks[0].role), "%s", "waiting");
	snprintf(g_fake_decks[0].state, sizeof(g_fake_decks[0].state), "%s", "ready");
	g_fake_decks[0].speed_pct = 50;
	g_fake_decks[0].want_pct = 50;

	struct fake_deck *owner = &g_fake_decks[1];
	snprintf(owner->source, sizeof(owner->source), "%s", "Reel A");
	snprintf(owner->deck_id, sizeof(owner->deck_id), "%s", "d_owner");
	snprintf(owner->role, sizeof(owner->role), "%s", "owner");
	snprintf(owner->state, sizeof(owner->state), "%s", "playing");
	snprintf(owner->tape, sizeof(owner->tape), "%s", "C:\\tapes\\loop.mp4");
	owner->has_tape = true;
	owner->playing = true;
	owner->tape_pitch = true;
	owner->speed_pct = 100;
	owner->want_pct = 100;
	owner->offered_pct = 100;
	tub_coalesce_init(&owner->coalesce);

	struct fake_deck *empty_deck = &g_fake_decks[2];
	snprintf(empty_deck->source, sizeof(empty_deck->source), "%s", "Empty Reel");
	snprintf(empty_deck->deck_id, sizeof(empty_deck->deck_id), "%s", "d_empty");
	snprintf(empty_deck->role, sizeof(empty_deck->role), "%s", "owner");
	snprintf(empty_deck->state, sizeof(empty_deck->state), "%s", "ready");
	empty_deck->has_tape = false;
	empty_deck->playing = true;
	empty_deck->speed_pct = 100;
	empty_deck->want_pct = 100;

	/* 1. Validation sentences (never silent no-ops). */
	obs_data_t *req = obs_data_create();
	assert_deck_cmd_error(req, "Field 'deck' must name an active Tape Deck source.");
	obs_data_set_string(req, "deck", "");
	assert_deck_cmd_error(req, "Field 'deck' must name an active Tape Deck source.");
	obs_data_set_string(req, "deck", "Reel A");
	assert_deck_cmd_error(
		req, "Field 'op' must be one of: play_pause, cue_to_start, speed_set, speed_step, normal_speed.");
	obs_data_set_string(req, "op", "rewind");
	assert_deck_cmd_error(
		req, "Field 'op' must be one of: play_pause, cue_to_start, speed_set, speed_step, normal_speed.");

	obs_data_set_string(req, "deck", "Missing Deck");
	obs_data_set_string(req, "op", "play_pause");
	assert_deck_cmd_error(req, "Tape Deck 'Missing Deck' was not found.");

	obs_data_set_string(req, "deck", "Empty Reel");
	obs_data_set_string(req, "op", "cue_to_start");
	assert_deck_cmd_error(req, "Choose a video file in Tape Deck before cueing to start.");

	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "speed_set");
	assert_deck_cmd_error(req, "Field 'percent' must be a number between 25 and 800.");
	obs_data_set_double(req, "percent", NAN);
	assert_deck_cmd_error(req, "Field 'percent' must be a number between 25 and 800.");
	obs_data_set_double(req, "percent", INFINITY);
	assert_deck_cmd_error(req, "Field 'percent' must be a number between 25 and 800.");
	obs_data_set_double(req, "percent", 24.9);
	assert_deck_cmd_error(req, "Field 'percent' must be between 25 and 800.");
	obs_data_set_double(req, "percent", 800.1);
	assert_deck_cmd_error(req, "Field 'percent' must be between 25 and 800.");

	obs_data_release(req);
	req = obs_data_create();
	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "speed_step");
	assert_deck_cmd_error(req, "Field 'steps' must be a non-zero integer.");
	obs_data_set_int(req, "steps", 0);
	assert_deck_cmd_error(req, "Field 'steps' must be a non-zero integer.");
	obs_data_set_double(req, "steps", 1.5);
	assert_deck_cmd_error(req, "Field 'steps' must be a non-zero integer.");
	obs_data_set_double(req, "steps", NAN);
	assert_deck_cmd_error(req, "Field 'steps' must be a non-zero integer.");
	obs_data_release(req);

	/* 2. play_pause prefers the owner deck on "Reel A", toggles playing, restarts ended tapes, and emits decks_changed. */
	req = obs_data_create();
	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "play_pause");
	obs_data_t *resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(strcmp(obs_data_get_string(resp, "deck_id"), "d_owner") == 0);
	assert(!obs_data_get_bool(resp, "playing"));
	assert(strcmp(obs_data_get_string(resp, "state"), "paused") == 0);
	obs_data_release(resp);
	assert(g_event_count == 1u);
	assert(strcmp(g_events[0].type, "decks_changed") == 0);
	assert(g_events[0].success);
	assert(g_events[0].api_version == 1);
	assert(g_events[0].deck_count == 3u);

	/* Ended tape: play_pause cues to start and plays again (D-11). */
	owner->ended = true;
	owner->playing = true;
	resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(obs_data_get_bool(resp, "playing"));
	assert(owner->cue_count == 1);
	assert(!owner->ended);
	obs_data_release(resp);
	obs_data_release(req);

	/* 3. cue_to_start cues when a tape is loaded. */
	req = obs_data_create();
	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "cue_to_start");
	resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(owner->cue_count == 2);
	obs_data_release(resp);
	obs_data_release(req);

	/* 4. speed_set snaps to 1% integer like the filter window and reports applied value; rapid calls coalesce to one
	 * rate command on the video frame tick. */
	static const double burst_pcts[] = {120.2, 185.7, 250.4, 333.6};
	static const int expected_snapped[] = {120, 186, 250, 334};
	for (size_t i = 0; i < 4; i++) {
		req = obs_data_create();
		obs_data_set_string(req, "deck", "Reel A");
		obs_data_set_string(req, "op", "speed_set");
		obs_data_set_double(req, "percent", burst_pcts[i]);
		resp = call_req_with_data("deck_command", req);
		assert(obs_data_get_bool(resp, "success"));
		assert(obs_data_get_int(resp, "speed_percent") == expected_snapped[i]);
		assert(obs_data_get_int(resp, "effective_speed_percent") == expected_snapped[i]);
		obs_data_release(resp);
		obs_data_release(req);
	}
	assert(owner->page_rate_cmds_sent == 0);
	fake_deck_video_tick(owner, 1000000000ULL);
	assert(owner->page_rate_cmds_sent == 1);
	assert(strstr(owner->last_rate_json, "\"rate\":3.34") != NULL);

	/* 5. speed_step rides the 11-rung ladder signed steps; normal_speed resets to 100%. */
	req = obs_data_create();
	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "normal_speed");
	resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(obs_data_get_int(resp, "speed_percent") == 100);
	obs_data_release(resp);
	obs_data_release(req);

	req = obs_data_create();
	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "speed_step");
	obs_data_set_int(req, "steps", 2);
	resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(obs_data_get_int(resp, "speed_percent") == 150);
	obs_data_release(resp);

	obs_data_set_int(req, "steps", -3);
	resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(obs_data_get_int(resp, "speed_percent") == 75);
	obs_data_release(resp);
	obs_data_release(req);

	/* 6. When a Speed Fader is live, speed_percent updates while effective_speed_percent stays at the fader's speed. */
	owner->fader_live = true;
	owner->fader_pct = 200;
	owner->want_pct = 200;
	req = obs_data_create();
	obs_data_set_string(req, "deck", "Reel A");
	obs_data_set_string(req, "op", "speed_set");
	obs_data_set_int(req, "percent", 125);
	resp = call_req_with_data("deck_command", req);
	assert(obs_data_get_bool(resp, "success"));
	assert(obs_data_get_int(resp, "speed_percent") == 125);
	assert(obs_data_get_int(resp, "effective_speed_percent") == 200);
	assert(obs_data_get_bool(resp, "speed_fader_live"));
	obs_data_release(resp);
	obs_data_release(req);

	/* 7. Panic and Restore leave Tape Decks untouched. */
	obs_data_t *p = call_req("panic");
	assert(obs_data_get_bool(p, "panic"));
	obs_data_release(p);
	assert(owner->playing);
	assert(owner->speed_pct == 125);
	obs_data_t *r = call_req("restore");
	assert(!obs_data_get_bool(r, "panic"));
	obs_data_release(r);
	assert(owner->playing);
	assert(owner->speed_pct == 125);

	g_fake_deck_count = 0;
	vs_vendor_api_reset();
	vs_fader_bridge_reset();
	proc_handler_destroy(g_fake_ws_ph);
	g_fake_ws_ph = NULL;
	signal_handler_destroy(global_sh);
	proc_handler_destroy(global_ph);
}

int main(void)
{
	test_vendor_safety_requests_and_events();
	test_vendor_tempo_requests_and_events();
	test_vendor_deck_commands_and_events();
	puts("test-vendor-api: all assertions passed");
	return 0;
}
