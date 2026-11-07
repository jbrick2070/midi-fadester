/*
 * Tubester - the Deck filter: a Browser source becomes a tape deck (REQUIREMENTS section 3)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The filter holds the settings and the link. The page in data/deck.html plays the tape. Everything the page must know
 * travels as a javascript_event to the deck's own source (R-CTL-2); everything the page reports arrives through the
 * listener (R-LNK-2).
 *
 * Threads and locks. video_tick runs on the graphics thread; filter_add and filter_remove run wherever the filter is
 * added or removed (the UI thread or an obs-websocket thread); reports arrive on the listener thread; properties are
 * built on the UI thread; the check and repair of the parent's settings is queued to the UI thread, because the
 * Browser source's Properties dialog writes those settings there and they have no lock of their own. Two locks,
 * always taken in this order:
 * - attach_mutex covers the attachment and every call into the parent (setup, the showing reference, sending to the
 *   page), so none of that can run after the filter has been detached. The listener and the properties never take
 *   it, and the parent's update signal handler never takes it (it only sets flags), so holding it across calls into
 *   OBS cannot close a cycle among libobs locks. The UI thread takes it in the settings check, so nothing called under
 *   it may wait for the UI thread: libobs's calls here do not, and obs-browser is relied on to hand javascript events
 *   and show changes to its own browser thread without waiting.
 * - mutex covers the deck's state. It is held only for short decisions and is never held across a call into OBS,
 *   because the listener thread takes its own lock and then this one.
 */

#include "tubester/deck-filter.h"
#include "tubester/command.h"
#include "tubester/deck-link.h"
#include "tubester/listener.h"
#include "tubester/random.h"
#include "tubester/speed.h"
#include "tubester/tape-url.h"
#include "obs/property-status.h"
#include "obs/text.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TUB_PARENT_ID "browser_source"
#define TUB_DECK_ID_CHARS 16
#define TUB_HELLO_INTERVAL_S 1.0f
/* self-repair (R-DCK-10): how long a command may take to show in the reports, and how far a reported rate may be off */
#define TUB_REPAIR_GRACE_NS 1000000000ULL
#define TUB_RATE_TOLERANCE_PCT 0.6
#define TUB_COMMAND_RESEND_NS 3000000000ULL
/* After setup changes the Browser source, nothing goes to the page until the source has applied the change; this is a
 * safety net in case its update signal never arrives. */
#define TUB_AWAIT_UPDATE_MAX_NS 5000000000ULL
#define TUB_REGISTER_RETRY_MIN_NS 1000000000ULL
#define TUB_REGISTER_RETRY_MAX_NS 30000000000ULL
/* One deck per Browser source; as many sources as the listener has decks */
#define TUB_OWNERS_MAX 64

enum tape_status {
	TAPE_NONE,     /* no file chosen */
	TAPE_UNUSABLE, /* a path tub_path_is_usable refuses */
	TAPE_SET,
};

enum deck_role {
	ROLE_DETACHED,
	ROLE_WRONG_PARENT, /* not on a Browser source (R-DCK-1) */
	ROLE_PENDING,      /* on a Browser source, not claimed yet (the next tick claims it) */
	ROLE_WAITING,      /* another deck already drives this Browser source */
	ROLE_OWNER,
};

struct tub_deck_filter {
	obs_source_t *context;
	pthread_mutex_t attach_mutex;
	pthread_mutex_t mutex;

	/* The attachment. Written with both locks held, read with either. */
	obs_weak_source_t *parent;
	/* the parent libobs attached this filter to; compared with obs_filter_get_parent to notice an attachment a
	 * racing remove left behind, never dereferenced */
	obs_source_t *parent_key;
	bool parent_is_browser;
	enum deck_role role;
	/* attach_mutex only */
	signal_handler_t *parent_signals;
	bool showing_held;
	bool waiting_logged;

	char *page_url; /* file:/// address of data/deck.html, or NULL when the data folder is missing; set at create */

	char deck_id[TUB_DECK_ID_CHARS + 2];
	char token[TUB_TOKEN_LEN + 1];
	/* graphics thread (and create, destroy) only */
	bool registered;
	uint64_t register_retry_ns;
	uint64_t register_backoff_ns;

	/* everything from here on: mutex */
	struct tub_link link;

	/* settings, as last read */
	char *tape;
	char *tape_url;
	enum tape_status tape_status;
	int speed_pct;
	bool tape_pitch;
	bool loop;
	double start_seconds;

	/* the parent's settings must be checked: after attach and after each parent update */
	bool recheck_parent;
	/* a check is queued to the UI thread, where the Browser source's Properties dialog also writes its settings */
	bool setup_queued;
	/* from the moment a check is needed until the parent shows the deck page: nothing goes to the page, so a page the
	 * Browser source showed before can never receive the deck's port and token */
	bool awaiting_parent_update;
	uint64_t awaiting_since_ns;

	/* work for the next tick, set from update and from the listener thread */
	bool send_tape;
	bool send_rate;
	bool send_loop;
	/* no position on the current tape has been confirmed yet, so a (re)sent tape command starts at start_seconds */
	bool tape_from_start;
	double last_position; /* the last position the page confirmed on the current tape */
	float hello_accum;

	/* self-repair from the reports (R-DCK-10) */
	int sent_pct; /* the speed in the last rate command, 0 before any */
	uint64_t last_rate_sent_ns;
	int rate_mismatch_streak;
	bool rate_repair_logged;
	bool current_cmd_is_eject;     /* the command that set link.gen was an eject, not a tape */
	uint64_t pending_cmd_since_ns; /* when that command was last sent; 0 before any */
	bool resend_current;           /* the page never acknowledged it: send it again with the same generation */
	bool resend_logged;

	/* the state last written to the log, so each change is logged once */
	enum tub_deck_state logged_state;
	bool logged_valid;
};

static void release_parent(struct tub_deck_filter *f, obs_source_t *parent_or_null);

/* ---- the owner registry: one deck drives a Browser source ---- */

static pthread_mutex_t g_owner_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
	obs_source_t *parent;
	struct tub_deck_filter *deck;
} g_owners[TUB_OWNERS_MAX];

/* True when f drives parent now, whether it just claimed it or already had. */
static bool claim_parent(struct tub_deck_filter *f, obs_source_t *parent)
{
	pthread_mutex_lock(&g_owner_mutex);
	size_t free_slot = TUB_OWNERS_MAX;
	bool ok = true;
	for (size_t i = 0; i < TUB_OWNERS_MAX; i++) {
		if (!g_owners[i].deck) {
			if (free_slot == TUB_OWNERS_MAX)
				free_slot = i;
		} else if (g_owners[i].parent == parent) {
			ok = g_owners[i].deck == f;
			pthread_mutex_unlock(&g_owner_mutex);
			return ok;
		}
	}
	ok = free_slot < TUB_OWNERS_MAX;
	if (ok) {
		g_owners[free_slot].parent = parent;
		g_owners[free_slot].deck = f;
	}
	pthread_mutex_unlock(&g_owner_mutex);
	return ok;
}

static void release_claim(struct tub_deck_filter *f)
{
	pthread_mutex_lock(&g_owner_mutex);
	for (size_t i = 0; i < TUB_OWNERS_MAX; i++) {
		if (g_owners[i].deck == f) {
			g_owners[i].deck = NULL;
			g_owners[i].parent = NULL;
		}
	}
	pthread_mutex_unlock(&g_owner_mutex);
}

/* ---- every deck, for get_decks (R-INT-4) ---- */

/* Lock order: g_decks_mutex, then a deck's mutex. A deck leaves the list at the start of destroy, so a deck found
 * here under the lock is alive. */
static pthread_mutex_t g_decks_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct tub_deck_filter *g_decks[TUB_OWNERS_MAX];
static size_t g_deck_count;

static void decks_add(struct tub_deck_filter *f)
{
	static bool full_logged = false;
	pthread_mutex_lock(&g_decks_mutex);
	if (g_deck_count < TUB_OWNERS_MAX) {
		g_decks[g_deck_count++] = f;
	} else if (!full_logged) {
		/* far past the two decks Tubester is for; the deck works, it is only missing from get_decks */
		full_logged = true;
		obs_log(LOG_WARNING, "tape deck %s: %d decks already listed; this one is left out of get_decks",
			f->deck_id, TUB_OWNERS_MAX);
	}
	pthread_mutex_unlock(&g_decks_mutex);
}

static void decks_remove(struct tub_deck_filter *f)
{
	pthread_mutex_lock(&g_decks_mutex);
	for (size_t i = 0; i < g_deck_count; i++) {
		if (g_decks[i] == f) {
			g_decks[i] = g_decks[--g_deck_count];
			g_decks[g_deck_count] = NULL;
			break;
		}
	}
	pthread_mutex_unlock(&g_decks_mutex);
}

/* ---- locale ---- */

static const char *state_text(enum tub_deck_state s)
{
	switch (s) {
	case TUB_STATE_NO_CONNECTION:
		return obs_module_text("Tubester.State.NoConnection");
	case TUB_STATE_READY:
		return obs_module_text("Tubester.State.Ready");
	case TUB_STATE_PLAYING:
		return obs_module_text("Tubester.State.Playing");
	case TUB_STATE_PAUSED:
		return obs_module_text("Tubester.State.Paused");
	case TUB_STATE_ENDED:
		return obs_module_text("Tubester.State.Ended");
	case TUB_STATE_CANNOT_PLAY:
		return obs_module_text("Tubester.State.CannotPlay");
	case TUB_STATE_NO_PICTURE:
		return obs_module_text("Tubester.State.NoPicture");
	}
	return "";
}

/* ---- settings ---- */

static void read_settings_locked(struct tub_deck_filter *f, obs_data_t *settings)
{
	const char *tape = obs_data_get_string(settings, TUB_SETTING_TAPE);
	if (!tape)
		tape = "";
	if (!f->tape || strcmp(f->tape, tape) != 0) {
		bfree(f->tape);
		bfree(f->tape_url);
		f->tape = bstrdup(tape);
		f->tape_url = NULL;
		if (tape[0] == '\0') {
			f->tape_status = TAPE_NONE;
		} else if (!tub_path_is_usable(tape)) {
			f->tape_status = TAPE_UNUSABLE;
		} else {
			size_t cap = strlen(tape) * 3 + 16;
			f->tape_url = bmalloc(cap);
			if (tub_path_to_file_url(tape, f->tape_url, cap) == 0) {
				bfree(f->tape_url);
				f->tape_url = NULL;
				f->tape_status = TAPE_UNUSABLE;
			} else {
				f->tape_status = TAPE_SET;
			}
		}
		f->send_tape = true;
		f->tape_from_start = true;
		f->last_position = 0.0;
	}

	int speed = tub_speed_clamp_pct((int)obs_data_get_int(settings, TUB_SETTING_SPEED));
	bool pitch = obs_data_get_bool(settings, TUB_SETTING_TAPE_PITCH);
	if (speed != f->speed_pct || pitch != f->tape_pitch)
		f->send_rate = true;
	f->speed_pct = speed;
	f->tape_pitch = pitch;

	bool loop = obs_data_get_bool(settings, TUB_SETTING_LOOP);
	if (loop != f->loop)
		f->send_loop = true;
	f->loop = loop;

	/* Applies to the next tape load, as the tooltip says; cueing a playing tape is a transport action (PLAN step 5) */
	double start = obs_data_get_double(settings, TUB_SETTING_START);
	f->start_seconds = (isfinite(start) && start >= 0.0) ? start : 0.0;
}

static void deck_update(void *data, obs_data_t *settings)
{
	struct tub_deck_filter *f = data;
	if (!f || !settings)
		return;
	pthread_mutex_lock(&f->mutex);
	read_settings_locked(f, settings);
	pthread_mutex_unlock(&f->mutex);
}

static void deck_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, TUB_SETTING_TAPE, "");
	obs_data_set_default_int(settings, TUB_SETTING_SPEED, TUB_SPEED_NORMAL_PCT);
	obs_data_set_default_bool(settings, TUB_SETTING_TAPE_PITCH, true);
	obs_data_set_default_bool(settings, TUB_SETTING_LOOP, true);
	obs_data_set_default_double(settings, TUB_SETTING_START, 0.0);
}

/* True when the page's latest report is about the tape the deck wants: the latest command's generation, with no newer
 * tape queued. Only then do its position, speed and length describe the deck's tape. */
static bool report_is_current_locked(const struct tub_deck_filter *f)
{
	return f->link.linked && f->link.last.gen == f->link.gen && !f->send_tape;
}

/* ---- reports, on the listener thread ---- */

/* A rate the page did not take (a command lost while the page was busy, a player that reset itself) is sent again:
 * after the grace period, two reports in a row that disagree with the last rate sent. Logged once per occurrence. */
static void check_rate_locked(struct tub_deck_filter *f, uint64_t now)
{
	if (f->sent_pct <= 0 || now < f->last_rate_sent_ns || now - f->last_rate_sent_ns < TUB_REPAIR_GRACE_NS)
		return;
	double reported_pct = f->link.last.rate * 100.0;
	if (fabs(reported_pct - (double)f->sent_pct) <= TUB_RATE_TOLERANCE_PCT) {
		f->rate_mismatch_streak = 0;
		f->rate_repair_logged = false;
		return;
	}
	if (++f->rate_mismatch_streak < 2)
		return;
	f->rate_mismatch_streak = 0;
	f->send_rate = true;
	if (!f->rate_repair_logged) {
		f->rate_repair_logged = true;
		obs_log(LOG_WARNING, "tape deck %s: the page plays at %.0f%%, not %d%%; sending the speed again",
			f->deck_id, reported_pct, f->sent_pct);
	}
}

static void on_report(void *deck, obs_data_t *report, uint64_t now_ns)
{
	struct tub_deck_filter *f = deck;
	pthread_mutex_lock(&f->mutex);
	enum tub_report_verdict v = tub_link_accept(&f->link, report, now_ns);
	switch (v) {
	case TUB_REPORT_NEW_PAGE:
		/* A page that just loaded knows nothing: it gets the tape (or an empty player), the rate and the loop
		 * again (R-LNK-4) */
		f->send_tape = true;
		f->send_rate = true;
		f->send_loop = true;
		f->resend_current = false;
		obs_log(LOG_INFO, "tape deck %s: page connected", f->deck_id);
		break;
	case TUB_REPORT_ACCEPTED:
		/* A position counts only once the page knows the tape (before its metadata it reports 0 whatever the
		 * start), only for the latest tape command, and not while a newer tape waits to be sent. Until then a
		 * resend after a reload starts at start_seconds again. */
		if (f->link.last.gen == f->link.gen) {
			/* the latest command arrived: nothing to send again, and the resend clock stops */
			f->resend_current = false;
			f->resend_logged = false;
			f->pending_cmd_since_ns = 0;
			if (f->link.last.ready_state >= 1 && report_is_current_locked(f)) {
				f->last_position = f->link.last.current_time;
				f->tape_from_start = false;
				check_rate_locked(f, now_ns);
			}
		}
		if (strcmp(f->link.last.event, "watchdog") == 0) {
			obs_log(LOG_WARNING,
				"tape deck %s: the tape had stopped moving; recovered with a seek (R-DCK-8)",
				f->deck_id);
		} else if (strcmp(f->link.last.event, "error") == 0) {
			obs_log(LOG_WARNING, "tape deck %s: the page cannot play this file (media error %d)",
				f->deck_id, f->link.last.error_code);
		} else if (strcmp(f->link.last.event, "nopicture") == 0) {
			obs_log(LOG_WARNING, "tape deck %s: this file has sound but no picture", f->deck_id);
		}
		break;
	case TUB_REPORT_BAD_TOKEN:
		if (f->link.bad_token_count == 1)
			obs_log(LOG_WARNING, "tape deck %s: a report with the wrong token was dropped", f->deck_id);
		break;
	case TUB_REPORT_OLD_GENERATION:
		/* The page is alive but still reports an earlier generation: the latest tape or eject command did not
		 * reach it (an event dropped while its browser was busy). After a while it is sent again. */
		/* not while a newer command is queued: the tick sends that one with a new generation anyway */
		if (!f->send_tape && f->pending_cmd_since_ns && now_ns > f->pending_cmd_since_ns &&
		    now_ns - f->pending_cmd_since_ns > TUB_COMMAND_RESEND_NS) {
			f->resend_current = true;
			f->pending_cmd_since_ns = now_ns;
			if (!f->resend_logged) {
				f->resend_logged = true;
				obs_log(LOG_WARNING,
					"tape deck %s: the page missed the last %s command; sending it again",
					f->deck_id, f->current_cmd_is_eject ? "eject" : "tape");
			}
		}
		break;
	case TUB_REPORT_STALE:
	case TUB_REPORT_MALFORMED:
		break;
	}
	pthread_mutex_unlock(&f->mutex);
}

/* ---- the parent ---- */

/* The parent applied new settings (on the graphics thread, right after its own update read them). Flags only: this
 * runs inside the parent's signal, so it must not take attach_mutex. */
static void on_parent_update(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct tub_deck_filter *f = data;
	pthread_mutex_lock(&f->mutex);
	f->recheck_parent = true;
	f->awaiting_parent_update = false;
	pthread_mutex_unlock(&f->mutex);
}

/* The Browser source settings a deck needs (R-DCK-3). True when something had to change. Called with attach_mutex
 * held, on the UI thread (see run_setup), only after attach and after a parent update, never on a timer. */
static bool setup_parent(struct tub_deck_filter *f, obs_source_t *parent)
{
	obs_data_t *current = obs_source_get_settings(parent);
	bool drift = strcmp(obs_data_get_string(current, "url"), f->page_url) != 0 ||
		     obs_data_get_bool(current, "is_local_file") || !obs_data_get_bool(current, "reroute_audio") ||
		     obs_data_get_bool(current, "shutdown") || obs_data_get_bool(current, "restart_when_active");
	obs_data_release(current);
	if (!drift)
		return false;
	obs_data_t *patch = obs_data_create();
	obs_data_set_string(patch, "url", f->page_url);
	obs_data_set_bool(patch, "is_local_file", false);
	obs_data_set_bool(patch, "reroute_audio", true);
	obs_data_set_bool(patch, "shutdown", false);
	obs_data_set_bool(patch, "restart_when_active", false);
	obs_source_update(parent, patch);
	obs_data_release(patch);
	return true;
}

/* Checks and repairs the parent's settings. Runs on the UI thread: the Browser source's Properties dialog writes those
 * same settings there, and libobs's settings object has no lock of its own. */
static void run_setup(struct tub_deck_filter *f)
{
	pthread_mutex_lock(&f->attach_mutex);
	obs_source_t *parent = NULL;
	if (f->parent && f->role == ROLE_OWNER && f->page_url)
		parent = obs_weak_source_get_source(f->parent);
	pthread_mutex_lock(&f->mutex);
	f->setup_queued = false;
	pthread_mutex_unlock(&f->mutex);
	bool changed = parent && setup_parent(f, parent);
	pthread_mutex_lock(&f->mutex);
	if (changed) {
		/* the page goes on waiting until the parent's update signal says the new address is in place */
		f->awaiting_parent_update = true;
		f->awaiting_since_ns = os_gettime_ns();
	} else if (parent) {
		f->awaiting_parent_update = false;
	}
	pthread_mutex_unlock(&f->mutex);
	pthread_mutex_unlock(&f->attach_mutex);
	if (changed)
		obs_log(LOG_INFO, "tape deck %s: Browser source set up for the deck page", f->deck_id);
	if (parent)
		obs_source_release(parent);
}

struct setup_task {
	obs_weak_source_t *filter;
};

/* The task holds only a weak reference, so a filter destroyed before the task runs is simply skipped. */
static void setup_on_ui_thread(void *param)
{
	struct setup_task *t = param;
	obs_source_t *source = obs_weak_source_get_source(t->filter);
	obs_weak_source_release(t->filter);
	bfree(t);
	if (!source)
		return;
	struct tub_deck_filter *f = obs_obj_get_data(source);
	if (f)
		run_setup(f);
	obs_source_release(source);
}

static void queue_setup(struct tub_deck_filter *f)
{
	struct setup_task *t = bzalloc(sizeof(*t));
	t->filter = obs_source_get_weak_source(f->context);
	obs_queue_task(OBS_TASK_UI, setup_on_ui_thread, t, false);
}

static void send_to_page(obs_source_t *parent, char *json)
{
	if (!json)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(parent);
	if (ph) {
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "eventName", TUB_EVENT_NAME);
		calldata_set_string(&cd, "jsonString", json);
		proc_handler_call(ph, "javascript_event", &cd);
		calldata_free(&cd);
	}
	bfree(json);
}

/* ---- the tick ---- */

/* A deck whose listener registration failed tries again, with a growing pause, without holding any lock. */
static void retry_registration(struct tub_deck_filter *f, uint64_t now)
{
	if (f->registered || !f->token[0] || now < f->register_retry_ns)
		return;
	f->registered = tub_listener_register(f->deck_id, on_report, f);
	if (f->registered) {
		obs_log(LOG_INFO, "tape deck %s: the report listener is up; the deck can link", f->deck_id);
		f->register_backoff_ns = 0;
		return;
	}
	f->register_backoff_ns = f->register_backoff_ns ? f->register_backoff_ns * 2 : TUB_REGISTER_RETRY_MIN_NS;
	if (f->register_backoff_ns > TUB_REGISTER_RETRY_MAX_NS)
		f->register_backoff_ns = TUB_REGISTER_RETRY_MAX_NS;
	f->register_retry_ns = now + f->register_backoff_ns;
}

struct page_commands {
	char *hello;
	char *tape;
	char *eject;
	char *rate;
	char *loop;
};

/* The tape command for generation gen, or an eject when no tape can be sent. Records what was sent so a missed
 * command can be sent again (R-DCK-10). */
static void build_tape_or_eject_locked(struct tub_deck_filter *f, uint64_t gen, uint64_t now, struct page_commands *c)
{
	if (f->tape_status == TAPE_SET) {
		double at = f->tape_from_start ? f->start_seconds : f->last_position;
		c->tape = tub_command_tape(gen, f->tape_url, at, f->loop);
		if (!c->tape)
			obs_log(LOG_WARNING, "tape deck %s: the tape address cannot be sent; ejecting", f->deck_id);
	}
	if (c->tape) {
		f->link.gen = gen;
		f->current_cmd_is_eject = false;
		/* the tape command carries the loop; loading resets the player's rate, so the rate follows it */
		f->send_loop = false;
		f->send_rate = true;
	} else {
		/* no usable tape: the page empties its player and the deck reads Ready with no tape */
		c->eject = tub_command_eject(gen);
		if (c->eject) {
			f->link.gen = gen;
			f->current_cmd_is_eject = true;
		}
		f->last_position = 0.0;
	}
	if (c->tape || c->eject)
		f->pending_cmd_since_ns = now;
}

static void build_commands_locked(struct tub_deck_filter *f, int port, float seconds, uint64_t now,
				  struct page_commands *c)
{
	if (f->awaiting_parent_update)
		return;
	f->hello_accum += seconds;
	if (f->hello_accum >= TUB_HELLO_INTERVAL_S && port > 0 && f->token[0]) {
		f->hello_accum = 0.0f;
		c->hello = tub_command_hello(f->deck_id, port, f->token);
	}
	if (!f->link.linked)
		return;
	if (f->send_tape) {
		f->send_tape = false;
		f->resend_current = false;
		/* a generation is used only by a command that is really sent, so the page never falls behind the link */
		build_tape_or_eject_locked(f, f->link.gen + 1, now, c);
	} else if (f->resend_current) {
		/* the page never acknowledged the latest command: the same command, the same generation */
		f->resend_current = false;
		if (f->link.gen > 0)
			build_tape_or_eject_locked(f, f->link.gen, now, c);
	}
	if (f->send_rate) {
		f->send_rate = false;
		c->rate = tub_command_rate(tub_speed_pct_to_rate(f->speed_pct), f->tape_pitch);
		if (c->rate) {
			f->sent_pct = f->speed_pct;
			f->last_rate_sent_ns = now;
			f->rate_mismatch_streak = 0;
		}
	}
	if (f->send_loop) {
		f->send_loop = false;
		c->loop = tub_command_loop(f->loop);
	}
}

/* Each change of an owning deck's state goes to the log once. Properties is not rebuilt for it: a rebuild takes the
 * focus from whatever is being edited, so the status there is a snapshot with a Refresh Status button (D-12). */
static void log_state_change_locked(struct tub_deck_filter *f)
{
	/* the clock is read under the lock: a report stamped after an earlier reading would look like no connection */
	enum tub_deck_state state = tub_link_state(&f->link, os_gettime_ns());
	if (f->logged_valid && state == f->logged_state)
		return;
	if (f->logged_valid && f->role == ROLE_OWNER)
		obs_log(LOG_INFO, "tape deck %s: %s", f->deck_id, tub_deck_state_name(state));
	f->logged_state = state;
	f->logged_valid = true;
}

static void deck_video_tick(void *data, float seconds)
{
	struct tub_deck_filter *f = data;
	if (!f || !f->context)
		return;

	uint64_t now = os_gettime_ns();
	retry_registration(f, now);
	/* asked before any lock: the listener thread takes its own lock and then a deck's */
	int port = tub_listener_port();
	struct page_commands c = {0};
	obs_source_t *parent = NULL;

	pthread_mutex_lock(&f->attach_mutex);
	if (f->parent_key && obs_filter_get_parent(f->context) != f->parent_key) {
		/* a remove raced the add (libobs calls them outside its filter lock) and left this attachment behind; the
		 * parent no longer lists this filter, so let it go */
		pthread_mutex_unlock(&f->attach_mutex);
		obs_log(LOG_WARNING, "tape deck %s: released an attachment its source had already dropped", f->deck_id);
		release_parent(f, NULL);
		return;
	}
	if (f->parent && f->parent_is_browser && f->page_url)
		parent = obs_weak_source_get_source(f->parent);
	if (parent && f->role != ROLE_OWNER) {
		/* first deck to claim the source drives it; a waiting deck takes over when that deck is removed */
		bool claimed = claim_parent(f, parent);
		pthread_mutex_lock(&f->mutex);
		f->role = claimed ? ROLE_OWNER : ROLE_WAITING;
		if (claimed) {
			f->recheck_parent = true;
			f->hello_accum = TUB_HELLO_INTERVAL_S; /* the first hello goes as soon as the page can get it */
		}
		pthread_mutex_unlock(&f->mutex);
		if (claimed && f->waiting_logged) {
			f->waiting_logged = false;
			obs_log(LOG_INFO, "tape deck %s: the other deck is gone; this one now drives the source",
				f->deck_id);
		} else if (!claimed && !f->waiting_logged) {
			f->waiting_logged = true;
			obs_log(LOG_WARNING, "tape deck %s: another deck already drives this Browser source; waiting",
				f->deck_id);
		}
	}
	if (parent && f->role == ROLE_OWNER) {
		pthread_mutex_lock(&f->mutex);
		bool recheck = f->recheck_parent;
		f->recheck_parent = false;
		/* the clock is read again here: run_setup on the UI thread may have stamped awaiting_since_ns after this
		 * tick's own reading, which would make the difference wrap around and fire the safety net at once */
		uint64_t locked_now = os_gettime_ns();
		if (f->awaiting_parent_update && locked_now >= f->awaiting_since_ns &&
		    locked_now - f->awaiting_since_ns > TUB_AWAIT_UPDATE_MAX_NS) {
			/* safety net: the UI task or the parent's update signal never came; look again */
			f->setup_queued = false;
			recheck = true;
		}
		bool queue = recheck && !f->setup_queued;
		if (queue) {
			f->setup_queued = true;
			f->awaiting_parent_update = true;
			f->awaiting_since_ns = locked_now;
		}
		pthread_mutex_unlock(&f->mutex);
		if (queue)
			queue_setup(f); /* does not wait: the UI thread runs it later */

		if (!f->showing_held) {
			obs_source_inc_showing(parent); /* a hidden deck still loads and plays, silently (R-DCK-4) */
			f->showing_held = true;
		}

		pthread_mutex_lock(&f->mutex);
		build_commands_locked(f, port, seconds, now, &c);
		pthread_mutex_unlock(&f->mutex);

		send_to_page(parent, c.hello);
		send_to_page(parent, c.tape);
		send_to_page(parent, c.eject);
		send_to_page(parent, c.rate);
		send_to_page(parent, c.loop);
	}
	pthread_mutex_lock(&f->mutex);
	log_state_change_locked(f);
	pthread_mutex_unlock(&f->mutex);
	pthread_mutex_unlock(&f->attach_mutex);

	/* last, with no lock held: if this was the parent's final reference, its destruction removes this filter */
	if (parent)
		obs_source_release(parent);
}

/* ---- lifecycle ---- */

static void deck_filter_add(void *data, obs_source_t *parent)
{
	struct tub_deck_filter *f = data;
	if (!f || !parent)
		return;
	const char *id = obs_source_get_unversioned_id(parent);
	bool is_browser = id && strcmp(id, TUB_PARENT_ID) == 0;

	release_parent(f, NULL); /* a filter is added once; this only guards against a missing remove */

	pthread_mutex_lock(&f->attach_mutex);
	obs_weak_source_t *weak = obs_source_get_weak_source(parent);
	signal_handler_t *signals = is_browser ? obs_source_get_signal_handler(parent) : NULL;
	/* the handler only sets flags under mutex, so connecting with attach_mutex held is safe */
	if (signals)
		signal_handler_connect(signals, "update", on_parent_update, f);
	f->parent_signals = signals;
	f->showing_held = false;
	f->waiting_logged = false;
	pthread_mutex_lock(&f->mutex);
	f->parent = weak;
	f->parent_key = parent;
	f->parent_is_browser = is_browser;
	f->role = is_browser ? ROLE_PENDING : ROLE_WRONG_PARENT;
	f->recheck_parent = true;
	f->awaiting_parent_update = false;
	f->hello_accum = TUB_HELLO_INTERVAL_S;
	f->logged_valid = false;
	pthread_mutex_unlock(&f->mutex);
	pthread_mutex_unlock(&f->attach_mutex);

	if (!is_browser) {
		obs_log(LOG_WARNING, "tape deck %s: added to a '%s' source; a deck needs a Browser source (R-DCK-1)",
			f->deck_id, id ? id : "?");
	} else if (!f->page_url) {
		obs_log(LOG_ERROR, "tape deck %s: data/deck.html was not found; the deck cannot start", f->deck_id);
	}
}

static void release_parent(struct tub_deck_filter *f, obs_source_t *parent_or_null)
{
	/* parent_or_null: the parent OBS handed filter_remove, still valid; from destroy it is promoted here */
	pthread_mutex_lock(&f->attach_mutex);
	/* waits for a parent update signal in flight; its handler takes only mutex, which is not held here */
	if (f->parent_signals)
		signal_handler_disconnect(f->parent_signals, "update", on_parent_update, f);
	f->parent_signals = NULL;
	bool held = f->showing_held;
	f->showing_held = false;
	f->waiting_logged = false;

	pthread_mutex_lock(&f->mutex);
	obs_weak_source_t *weak = f->parent;
	bool owner = f->role == ROLE_OWNER;
	f->parent = NULL;
	f->parent_key = NULL;
	f->parent_is_browser = false;
	f->role = ROLE_DETACHED;
	f->recheck_parent = false;
	f->awaiting_parent_update = false;
	f->logged_valid = false;
	pthread_mutex_unlock(&f->mutex);

	obs_source_t *promoted = NULL;
	if (held) {
		obs_source_t *parent = parent_or_null;
		if (!parent && weak)
			parent = promoted = obs_weak_source_get_source(weak);
		if (parent)
			obs_source_dec_showing(parent);
	}
	if (owner)
		release_claim(f);
	pthread_mutex_unlock(&f->attach_mutex);

	if (promoted)
		obs_source_release(promoted);
	if (weak)
		obs_weak_source_release(weak);
}

static void deck_filter_remove(void *data, obs_source_t *parent)
{
	struct tub_deck_filter *f = data;
	if (!f)
		return;
	release_parent(f, parent);
}

static void *deck_create(obs_data_t *settings, obs_source_t *source)
{
	struct tub_deck_filter *f = bzalloc(sizeof(*f));
	f->context = source;
	if (pthread_mutex_init_recursive(&f->attach_mutex) != 0) {
		bfree(f);
		return NULL;
	}
	if (pthread_mutex_init_recursive(&f->mutex) != 0) {
		pthread_mutex_destroy(&f->attach_mutex);
		bfree(f);
		return NULL;
	}

	char hex[TUB_DECK_ID_CHARS + 1];
	if (tub_random_hex(hex, TUB_DECK_ID_CHARS)) {
		snprintf(f->deck_id, sizeof(f->deck_id), "d%s", hex);
	} else {
		/* no random source: the deck has an id for its logs but no token, and so accepts no report */
		snprintf(f->deck_id, sizeof(f->deck_id), "d%016llx", (unsigned long long)(uintptr_t)f);
	}
	if (tub_random_hex(f->token, TUB_TOKEN_LEN) && tub_link_init(&f->link, f->token)) {
		f->registered = tub_listener_register(f->deck_id, on_report, f);
		if (!f->registered) {
			obs_log(LOG_ERROR, "tape deck %s: the report listener did not start; retrying", f->deck_id);
			f->register_backoff_ns = TUB_REGISTER_RETRY_MIN_NS;
			f->register_retry_ns = os_gettime_ns() + f->register_backoff_ns;
		}
	} else {
		f->token[0] = '\0';
		tub_link_init(&f->link, NULL);
		obs_log(LOG_ERROR, "tape deck %s: no random token; the deck cannot link to its page", f->deck_id);
	}

	/* OBS may give the data file as a path relative to its working directory (portable OBS does); the page needs a
	 * full file:/// address, so it is made absolute first */
	char *page = obs_module_file("deck.html");
	char *abs_page = page ? os_get_abs_path_ptr(page) : NULL;
	const char *page_path = abs_page ? abs_page : page;
	if (page_path) {
		size_t cap = strlen(page_path) * 3 + 16;
		f->page_url = bmalloc(cap);
		if (tub_path_to_file_url(page_path, f->page_url, cap) == 0) {
			bfree(f->page_url);
			f->page_url = NULL;
			obs_log(LOG_ERROR, "tape deck %s: the deck page path cannot be used: %s", f->deck_id,
				page_path);
		}
	}
	bfree(abs_page);
	bfree(page);

	deck_update(f, settings);
	decks_add(f);
	return f;
}

static void deck_destroy(void *data)
{
	struct tub_deck_filter *f = data;
	if (!f)
		return;
	/* first: neither get_decks nor a report callback may reach this memory after these lines */
	decks_remove(f);
	if (f->registered)
		tub_listener_unregister(f);
	/* libobs calls filter_remove before destroy, so this normally finds nothing left to release */
	release_parent(f, NULL);
	bfree(f->tape);
	bfree(f->tape_url);
	bfree(f->page_url);
	pthread_mutex_destroy(&f->mutex);
	pthread_mutex_destroy(&f->attach_mutex);
	bfree(f);
}

static void deck_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct tub_deck_filter *f = data;
	if (f && f->context)
		obs_source_skip_video_filter(f->context);
}

static const char *deck_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Tubester.Deck");
}

/* ---- properties ---- */

/* m:ss, or h:mm:ss from an hour on */
static void format_time(double seconds, char *out, size_t size)
{
	if (!isfinite(seconds) || seconds < 0.0)
		seconds = 0.0;
	long long total = (long long)seconds;
	long long h = total / 3600, m = (total / 60) % 60, s = total % 60;
	if (h > 0)
		snprintf(out, size, "%lld:%02lld:%02lld", h, m, s);
	else
		snprintf(out, size, "%lld:%02lld", m, s);
}

static void status_text(struct tub_deck_filter *f, struct dstr *out)
{
	pthread_mutex_lock(&f->mutex);
	enum deck_role role = f->role;
	bool no_page = f->parent_is_browser && !f->page_url;
	enum tape_status tape = f->tape_status;
	enum tub_deck_state state = tub_link_state(&f->link, os_gettime_ns());
	/* the page's own figures, once it knows the current tape (R-DCK-9); a tape still waiting to be sent reads Ready */
	bool known = report_is_current_locked(f) && f->link.last.ready_state >= 1;
	if (f->link.linked && f->send_tape && state != TUB_STATE_NO_CONNECTION)
		state = TUB_STATE_READY;
	double position = f->link.last.current_time, duration = f->link.last.duration, rate = f->link.last.rate;
	pthread_mutex_unlock(&f->mutex);

	if (role == ROLE_WRONG_PARENT) {
		dstr_copy(out, obs_module_text("Tubester.Status.WrongParent"));
		return;
	}
	if (no_page) {
		dstr_copy(out, obs_module_text("Tubester.Status.SetupFailed"));
		return;
	}
	if (role == ROLE_WAITING) {
		dstr_copy(out, obs_module_text("Tubester.Status.Waiting"));
		return;
	}
	dstr_copy(out, state_text(state));
	if (tape == TAPE_NONE) {
		dstr_cat(out, " ");
		dstr_cat(out, obs_module_text("Tubester.Status.NoTape"));
	} else if (tape == TAPE_UNUSABLE) {
		dstr_cat(out, " ");
		dstr_cat(out, obs_module_text("Tubester.Status.BadPath"));
	} else if (known && (state == TUB_STATE_PLAYING || state == TUB_STATE_PAUSED || state == TUB_STATE_ENDED)) {
		char pos[32], len[32], line[256];
		int pct = (int)lround(rate * 100.0);
		format_time(position, pos, sizeof(pos));
		if (duration > 0.0) {
			format_time(duration, len, sizeof(len));
			snprintf(line, sizeof(line),
				 vs_text_format("Tubester.Status.Progress", "Speed %d%%, at %s of %s."), pct, pos, len);
		} else {
			snprintf(line, sizeof(line),
				 vs_text_format("Tubester.Status.ProgressNoLength", "Speed %d%%, at %s."), pct, pos);
		}
		dstr_cat(out, " ");
		dstr_cat(out, line);
	}
}

static bool refresh_status_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	/* A true return only redraws the properties already built; asking OBS to rebuild them calls get_properties,
	 * which writes a fresh status (the shader filter's Reload button does the same) */
	struct tub_deck_filter *f = data;
	if (f && f->context)
		obs_source_update_properties(f->context);
	return false;
}

static obs_properties_t *deck_get_properties(void *data)
{
	struct tub_deck_filter *f = data;
	obs_properties_t *props = obs_properties_create();

	struct dstr status = {0};
	if (f)
		status_text(f, &status);
	vs_properties_add_info(props, "tubester_status", status.array ? status.array : "");
	dstr_free(&status);
	obs_properties_add_button2(props, "tubester_refresh", obs_module_text("Tubester.RefreshStatus"),
				   refresh_status_clicked, f);

	struct dstr filter = {0};
	dstr_cat(&filter, obs_module_text("Tubester.FileFilter.Videos"));
	dstr_cat(&filter, " (*.mp4 *.m4v *.mov *.mkv *.webm *.ogv *.avi *.ts *.mts *.m2ts *.3gp);;");
	dstr_cat(&filter, obs_module_text("Tubester.FileFilter.All"));
	dstr_cat(&filter, " (*.*)");
	obs_property_t *p_tape = obs_properties_add_path(props, TUB_SETTING_TAPE, obs_module_text("Tubester.Tape"),
							 OBS_PATH_FILE, filter.array, NULL);
	obs_property_set_long_description(p_tape, obs_module_text("Tubester.Tape.Tooltip"));
	dstr_free(&filter);

	obs_property_t *p_speed = obs_properties_add_int_slider(
		props, TUB_SETTING_SPEED, obs_module_text("Tubester.Speed"), TUB_SPEED_MIN_PCT, TUB_SPEED_MAX_PCT, 1);
	obs_property_int_set_suffix(p_speed, "%");
	obs_property_set_long_description(p_speed, obs_module_text("Tubester.Speed.Tooltip"));

	obs_property_t *p_pitch =
		obs_properties_add_bool(props, TUB_SETTING_TAPE_PITCH, obs_module_text("Tubester.TapePitch"));
	obs_property_set_long_description(p_pitch, obs_module_text("Tubester.TapePitch.Tooltip"));

	obs_properties_add_bool(props, TUB_SETTING_LOOP, obs_module_text("Tubester.Loop"));

	obs_property_t *p_start = obs_properties_add_float(
		props, TUB_SETTING_START, obs_module_text("Tubester.StartSeconds"), 0.0, 359999.0, 0.1);
	obs_property_float_set_suffix(p_start, " s");
	obs_property_set_long_description(p_start, obs_module_text("Tubester.StartSeconds.Tooltip"));

	return props;
}

struct obs_source_info tub_deck_filter_info = {
	.id = TUB_DECK_FILTER_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = deck_get_name,
	.create = deck_create,
	.destroy = deck_destroy,
	.get_defaults = deck_get_defaults,
	.get_properties = deck_get_properties,
	.update = deck_update,
	.video_tick = deck_video_tick,
	.video_render = deck_video_render,
	.filter_add = deck_filter_add,
	.filter_remove = deck_filter_remove,
};

void tub_register_deck_filter(void)
{
	obs_register_source(&tub_deck_filter_info);
}

static const char *role_name(enum deck_role role)
{
	switch (role) {
	case ROLE_DETACHED:
		return "detached";
	case ROLE_WRONG_PARENT:
		return "wrong_parent";
	case ROLE_PENDING:
		return "pending";
	case ROLE_WAITING:
		return "waiting";
	case ROLE_OWNER:
		return "owner";
	}
	return "unknown";
}

void tub_decks_describe(obs_data_t *out)
{
	if (!out)
		return;
	struct {
		obs_weak_source_t *parent;
		obs_data_t *item;
	} found[TUB_OWNERS_MAX];
	size_t count = 0;

	pthread_mutex_lock(&g_decks_mutex);
	for (size_t i = 0; i < g_deck_count && count < TUB_OWNERS_MAX; i++) {
		struct tub_deck_filter *f = g_decks[i];
		obs_data_t *item = obs_data_create();
		const char *filter_name = obs_source_get_name(f->context);
		obs_data_set_string(item, "filter", filter_name ? filter_name : "");
		obs_data_set_string(item, "deck_id", f->deck_id);

		pthread_mutex_lock(&f->mutex);
		obs_weak_source_t *parent = f->parent;
		if (parent)
			obs_weak_source_addref(parent);
		obs_data_set_string(item, "role", role_name(f->role));
		/* the clock read under the deck's lock, so a report that just landed is not taken for silence; a tape still
		 * waiting to be sent reads Ready, as in Properties */
		enum tub_deck_state state = tub_link_state(&f->link, os_gettime_ns());
		if (f->link.linked && f->send_tape && state != TUB_STATE_NO_CONNECTION)
			state = TUB_STATE_READY;
		obs_data_set_string(item, "state", tub_deck_state_name(state));
		obs_data_set_string(item, "tape", f->tape ? f->tape : "");
		obs_data_set_int(item, "speed_percent", f->speed_pct);
		obs_data_set_bool(item, "tape_pitch", f->tape_pitch);
		obs_data_set_bool(item, "loop", f->loop);
		obs_data_set_double(item, "start_seconds", f->start_seconds);
		obs_data_set_bool(item, "linked", f->link.linked);
		if (f->link.linked) {
			/* what the page last said, as it said it (R-DCK-7): an earlier tape's report is marked as such */
			const struct tub_report *r = &f->link.last;
			obs_data_t *reported = obs_data_create();
			obs_data_set_bool(reported, "current_tape", report_is_current_locked(f));
			obs_data_set_double(reported, "rate", r->rate);
			obs_data_set_double(reported, "position", r->current_time);
			obs_data_set_double(reported, "duration", r->duration);
			obs_data_set_bool(reported, "paused", r->paused);
			obs_data_set_bool(reported, "ended", r->ended);
			obs_data_set_int(reported, "ready_state", r->ready_state);
			obs_data_set_int(reported, "error", r->error_code);
			obs_data_set_bool(reported, "no_picture", r->no_picture);
			obs_data_set_obj(item, "reported", reported);
			obs_data_release(reported);
		}
		pthread_mutex_unlock(&f->mutex);

		found[count].parent = parent;
		found[count].item = item;
		count++;
	}
	pthread_mutex_unlock(&g_decks_mutex);

	/* the Browser source's name, read with no lock of ours held */
	obs_data_array_t *decks = obs_data_array_create();
	for (size_t i = 0; i < count; i++) {
		const char *source_name = "";
		obs_source_t *source = found[i].parent ? obs_weak_source_get_source(found[i].parent) : NULL;
		if (source && obs_source_get_name(source))
			source_name = obs_source_get_name(source);
		obs_data_set_string(found[i].item, "source", source_name);
		if (source)
			obs_source_release(source);
		if (found[i].parent)
			obs_weak_source_release(found[i].parent);
		obs_data_array_push_back(decks, found[i].item);
		obs_data_release(found[i].item);
	}
	obs_data_set_bool(out, "success", true);
	obs_data_set_int(out, "api_version", 1);
	obs_data_set_array(out, "decks", decks);
	obs_data_array_release(decks);
}
