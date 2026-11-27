/*
 * Visual Sidechain - Tape Deck filter lifetime tests (headless libobs, no OBS executable)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "tubester/deck-filter.h"
#include "obs/vendor-api.h"

#include <obs-module.h>
#include <util/threading.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- what the plugin module would give: no module, no data folder, no obs-websocket ---- */

const char *obs_module_text(const char *key)
{
	return key;
}

/* no module, so obs_module_file finds no data/deck.html: a deck still attaches to its Browser source, it only never
 * claims it to load the page */
obs_module_t *obs_current_module(void)
{
	return NULL;
}

void vs_vendor_api_emit_decks_changed(void) {}

extern struct obs_source_info tub_deck_filter_info;

/* A stand-in for obs-browser's Browser source: only its id matters to the deck. */
static int g_browser_placeholder;

static const char *browser_get_name(void *unused)
{
	(void)unused;
	return "Browser (test)";
}

static void *browser_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	(void)source;
	return &g_browser_placeholder;
}

static void browser_destroy(void *data)
{
	(void)data;
}

static uint32_t browser_get_size(void *data)
{
	(void)data;
	return 64;
}

static struct obs_source_info test_browser = {
	.id = "browser_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = browser_get_name,
	.create = browser_create,
	.destroy = browser_destroy,
	.get_width = browser_get_size,
	.get_height = browser_get_size,
};

/* The first deck tub_decks_describe lists: its role, source and deck_id. */
static bool first_deck(char *role, char *source, char *deck_id)
{
	obs_data_t *out = obs_data_create();
	tub_decks_describe(out);
	obs_data_array_t *decks = obs_data_get_array(out, "decks");
	bool found = decks && obs_data_array_count(decks) > 0;
	if (found) {
		obs_data_t *d = obs_data_array_item(decks, 0);
		snprintf(role, 32, "%s", obs_data_get_string(d, "role"));
		snprintf(source, 256, "%s", obs_data_get_string(d, "source"));
		snprintf(deck_id, 32, "%s", obs_data_get_string(d, "deck_id"));
		obs_data_release(d);
	}
	obs_data_array_release(decks);
	obs_data_release(out);
	return found;
}

static void destroy_queue_noop(void *param)
{
	(void)param;
}

/* obs_wait_for_destroy_queue needs the video and audio threads, which a headless libobs has not started */
static void drain_destroy_queue(void)
{
	obs_queue_task(OBS_TASK_DESTROY, destroy_queue_noop, NULL, true);
}

static void tick_filter(obs_source_t *filter)
{
	tub_deck_filter_info.video_tick(obs_obj_get_data(filter), 0.016f);
}

/* Paste Filters, Undo, Redo: OBS rebuilds the filter list with obs_source_restore_filters, which gives a filter its
 * parent without calling filter_add. The next tick attaches the deck (review 2026-11-27, finding 10). */
static void test_restored_deck_attaches_on_tick(void)
{
	obs_source_t *browser = obs_source_create_private("browser_source", "Deck Browser", NULL);
	assert(browser);
	obs_source_t *deck = obs_source_create_private(TUB_DECK_FILTER_ID, "Tape Deck", NULL);
	assert(deck);
	obs_data_t *saved = obs_save_source(deck);
	obs_source_release(deck);
	drain_destroy_queue();

	obs_data_array_t *backup = obs_data_array_create();
	obs_data_array_push_back(backup, saved);
	obs_data_release(saved);
	obs_source_restore_filters(browser, backup); /* what Redo does */
	obs_data_array_release(backup);

	obs_source_t *restored = obs_source_get_filter_by_name(browser, "Tape Deck");
	assert(restored);
	char role[32], source[256], deck_id[32];
	assert(first_deck(role, source, deck_id));
	assert(strcmp(role, "detached") == 0); /* no filter_add came */

	tick_filter(restored);
	assert(first_deck(role, source, deck_id));
	assert(strcmp(role, "pending") == 0);
	assert(strcmp(source, "Deck Browser") == 0);

	/* a tick that finds the parent gone for a moment (the undo's first loop clears it) lets go; the next tick, with
	 * the parent back, attaches again */
	obs_data_array_t *again = obs_source_backup_filters(browser);
	obs_source_restore_filters(browser, again);
	obs_data_array_release(again);
	tick_filter(restored);
	assert(first_deck(role, source, deck_id));
	assert(strcmp(role, "pending") == 0 && strcmp(source, "Deck Browser") == 0);

	obs_source_filter_remove(browser, restored);
	obs_source_release(restored);
	obs_source_release(browser);
	drain_destroy_queue();
}

static os_event_t *g_destroy_gate;

static void hold_destroy_queue(void *param)
{
	(void)param;
	os_event_wait(g_destroy_gate);
}

/* A deck_command that arrives after the deck's source was released and before its destroy ran (libobs frees the
 * source's weak control at the release, the destroy waits on the destruction thread) finds no deck and touches no
 * freed memory (review 2026-11-27, finding 2). */
static void test_command_between_release_and_destroy(void)
{
	obs_source_t *deck = obs_source_create_private(TUB_DECK_FILTER_ID, "Doomed Deck", NULL);
	assert(deck);
	char role[32], source[256], deck_id[32];
	assert(first_deck(role, source, deck_id));

	assert(os_event_init(&g_destroy_gate, OS_EVENT_TYPE_MANUAL) == 0);
	obs_queue_task(OBS_TASK_DESTROY, hold_destroy_queue, NULL, false);
	obs_source_release(deck); /* the destroy is queued behind the gate; the deck is still listed */
	for (int i = 0; i < 4; i++) {
		struct tub_deck_command_result r;
		memset(&r, 0, sizeof(r));
		assert(tub_deck_command_apply(deck_id, TUB_DECK_OP_PLAY_PAUSE, 0, &r) == TUB_DECK_CMD_NOT_FOUND);
	}
	os_event_signal(g_destroy_gate);
	drain_destroy_queue();
	os_event_destroy(g_destroy_gate);

	obs_data_t *out = obs_data_create();
	tub_decks_describe(out);
	obs_data_array_t *decks = obs_data_get_array(out, "decks");
	assert(!decks || obs_data_array_count(decks) == 0);
	obs_data_array_release(decks);
	obs_data_release(out);
}

int main(void)
{
	/* Headless libobs only: no OBS executable, modules, video, audio or user configuration. */
	assert(obs_startup("en-US", NULL, NULL));
	obs_register_source(&test_browser);
	tub_register_deck_filter();

	test_restored_deck_attaches_on_tick();
	test_command_between_release_and_destroy();

	printf("vs-test-tubester-deck: all passed\n");
	return 0;
}
