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
 *
 * Hotkeys (D-10). The owning deck registers its hotkeys on the Browser source in the settings check, on the UI thread,
 * because registering reads the source's saved bindings, which saving the scene collection replaces there. They are
 * registered, unregistered and saved with attach_mutex held, so the full order is attach_mutex, then libobs's hotkey
 * lock, then mutex. A hotkey callback runs with that hotkey lock held and hands the change to the UI thread. From an
 * obs-websocket thread the hand-off is queued; for a key press the frontend already calls back on the UI thread, where
 * obs_queue_task runs the task at once, still under the hotkey lock. So the task may take mutex (nothing that holds
 * mutex ever waits for the hotkey lock) and may update the deck's own settings (a video filter's update is only marked,
 * and applied at the next tick), but it must never take attach_mutex or call into the parent. The deck's memory
 * outlives every callback, because the hotkeys are unregistered when the deck lets its parent go, before destroy frees
 * it. When libobs saves a Browser source (which always has hotkeys of its own), the bindings it writes cover only the
 * hotkeys registered at that moment, so the deck also keeps a copy in its own settings (deck_save) and restores it when
 * it registers again.
 *
 * The Speed Fader (R-SPD-5) is read from Visual Sidechain's Midi Fadester bridge at the start of each tick, with no
 * deck lock held.
 */

#include "tubester/deck-filter.h"
#include "tubester/command.h"
#include "tubester/deck-link.h"
#include "tubester/listener.h"
#include "tubester/random.h"
#include "tubester/speed.h"
#include "tubester/tape-url.h"
#include "core/fader-bridge.h"
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
#define S_SPEED_FADER_STATUS "tubester_speed_fader_status"
#define S_SPEED_FADER_CLEAR "tubester_speed_fader_clear"

/* The hotkeys on the deck's Browser source (R-CTL-3). Their names are the keys of the saved bindings, so they never
 * change. */
enum hotkey_action {
	HOTKEY_PLAY_PAUSE,
	HOTKEY_CUE_TO_START,
	HOTKEY_SPEED_UP,
	HOTKEY_SPEED_DOWN,
	HOTKEY_NORMAL_SPEED,
	TUB_HOTKEY_COUNT,
};

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
	obs_hotkey_id hotkeys[TUB_HOTKEY_COUNT];
	bool hotkeys_registered;

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
	bool playing;   /* the transport (R-CTL-4): true plays, false pauses */
	int fader_slot; /* VS_FADER_SLOT_NONE, or a Fadester slot index */
	int fader_min_pct;
	int fader_max_pct;

	/* the speed the deck wants now: the Speed Fader's while it is live, else speed_pct (R-SPD-5) */
	int want_pct;
	bool fader_live;
	/* every speed change passes the coalescer, so the page gets at most one per interval and always the last (R-SPD-3) */
	struct tub_coalesce coalesce;
	int offered_pct; /* the last speed offered to it, 0 before any */

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
	bool send_transport; /* play or pause, as playing says: a change, a repair, or play again after the end */
	bool send_cue;       /* go to start_seconds, keeping the play state */
	/* no position on the current tape has been confirmed yet, so a (re)sent tape command starts at start_seconds */
	bool tape_from_start;
	double last_position; /* the last position the page confirmed on the current tape */
	float hello_accum;

	/* self-repair from the reports (R-DCK-10) */
	int sent_pct; /* the speed in the last rate command, 0 before any */
	uint64_t last_rate_sent_ns;
	int rate_mismatch_streak;
	bool rate_repair_logged;
	uint64_t last_play_sent_ns; /* the last command that set the play state: a tape, a play or a pause */
	int play_mismatch_streak;
	bool play_repair_logged;
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

	/* a new speed reaches the page through the coalescer in the tick; a pitch change is sent at once */
	int speed = tub_speed_clamp_pct((int)obs_data_get_int(settings, TUB_SETTING_SPEED));
	bool pitch = obs_data_get_bool(settings, TUB_SETTING_TAPE_PITCH);
	if (pitch != f->tape_pitch)
		f->send_rate = true;
	f->speed_pct = speed;
	f->tape_pitch = pitch;

	long long slot = obs_data_get_int(settings, TUB_SETTING_SPEED_FADER);
	f->fader_slot = (slot >= 0 && slot < VS_FADER_SLOT_COUNT) ? (int)slot : VS_FADER_SLOT_NONE;
	f->fader_min_pct = tub_speed_clamp_pct((int)obs_data_get_int(settings, TUB_SETTING_SPEED_FADER_MIN));
	f->fader_max_pct = tub_speed_clamp_pct((int)obs_data_get_int(settings, TUB_SETTING_SPEED_FADER_MAX));
	/* the tick works out the fader's speed; without a live fader the slider's applies from now on */
	if (f->fader_slot == VS_FADER_SLOT_NONE)
		f->fader_live = false;
	if (!f->fader_live)
		f->want_pct = speed;

	bool loop = obs_data_get_bool(settings, TUB_SETTING_LOOP);
	if (loop != f->loop)
		f->send_loop = true;
	f->loop = loop;

	/* Applies to the next tape load, to Cue to Start and to play after the end; it never moves a tape by itself */
	double start = obs_data_get_double(settings, TUB_SETTING_START);
	f->start_seconds = (isfinite(start) && start >= 0.0) ? start : 0.0;

	bool playing = obs_data_get_bool(settings, TUB_SETTING_PLAYING);
	if (playing != f->playing)
		f->send_transport = true;
	f->playing = playing;
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
	obs_data_set_default_bool(settings, TUB_SETTING_PLAYING, true);
	obs_data_set_default_int(settings, TUB_SETTING_SPEED_FADER, VS_FADER_SLOT_NONE);
	obs_data_set_default_int(settings, TUB_SETTING_SPEED_FADER_MIN, TUB_SPEED_FADER_MIN_DEFAULT);
	obs_data_set_default_int(settings, TUB_SETTING_SPEED_FADER_MAX, TUB_SPEED_FADER_MAX_DEFAULT);
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

/* A play state the page did not take is sent again the same way (R-DCK-10). A tape that ended, failed, or is still
 * without its metadata is not a disagreement: Ended is a state of its own, and play after the end is the operator's
 * call (D-11). */
static void check_play_locked(struct tub_deck_filter *f, uint64_t now)
{
	const struct tub_report *r = &f->link.last;
	if (f->tape_status != TAPE_SET || f->current_cmd_is_eject || r->ended || r->error_code != 0 ||
	    r->ready_state < 1) {
		f->play_mismatch_streak = 0;
		return;
	}
	if (now < f->last_play_sent_ns || now - f->last_play_sent_ns < TUB_REPAIR_GRACE_NS)
		return;
	if (!r->paused == f->playing) {
		f->play_mismatch_streak = 0;
		f->play_repair_logged = false;
		return;
	}
	if (++f->play_mismatch_streak < 2)
		return;
	f->play_mismatch_streak = 0;
	f->send_transport = true;
	if (!f->play_repair_logged) {
		f->play_repair_logged = true;
		obs_log(LOG_WARNING, "tape deck %s: the page is %s, not %s; sending %s again", f->deck_id,
			r->paused ? "paused" : "playing", f->playing ? "playing" : "paused",
			f->playing ? "play" : "pause");
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
				/* a cue still waiting to be sent keeps its start position for a tape resend */
				if (!f->send_cue) {
					f->last_position = f->link.last.current_time;
					f->tape_from_start = false;
				}
				check_rate_locked(f, now_ns);
				check_play_locked(f, now_ns);
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

/* ---- hotkeys (R-CTL-3, D-10) ---- */

struct hotkey_task {
	obs_weak_source_t *filter;
	enum hotkey_action action;
};

static void hotkey_on_ui_thread(void *param);
static void on_hotkey_play_pause(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void on_hotkey_cue_to_start(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void on_hotkey_speed_up(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void on_hotkey_speed_down(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
static void on_hotkey_normal_speed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

/* in the order Settings > Hotkeys lists them (R-CTL-3) */
static const struct {
	const char *name; /* saved with the Browser source's bindings */
	const char *text_key;
	const char *log_name;
	obs_hotkey_func func;
} k_hotkeys[TUB_HOTKEY_COUNT] = {
	{"tubester_deck.play_pause", "Tubester.Hotkey.PlayPause", "Play / Pause", on_hotkey_play_pause},
	{"tubester_deck.cue_to_start", "Tubester.Hotkey.CueToStart", "Cue to Start", on_hotkey_cue_to_start},
	{"tubester_deck.speed_up", "Tubester.Hotkey.SpeedUp", "Speed Up", on_hotkey_speed_up},
	{"tubester_deck.speed_down", "Tubester.Hotkey.SpeedDown", "Speed Down", on_hotkey_speed_down},
	{"tubester_deck.normal_speed", "Tubester.Hotkey.NormalSpeed", "Normal Speed", on_hotkey_normal_speed},
};

/* Cue to Start (R-CTL-4): the tape goes to start_seconds and keeps its play state. False when there is no tape to cue.
 * mutex held. */
static bool request_cue_locked(struct tub_deck_filter *f)
{
	if (f->tape_status != TAPE_SET)
		return false;
	f->send_cue = true;
	/* a tape command that goes first (a page reload) starts there too */
	f->tape_from_start = true;
	f->last_position = f->start_seconds;
	return true;
}

/* Runs with libobs's hotkey lock held, on the UI thread or an obs-websocket thread. It takes no deck lock and hands the
 * change to the UI thread, where it may run at once (see the file comment). f is alive: its hotkeys are unregistered
 * before it is freed. */
static void hotkey_pressed(void *data, enum hotkey_action action, bool pressed)
{
	struct tub_deck_filter *f = data;
	if (!pressed || !f || !f->context)
		return;
	struct hotkey_task *t = bzalloc(sizeof(*t));
	t->filter = obs_source_get_weak_source(f->context);
	t->action = action;
	obs_queue_task(OBS_TASK_UI, hotkey_on_ui_thread, t, false);
}

static void on_hotkey_play_pause(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	hotkey_pressed(data, HOTKEY_PLAY_PAUSE, pressed);
}

static void on_hotkey_cue_to_start(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	hotkey_pressed(data, HOTKEY_CUE_TO_START, pressed);
}

static void on_hotkey_speed_up(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	hotkey_pressed(data, HOTKEY_SPEED_UP, pressed);
}

static void on_hotkey_speed_down(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	hotkey_pressed(data, HOTKEY_SPEED_DOWN, pressed);
}

static void on_hotkey_normal_speed(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	hotkey_pressed(data, HOTKEY_NORMAL_SPEED, pressed);
}

/* Play / Pause flips the playing setting, as the checkbox would. On a tape that has ended it plays the tape again from
 * the start position instead (D-11). */
static void play_pause_hotkey(struct tub_deck_filter *f, obs_source_t *source)
{
	obs_data_t *settings = obs_source_get_settings(source);
	bool playing = obs_data_get_bool(settings, TUB_SETTING_PLAYING);
	obs_data_release(settings);

	pthread_mutex_lock(&f->mutex);
	/* Ended by the latest report, with no restart or cue already on its way: a report older than the last play or tape
	 * command, or one while a cue or play waits to be sent, does not count, so a quick second press pauses rather than
	 * restarting again */
	bool ended = report_is_current_locked(f) && f->link.last.ended && !f->send_cue && !f->send_transport &&
		     f->link.last_report_ns >= f->last_play_sent_ns;
	if (ended) {
		/* a restart is a cue plus play: the start position holds through a page reload in between, and the play goes
		 * whatever write of the playing setting is still waiting for the tick */
		request_cue_locked(f);
		f->send_transport = true;
	}
	pthread_mutex_unlock(&f->mutex);

	bool next = ended ? true : !playing;
	if (next != playing) {
		obs_data_t *patch = obs_data_create();
		obs_data_set_bool(patch, TUB_SETTING_PLAYING, next);
		obs_source_update(source, patch);
		obs_data_release(patch);
	}
	obs_log(LOG_INFO, "tape deck %s: Play / Pause hotkey; %s", f->deck_id,
		ended  ? "the tape had ended, so it plays again from the start position"
		: next ? "playing"
		       : "paused");
}

static void cue_hotkey(struct tub_deck_filter *f)
{
	pthread_mutex_lock(&f->mutex);
	bool cued = request_cue_locked(f);
	double start = f->start_seconds;
	pthread_mutex_unlock(&f->mutex);
	if (cued)
		obs_log(LOG_INFO, "tape deck %s: Cue to Start hotkey; to %.1f s", f->deck_id, start);
	else
		obs_log(LOG_INFO, "tape deck %s: Cue to Start hotkey; no tape to cue", f->deck_id);
}

/* The transport hotkeys work as the Properties controls do. The speed hotkeys move the Speed setting along the ladder,
 * as the Speed slider would; while a Speed Fader is live it keeps driving the speed, and the new setting applies once
 * the fader is cleared (R-SPD-5). For a key press this runs inside the hotkey callback, under libobs's hotkey lock: it
 * may take mutex and update the deck's own settings, and must never take attach_mutex or call into the parent. */
static void hotkey_on_ui_thread(void *param)
{
	struct hotkey_task *t = param;
	enum hotkey_action action = t->action;
	obs_source_t *source = obs_weak_source_get_source(t->filter);
	obs_weak_source_release(t->filter);
	bfree(t);
	if (!source)
		return;
	struct tub_deck_filter *f = obs_obj_get_data(source);
	if (!f) {
		obs_source_release(source);
		return;
	}
	if (action == HOTKEY_PLAY_PAUSE || action == HOTKEY_CUE_TO_START) {
		if (action == HOTKEY_PLAY_PAUSE)
			play_pause_hotkey(f, source);
		else
			cue_hotkey(f);
		obs_source_release(source);
		return;
	}

	obs_data_t *settings = obs_source_get_settings(source);
	int pct = tub_speed_clamp_pct((int)obs_data_get_int(settings, TUB_SETTING_SPEED));
	obs_data_release(settings);
	int next = TUB_SPEED_NORMAL_PCT;
	if (action == HOTKEY_SPEED_UP)
		next = tub_speed_step_pct(pct, 1);
	else if (action == HOTKEY_SPEED_DOWN)
		next = tub_speed_step_pct(pct, -1);
	if (next != pct) {
		obs_data_t *patch = obs_data_create();
		obs_data_set_int(patch, TUB_SETTING_SPEED, next);
		obs_source_update(source, patch);
		obs_data_release(patch);
	}

	pthread_mutex_lock(&f->mutex);
	bool fader_live = f->fader_live;
	pthread_mutex_unlock(&f->mutex);
	obs_log(LOG_INFO, "tape deck %s: %s hotkey; the Speed setting is %d%%%s", f->deck_id,
		k_hotkeys[action].log_name, next,
		fader_live ? " (the Speed Fader drives the speed while it is live)" : "");
	obs_source_release(source);
}

/* A hotkey libobs gave no bindings takes them from the deck's own copy (see deck_save), when the copy belongs to this
 * Browser source. Returns how many hotkeys took bindings. attach_mutex held, on the UI thread. */
static size_t restore_hotkey_bindings(struct tub_deck_filter *f, obs_source_t *parent)
{
	size_t restored = 0;
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_data_t *copy = settings ? obs_data_get_obj(settings, TUB_SETTING_HOTKEY_COPY) : NULL;
	const char *uuid = obs_source_get_uuid(parent);
	if (copy && uuid && strcmp(obs_data_get_string(copy, "source_uuid"), uuid) == 0) {
		for (size_t i = 0; i < TUB_HOTKEY_COUNT; i++) {
			if (f->hotkeys[i] == OBS_INVALID_HOTKEY_ID)
				continue;
			obs_data_array_t *now = obs_hotkey_save(f->hotkeys[i]);
			bool unbound = obs_data_array_count(now) == 0;
			obs_data_array_release(now);
			obs_data_array_t *saved = obs_data_get_array(copy, k_hotkeys[i].name);
			if (unbound && obs_data_array_count(saved) > 0) {
				obs_hotkey_load(f->hotkeys[i], saved);
				restored++;
			}
			obs_data_array_release(saved);
		}
	}
	obs_data_release(copy);
	obs_data_release(settings);
	return restored;
}

/* attach_mutex held, on the UI thread (see the file comment) */
static void register_hotkeys(struct tub_deck_filter *f, obs_source_t *parent)
{
	if (f->hotkeys_registered)
		return;
	for (size_t i = 0; i < TUB_HOTKEY_COUNT; i++)
		f->hotkeys[i] = obs_hotkey_register_source(
			parent, k_hotkeys[i].name, obs_module_text(k_hotkeys[i].text_key), k_hotkeys[i].func, f);
	f->hotkeys_registered = true;
	size_t restored = restore_hotkey_bindings(f, parent);
	const char *name = obs_source_get_name(parent);
	obs_log(LOG_INFO, "tape deck %s: its hotkeys are in Settings > Hotkeys under the Browser source '%s'",
		f->deck_id, name ? name : "");
	if (restored)
		obs_log(LOG_INFO, "tape deck %s: %zu hotkey binding(s) brought back from the deck's own copy",
			f->deck_id, restored);
}

/* Keeps a copy of the hotkey bindings in the deck's own settings, refreshed at every save while the deck has its
 * hotkeys. libobs saves a Browser source's bindings only for the hotkeys registered at that moment, so without the copy
 * a deck removed and put back with Undo would return unbound. A deck without its hotkeys leaves its last copy as it is. The copy names its Browser source, so a pasted or duplicated deck
 * does not take another source's keys. A waiting deck that takes over has no copy of the other deck's bindings: it
 * starts with whatever the source saved. Called from obs_source_save (the UI thread, for a project save or an undo
 * snapshot). */
static void deck_save(void *data, obs_data_t *settings)
{
	struct tub_deck_filter *f = data;
	if (!f || !settings)
		return;
	pthread_mutex_lock(&f->attach_mutex);
	obs_source_t *parent = (f->hotkeys_registered && f->parent) ? obs_weak_source_get_source(f->parent) : NULL;
	if (parent) {
		obs_data_t *copy = obs_data_create();
		const char *uuid = obs_source_get_uuid(parent);
		obs_data_set_string(copy, "source_uuid", uuid ? uuid : "");
		for (size_t i = 0; i < TUB_HOTKEY_COUNT; i++) {
			obs_data_array_t *bindings =
				f->hotkeys[i] != OBS_INVALID_HOTKEY_ID ? obs_hotkey_save(f->hotkeys[i]) : NULL;
			if (bindings) {
				obs_data_set_array(copy, k_hotkeys[i].name, bindings);
				obs_data_array_release(bindings);
			}
		}
		obs_data_set_obj(settings, TUB_SETTING_HOTKEY_COPY, copy);
		obs_data_release(copy);
	}
	pthread_mutex_unlock(&f->attach_mutex);
	/* with no lock held, as in the tick */
	if (parent)
		obs_source_release(parent);
}

/* attach_mutex held, mutex not held: waits for a hotkey callback in flight, which never takes attach_mutex */
static void unregister_hotkeys(struct tub_deck_filter *f)
{
	if (!f->hotkeys_registered)
		return;
	for (size_t i = 0; i < TUB_HOTKEY_COUNT; i++) {
		/* an id the parent's own destruction already removed is ignored */
		if (f->hotkeys[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(f->hotkeys[i]);
		f->hotkeys[i] = OBS_INVALID_HOTKEY_ID;
	}
	f->hotkeys_registered = false;
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

/* Checks and repairs the parent's settings, and gives the owning deck its hotkeys. Runs on the UI thread: the Browser
 * source's Properties dialog writes those same settings there, and libobs's settings object has no lock of its own. */
static void run_setup(struct tub_deck_filter *f)
{
	pthread_mutex_lock(&f->attach_mutex);
	obs_source_t *parent = NULL;
	if (f->parent && f->role == ROLE_OWNER && f->page_url)
		parent = obs_weak_source_get_source(f->parent);
	pthread_mutex_lock(&f->mutex);
	f->setup_queued = false;
	pthread_mutex_unlock(&f->mutex);
	if (parent)
		register_hotkeys(f, parent);
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

/* sent in this order */
struct page_commands {
	char *hello;
	char *tape;
	char *eject;
	char *seek;
	char *transport;
	char *rate;
	char *loop;
};

/* The tape command for generation gen, or an eject when no tape can be sent. Records what was sent so a missed
 * command can be sent again (R-DCK-10). */
static void build_tape_or_eject_locked(struct tub_deck_filter *f, uint64_t gen, uint64_t now, struct page_commands *c)
{
	if (f->tape_status == TAPE_SET) {
		double at = f->tape_from_start ? f->start_seconds : f->last_position;
		c->tape = tub_command_tape(gen, f->tape_url, at, f->loop, f->playing);
		if (!c->tape)
			obs_log(LOG_WARNING, "tape deck %s: the tape address cannot be sent; ejecting", f->deck_id);
	}
	if (c->tape) {
		f->link.gen = gen;
		f->current_cmd_is_eject = false;
		/* the tape command carries the loop and the play state, and starts where a cue would have gone; loading
		 * resets the player's rate, so the rate follows it */
		f->send_loop = false;
		f->send_transport = false;
		f->send_cue = false;
		f->last_play_sent_ns = now;
		f->play_mismatch_streak = 0;
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
	/* Cue and play state go to the tape the page holds (R-CTL-4). With no tape there, or a tape command going now
	 * (which carries both), there is nothing to send. */
	if (c->tape || c->eject || f->tape_status != TAPE_SET || f->current_cmd_is_eject || f->link.gen == 0) {
		f->send_cue = false;
		f->send_transport = false;
	} else {
		if (f->send_cue) {
			f->send_cue = false;
			c->seek = tub_command_seek(f->start_seconds);
			/* a playing deck plays on from there, also after the tape has ended */
			if (f->playing)
				f->send_transport = true;
		}
		if (f->send_transport) {
			f->send_transport = false;
			/* play carries the start position, where a tape that has ended starts again (D-11) */
			c->transport = f->playing ? tub_command_play(f->start_seconds) : tub_command_pause();
			if (c->transport) {
				f->last_play_sent_ns = now;
				f->play_mismatch_streak = 0;
			}
		}
	}
	/* Every speed change passes the coalescer (R-SPD-3). A forced send (a new tape or page, a pitch change, a repair)
	 * goes at once with the latest speed and counts as the interval's send. */
	bool forced = f->send_rate;
	f->send_rate = false;
	int want = f->want_pct;
	bool send_speed = false;
	if (forced) {
		tub_coalesce_note_sent(&f->coalesce, now);
		send_speed = true;
	} else if (want != f->offered_pct) {
		send_speed = tub_coalesce_offer(&f->coalesce, want, now);
	} else {
		/* a held value is the last one offered, which is want */
		int held;
		send_speed = tub_coalesce_flush(&f->coalesce, now, &held);
	}
	f->offered_pct = want;
	if (send_speed) {
		c->rate = tub_command_rate(tub_speed_pct_to_rate(want), f->tape_pitch);
		if (c->rate) {
			f->sent_pct = want;
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

/* The speed the deck wants: the Speed Fader's while one is bound and Midi Fadester is loaded, else the Speed setting's
 * (R-SPD-5). The bridge is asked with no deck lock held. */
static void update_wanted_speed(struct tub_deck_filter *f)
{
	pthread_mutex_lock(&f->mutex);
	int slot = f->fader_slot;
	pthread_mutex_unlock(&f->mutex);

	bool live = false;
	float value = 0.0f;
	if (slot >= 0 && slot < VS_FADER_SLOT_COUNT) {
		/* one slot per call, in turn; Midi Fadester also pushes each change as it happens */
		vs_fader_bridge_poll_once();
		live = vs_fader_bridge_is_midi_fadester_loaded();
		value = vs_fader_bridge_get_slot(slot);
	}

	pthread_mutex_lock(&f->mutex);
	/* a binding changed meanwhile is taken up on the next tick */
	f->fader_live = live && f->fader_slot == slot;
	f->want_pct = f->fader_live ? tub_speed_from_fader(value, f->fader_min_pct, f->fader_max_pct) : f->speed_pct;
	pthread_mutex_unlock(&f->mutex);
}

static void deck_video_tick(void *data, float seconds)
{
	struct tub_deck_filter *f = data;
	if (!f || !f->context)
		return;

	uint64_t now = os_gettime_ns();
	retry_registration(f, now);
	update_wanted_speed(f);
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
		send_to_page(parent, c.seek);
		send_to_page(parent, c.transport);
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
	unregister_hotkeys(f);
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
	for (size_t i = 0; i < TUB_HOTKEY_COUNT; i++)
		f->hotkeys[i] = OBS_INVALID_HOTKEY_ID;
	f->fader_slot = VS_FADER_SLOT_NONE;
	tub_coalesce_init(&f->coalesce);
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

static bool cue_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct tub_deck_filter *f = data;
	if (!f)
		return false;
	pthread_mutex_lock(&f->mutex);
	bool cued = request_cue_locked(f);
	double start = f->start_seconds;
	pthread_mutex_unlock(&f->mutex);
	if (cued)
		obs_log(LOG_INFO, "tape deck %s: Cue to Start; to %.1f s", f->deck_id, start);
	/* nothing in Properties changes */
	return false;
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

/* the slot names the shader filter and the Modulator use */
static const char *const k_fader_text_keys[VS_FADER_SLOT_COUNT] = {
	"Signal.FaderCrossfader", "Signal.Fader1",   "Signal.Fader2",   "Signal.Fader3",   "Signal.Fader4",
	"Signal.Fader5",          "Signal.Fader6",   "Signal.Fader7",   "Signal.Fader8",   "Signal.FaderMaster",
	"Signal.FaderFx1",        "Signal.FaderFx2", "Signal.FaderFx3", "Signal.FaderFx4", "Signal.FaderFx5",
	"Signal.FaderFx6",        "Signal.FaderFx7", "Signal.FaderFx8",
};

/* The Speed Fader's status line. It names no speeds, so dragging the two speed sliders never needs a rebuild. */
static void refresh_speed_fader_status(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *status = props ? obs_properties_get(props, S_SPEED_FADER_STATUS) : NULL;
	if (!status || !settings)
		return;
	long long slot = obs_data_get_int(settings, TUB_SETTING_SPEED_FADER);
	if (slot < 0 || slot >= VS_FADER_SLOT_COUNT) {
		vs_property_info_set_text(status, obs_module_text("Tubester.SpeedFader.StatusFixed"));
		return;
	}
	const char *name = obs_module_text(k_fader_text_keys[slot]);
	char line[512];
	if (vs_fader_bridge_is_midi_fadester_loaded())
		snprintf(line, sizeof(line),
			 vs_text_format("Tubester.SpeedFader.StatusMapped",
					"%s drives the speed, from Speed at Fader Bottom to Speed at Fader Top."),
			 name);
	else
		snprintf(line, sizeof(line),
			 vs_text_format(
				 "Tubester.SpeedFader.StatusNotLoaded",
				 "%s is assigned, but Midi Fadester is not loaded: the Speed slider sets the speed."),
			 name);
	vs_property_info_set_text(status, line);
}

static bool speed_fader_modified(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(property);
	refresh_speed_fader_status(props, settings);
	return true;
}

static bool clear_speed_fader_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	struct tub_deck_filter *f = data;
	if (!f || !f->context)
		return false;
	obs_data_t *patch = obs_data_create();
	obs_data_set_int(patch, TUB_SETTING_SPEED_FADER, VS_FADER_SLOT_NONE);
	obs_source_update(f->context, patch);
	obs_data_release(patch);
	obs_data_t *settings = obs_source_get_settings(f->context);
	refresh_speed_fader_status(props, settings);
	obs_data_release(settings);
	/* redraws the properties already built: the list shows None, the status line its new text */
	return true;
}

static void add_speed_fader_group(obs_properties_t *props, struct tub_deck_filter *f)
{
	/* ask Midi Fadester once for the whole build */
	vs_fader_bridge_poll_once();
	bool fadester_loaded = vs_fader_bridge_is_midi_fadester_loaded();

	obs_properties_t *grp = obs_properties_create();
	obs_property_t *p_slot = obs_properties_add_list(grp, TUB_SETTING_SPEED_FADER,
							 obs_module_text("Tubester.SpeedFader"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p_slot, obs_module_text("Tubester.SpeedFader.None"), VS_FADER_SLOT_NONE);
	const char *not_loaded = obs_module_text("Modulator.MidiFadesterNotLoaded");
	char label[160];
	for (int i = 0; i < VS_FADER_SLOT_COUNT; i++) {
		const char *base = obs_module_text(k_fader_text_keys[i]);
		if (fadester_loaded) {
			obs_property_list_add_int(p_slot, base, i);
		} else {
			snprintf(label, sizeof(label), "%s - %s", base, not_loaded);
			obs_property_list_add_int(p_slot, label, i);
		}
	}
	obs_property_set_long_description(p_slot, obs_module_text("Tubester.SpeedFader.Tooltip"));
	obs_property_set_modified_callback2(p_slot, speed_fader_modified, f);

	obs_property_t *p_min = obs_properties_add_int_slider(grp, TUB_SETTING_SPEED_FADER_MIN,
							      obs_module_text("Tubester.SpeedFader.Min"),
							      TUB_SPEED_MIN_PCT, TUB_SPEED_MAX_PCT, 1);
	obs_property_int_set_suffix(p_min, "%");
	obs_property_t *p_max = obs_properties_add_int_slider(grp, TUB_SETTING_SPEED_FADER_MAX,
							      obs_module_text("Tubester.SpeedFader.Max"),
							      TUB_SPEED_MIN_PCT, TUB_SPEED_MAX_PCT, 1);
	obs_property_int_set_suffix(p_max, "%");

	vs_properties_add_info(grp, S_SPEED_FADER_STATUS, "");
	obs_properties_add_button2(grp, S_SPEED_FADER_CLEAR, obs_module_text("Tubester.SpeedFader.Clear"),
				   clear_speed_fader_clicked, f);
	obs_properties_add_group(props, "tubester_speed_fader_group", obs_module_text("Tubester.SpeedFaderGroup"),
				 OBS_GROUP_NORMAL, grp);

	/* the modified callback writes it too, whenever OBS applies the settings */
	if (f && f->context) {
		obs_data_t *settings = obs_source_get_settings(f->context);
		refresh_speed_fader_status(props, settings);
		obs_data_release(settings);
	}
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

	obs_property_t *p_playing =
		obs_properties_add_bool(props, TUB_SETTING_PLAYING, obs_module_text("Tubester.Playing"));
	obs_property_set_long_description(p_playing, obs_module_text("Tubester.Playing.Tooltip"));
	obs_property_t *p_cue = obs_properties_add_button2(props, "tubester_cue",
							   obs_module_text("Tubester.CueToStart"), cue_clicked, f);
	obs_property_set_long_description(p_cue, obs_module_text("Tubester.CueToStart.Tooltip"));

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

	add_speed_fader_group(props, f);
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
	.save = deck_save,
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
		obs_data_set_int(item, "effective_speed_percent", f->want_pct);
		obs_data_t *fader = obs_data_create();
		const char *slot_id = vs_fader_slot_id(f->fader_slot);
		obs_data_set_int(fader, "slot", f->fader_slot);
		obs_data_set_string(fader, "slot_id", slot_id ? slot_id : "");
		obs_data_set_int(fader, "min_percent", f->fader_min_pct);
		obs_data_set_int(fader, "max_percent", f->fader_max_pct);
		obs_data_set_bool(fader, "live", f->fader_live);
		obs_data_set_obj(item, "speed_fader", fader);
		obs_data_release(fader);
		obs_data_set_bool(item, "tape_pitch", f->tape_pitch);
		obs_data_set_bool(item, "playing", f->playing);
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
