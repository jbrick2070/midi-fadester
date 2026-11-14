/*
 * Visual Sidechain - Headless obs-websocket 5 Vendor API & Panic Truth Channel Tests (P0-3)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "obs/vendor-api.h"
#include "tubester/deck-filter.h"

#include <util/platform.h>

#include <assert.h>
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
	long long since_ms;
	long long api_version;
};

static int g_vendor_token = 42;
static proc_handler_t *g_fake_ws_ph = NULL;
static struct registered_req g_reqs[16];
static size_t g_req_count = 0;
static struct emitted_event g_events[32];
static size_t g_event_count = 0;
static int g_panic_walks = 0;
static int g_restore_walks = 0;

void tub_decks_describe(obs_data_t *out)
{
	obs_data_set_bool(out, "success", true);
	obs_data_set_int(out, "api_version", 1);
	obs_data_array_t *arr = obs_data_array_create();
	obs_data_set_array(out, "decks", arr);
	obs_data_array_release(arr);
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
	ev->panic = obs_data_get_bool(data, "panic");
	ev->since_ms = obs_data_get_int(data, "since_ms");
	ev->api_version = obs_data_get_int(data, "api_version");
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
	assert(g_req_count == 5u);
	assert(find_req("get_signals") != NULL);
	assert(find_req("get_decks") != NULL);
	assert(find_req("get_safety") != NULL);
	assert(find_req("panic") != NULL);
	assert(find_req("restore") != NULL);

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

int main(void)
{
	test_vendor_safety_requests_and_events();
	puts("test-vendor-api: all assertions passed");
	return 0;
}
