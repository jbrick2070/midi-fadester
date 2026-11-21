/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "obs/mfd-engine.h"
#include "obs/mfd-blackout.h"
#include "obs/mfd-layer-filter.h"
#include "obs/mfd-vendor-api.h"
#include "core/mixer-math.h"
#include "core/slot-messages.h"
#include "core/mfd-text.h"
#include "core/tbar-logic.h"
#include "vs/core/tempo.h"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/darray.h>
#include <util/platform.h>
#include "plugin-support.h"
#include <ctype.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MFD_SAVE_DATA_KEY "midi_fadester_scene_collection"
#define MFD_DEFAULT_PROFILE_NAME "apc40mk2"

static struct mfd_engine g_engine;

static float clamp01(float v)
{
	if (!isfinite(v) || v <= 0.0f) {
		return 0.0f;
	}
	if (v >= 1.0f) {
		return 1.0f;
	}
	return v;
}

static int ascii_stricmp(const char *a, const char *b)
{
	if (!a && !b) {
		return 0;
	}
	if (!a) {
		return -1;
	}
	if (!b) {
		return 1;
	}
	while (*a && *b) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb) {
			return ca - cb;
		}
		++a;
		++b;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* An empty label means the slot's default; the API, logs and saved config read the English default (R-I18N-2). */
void mfd_engine_slot_english_label(const struct mfd_engine *eng, int slot_index, char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	const struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (s->label[0] != '\0') {
		snprintf(out, out_size, "%s", s->label);
	} else {
		mfd_profile_default_slot_label(mfd_engine_get_active_profile(eng), slot_index, out, out_size);
	}
}

static void set_already_controlled_msg(const struct mfd_engine *eng, struct mfd_msg *msg, const char *filter_prop,
				       const char *filter_name, int owner_slot)
{
	char owner_label[MFD_SLOT_LABEL_MAX];
	char owner_index[16];
	mfd_engine_slot_english_label(eng, owner_slot, owner_label, sizeof(owner_label));
	if (owner_label[0] == '\0') {
		snprintf(owner_label, sizeof(owner_label), "%s", mfd_slot_id_for_index(owner_slot));
	}
	snprintf(owner_index, sizeof(owner_index), "%d", owner_slot);
	/* %4 carries the owner's slot index so the UI can show its translated strip title. */
	mfd_msg_set(msg, MFD_MSG_SETTING_ALREADY_CONTROLLED, filter_prop, filter_name, owner_label, owner_index, NULL);
}

static void set_range_outside_msg(struct mfd_msg *msg, double from_val, double to_val, const char *filter_prop,
				  double min_val, double max_val)
{
	char from_text[32];
	char to_text[32];
	char min_text[32];
	char max_text[32];
	snprintf(from_text, sizeof(from_text), "%.6g", from_val);
	snprintf(to_text, sizeof(to_text), "%.6g", to_val);
	snprintf(min_text, sizeof(min_text), "%.6g", min_val);
	snprintf(max_text, sizeof(max_text), "%.6g", max_val);
	mfd_msg_set(msg, MFD_MSG_RANGE_OUTSIDE_LIMITS, from_text, to_text, filter_prop, min_text, max_text);
}

static bool slot_addr_is_14bit(const struct mfd_midi_addr *addr)
{
	if (!addr) {
		return false;
	}
	return addr->is_14bit || (addr->type == MFD_MIDI_PITCH_BEND);
}

/* A row's MIDI Learn ends whenever the strips it belongs to are replaced (layout change, import, dialog closed):
 * otherwise the next control moved, minutes later, silently rebinds a strip. The setup sheet's own Learn is
 * separate and stays. Caller holds the engine lock. */
static void cancel_row_learn_locked(struct mfd_engine *eng)
{
	eng->learning_slot_index = -1;
	if (!eng->setup_session.learning) {
		mfd_midi_parser_cancel_learn(&eng->midi_parser);
	}
}

static bool midi_addr_exact_equal(const struct mfd_midi_addr *a, const struct mfd_midi_addr *b)
{
	if (!a || !b) {
		return false;
	}
	if (a->type == MFD_MIDI_NONE || b->type == MFD_MIDI_NONE) {
		return false;
	}
	if (a->type != b->type || a->channel != b->channel) {
		return false;
	}
	if (a->type == MFD_MIDI_PITCH_BEND) {
		return true;
	}
	return (a->number == b->number) && (a->is_14bit == b->is_14bit);
}

static const char *effective_profile_name(const struct mfd_engine *eng)
{
	if (eng && eng->active_profile_name[0] != '\0') {
		return eng->active_profile_name;
	}
	return MFD_DEFAULT_PROFILE_NAME;
}

bool mfd_engine_init_mutex(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	if (eng->state_mutex_initialized) {
		return true;
	}
	mfd_vj_pads_init(&eng->vj);
	memset(eng->vj_led_sent, 255, sizeof(eng->vj_led_sent));
	eng->vj_refresh_pending = true;
	if (pthread_mutex_init_recursive(&eng->state_mutex) != 0) {
		return false;
	}
	if (pthread_mutex_init(&eng->graph_mutex, NULL) != 0) {
		pthread_mutex_destroy(&eng->state_mutex);
		return false;
	}
	eng->graph_mutex_initialized = true;
	eng->state_mutex_initialized = true;
	eng->lock_depth = 0U;
	return true;
}

void mfd_engine_destroy_mutex(struct mfd_engine *eng)
{
	if (!eng || !eng->state_mutex_initialized) {
		return;
	}
	eng->state_mutex_initialized = false;
	eng->lock_depth = 0U;
	pthread_mutex_destroy(&eng->state_mutex);
	if (eng->graph_mutex_initialized) {
		eng->graph_mutex_initialized = false;
		pthread_mutex_destroy(&eng->graph_mutex);
	}
	bfree(eng->graph_items);
	eng->graph_items = NULL;
	eng->graph_count = 0U;
	eng->graph_cap = 0U;
}

static long current_thread_key(void)
{
#ifdef _WIN32
	return (long)GetCurrentThreadId();
#else
	return (long)(uintptr_t)pthread_self();
#endif
}

void mfd_engine_lock(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	if (eng->state_mutex_initialized) {
		pthread_mutex_lock(&eng->state_mutex);
	}
	if (eng->lock_depth == 0U) {
		os_atomic_store_long(&eng->lock_owner, current_thread_key());
	}
	++eng->lock_depth;
}

bool mfd_engine_held_by_current_thread(const struct mfd_engine *eng)
{
	return eng && os_atomic_load_long((volatile long *)&eng->lock_owner) == current_thread_key();
}

/*
 * Speed events (vjster_action) are sent after the engine lock is let go, from the thread that let it go (a dock preset
 * from the UI thread, desk steps from the video tick). To keep their order across threads, each batch takes a ticket
 * while the engine lock is still held, and batches are sent in ticket order. A thread waits for its turn with no engine
 * lock held, and sends its speed events first, before anything that takes the engine lock again (the state event reads
 * get_state), so no turn ever waits on the engine lock: the batch ahead of it is only sending events.
 */
static pthread_mutex_t g_speed_turn_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_speed_turn_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_speed_ticket_next;
static uint64_t g_speed_ticket_serving;

void mfd_engine_unlock(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}

	bool emit_state = false;
	bool wake_ui = false;
	mfd_ui_notify_fn ui_fn = NULL;
	void *ui_ud = NULL;
	struct mfd_pending_fader_emit fader_emits[MFD_MAX_FADER_SLOTS];
	size_t fader_emit_count = 0U;
	unsigned speed_actions = 0U;
	int speed_steps = 0;
	double speed_rate = 0.0;

	if (eng->lock_depth > 0U) {
		--eng->lock_depth;
	}
	if (eng->lock_depth == 0U) {
		emit_state = eng->pending_state_event;
		eng->pending_state_event = false;

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			if (eng->pending_fader_emits[i].pending) {
				fader_emits[fader_emit_count++] = eng->pending_fader_emits[i];
				eng->pending_fader_emits[i].pending = false;
			}
		}

		/* APC40 video speed: Tap Tempo and Play at once, Tempo steps added up and sent at most every 50 ms. */
		speed_actions = eng->vj_speed_actions;
		eng->vj_speed_actions = 0U;
		speed_rate = eng->vj_speed_set_rate;
		if (eng->vj_speed_steps != 0) {
			uint64_t now = os_gettime_ns();
			if (now - eng->vj_speed_sent_ns >= MFD_VJ_SPEED_SEND_NS) {
				speed_steps = eng->vj_speed_steps;
				eng->vj_speed_steps = 0;
				eng->vj_speed_sent_ns = now;
			}
		}

		wake_ui = eng->pending_ui_notify || eng->pending_config_save;
		eng->pending_ui_notify = false;
		ui_fn = eng->ui_notify_fn;
		ui_ud = eng->ui_notify_user_data;
		os_atomic_store_long(&eng->lock_owner, 0);
	}

	bool speed_work = speed_actions != 0U || speed_steps != 0;
	uint64_t ticket = 0U;
	if (speed_work) {
		pthread_mutex_lock(
			&g_speed_turn_mutex); /* the ticket is taken while the engine lock still holds the order */
		ticket = g_speed_ticket_next++;
		pthread_mutex_unlock(&g_speed_turn_mutex);
	}
	if (eng->state_mutex_initialized) {
		pthread_mutex_unlock(&eng->state_mutex);
	}

	if (speed_work) {
		pthread_mutex_lock(&g_speed_turn_mutex);
		while (g_speed_ticket_serving != ticket) {
			pthread_cond_wait(&g_speed_turn_cond, &g_speed_turn_mutex);
		}
		pthread_mutex_unlock(&g_speed_turn_mutex);
	}
	if (speed_actions & MFD_VJ_SPEED_RESET_BIT) {
		mfd_vendor_emit_vjster_action("vjster.speed.reset", 0);
	}
	if (speed_actions & MFD_VJ_SPEED_SET_BIT) {
		mfd_vendor_emit_vjster_rate(speed_rate);
	}
	if (speed_actions & MFD_VJ_SPEED_PLAY_BIT) {
		mfd_vendor_emit_vjster_action("vjster.playPause", 0);
	}
	if (speed_steps != 0) {
		mfd_vendor_emit_vjster_action("vjster.speed.step", speed_steps);
	}
	if (speed_work) {
		pthread_mutex_lock(&g_speed_turn_mutex);
		++g_speed_ticket_serving; /* the next batch's turn */
		pthread_cond_broadcast(&g_speed_turn_cond);
		pthread_mutex_unlock(&g_speed_turn_mutex);
	}

	/* after the speed events: these may take the engine lock again (get_state) */
	for (size_t k = 0U; k < fader_emit_count; ++k) {
		mfd_vendor_emit_fader_changed_ex(fader_emits[k].slot_id, fader_emits[k].value,
						 fader_emits[k].display_value, fader_emits[k].origin);
	}
	if (emit_state) {
		mfd_vendor_emit_state_changed(eng);
	}
	if (wake_ui && ui_fn) {
		ui_fn(ui_ud);
	}
}

void mfd_engine_mark_ui_dirty(struct mfd_engine *eng, bool is_topology)
{
	if (!eng) {
		return;
	}
	if (is_topology) {
		++eng->config_topology_rev;
	} else {
		++eng->telemetry_rev;
	}
	eng->pending_ui_notify = true;
}

void mfd_engine_bump_slot_rev(struct mfd_engine *eng, int slot_index)
{
	if (!eng) {
		return;
	}
	if (slot_index >= 0 && slot_index < MFD_MAX_FADER_SLOTS) {
		++eng->slot_config_rev[slot_index];
	} else {
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			++eng->slot_config_rev[i];
		}
	}
	mfd_engine_mark_ui_dirty(eng, true);
}

void mfd_engine_set_ui_notify_fn(struct mfd_engine *eng, mfd_ui_notify_fn fn, void *user_data)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->ui_notify_fn = fn;
	eng->ui_notify_user_data = user_data;
	mfd_engine_unlock(eng);
}

static void vj_run_command(struct mfd_engine *eng, const struct mfd_vj_command *cmd);
static void vj_try_autofill(struct mfd_engine *eng);
static bool vj_tbar_busy_locked(const struct mfd_engine *eng);
static void vj_hold_epoch_sync_locked(struct mfd_engine *eng);
static bool vj_active(const struct mfd_engine *eng);
static bool midi_closed_unexpectedly_locked(struct mfd_engine *eng);
static void refresh_filter_meta_on_ui_thread(struct mfd_engine *eng);

void mfd_engine_flush_pending_ui_work(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	bool do_save = false;
	bool check_health = false;
	struct mfd_vj_command cmds[MFD_VJ_CMD_MAX + 1];
	int ncmd = 0;
	bool autofill = false;
	bool follow = false;
	mfd_engine_lock(eng);
	if (eng->vj_cue_ms != 0) {
		/* First: a take pressed right after turning Cue Level uses the new length. */
		memset(&cmds[ncmd], 0, sizeof(cmds[ncmd]));
		cmds[ncmd].strip = -1;
		cmds[ncmd].kind = MFD_VJ_CMD_TRANSITION;
		cmds[ncmd].arg = eng->vj_cue_ms;
		++ncmd;
		eng->vj_cue_ms = 0;
	}
	for (int i = 0; i < eng->vj_cmd_count && i < MFD_VJ_CMD_MAX; ++i) {
		cmds[ncmd++] = eng->vj_cmds[i];
	}
	eng->vj_cmd_count = 0;
	autofill = eng->vj_autofill_pending;
	eng->vj_autofill_pending = false;
	bool vs_poll = eng->vj_vs_poll_pending && !eng->scene_graph_ops;
	eng->vj_vs_poll_pending = false;
	if (eng->vj_follow_pending && !vj_tbar_busy_locked(eng)) {
		eng->vj_follow_pending = false;
		follow = vj_active(eng); /* the pads may have gone off since it was asked for */
	}
	bool live_ui = !eng->scene_graph_ops && obs_initialized();
	mfd_engine_unlock(eng);

	if (live_ui) {
		int tms = obs_frontend_get_transition_duration();
		mfd_engine_lock(eng);
		eng->cached_transition_ms = tms;
		mfd_engine_unlock(eng);
		mfd_blackout_ui(); /* attach the black source when asked for, let it go when clear */
	}

	uint8_t eyes_done = 0U;
	for (int i = 0; i < ncmd; ++i) {
		vj_run_command(eng, &cmds[i]);
		if (cmds[i].kind == MFD_VJ_CMD_PREVIEW) {
			/* OBS has the scene now (its event refreshed the cache), unless a later press asked for another */
			mfd_engine_lock(eng);
			if (strcmp(eng->vj_preview_asked, cmds[i].scene) == 0) {
				eng->vj_preview_asked[0] = '\0';
			}
			mfd_engine_unlock(eng);
		}
		if (cmds[i].kind == MFD_VJ_CMD_SET_VISIBLE && cmds[i].strip >= 0 &&
		    cmds[i].strip < (int)MFD_VJ_STRIPS) {
			eyes_done |= (uint8_t)(1U << (unsigned)cmds[i].strip);
		}
	}
	if (eyes_done != 0U) {
		mfd_engine_lock(eng);
		for (int i = 0; i < eng->vj_cmd_count; ++i) {
			/* A later press on the same strip is still queued: its eye is not settled yet. */
			if (eng->vj_cmds[i].kind == MFD_VJ_CMD_SET_VISIBLE && eng->vj_cmds[i].strip >= 0 &&
			    eng->vj_cmds[i].strip < (int)MFD_VJ_STRIPS) {
				eyes_done &= (uint8_t) ~(1U << (unsigned)eng->vj_cmds[i].strip);
			}
		}
		eng->vj_eye_hold &= (uint8_t)~eyes_done; /* those eyes are set: read them from OBS again */
		eng->vj_refresh_pending = true;
		mfd_engine_unlock(eng);
	}
	if (vs_poll && obs_initialized()) {
		bool panic = false;
		bool answered = mfd_vendor_visual_sidechain_panic_state(&panic);
		mfd_engine_vs_result(eng, true, MFD_VJ_CMD_VS_PANIC, answered, panic);
	}
	if (autofill) {
		vj_try_autofill(eng);
	}
	if (follow) {
		mfd_engine_fill_strips_from_scene(eng, NULL, false, false, NULL, NULL, 0U);
	}

	mfd_engine_lock(eng);
	if (eng->pending_config_save) {
		eng->pending_config_save = false;
		do_save = true;
	}
	if (midi_closed_unexpectedly_locked(eng)) {
		check_health = true;
	}
	mfd_tbar_deferred_fn tbar_retry = eng->tbar_deferred_due ? eng->tbar_deferred : NULL;
	eng->tbar_deferred_due = false;
	mfd_engine_unlock(eng);

	if (tbar_retry) {
		(void)tbar_retry(true); /* UI thread, no engine lock: the driver may call back into the engine */
	}

	if (do_save && obs_initialized()) {
		mfd_engine_save_global_config(eng);
	}
	refresh_filter_meta_on_ui_thread(eng);
	if (check_health) {
		mfd_engine_poll_midi_health_on_ui_thread(eng);
	}
}

static void unlock_and_persist_global_config(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	bool save_now = false;
	if (obs_initialized()) {
		if (eng->lock_depth == 1U) {
			eng->pending_config_save = false;
			save_now = true;
		} else {
			eng->pending_config_save = true;
		}
	}
	mfd_engine_unlock(eng);
	if (save_now) {
		mfd_engine_save_global_config(eng);
	}
}

/*
 * The MIDI In ports as the system lists them now. Listing asks the MIDI driver (WinMM, CoreMIDI), which can be slow
 * or stall on a device that was just pulled, so it is always read with no engine lock held (A-02) and handed to the
 * code that needs it under the lock.
 */
struct mfd_midi_port_list {
	struct mfd_midi_device_info list[MFD_MAX_MIDI_DEVICES];
	size_t count;
	bool valid; /* false before OBS is up: nothing was read */
};

static void midi_port_list_read(struct mfd_midi_port_list *ports)
{
	ports->count = 0U;
	ports->valid = obs_initialized();
	if (ports->valid) {
		ports->count = mfd_winmm_enumerate(ports->list, MFD_MAX_MIDI_DEVICES);
	}
}

/* Serialises MIDI port work (open, close) between threads. Taken with no engine lock held; the engine lock is taken
 * inside it for short state updates only, never the other way round. */
static pthread_mutex_t g_midi_io_mutex = PTHREAD_MUTEX_INITIALIZER;

static void refresh_apc40_detected_locked(struct mfd_engine *eng, const struct mfd_midi_port_list *ports)
{
	if (!eng) {
		return;
	}
	if (eng->midi_dev.is_open && mfd_is_apc40_mk2_name(eng->midi_dev.opened_name)) {
		eng->apc40_controller_detected = true;
		return;
	}
	if (mfd_is_apc40_mk2_name(eng->configured_device_name)) {
		eng->apc40_controller_detected = true;
		return;
	}
	if (ports && ports->valid) {
		for (size_t i = 0U; i < ports->count; ++i) {
			if (ports->list[i].is_apc40_mk2) {
				eng->apc40_controller_detected = true;
				return;
			}
		}
		eng->apc40_controller_detected = false;
	}
}

/*
 * Takes the open handles out of the engine's device into `old`, under the engine lock, so the caller can close them
 * with no engine lock held. The engine's device keeps is_open and opened_name until the caller stores the outcome, so
 * the tick sees no "no controller" frame in between; with no MIDI Out handle it sends no light meanwhile.
 */
static void midi_detach_handles_locked(struct mfd_engine *eng, struct mfd_winmm_device *old)
{
	*old = eng->midi_dev;
	old->callback_owner = NULL;
	eng->midi_dev.handle = NULL;
	eng->midi_dev.out_handle = NULL;
	/* Closing a port raises device_closed_unexpectedly through its callback; the tick must not read that as an
	 * unplug. Cleared again under the lock by midi_io_done_locked once the flag is back down. */
	eng->midi_io_busy = true;
}

static void midi_io_done_locked(struct mfd_engine *eng)
{
	eng->midi_io_busy = false;
}

/* The MIDI In callback reported the port closed, and it was not this plugin closing it. Engine lock held. */
static bool midi_closed_unexpectedly_locked(struct mfd_engine *eng)
{
	return !eng->midi_io_busy && os_atomic_load_bool(&eng->midi_dev.device_closed_unexpectedly);
}

/* Stores a device opened (or not) on the caller's stack into the engine's device, under the engine lock. */
static void midi_install_device_locked(struct mfd_engine *eng, const struct mfd_winmm_device *fresh)
{
	struct mfd_winmm_device *dst = &eng->midi_dev;
	dst->handle = fresh->handle;
	dst->out_handle = fresh->out_handle;
	dst->is_open = fresh->is_open;
	dst->error_kind = fresh->error_kind;
	dst->error_operation = fresh->error_operation;
	dst->error_code = fresh->error_code;
	memcpy(dst->error_device_name, fresh->error_device_name, sizeof(dst->error_device_name));
	memcpy(dst->opened_name, fresh->opened_name, sizeof(dst->opened_name));
	memcpy(dst->last_error, fresh->last_error, sizeof(dst->last_error));
	memcpy(dst->out_name, fresh->out_name, sizeof(dst->out_name));
}

/*
 * Closes `old` (handles taken with midi_detach_handles_locked) and opens `device_name` into `fresh`. No engine lock
 * is held: the video tick keeps running while the MIDI driver works. The MIDI In callback of the new port is bound to
 * the engine's own device (its queue and its closed flag).
 */
static bool midi_reopen_unlocked(struct mfd_engine *eng, struct mfd_winmm_device *old, const char *device_name,
				 struct mfd_winmm_device *fresh)
{
	mfd_winmm_close(old);
	/* Closing the old port may have raised the flag through its callback; no port is open now. */
	os_atomic_store_bool(&eng->midi_dev.device_closed_unexpectedly, false);
	mfd_winmm_init(fresh, &eng->midi_queue);
	fresh->callback_owner = &eng->midi_dev;
	bool ok = mfd_winmm_open_by_name(fresh, device_name);
	fresh->callback_owner = NULL;
	return ok;
}

static void clear_slot_filter_fields(struct mfd_fader_slot *s);

static void refresh_frontend_cache_on_ui_thread_locked(struct mfd_engine *eng)
{
	if (!eng || !obs_initialized()) {
		return;
	}
	if (!eng->scene_list_override_set) {
		/* The APC40's Scene Launch buttons: the first scenes in the Scenes list, top first. */
		eng->cached_scene_count = 0;
		struct obs_frontend_source_list list = {0};
		obs_frontend_get_scenes(&list);
		for (size_t i = 0U; i < list.sources.num && eng->cached_scene_count < (int)MFD_VJ_SCENES; ++i) {
			const char *nm = obs_source_get_name(list.sources.array[i]);
			if (nm && nm[0] != '\0') {
				snprintf(eng->cached_scenes[eng->cached_scene_count],
					 sizeof(eng->cached_scenes[eng->cached_scene_count]), "%s", nm);
				++eng->cached_scene_count;
			}
		}
		obs_frontend_source_list_free(&list);
	}
	eng->cached_studio_mode = obs_frontend_preview_program_mode_active();
	eng->cached_program_scene[0] = '\0';
	eng->cached_preview_scene[0] = '\0';

	obs_source_t *prog = obs_frontend_get_current_scene();
	if (prog) {
		const char *nm = obs_source_get_name(prog);
		if (nm) {
			snprintf(eng->cached_program_scene, sizeof(eng->cached_program_scene), "%s", nm);
		}
		obs_source_release(prog);
	}
	if (eng->cached_studio_mode) {
		obs_source_t *prev = obs_frontend_get_current_preview_scene();
		if (prev) {
			const char *nm = obs_source_get_name(prev);
			if (nm) {
				snprintf(eng->cached_preview_scene, sizeof(eng->cached_preview_scene), "%s", nm);
			}
			obs_source_release(prev);
		}
	}
}

static void store_persisted_calibration(struct mfd_engine *eng, const char *profile_name,
					const struct mfd_midi_addr *addr, const struct mfd_control_calibration *cal)
{
	if (!eng || !addr || !cal || addr->type == MFD_MIDI_NONE) {
		return;
	}
	const char *prof = (profile_name && profile_name[0] != '\0') ? profile_name : MFD_DEFAULT_PROFILE_NAME;
	int free_idx = -1;
	for (int i = 0; i < MFD_MAX_CAL_ENTRIES; ++i) {
		struct mfd_persisted_cal_entry *e = &eng->cal_entries[i];
		if (!e->in_use) {
			if (free_idx < 0) {
				free_idx = i;
			}
			continue;
		}
		if (strcmp(e->profile_name, prof) == 0 && e->addr.type == addr->type &&
		    e->addr.channel == addr->channel && e->addr.number == addr->number &&
		    e->addr.is_14bit == addr->is_14bit) {
			e->cal = *cal;
			return;
		}
	}
	if (free_idx >= 0) {
		struct mfd_persisted_cal_entry *e = &eng->cal_entries[free_idx];
		e->in_use = true;
		snprintf(e->profile_name, sizeof(e->profile_name), "%s", prof);
		e->addr = *addr;
		e->cal = *cal;
	}
}

bool mfd_engine_lookup_persisted_calibration(const struct mfd_engine *eng, const char *profile_name,
					     const struct mfd_midi_addr *addr, struct mfd_control_calibration *out_cal)
{
	if (!out_cal) {
		return false;
	}
	bool is_14 = slot_addr_is_14bit(addr);
	mfd_calibration_init(out_cal, is_14);
	if (!eng || !addr || addr->type == MFD_MIDI_NONE) {
		return false;
	}
	const char *prof = (profile_name && profile_name[0] != '\0') ? profile_name : MFD_DEFAULT_PROFILE_NAME;
	for (int i = 0; i < MFD_MAX_CAL_ENTRIES; ++i) {
		const struct mfd_persisted_cal_entry *e = &eng->cal_entries[i];
		if (!e->in_use) {
			continue;
		}
		if (strcmp(e->profile_name, prof) == 0 && e->addr.type == addr->type &&
		    e->addr.channel == addr->channel && e->addr.number == addr->number &&
		    e->addr.is_14bit == addr->is_14bit) {
			*out_cal = e->cal;
			return true;
		}
	}
	return false;
}

void mfd_engine_set_slot_calibration(struct mfd_engine *eng, int slot_index, int cal_min, int cal_max)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	mfd_calibration_set(&s->cal, slot_addr_is_14bit(&s->addr), cal_min, cal_max);
	store_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
	mfd_engine_bump_slot_rev(eng, slot_index);
	unlock_and_persist_global_config(eng);
}

struct mfd_engine *mfd_engine_get(void)
{
	return &g_engine;
}

void mfd_engine_set_tbar_driver(mfd_tbar_apply_fn driver_fn)
{
	mfd_engine_lock(&g_engine);
	g_engine.tbar_driver = driver_fn;
	mfd_engine_unlock(&g_engine);
}

void mfd_engine_set_tbar_deferred_fn(mfd_tbar_deferred_fn deferred_fn)
{
	mfd_engine_lock(&g_engine);
	g_engine.tbar_deferred = deferred_fn;
	g_engine.tbar_deferred_due = false;
	g_engine.tbar_deferred_wait_sec = 0.0f;
	mfd_engine_unlock(&g_engine);
}

void mfd_engine_set_tbar_unlatch_fn(mfd_tbar_unlatch_fn unlatch_fn)
{
	mfd_engine_lock(&g_engine);
	g_engine.tbar_unlatch = unlatch_fn;
	mfd_engine_unlock(&g_engine);
}

void mfd_engine_unlatch_tbar(struct mfd_engine *eng)
{
	if (eng && eng->tbar_unlatch) {
		eng->tbar_unlatch();
	}
}

static float query_live_filter_unmapped_deflection_locked(const struct mfd_engine *eng,
							  const struct mfd_fader_slot *slot);

static void format_percent_value(bool translate, float val, char *out, size_t out_size)
{
	char number[16];
	snprintf(number, sizeof(number), "%d", (int)lroundf(val * 100.0f));
	const char *const values[1] = {number};
	mfd_text_fill(out, out_size, mfd_text_or_english(translate, "Readout.Percent", "%1%"), values, 1U);
}

void mfd_engine_format_slot_display_value(const struct mfd_fader_slot *slot, float norm_01, char *out, size_t out_size)
{
	mfd_engine_format_slot_display_value_ex(slot, norm_01, false, out, out_size);
}

void mfd_engine_format_slot_display_value_ex(const struct mfd_fader_slot *slot, float norm_01, bool translate,
					     char *out, size_t out_size)
{
	if (!out || out_size == 0U) {
		return;
	}
	out[0] = '\0';
	float val = clamp01(norm_01);
	if (!slot) {
		format_percent_value(translate, val, out, out_size);
		return;
	}

	switch (slot->target_type) {
	case MFD_TARGET_MIX_MASTER: {
		char db[24];
		snprintf(db, sizeof(db), "%.1f", slot->mix_reported_db);
		const char *values[] = {db};
		const char *label;
		switch (slot->mix_delivery) {
		case MFD_MIX_WAITING:
			label = mfd_text_or_english(translate, "Mix.Waiting", "Requested %1 dB");
			break;
		case MFD_MIX_CONFIRMED:
			label = mfd_text_or_english(translate, "Mix.Confirmed", "Confirmed %1 dB");
			break;
		case MFD_MIX_DECLINED:
			label = mfd_text_or_english(translate, "Mix.Declined", "Fadester Companion said no: %1");
			values[0] = slot->mix_reason;
			break;
		case MFD_MIX_NO_REPLY:
			label = mfd_text_or_english(translate, "Mix.NoReply", "No answer from Fadester Companion");
			break;
		case MFD_MIX_UNAVAILABLE:
			label = mfd_text_or_english(translate, "Mix.Unavailable", "Mix request unavailable");
			break;
		case MFD_MIX_CONFLICT:
			label = mfd_text_or_english(translate, "Mix.Conflict", "Two mix providers are answering");
			break;
		default:
			label = mfd_text_or_english(translate, "Readout.MixMasterIdle", "Needs Fadester Companion");
			break;
		}
		mfd_text_fill(out, out_size, label, values, 1U);
		break;
	}
	case MFD_TARGET_FILTER_PARAM: {
		double pmin = slot->filter_prop_valid ? slot->filter_prop_min
						      : fmin(slot->filter_from_val, slot->filter_to_val);
		double pmax = slot->filter_prop_valid ? slot->filter_prop_max
						      : fmax(slot->filter_from_val, slot->filter_to_val);
		double step = (slot->filter_prop_valid && slot->filter_prop_step > 0.0) ? slot->filter_prop_step : 0.01;
		mfd_filter_format_display_value_ex(val, slot->filter_from_val, slot->filter_to_val, pmin, pmax,
						   slot->filter_prop_is_int, step, slot->filter_prop_suffix,
						   slot->filter_bypass_at_zero, translate, out, out_size);
		break;
	}
	case MFD_TARGET_AUDIO_VOLUME: {
		float db = mfd_deflection_to_db(val);
		if (!isfinite(db)) {
			mfd_text_fill(out, out_size, mfd_text_or_english(translate, "Readout.DbSilent", "-inf dB"),
				      NULL, 0U);
		} else {
			char number[32];
			snprintf(number, sizeof(number), "%.1f", (fabsf(db) < 0.05f) ? 0.0 : (double)db);
			const char *const values[1] = {number};
			mfd_text_fill(out, out_size, mfd_text_or_english(translate, "Readout.Db", "%1 dB"), values, 1U);
		}
		break;
	}
	case MFD_TARGET_UNASSIGNED:
	case MFD_TARGET_TBAR:
	case MFD_TARGET_SINGLE_LAYER:
	case MFD_TARGET_TWO_SOURCE_MIX:
	default:
		format_percent_value(translate, val, out, out_size);
		break;
	}
}

static void configure_slot_takeover_mode(const struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	if (!slot) {
		return;
	}
	bool is_tbar = (slot->target_type == MFD_TARGET_TBAR);
	bool is_two_rail =
		(slot->target_type == MFD_TARGET_TWO_SOURCE_MIX || slot->target_type == MFD_TARGET_SINGLE_LAYER ||
		 slot->target_type == MFD_TARGET_AUDIO_VOLUME || slot->target_type == MFD_TARGET_FILTER_PARAM);
	slot->takeover.bidirectional_flip = is_tbar ? (eng ? eng->tbar_bidirectional_flip : true) : false;
	mfd_takeover_set_two_rail_mode(&slot->takeover, is_two_rail);
}

static void init_slot_runtime(struct mfd_engine *eng, struct mfd_fader_slot *slot, float initial_val)
{
	bool bidir = (slot->target_type == MFD_TARGET_TBAR) ? eng->tbar_bidirectional_flip : false;
	mfd_smoother_init(&slot->smoother, eng->smooth_tau_ms, initial_val);
	mfd_takeover_init(&slot->takeover, initial_val, MFD_DEFAULT_PICKUP_THRESHOLD, bidir);
	configure_slot_takeover_mode(eng, slot);
	slot->tbar_state = MFD_TBAR_IDLE;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->last_raw_norm = initial_val;
	slot->last_applied_val = initial_val;
	slot->last_alpha_a = 1.0f;
	slot->last_alpha_b = 0.0f;
	slot->mix_start_pending = (slot->target_type == MFD_TARGET_TWO_SOURCE_MIX);
	slot->last_audio_mul = mfd_deflection_to_mul(initial_val);
	slot->last_audio_db = mfd_deflection_to_db(initial_val);
	slot->output_write_count = 0U;
	slot->has_last_written_mapped = false;
	slot->last_written_int = 0;
	slot->last_written_float = 0.0;
	slot->writing_filter_settings = false;
	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->auto_fade_from = initial_val;
	slot->auto_fade_to = initial_val;
	slot->auto_fade_duration_sec = 0.0f;
	slot->auto_fade_elapsed_sec = 0.0f;
	slot->sec_since_last_fader_event = 1.0f;
	slot->fader_event_pending = false;
	slot->pending_origin[0] = '\0';
	if (slot->target_type == MFD_TARGET_TBAR) {
		mfd_engine_unlatch_tbar(eng);
	}
}

void mfd_engine_abort_tbar_for_config_change_locked(struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	if (!eng || !slot || slot->target_type != MFD_TARGET_TBAR) {
		return;
	}
	if (slot->enabled && slot->tbar_state == MFD_TBAR_THROWING) {
		mfd_engine_apply_tbar_abort(eng, slot);
	}
	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
}

static void abort_open_tbar_slots_on_midi_disconnect_locked(struct mfd_engine *eng)
{
	if (!eng || eng->midi_disconnect_handled) {
		return;
	}
	eng->midi_disconnect_handled = true;
	mfd_midi_queue_init(&eng->midi_queue);
	bool aborted_any = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
			continue;
		}
		bool was_throwing = (slot->tbar_state == MFD_TBAR_THROWING);
		mfd_engine_abort_tbar_for_config_change_locked(eng, slot);
		slot->takeover.state = MFD_TAKEOVER_WAITING;
		slot->takeover.target_obs = 0.0f;
		slot->takeover.rescale_origin = slot->takeover.polarity_reversed ? 1.0f : 0.0f;
		slot->takeover.has_last_fader = false;
		slot->takeover.hint = MFD_HINT_NONE;
		if (was_throwing) {
			aborted_any = true;
		}
	}
	if (aborted_any) {
		mfd_engine_notify_state_changed(eng);
	}
}

void mfd_engine_reset_slot_mode_state(struct mfd_engine *eng, struct mfd_fader_slot *slot,
				      enum mfd_target_type new_target)
{
	if (!eng || !slot) {
		return;
	}
	mfd_engine_lock(eng);
	mfd_engine_abort_tbar_for_config_change_locked(eng, slot);
	enum mfd_target_type prev_target = slot->target_type;
	slot->target_type = new_target;
	if (prev_target == MFD_TARGET_MIX_MASTER || new_target == MFD_TARGET_MIX_MASTER) {
		slot->mix_request_id[0] = '\0';
		slot->mix_delivery = MFD_MIX_IDLE;
		slot->mix_sent_ns = 0;
		slot->mix_pending = false;
		eng->mix_provider_id[0] = '\0';
		eng->last_mix_sent_id[0] = '\0';
		eng->last_mix_sent_ns = 0;
		eng->mix_provider_conflict = false;
	}
	if (prev_target != new_target) {
		slot->mix_request_id[0] = '\0';
		slot->mix_delivery = MFD_MIX_IDLE;
		slot->mix_sent_ns = 0;
		slot->mix_pending = false;
	}
	if (prev_target == new_target) {
		configure_slot_takeover_mode(eng, slot);
		if (new_target == MFD_TARGET_TBAR && slot->takeover.state != MFD_TAKEOVER_PARKED) {
			slot->tbar_state = MFD_TBAR_IDLE;
			mfd_engine_unlatch_tbar(eng);
		} else if (new_target != MFD_TARGET_TBAR) {
			slot->tbar_state = MFD_TBAR_IDLE;
		}
		mfd_engine_unlock(eng);
		return;
	}

	bool had_raw = slot->takeover.has_last_fader;
	float preserved_raw = slot->last_raw_norm;
	if (prev_target == MFD_TARGET_TBAR || new_target == MFD_TARGET_TBAR) {
		mfd_engine_unlatch_tbar(eng);
	}

	float init_val = 0.0f;
	if (new_target == MFD_TARGET_SINGLE_LAYER || new_target == MFD_TARGET_AUDIO_VOLUME) {
		init_val = 1.0f;
	} else if (new_target == MFD_TARGET_FILTER_PARAM) {
		init_val = query_live_filter_unmapped_deflection_locked(eng, slot);
	}
	bool bidir = (new_target == MFD_TARGET_TBAR) ? eng->tbar_bidirectional_flip : false;
	mfd_smoother_reset(&slot->smoother, init_val);
	mfd_takeover_init(&slot->takeover, init_val, MFD_DEFAULT_PICKUP_THRESHOLD, bidir);
	configure_slot_takeover_mode(eng, slot);

	slot->tbar_state = MFD_TBAR_IDLE;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->has_last_written_mapped = false;
	slot->last_written_int = 0;
	slot->last_written_float = 0.0;
	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->last_applied_val = init_val;
	slot->last_alpha_a = 1.0f;
	slot->last_alpha_b = 0.0f;
	slot->mix_start_pending = (new_target == MFD_TARGET_TWO_SOURCE_MIX);
	slot->last_audio_mul = mfd_deflection_to_mul(init_val);
	slot->last_audio_db = mfd_deflection_to_db(init_val);

	if (had_raw) {
		slot->last_raw_norm = preserved_raw;
		slot->takeover.has_last_fader = true;
		slot->takeover.last_raw_fader = preserved_raw;
		slot->takeover.last_fader = preserved_raw;
		float diff = preserved_raw - init_val;
		if (fabsf(diff) > slot->takeover.pickup_threshold) {
			slot->takeover.hint = (diff < 0.0f) ? MFD_HINT_MOVE_UP : MFD_HINT_MOVE_DOWN;
		} else {
			slot->takeover.hint = MFD_HINT_NONE;
		}
	} else {
		slot->last_raw_norm = init_val;
	}
	mfd_engine_unlock(eng);
}

enum mfd_controller_profile mfd_engine_get_active_profile(const struct mfd_engine *eng)
{
	if (!eng) {
		return MFD_PROFILE_APC40_MK2;
	}
	if (eng->active_profile_name[0] != '\0') {
		const struct mfd_controller_profile_def *def = mfd_profile_find_by_slug(eng->active_profile_name);
		if (def) {
			return def->id;
		}
	}
	enum mfd_controller_profile from_dev =
		mfd_profile_for_device(eng->configured_device_name, eng->midi_dev.is_open, eng->midi_dev.opened_name);
	if (from_dev != MFD_PROFILE_GENERIC) {
		return from_dev;
	}
	return MFD_PROFILE_APC40_MK2;
}

bool mfd_engine_load_profile_defaults(struct mfd_engine *eng, enum mfd_controller_profile prof)
{
	if (!eng) {
		return false;
	}
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(prof);
	if (!def) {
		return false;
	}

	mfd_engine_lock(eng);
	for (int k = 0; k < MFD_MAX_FADER_SLOTS; ++k) {
		mfd_engine_abort_tbar_for_config_change_locked(eng, &eng->fader_slots[k]);
		mfd_engine_detach_filter_binding_locked(eng, k);
	}
	snprintf(eng->active_profile_name, sizeof(eng->active_profile_name), "%s", def->slug);
	mfd_midi_parser_reset_cc_state(&eng->midi_parser);
	if (!eng->has_last_midi) {
		eng->last_moved_slot_index = -1;
	}

	for (size_t i = 0U; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		memset(s, 0, sizeof(*s));
		s->filter_pre_bypass_enabled = true;
		if (i < MFD_APC40_DEFAULT_SLOT_COUNT) {
			bool phys = mfd_profile_has_physical_slot(def->id, (int)i);
			s->enabled = (mfd_slot_is_fx_bank((int)i) || def->id == MFD_PROFILE_GENERIC)
					     ? true
					     : (phys && def->slots[i].addr.type != MFD_MIDI_NONE);
			snprintf(s->id, sizeof(s->id), "%s", def->slots[i].id);
			snprintf(s->kind, sizeof(s->kind), "%s", def->slots[i].kind);
			s->label[0] = '\0';
			s->addr = def->slots[i].addr;
			s->target_type = def->slots[i].default_target;
			s->curve = def->slots[i].default_curve;
			s->audio_follows_video = false;
			s->invert = false;
		} else {
			s->enabled = false;
			snprintf(s->id, sizeof(s->id), "fader_%u", (unsigned)(i + 1U));
			snprintf(s->kind, sizeof(s->kind), "fader");
			s->label[0] = '\0';
			s->addr.type = MFD_MIDI_NONE;
			s->target_type = MFD_TARGET_UNASSIGNED;
			s->curve = MFD_CURVE_DIP_FREE;
		}
		mfd_engine_lookup_persisted_calibration(eng, eng->active_profile_name, &s->addr, &s->cal);
		float init_val =
			(s->target_type == MFD_TARGET_SINGLE_LAYER || s->target_type == MFD_TARGET_AUDIO_VOLUME) ? 1.0f
														 : 0.0f;
		init_slot_runtime(eng, s, init_val);
	}
	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_unlock(eng);
	return true;
}

bool mfd_engine_select_profile(struct mfd_engine *eng, enum mfd_controller_profile prof, bool reset_targets)
{
	if (!eng) {
		return false;
	}
	const struct mfd_controller_profile_def *def = mfd_profile_get_def(prof);
	if (!def) {
		return false;
	}

	mfd_engine_lock(eng);
	mfd_engine_cancel_setup_session(eng);
	cancel_row_learn_locked(eng);
	mfd_midi_parser_reset_cc_state(&eng->midi_parser);
	if (reset_targets) {
		mfd_engine_load_profile_defaults(eng, prof);
	} else {
		const enum mfd_controller_profile prev_profile = mfd_engine_get_active_profile(eng);
		snprintf(eng->active_profile_name, sizeof(eng->active_profile_name), "%s", def->slug);
		if (def->id == MFD_PROFILE_GENERIC) {
			for (int i = 0; i < MFD_MAX_FADER_SLOTS && (size_t)i < MFD_APC40_DEFAULT_SLOT_COUNT; ++i) {
				/* FX always; slots the previous desk lacked return as Not used. Bindings are kept. */
				if (mfd_slot_is_fx_bank(i) || !mfd_profile_has_physical_slot(prev_profile, i)) {
					eng->fader_slots[i].enabled = true;
				}
			}
		} else {
			for (int i = 0; i < MFD_MAX_FADER_SLOTS && (size_t)i < MFD_APC40_DEFAULT_SLOT_COUNT; ++i) {
				struct mfd_fader_slot *s = &eng->fader_slots[i];
				if (mfd_slot_is_fx_bank(i)) {
					s->enabled = true;
					s->label[0] = '\0';
					s->addr = def->slots[i].addr;
					mfd_engine_lookup_persisted_calibration(eng, eng->active_profile_name, &s->addr,
										&s->cal);
				} else if (mfd_profile_has_physical_slot(def->id, i) &&
					   def->slots[i].addr.type != MFD_MIDI_NONE) {
					s->enabled = true;
					s->label[0] = '\0';
					s->addr = def->slots[i].addr;
					mfd_engine_lookup_persisted_calibration(eng, eng->active_profile_name, &s->addr,
										&s->cal);
					if (s->target_type == MFD_TARGET_UNASSIGNED &&
					    def->slots[i].default_target != MFD_TARGET_UNASSIGNED) {
						mfd_engine_reset_slot_mode_state(eng, s, def->slots[i].default_target);
					}
				} else {
					mfd_engine_abort_tbar_for_config_change_locked(eng, s);
					mfd_engine_detach_filter_binding_locked(eng, i);
					s->enabled = false;
					s->addr.type = MFD_MIDI_NONE;
					s->scene_name[0] = '\0';
					s->resolved_scene_name[0] = '\0';
					s->source_a_name[0] = '\0';
					s->source_b_name[0] = '\0';
					s->filter_name[0] = '\0';
					s->filter_prop[0] = '\0';
					s->filter_from_val = 0.0;
					s->filter_to_val = 0.0;
					s->filter_bypass_at_zero = false;
					s->filter_prop_valid = false;
					mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
				}
			}
		}
		mfd_engine_bump_slot_rev(eng, -1);
		mfd_engine_reevaluate_slots(eng, false);
	}
	if (def->id != MFD_PROFILE_APC40_MK2) {
		eng->first_run_offer_dismissed = true;
	}
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

void mfd_engine_load_apc40_defaults(struct mfd_engine *eng)
{
	(void)mfd_engine_load_profile_defaults(eng, MFD_PROFILE_APC40_MK2);
}

static bool slots_differ_from_apc40_defaults(const struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	struct mfd_apc40_default_slot defs[MFD_APC40_DEFAULT_SLOT_COUNT];
	size_t count = mfd_apc40_get_default_slots(defs, MFD_APC40_DEFAULT_SLOT_COUNT);
	for (size_t i = 0U; i < MFD_MAX_FADER_SLOTS && i < count; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		/* A strip filled from the scene on air by Midi Fadester: its job and source are not the user's. */
		bool follows = s->auto_follow;
		if (!s->enabled) {
			return true;
		}
		if (!follows && s->target_type != defs[i].default_target) {
			return true;
		}
		if (!midi_addr_exact_equal(&s->addr, &defs[i].addr)) {
			return true;
		}
		if (!follows &&
		    (s->scene_name[0] != '\0' || s->source_a_name[0] != '\0' || s->source_b_name[0] != '\0' ||
		     s->filter_name[0] != '\0' || s->filter_prop[0] != '\0' || s->filter_bypass_at_zero)) {
			return true;
		}
		if (s->curve != defs[i].default_curve || s->audio_follows_video || s->invert) {
			return true;
		}
		int max_def = slot_addr_is_14bit(&s->addr) ? MFD_CAL_DEFAULT_MAX_14BIT : MFD_CAL_DEFAULT_MAX_7BIT;
		if (s->cal.cal_min != MFD_CAL_DEFAULT_MIN || s->cal.cal_max != max_def) {
			return true;
		}
	}
	return false;
}

struct mfd_stack_find_ctx {
	const char *target_a;
	const char *target_b;
	int idx;
	int found_a;
	int found_b;
};

static bool enum_scene_items_stack_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct mfd_stack_find_ctx *ctx = (struct mfd_stack_find_ctx *)param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && !obs_source_removed(src)) {
		const char *nm = obs_source_get_name(src);
		if (nm) {
			if (ctx->target_a && strcmp(nm, ctx->target_a) == 0) {
				ctx->found_a = ctx->idx;
			}
			if (ctx->target_b && strcmp(nm, ctx->target_b) == 0) {
				ctx->found_b = ctx->idx;
			}
		}
	}
	++ctx->idx;
	return true;
}

static int live_scene_item_stack_index_on_source(obs_source_t *scene_src, const char *source_name)
{
	if (!scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src) || !source_name ||
	    source_name[0] == '\0') {
		return -1;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		return -1;
	}
	struct mfd_stack_find_ctx ctx = {source_name, NULL, 0, -1, -1};
	obs_scene_enum_items(scene, enum_scene_items_stack_cb, &ctx);
	return ctx.found_a;
}

struct mfd_enum_scene_layer_ctx {
	const char *source_a;
	const char *source_b;
	const char *exclude_scene;
	bool pair_mode;
	bool found;
	char scene_name[MFD_SOURCE_NAME_MAX];
	int idx_a;
	int idx_b;
};

static bool enum_scenes_find_cb(void *param, obs_source_t *scene_src)
{
	struct mfd_enum_scene_layer_ctx *ctx = (struct mfd_enum_scene_layer_ctx *)param;
	if (!ctx || !scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src)) {
		return true;
	}
	const char *sc_name = obs_source_get_name(scene_src);
	if (!sc_name || sc_name[0] == '\0') {
		return true;
	}
	if (ctx->exclude_scene && ctx->exclude_scene[0] != '\0' && strcmp(sc_name, ctx->exclude_scene) == 0) {
		return true;
	}
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		return true;
	}
	struct mfd_stack_find_ctx sctx = {ctx->source_a, ctx->pair_mode ? ctx->source_b : NULL, 0, -1, -1};
	obs_scene_enum_items(scene, enum_scene_items_stack_cb, &sctx);
	if (ctx->pair_mode) {
		if (sctx.found_a >= 0 && sctx.found_b >= 0) {
			ctx->found = true;
			snprintf(ctx->scene_name, sizeof(ctx->scene_name), "%s", sc_name);
			ctx->idx_a = sctx.found_a;
			ctx->idx_b = sctx.found_b;
			return false;
		}
	} else {
		if (sctx.found_a >= 0) {
			ctx->found = true;
			snprintf(ctx->scene_name, sizeof(ctx->scene_name), "%s", sc_name);
			ctx->idx_a = sctx.found_a;
			return false;
		}
	}
	return true;
}

bool mfd_engine_source_exists(const struct mfd_engine *eng, const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->source_exists) {
		return eng->scene_graph_ops->source_exists(source_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	bool alive = !obs_source_removed(src);
	obs_source_release(src);
	return alive;
}

bool mfd_engine_scene_exists(const struct mfd_engine *eng, const char *scene_name)
{
	if (!scene_name || scene_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->scene_exists) {
		return eng->scene_graph_ops->scene_exists(scene_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(scene_name);
	if (!src) {
		return false;
	}
	bool ok = !obs_source_removed(src) && obs_source_is_scene(src);
	obs_source_release(src);
	return ok;
}

bool mfd_engine_source_has_audio(const struct mfd_engine *eng, const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->source_has_audio) {
		return eng->scene_graph_ops->source_has_audio(source_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	bool has_audio = !obs_source_removed(src) && ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0U);
	obs_source_release(src);
	return has_audio;
}

static void check_internal_opacity_filter_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	UNUSED_PARAMETER(enabled);
	bool *found = (bool *)param;
	if (!found || *found) {
		return;
	}
	if ((filter_name && strcmp(filter_name, MFD_INTERNAL_OPACITY_FILTER_NAME) == 0) ||
	    (type_id && strcmp(type_id, MFD_LAYER_FILTER_ID) == 0)) {
		*found = true;
	}
}

bool mfd_engine_source_has_internal_opacity(const struct mfd_engine *eng, const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->source_has_internal_opacity) {
		return eng->scene_graph_ops->source_has_internal_opacity(source_name, eng->scene_graph_user_data);
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_source_filters) {
		bool found = false;
		(void)eng->scene_graph_ops->enum_source_filters(source_name, NULL, check_internal_opacity_filter_cb,
								&found, eng->scene_graph_user_data);
		return found;
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	bool has_opacity = false;
	if (!obs_source_removed(src)) {
		obs_source_t *flt = obs_source_get_filter_by_name(src, MFD_INTERNAL_OPACITY_FILTER_NAME);
		if (flt) {
			has_opacity = !obs_source_removed(flt);
			obs_source_release(flt);
		}
	}
	obs_source_release(src);
	return has_opacity;
}

static int sg_scene_item_stack_index(const struct mfd_engine *eng, const char *scene_name, const char *source_name)
{
	if (!scene_name || scene_name[0] == '\0' || !source_name || source_name[0] == '\0') {
		return -1;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->scene_item_stack_index) {
		return eng->scene_graph_ops->scene_item_stack_index(scene_name, source_name,
								    eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return -1;
	}
	obs_source_t *scene_src = obs_get_source_by_name(scene_name);
	if (!scene_src) {
		return -1;
	}
	int idx = live_scene_item_stack_index_on_source(scene_src, source_name);
	obs_source_release(scene_src);
	return idx;
}

bool mfd_engine_find_other_scene_with_layer(const struct mfd_engine *eng, const char *source_name,
					    const char *exclude_scene, char *out_scene, size_t out_size)
{
	if (out_scene && out_size > 0U) {
		out_scene[0] = '\0';
	}
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->find_scene_with_layer) {
		return eng->scene_graph_ops->find_scene_with_layer(source_name, exclude_scene, out_scene, out_size,
								   eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	struct mfd_enum_scene_layer_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.source_a = source_name;
	ctx.exclude_scene = exclude_scene;
	ctx.pair_mode = false;
	obs_enum_scenes(enum_scenes_find_cb, &ctx);
	if (ctx.found && out_scene && out_size > 0U) {
		snprintf(out_scene, out_size, "%s", ctx.scene_name);
	}
	return ctx.found;
}

static bool sg_find_scene_with_pair(const struct mfd_engine *eng, const char *source_a, const char *source_b,
				    char *out_scene, size_t out_size, int *out_idx_a, int *out_idx_b)
{
	if (out_scene && out_size > 0U) {
		out_scene[0] = '\0';
	}
	if (out_idx_a) {
		*out_idx_a = -1;
	}
	if (out_idx_b) {
		*out_idx_b = -1;
	}
	if (!source_a || source_a[0] == '\0' || !source_b || source_b[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->find_scene_with_pair) {
		return eng->scene_graph_ops->find_scene_with_pair(source_a, source_b, out_scene, out_size, out_idx_a,
								  out_idx_b, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	struct mfd_enum_scene_layer_ctx ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.source_a = source_a;
	ctx.source_b = source_b;
	ctx.pair_mode = true;
	ctx.idx_a = -1;
	ctx.idx_b = -1;
	obs_enum_scenes(enum_scenes_find_cb, &ctx);
	if (ctx.found) {
		if (out_scene && out_size > 0U) {
			snprintf(out_scene, out_size, "%s", ctx.scene_name);
		}
		if (out_idx_a) {
			*out_idx_a = ctx.idx_a;
		}
		if (out_idx_b) {
			*out_idx_b = ctx.idx_b;
		}
	}
	return ctx.found;
}

bool mfd_engine_is_studio_mode_active(const struct mfd_engine *eng)
{
	if (eng && eng->studio_mode_override_set) {
		return eng->studio_mode_override;
	}
	if (!obs_initialized()) {
		return false;
	}
	return eng ? eng->cached_studio_mode : false;
}

bool mfd_is_internal_filter(const char *filter_name, const char *type_id)
{
	if (filter_name &&
	    (strncmp(filter_name, "__mfd_", 6) == 0 || strncmp(filter_name, "__midi_fadester_", 16) == 0 ||
	     strcmp(filter_name, MFD_INTERNAL_OPACITY_FILTER_NAME) == 0)) {
		return true;
	}
	if (type_id && strcmp(type_id, MFD_LAYER_FILTER_ID) == 0) {
		return true;
	}
	return false;
}

static bool is_internal_source_name(const char *source_name)
{
	if (!source_name || source_name[0] == '\0') {
		return true;
	}
	return strncmp(source_name, "__mfd_", 6) == 0 || strncmp(source_name, "__midi_fadester_", 16) == 0;
}

static void collect_filter_numeric_props_depth(obs_properties_t *props, obs_data_t *settings, mfd_numeric_prop_cb cb,
					       void *param, int depth)
{
	if (!props || !cb || depth > 16) {
		return;
	}
	for (obs_property_t *p = obs_properties_first(props); p != NULL; obs_property_next(&p)) {
		enum obs_property_type ptype = obs_property_get_type(p);
		if (ptype == OBS_PROPERTY_GROUP) {
			obs_properties_t *sub = obs_property_group_content(p);
			if (sub && sub != props) {
				collect_filter_numeric_props_depth(sub, settings, cb, param, depth + 1);
			}
			continue;
		}
		if (ptype != OBS_PROPERTY_FLOAT && ptype != OBS_PROPERTY_INT) {
			continue;
		}
		const char *nm = obs_property_name(p);
		if (!nm || nm[0] == '\0') {
			continue;
		}
		struct mfd_filter_prop_info info;
		memset(&info, 0, sizeof(info));
		snprintf(info.name, sizeof(info.name), "%s", nm);
		const char *desc = obs_property_description(p);
		snprintf(info.label, sizeof(info.label), "%s", (desc && desc[0] != '\0') ? desc : nm);
		info.is_int = (ptype == OBS_PROPERTY_INT);
		if (info.is_int) {
			mfd_filter_normalize_bounds((double)obs_property_int_min(p), (double)obs_property_int_max(p),
						    &info.min_val, &info.max_val);
			int st = obs_property_int_step(p);
			info.step = (st > 0) ? (double)st : 1.0;
			const char *sfx = obs_property_int_suffix(p);
			snprintf(info.suffix, sizeof(info.suffix), "%s", sfx ? sfx : "");
			info.current_val = settings ? (double)obs_data_get_int(settings, nm) : info.min_val;
		} else {
			mfd_filter_normalize_bounds(obs_property_float_min(p), obs_property_float_max(p), &info.min_val,
						    &info.max_val);
			double st = obs_property_float_step(p);
			info.step = (isfinite(st) && st > 0.0) ? st : 0.01;
			const char *sfx = obs_property_float_suffix(p);
			snprintf(info.suffix, sizeof(info.suffix), "%s", sfx ? sfx : "");
			info.current_val = settings ? obs_data_get_double(settings, nm) : info.min_val;
		}
		cb(&info, param);
	}
}

void mfd_collect_filter_numeric_props(obs_properties_t *props, obs_data_t *settings, mfd_numeric_prop_cb cb,
				      void *param)
{
	collect_filter_numeric_props_depth(props, settings, cb, param, 0);
}

static bool find_filter_prop_in_props_depth(obs_properties_t *props, obs_data_t *settings, const char *prop_name,
					    bool *out_found_non_numeric, struct mfd_filter_prop_info *out_info,
					    int depth)
{
	if (!props || !prop_name || prop_name[0] == '\0' || depth > 16) {
		return false;
	}
	for (obs_property_t *p = obs_properties_first(props); p != NULL; obs_property_next(&p)) {
		enum obs_property_type ptype = obs_property_get_type(p);
		const char *nm = obs_property_name(p);
		if (ptype == OBS_PROPERTY_GROUP) {
			obs_properties_t *sub = obs_property_group_content(p);
			if (sub && sub != props &&
			    find_filter_prop_in_props_depth(sub, settings, prop_name, out_found_non_numeric, out_info,
							    depth + 1)) {
				return true;
			}
			if (nm && strcmp(nm, prop_name) == 0 && out_found_non_numeric) {
				*out_found_non_numeric = true;
			}
			continue;
		}
		if (!nm || strcmp(nm, prop_name) != 0) {
			continue;
		}
		if (ptype != OBS_PROPERTY_FLOAT && ptype != OBS_PROPERTY_INT) {
			if (out_found_non_numeric) {
				*out_found_non_numeric = true;
			}
			return false;
		}
		if (out_info) {
			memset(out_info, 0, sizeof(*out_info));
			snprintf(out_info->name, sizeof(out_info->name), "%s", nm);
			const char *desc = obs_property_description(p);
			snprintf(out_info->label, sizeof(out_info->label), "%s", (desc && desc[0] != '\0') ? desc : nm);
			out_info->is_int = (ptype == OBS_PROPERTY_INT);
			if (out_info->is_int) {
				mfd_filter_normalize_bounds((double)obs_property_int_min(p),
							    (double)obs_property_int_max(p), &out_info->min_val,
							    &out_info->max_val);
				int st = obs_property_int_step(p);
				out_info->step = (st > 0) ? (double)st : 1.0;
				const char *sfx = obs_property_int_suffix(p);
				snprintf(out_info->suffix, sizeof(out_info->suffix), "%s", sfx ? sfx : "");
				out_info->current_val = settings ? (double)obs_data_get_int(settings, nm)
								 : out_info->min_val;
			} else {
				mfd_filter_normalize_bounds(obs_property_float_min(p), obs_property_float_max(p),
							    &out_info->min_val, &out_info->max_val);
				double st = obs_property_float_step(p);
				out_info->step = (isfinite(st) && st > 0.0) ? st : 0.01;
				const char *sfx = obs_property_float_suffix(p);
				snprintf(out_info->suffix, sizeof(out_info->suffix), "%s", sfx ? sfx : "");
				out_info->current_val = settings ? obs_data_get_double(settings, nm)
								 : out_info->min_val;
			}
		}
		return true;
	}
	return false;
}

/*
 * obs_source_enum_filters holds the source's filter_mutex while it calls back. A scene-collection save takes
 * sources_mutex and then each filter_mutex (obs_save_sources_filtered -> obs_save_source), so a callback that
 * looks a source up by name (sources_mutex) under filter_mutex can deadlock OBS. The enumeration therefore only
 * copies name, type and enabled; the callbacks run after it has returned.
 */
struct mfd_filter_copy {
	char *name;
	char *id;
	bool enabled;
};

struct mfd_live_enum_filters_ctx {
	struct mfd_filter_copy *items;
	size_t num;
	size_t cap;
};

static void live_enum_source_filters_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct mfd_live_enum_filters_ctx *ctx = (struct mfd_live_enum_filters_ctx *)param;
	if (!ctx || !child || obs_source_removed(child)) {
		return;
	}
	const char *fnm = obs_source_get_name(child);
	const char *fid = obs_source_get_id(child);
	if (!fnm || fnm[0] == '\0' || mfd_is_internal_filter(fnm, fid)) {
		return;
	}
	if (ctx->num == ctx->cap) {
		size_t cap = ctx->cap ? ctx->cap * 2U : 8U;
		ctx->items = brealloc(ctx->items, cap * sizeof(*ctx->items));
		ctx->cap = cap;
	}
	ctx->items[ctx->num].name = bstrdup(fnm);
	ctx->items[ctx->num].id = bstrdup(fid ? fid : "");
	ctx->items[ctx->num].enabled = obs_source_enabled(child);
	ctx->num++;
}

static bool sg_enum_source_filters(const struct mfd_engine *eng, const char *source_name, bool *out_is_scene,
				   mfd_source_filter_cb cb, void *cb_param)
{
	if (out_is_scene) {
		*out_is_scene = false;
	}
	if (!source_name || source_name[0] == '\0') {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_source_filters) {
		return eng->scene_graph_ops->enum_source_filters(source_name, out_is_scene, cb, cb_param,
								 eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	if (obs_source_removed(src)) {
		obs_source_release(src);
		return false;
	}
	if (out_is_scene) {
		*out_is_scene = obs_source_is_scene(src);
	}
	if (cb) {
		struct mfd_live_enum_filters_ctx ctx = {NULL, 0U, 0U};
		obs_source_enum_filters(src, live_enum_source_filters_cb, &ctx);
		for (size_t i = 0; i < ctx.num; ++i) {
			cb(ctx.items[i].name, ctx.items[i].id, ctx.items[i].enabled, cb_param);
			bfree(ctx.items[i].name);
			bfree(ctx.items[i].id);
		}
		bfree(ctx.items);
	}
	obs_source_release(src);
	return true;
}

struct mfd_find_filter_cb_ctx {
	const char *target_filter;
	bool found;
	bool enabled;
};

static void find_filter_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	if (mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	struct mfd_find_filter_cb_ctx *ctx = (struct mfd_find_filter_cb_ctx *)param;
	if (ctx && ctx->target_filter && filter_name && strcmp(filter_name, ctx->target_filter) == 0) {
		ctx->found = true;
		ctx->enabled = enabled;
	}
}

static bool sg_filter_exists(const struct mfd_engine *eng, const char *source_name, const char *filter_name,
			     bool *out_enabled)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return false;
	}
	if (eng && eng->removing_filter_host && eng->removing_filter_name &&
	    strcmp(eng->removing_filter_host, source_name) == 0 &&
	    strcmp(eng->removing_filter_name, filter_name) == 0) {
		return false;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->get_filter_enabled) {
		bool en = true;
		if (!eng->scene_graph_ops->get_filter_enabled(source_name, filter_name, &en,
							      eng->scene_graph_user_data)) {
			return false;
		}
		if (out_enabled) {
			*out_enabled = en;
		}
		return true;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_source_filters) {
		struct mfd_find_filter_cb_ctx ctx = {filter_name, false, true};
		if (!sg_enum_source_filters(eng, source_name, NULL, find_filter_cb, &ctx)) {
			return false;
		}
		if (ctx.found && out_enabled) {
			*out_enabled = ctx.enabled;
		}
		return ctx.found;
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	if (obs_source_removed(src)) {
		obs_source_release(src);
		return false;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (!flt) {
		obs_source_release(src);
		return false;
	}
	bool ok = !obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt));
	if (ok && out_enabled) {
		*out_enabled = obs_source_enabled(flt);
	}
	obs_source_release(flt);
	obs_source_release(src);
	return ok;
}

static obs_properties_t *sg_create_filter_properties(const struct mfd_engine *eng, const char *source_name,
						     const char *filter_name)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return NULL;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->create_filter_properties) {
		return eng->scene_graph_ops->create_filter_properties(source_name, filter_name,
								      eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return NULL;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return NULL;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (!flt) {
		obs_source_release(src);
		return NULL;
	}
	obs_properties_t *props = NULL;
	if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
		props = obs_source_properties(flt);
	}
	obs_source_release(flt);
	obs_source_release(src);
	return props;
}

static obs_data_t *sg_get_filter_settings(const struct mfd_engine *eng, const char *source_name,
					  const char *filter_name)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return NULL;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->get_filter_settings) {
		return eng->scene_graph_ops->get_filter_settings(source_name, filter_name, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return NULL;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return NULL;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (!flt) {
		obs_source_release(src);
		return NULL;
	}
	obs_data_t *settings = NULL;
	if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
		settings = obs_source_get_settings(flt);
	}
	obs_source_release(flt);
	obs_source_release(src);
	return settings;
}

static void sg_update_filter_settings(const struct mfd_engine *eng, const char *source_name, const char *filter_name,
				      obs_data_t *settings)
{
	if (!settings || !source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->update_filter_settings) {
		eng->scene_graph_ops->update_filter_settings(source_name, filter_name, settings,
							     eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (flt) {
		if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
			obs_source_update(flt, settings);
		}
		obs_source_release(flt);
	}
	obs_source_release(src);
}

static void sg_set_filter_enabled(const struct mfd_engine *eng, const char *source_name, const char *filter_name,
				  bool enabled)
{
	if (!source_name || !source_name[0] || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, NULL)) {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->set_filter_enabled) {
		eng->scene_graph_ops->set_filter_enabled(source_name, filter_name, enabled, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return;
	}
	obs_source_t *flt = obs_source_get_filter_by_name(src, filter_name);
	if (flt) {
		if (!obs_source_removed(flt) && !mfd_is_internal_filter(filter_name, obs_source_get_id(flt))) {
			obs_source_set_enabled(flt, enabled);
		}
		obs_source_release(flt);
	}
	obs_source_release(src);
}

/* The value OBS holds now for the strip's filter setting, in the setting's own units. False when the source, the
 * filter or the setting is not there. */
static bool query_live_filter_value_locked(const struct mfd_engine *eng, const struct mfd_fader_slot *slot,
					   double *out_live)
{
	if (!eng || !slot || !out_live || slot->source_a_name[0] == '\0' || slot->filter_name[0] == '\0' ||
	    slot->filter_prop[0] == '\0') {
		return false;
	}
	if (!mfd_engine_source_exists(eng, slot->source_a_name) ||
	    !sg_filter_exists(eng, slot->source_a_name, slot->filter_name, NULL)) {
		return false;
	}
	obs_data_t *settings = sg_get_filter_settings(eng, slot->source_a_name, slot->filter_name);
	if (!settings) {
		return false;
	}
	bool has_val = obs_data_has_user_value(settings, slot->filter_prop) ||
		       obs_data_has_default_value(settings, slot->filter_prop);
	if (has_val) {
		bool is_int = slot->filter_prop_is_int;
		if (!slot->filter_prop_valid) {
			obs_data_item_t *it = obs_data_item_byname(settings, slot->filter_prop);
			if (it) {
				is_int = (obs_data_item_numtype(it) == OBS_DATA_NUM_INT);
				obs_data_item_release(&it);
			}
		}
		*out_live = is_int ? (double)obs_data_get_int(settings, slot->filter_prop)
				   : obs_data_get_double(settings, slot->filter_prop);
	}
	obs_data_release(settings);
	return has_val;
}

static float query_live_filter_unmapped_deflection_locked(const struct mfd_engine *eng,
							  const struct mfd_fader_slot *slot)
{
	if (!slot || fabs(slot->filter_to_val - slot->filter_from_val) < MFD_FILTER_MIN_RANGE_SPAN) {
		return 0.0f;
	}
	double live_val = 0.0;
	if (!query_live_filter_value_locked(eng, slot, &live_val)) {
		return 0.0f;
	}
	return mfd_filter_unmap_value(live_val, slot->filter_from_val, slot->filter_to_val);
}

/* True when the filter setting OBS holds is still what this strip last wrote (before its first write: what its value
 * maps to), so nobody else changed it. It compares with the written value, not with last_applied_val: that one runs
 * ahead whenever the write gate skips a step too small to write, and a second strip on the same filter would then
 * look like an outside edit and stop this strip's fade (B-02). */
static bool filter_live_value_is_ours(const struct mfd_fader_slot *slot, double live_val)
{
	if (slot->has_last_written_mapped) {
		return slot->filter_prop_is_int ? (llround(live_val) == slot->last_written_int)
						: (fabs(live_val - slot->last_written_float) <= 1e-9);
	}
	double cur_mapped = mfd_filter_map_value(slot->last_applied_val, slot->filter_from_val, slot->filter_to_val,
						 slot->filter_prop_min, slot->filter_prop_max,
						 slot->filter_prop_is_int);
	return slot->filter_prop_is_int ? (llround(live_val) == llround(cur_mapped))
					: (fabs(live_val - cur_mapped) <= 1e-6);
}

enum mfd_filter_meta_result {
	MFD_FILTER_META_NO_PROPS = 0, /* the filter gave no properties */
	MFD_FILTER_META_NUMERIC,      /* the setting is a number: filter_meta_* hold its range */
	MFD_FILTER_META_NOT_NUMERIC,  /* the setting exists but is not a number */
	MFD_FILTER_META_MISSING,      /* the filter has no setting of that name */
};

struct mfd_filter_meta {
	enum mfd_filter_meta_result result;
	struct mfd_filter_prop_info info;
};

static void read_filter_meta(const struct mfd_engine *eng, const char *source, const char *filter, const char *prop,
			     struct mfd_filter_meta *out);

/* prefetched: the setting's description, read by the caller with no engine lock held (QA-A03), or NULL to read it
 * here. Reading runs the filter's own properties code, so a caller that checks often (the Set up sheet, on every
 * change of a choice) reads first and locks afterwards; the video tick then never waits for a filter. */
static bool validate_filter_param_binding(const struct mfd_engine *eng, int slot_index, bool enabled,
					  const char *source_a, const char *filter_name, const char *filter_prop,
					  bool has_from_to, double *inout_from_val, double *inout_to_val,
					  struct mfd_filter_prop_info *out_prop_info,
					  const struct mfd_filter_meta *prefetched, struct mfd_msg *out_msg);

bool mfd_engine_validate_filter_param_binding_msg_locked(const struct mfd_engine *eng, int slot_index, bool enabled,
							 const char *source_a, const char *filter_name,
							 const char *filter_prop, bool has_from_to,
							 double *inout_from_val, double *inout_to_val,
							 struct mfd_filter_prop_info *out_prop_info,
							 struct mfd_msg *out_msg)
{
	return validate_filter_param_binding(eng, slot_index, enabled, source_a, filter_name, filter_prop, has_from_to,
					     inout_from_val, inout_to_val, out_prop_info, NULL, out_msg);
}

static bool validate_filter_param_binding(const struct mfd_engine *eng, int slot_index, bool enabled,
					  const char *source_a, const char *filter_name, const char *filter_prop,
					  bool has_from_to, double *inout_from_val, double *inout_to_val,
					  struct mfd_filter_prop_info *out_prop_info,
					  const struct mfd_filter_meta *prefetched, struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		mfd_msg_set(out_msg, MFD_MSG_INVALID_STRIP, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (!source_a || source_a[0] == '\0') {
		mfd_msg_set(out_msg, MFD_MSG_MISSING_SOURCE_A, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (!filter_name || filter_name[0] == '\0') {
		mfd_msg_set(out_msg, MFD_MSG_MISSING_FILTER_NAME, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (mfd_is_internal_filter(filter_name, NULL)) {
		mfd_msg_set(out_msg, MFD_MSG_FILTER_RESERVED, filter_name, NULL, NULL, NULL, NULL);
		return false;
	}
	if (!filter_prop || filter_prop[0] == '\0') {
		mfd_msg_set(out_msg, MFD_MSG_MISSING_FILTER_PROP, NULL, NULL, NULL, NULL, NULL);
		return false;
	}

	if (enabled) {
		for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
			if (j == slot_index) {
				continue;
			}
			const struct mfd_fader_slot *other = &eng->fader_slots[j];
			if (other->enabled && other->target_type == MFD_TARGET_FILTER_PARAM &&
			    strcmp(other->source_a_name, source_a) == 0 &&
			    strcmp(other->filter_name, filter_name) == 0 &&
			    strcmp(other->filter_prop, filter_prop) == 0) {
				set_already_controlled_msg(eng, out_msg, filter_prop, filter_name, j);
				return false;
			}
		}
	}

	if (!mfd_engine_source_exists(eng, source_a)) {
		mfd_msg_set(out_msg, MFD_MSG_SOURCE_NOT_FOUND, source_a, NULL, NULL, NULL, NULL);
		return false;
	}
	if (!sg_filter_exists(eng, source_a, filter_name, NULL)) {
		mfd_msg_set(out_msg, MFD_MSG_FILTER_NOT_FOUND, filter_name, source_a, NULL, NULL, NULL);
		return false;
	}

	bool found_non_numeric = false;
	bool found_numeric = false;
	struct mfd_filter_prop_info info;
	memset(&info, 0, sizeof(info));
	if (prefetched) {
		if (prefetched->result == MFD_FILTER_META_NO_PROPS) {
			mfd_msg_set(out_msg, MFD_MSG_FILTER_NO_PROPERTIES, filter_name, source_a, NULL, NULL, NULL);
			return false;
		}
		info = prefetched->info;
		found_numeric = prefetched->result == MFD_FILTER_META_NUMERIC;
		found_non_numeric = prefetched->result == MFD_FILTER_META_NOT_NUMERIC;
	} else {
		obs_properties_t *props = sg_create_filter_properties(eng, source_a, filter_name);
		if (!props) {
			mfd_msg_set(out_msg, MFD_MSG_FILTER_NO_PROPERTIES, filter_name, source_a, NULL, NULL, NULL);
			return false;
		}
		obs_data_t *settings = sg_get_filter_settings(eng, source_a, filter_name);
		found_numeric =
			find_filter_prop_in_props_depth(props, settings, filter_prop, &found_non_numeric, &info, 0);
		if (settings) {
			obs_data_release(settings);
		}
		obs_properties_destroy(props);
	}

	if (!found_numeric) {
		mfd_msg_set(out_msg, found_non_numeric ? MFD_MSG_SETTING_NOT_NUMERIC : MFD_MSG_SETTING_NOT_FOUND,
			    filter_prop, filter_name, NULL, NULL, NULL);
		return false;
	}

	double fval = info.min_val;
	double tval = info.max_val;
	if (has_from_to) {
		fval = inout_from_val ? *inout_from_val : info.min_val;
		tval = inout_to_val ? *inout_to_val : info.max_val;
	}
	if (!isfinite(fval) || !isfinite(tval)) {
		mfd_msg_set(out_msg, MFD_MSG_RANGE_NOT_FINITE, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (fabs(tval - fval) < MFD_FILTER_MIN_RANGE_SPAN) {
		mfd_msg_set(out_msg, MFD_MSG_RANGE_EMPTY, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (!mfd_filter_range_is_valid(fval, tval, info.min_val, info.max_val)) {
		set_range_outside_msg(out_msg, fval, tval, filter_prop, info.min_val, info.max_val);
		return false;
	}

	if (inout_from_val) {
		*inout_from_val = fval;
	}
	if (inout_to_val) {
		*inout_to_val = tval;
	}
	if (out_prop_info) {
		*out_prop_info = info;
	}
	return true;
}

static struct mfd_filter_bypass_lease *find_filter_lease_locked(struct mfd_engine *eng, const char *source_name,
								const char *filter_name)
{
	if (!eng || !source_name || !source_name[0] || !filter_name || !filter_name[0]) {
		return NULL;
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (l->in_use && strcmp(l->source_name, source_name) == 0 && strcmp(l->filter_name, filter_name) == 0) {
			return l;
		}
	}
	return NULL;
}

static void invalidate_filter_leases_for_removed_locked(struct mfd_engine *eng, const char *source_name,
							const char *filter_name)
{
	if (!eng || !source_name || !source_name[0]) {
		return;
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (!l->in_use || strcmp(l->source_name, source_name) != 0) {
			continue;
		}
		if (!filter_name || filter_name[0] == '\0' || strcmp(l->filter_name, filter_name) == 0) {
			memset(l, 0, sizeof(*l));
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->target_type == MFD_TARGET_FILTER_PARAM && strcmp(s->source_a_name, source_name) == 0 &&
		    (!filter_name || filter_name[0] == '\0' || strcmp(s->filter_name, filter_name) == 0)) {
			s->restore_persisted_pre_bypass = false;
		}
	}
}

static void sync_filter_bypass_lease_ex_locked(struct mfd_engine *eng, const char *source_name, const char *filter_name,
					       int exclude_slot, bool apply_enable_write)
{
	if (!eng || !source_name || !source_name[0] || !filter_name || !filter_name[0]) {
		return;
	}

	bool live_en = true;
	bool flt_exists = sg_filter_exists(eng, source_name, filter_name, &live_en);
	if (!flt_exists) {
		invalidate_filter_leases_for_removed_locked(eng, source_name, filter_name);
		return;
	}

	int active_slot_count = 0;
	int bypass_owner_count = 0;
	bool all_bypassed_at_zero = true;
	bool has_persisted_restore = false;
	bool persisted_pre_en = true;

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (i == exclude_slot) {
			continue;
		}
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (!s->enabled || s->target_type != MFD_TARGET_FILTER_PARAM || !s->cached_available) {
			continue;
		}
		if (strcmp(s->source_a_name, source_name) != 0 || strcmp(s->filter_name, filter_name) != 0) {
			continue;
		}
		++active_slot_count;
		if (s->filter_bypass_at_zero) {
			++bypass_owner_count;
			if (s->restore_persisted_pre_bypass && !has_persisted_restore) {
				has_persisted_restore = true;
				persisted_pre_en = s->filter_pre_bypass_enabled;
			}
			if (s->last_applied_val > MFD_FILTER_BYPASS_ZERO_THRESHOLD) {
				all_bypassed_at_zero = false;
			}
		} else {
			all_bypassed_at_zero = false;
		}
	}

	struct mfd_filter_bypass_lease *lease = find_filter_lease_locked(eng, source_name, filter_name);
	if (bypass_owner_count == 0) {
		if (lease) {
			bool restore_en = lease->pre_mfd_enabled;
			lease->writing_enable = true;
			lease->writing_value = restore_en;
			sg_set_filter_enabled(eng, source_name, filter_name, restore_en);
			lease->writing_enable = false;
			memset(lease, 0, sizeof(*lease));
		}
		return;
	}

	if (!lease) {
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			if (!eng->filter_leases[i].in_use) {
				lease = &eng->filter_leases[i];
				memset(lease, 0, sizeof(*lease));
				lease->in_use = true;
				snprintf(lease->source_name, sizeof(lease->source_name), "%s", source_name);
				snprintf(lease->filter_name, sizeof(lease->filter_name), "%s", filter_name);
				lease->pre_mfd_enabled = has_persisted_restore ? persisted_pre_en : live_en;
				lease->last_mfd_enabled = live_en;
				lease->has_written_enable = false;
				break;
			}
		}
	}
	if (!lease) {
		return;
	}

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (i == exclude_slot) {
			continue;
		}
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->enabled && s->target_type == MFD_TARGET_FILTER_PARAM &&
		    strcmp(s->source_a_name, source_name) == 0 && strcmp(s->filter_name, filter_name) == 0) {
			s->filter_pre_bypass_enabled = lease->pre_mfd_enabled;
			s->restore_persisted_pre_bypass = false;
		}
	}

	if (!apply_enable_write || active_slot_count <= 0) {
		return;
	}

	bool want_enabled = !all_bypassed_at_zero;
	if (!lease->has_written_enable || live_en != want_enabled || lease->last_mfd_enabled != want_enabled) {
		if (live_en != want_enabled) {
			lease->writing_enable = true;
			lease->writing_value = want_enabled;
			sg_set_filter_enabled(eng, source_name, filter_name, want_enabled);
			lease->writing_enable = false;
		}
		lease->has_written_enable = true;
		lease->last_mfd_enabled = want_enabled;
	}
}

void mfd_engine_detach_filter_binding_locked(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (s->target_type != MFD_TARGET_FILTER_PARAM || s->source_a_name[0] == '\0' || s->filter_name[0] == '\0') {
		return;
	}
	char prev_sa[MFD_SOURCE_NAME_MAX];
	char prev_fn[MFD_SOURCE_NAME_MAX];
	snprintf(prev_sa, sizeof(prev_sa), "%s", s->source_a_name);
	snprintf(prev_fn, sizeof(prev_fn), "%s", s->filter_name);
	sync_filter_bypass_lease_ex_locked(eng, prev_sa, prev_fn, slot_index, true);
}

void mfd_engine_restore_all_bypass_leases(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (!l->in_use) {
			continue;
		}
		for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
			struct mfd_fader_slot *s = &eng->fader_slots[j];
			if (s->target_type == MFD_TARGET_FILTER_PARAM &&
			    strcmp(s->source_a_name, l->source_name) == 0 &&
			    strcmp(s->filter_name, l->filter_name) == 0) {
				s->filter_pre_bypass_enabled = l->pre_mfd_enabled;
			}
		}
		if (sg_filter_exists(eng, l->source_name, l->filter_name, NULL)) {
			l->writing_enable = true;
			l->writing_value = l->pre_mfd_enabled;
			sg_set_filter_enabled(eng, l->source_name, l->filter_name, l->pre_mfd_enabled);
			l->writing_enable = false;
		}
		memset(l, 0, sizeof(*l));
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		eng->fader_slots[i].restore_persisted_pre_bypass = false;
	}
	mfd_engine_unlock(eng);
}

/* Reads one setting's description from the filter's properties. This runs the filter's own code
 * (obs_source_properties): never call it on the video thread. */
static void read_filter_meta(const struct mfd_engine *eng, const char *source, const char *filter, const char *prop,
			     struct mfd_filter_meta *out)
{
	memset(out, 0, sizeof(*out));
	obs_properties_t *props = sg_create_filter_properties(eng, source, filter);
	if (!props) {
		out->result = MFD_FILTER_META_NO_PROPS;
		return;
	}
	bool found_non_num = false;
	bool found_num = find_filter_prop_in_props_depth(props, NULL, prop, &found_non_num, &out->info, 0);
	obs_properties_destroy(props);
	out->result = found_num ? MFD_FILTER_META_NUMERIC
				: (found_non_num ? MFD_FILTER_META_NOT_NUMERIC : MFD_FILTER_META_MISSING);
}

static bool filter_meta_matches_slot(const struct mfd_fader_slot *slot)
{
	return slot->filter_meta_cached && strcmp(slot->filter_meta_source, slot->source_a_name) == 0 &&
	       strcmp(slot->filter_meta_filter, slot->filter_name) == 0 &&
	       strcmp(slot->filter_meta_prop, slot->filter_prop) == 0;
}

static void store_filter_meta_locked(struct mfd_fader_slot *slot, const char *source, const char *filter,
				     const char *prop, const struct mfd_filter_meta *meta)
{
	slot->filter_meta_cached = true;
	slot->filter_meta_stale = false;
	slot->filter_meta_result = (uint8_t)meta->result;
	snprintf(slot->filter_meta_source, sizeof(slot->filter_meta_source), "%s", source);
	snprintf(slot->filter_meta_filter, sizeof(slot->filter_meta_filter), "%s", filter);
	snprintf(slot->filter_meta_prop, sizeof(slot->filter_meta_prop), "%s", prop);
	slot->filter_meta_is_int = meta->info.is_int;
	slot->filter_meta_min = meta->info.min_val;
	slot->filter_meta_max = meta->info.max_val;
	slot->filter_meta_step = meta->info.step;
	snprintf(slot->filter_meta_suffix, sizeof(slot->filter_meta_suffix), "%s", meta->info.suffix);
	snprintf(slot->filter_meta_label, sizeof(slot->filter_meta_label), "%s", meta->info.label);
}

/* Whether this thread may run a filter's properties code inside a re-check. Not the video thread (A-03). */
static bool filter_meta_may_read_here(const struct mfd_engine *eng)
{
	return !eng->filter_meta_defer_for_test && !obs_in_task_thread(OBS_TASK_GRAPHICS);
}

/* Engine lock held. Every Filter Setting strip reads its filter's properties again at its next re-check. */
static void mark_filter_meta_stale_locked(struct mfd_engine *eng, const char *source, const char *filter)
{
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		/* a strip whose properties are being read for the first time is named by its binding */
		const char *s_source = s->filter_meta_cached ? s->filter_meta_source : s->source_a_name;
		const char *s_filter = s->filter_meta_cached ? s->filter_meta_filter : s->filter_name;
		if (source && strcmp(s_source, source) != 0) {
			continue;
		}
		if (filter && strcmp(s_filter, filter) != 0) {
			continue;
		}
		s->filter_meta_gen++; /* a read that began before this change is out of date when it lands */
		if (s->filter_meta_cached) {
			s->filter_meta_stale = true;
		}
	}
}

/*
 * UI thread, no engine lock held. Reads the filter properties that a re-check on the video thread could not, stores
 * them under the lock and re-checks. The properties code of a filter (this plugin's own, or anyone's) so never runs on
 * the video thread and never holds up the engine lock (A-03, B-09).
 */
static void refresh_filter_meta_on_ui_thread(struct mfd_engine *eng)
{
	struct {
		int slot;
		uint32_t gen;
		char source[MFD_SOURCE_NAME_MAX];
		char filter[MFD_SOURCE_NAME_MAX];
		char prop[MFD_FILTER_PROP_NAME_MAX];
		struct mfd_filter_meta meta;
	} *jobs = NULL;
	int n = 0;

	mfd_engine_lock(eng);
	if (!eng->filter_meta_refresh_pending) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->filter_meta_refresh_pending = false;
	jobs = bzalloc(sizeof(*jobs) * MFD_MAX_FADER_SLOTS);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (!s->enabled || s->target_type != MFD_TARGET_FILTER_PARAM || s->source_a_name[0] == '\0' ||
		    s->filter_name[0] == '\0' || s->filter_prop[0] == '\0') {
			continue;
		}
		if (filter_meta_matches_slot(s) && !s->filter_meta_stale) {
			continue;
		}
		jobs[n].slot = i;
		jobs[n].gen = s->filter_meta_gen;
		snprintf(jobs[n].source, sizeof(jobs[n].source), "%s", s->source_a_name);
		snprintf(jobs[n].filter, sizeof(jobs[n].filter), "%s", s->filter_name);
		snprintf(jobs[n].prop, sizeof(jobs[n].prop), "%s", s->filter_prop);
		++n;
	}
	mfd_engine_unlock(eng);

	for (int k = 0; k < n; ++k) {
		read_filter_meta(eng, jobs[k].source, jobs[k].filter, jobs[k].prop, &jobs[k].meta);
	}

	if (n > 0) {
		mfd_engine_lock(eng);
		for (int k = 0; k < n; ++k) {
			struct mfd_fader_slot *s = &eng->fader_slots[jobs[k].slot];
			/* the strip may have been set to something else meanwhile: then its own re-check reads */
			if (strcmp(s->source_a_name, jobs[k].source) == 0 &&
			    strcmp(s->filter_name, jobs[k].filter) == 0 && strcmp(s->filter_prop, jobs[k].prop) == 0) {
				store_filter_meta_locked(s, jobs[k].source, jobs[k].filter, jobs[k].prop,
							 &jobs[k].meta);
				if (s->filter_meta_gen != jobs[k].gen) {
					/* the filter changed while it was being read: what was stored may be the
					 * older range, so it is read once more */
					s->filter_meta_stale = true;
				}
			}
		}
		mfd_engine_reevaluate_slots(eng, true);
		mfd_engine_unlock(eng);
	}
	bfree(jobs);
}

static void refresh_filter_slot_metadata_locked(struct mfd_engine *eng, int slot_index, bool *out_avail,
						struct mfd_msg *out_reason)
{
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	bool avail = false;
	struct mfd_msg rsn;
	mfd_msg_clear(&rsn);

	slot->filter_prop_valid = false;
	if (!slot->enabled || slot->source_a_name[0] == '\0' || slot->filter_name[0] == '\0' ||
	    slot->filter_prop[0] == '\0') {
		mfd_msg_set(&rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
	} else if (mfd_is_internal_filter(slot->filter_name, NULL)) {
		mfd_msg_set(&rsn, MFD_MSG_FILTER_RESERVED, slot->filter_name, NULL, NULL, NULL, NULL);
	} else if (!mfd_engine_source_exists(eng, slot->source_a_name)) {
		invalidate_filter_leases_for_removed_locked(eng, slot->source_a_name, NULL);
		/* a source of this name that comes back (undo, a scene copy) may carry a same-named filter of another
		 * kind or range, and its filters load before any signal of it reaches this strip: read afresh (QA-A04) */
		slot->filter_meta_cached = false;
		mfd_msg_set(&rsn, MFD_MSG_SOURCE_DELETED, slot->source_a_name, NULL, NULL, NULL, NULL);
	} else if (!sg_filter_exists(eng, slot->source_a_name, slot->filter_name, NULL)) {
		invalidate_filter_leases_for_removed_locked(eng, slot->source_a_name, slot->filter_name);
		slot->filter_meta_cached = false; /* a filter of this name added later is read afresh */
		mfd_msg_set(&rsn, MFD_MSG_FILTER_REMOVED, slot->filter_name, slot->source_a_name, NULL, NULL, NULL);
	} else {
		/* The filter's properties are read once per binding and kept (A-03): a re-check on a scene change costs
		 * no properties call. They are read again when the binding changed or the filter changed, on a thread
		 * that may run the filter's code; the video thread hands that to the UI thread and goes on with what
		 * it last knew. */
		bool have = filter_meta_matches_slot(slot);
		if (!have || slot->filter_meta_stale) {
			if (filter_meta_may_read_here(eng)) {
				struct mfd_filter_meta meta;
				read_filter_meta(eng, slot->source_a_name, slot->filter_name, slot->filter_prop, &meta);
				store_filter_meta_locked(slot, slot->source_a_name, slot->filter_name,
							 slot->filter_prop, &meta);
				have = true;
			} else {
				eng->filter_meta_refresh_pending = true;
				eng->pending_ui_notify = true; /* wakes the UI thread's flush */
			}
		}
		if (!have || slot->filter_meta_result == MFD_FILTER_META_NO_PROPS ||
		    slot->filter_meta_result == MFD_FILTER_META_MISSING) {
			mfd_msg_set(&rsn, MFD_MSG_SETTING_UNAVAILABLE, slot->filter_prop, slot->filter_name, NULL, NULL,
				    NULL);
		} else if (slot->filter_meta_result == MFD_FILTER_META_NOT_NUMERIC) {
			mfd_msg_set(&rsn, MFD_MSG_SETTING_NOT_NUMERIC, slot->filter_prop, slot->filter_name, NULL, NULL,
				    NULL);
		} else {
			slot->filter_prop_is_int = slot->filter_meta_is_int;
			slot->filter_prop_min = slot->filter_meta_min;
			slot->filter_prop_max = slot->filter_meta_max;
			slot->filter_prop_step = slot->filter_meta_step;
			snprintf(slot->filter_prop_suffix, sizeof(slot->filter_prop_suffix), "%s",
				 slot->filter_meta_suffix);
			snprintf(slot->filter_prop_label, sizeof(slot->filter_prop_label), "%s",
				 slot->filter_meta_label);
			if (!mfd_filter_range_is_valid(slot->filter_from_val, slot->filter_to_val,
						       slot->filter_meta_min, slot->filter_meta_max)) {
				set_range_outside_msg(&rsn, slot->filter_from_val, slot->filter_to_val,
						      slot->filter_prop, slot->filter_meta_min, slot->filter_meta_max);
			} else {
				slot->filter_prop_valid = true;
				avail = true;
				mfd_msg_clear(&rsn);
			}
		}
	}

	if (out_avail) {
		*out_avail = avail;
	}
	if (out_reason) {
		*out_reason = rsn;
	}
}

/* What evaluate_slot_preconditions works out for one strip: its log label, whether it can act and why not, the
 * scene it acts in, and which of a mix's two layers is on top. */
struct slot_verdict {
	char lbl[128];
	bool avail;
	struct mfd_msg rsn;
	char res_scene[MFD_SOURCE_NAME_MAX];
	bool a_above_b;
};

/* A Two-Source Mix strip: both layers named, different, and in one scene (its own, or the first that holds both). */
static void verdict_two_source_mix(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, bool live,
				   struct slot_verdict *v)
{
	if (slot->source_a_name[0] != '\0' && slot->source_b_name[0] != '\0') {
		snprintf(v->lbl, sizeof(v->lbl), "%s -> %s", slot->source_a_name, slot->source_b_name);
	} else {
		snprintf(v->lbl, sizeof(v->lbl), "Crossfade A/B");
	}
	if (slot->source_a_name[0] == '\0' || slot->source_b_name[0] == '\0') {
		v->avail = false;
		mfd_msg_set(&v->rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
	} else if (strcmp(slot->source_a_name, slot->source_b_name) == 0) {
		v->avail = false;
		mfd_msg_set(&v->rsn, MFD_MSG_SAME_LAYER, NULL, NULL, NULL, NULL, NULL);
	} else if (!live) {
		v->avail = slot->cached_available;
		v->rsn = slot->cached_reason_msg;
	} else if (slot->scene_name[0] != '\0') {
		if (!mfd_engine_scene_exists(eng, slot->scene_name)) {
			v->avail = false;
			mfd_msg_set(&v->rsn, MFD_MSG_SCENE_DELETED, slot->scene_name, NULL, NULL, NULL, NULL);
		} else {
			int idx_a = sg_scene_item_stack_index(eng, slot->scene_name, slot->source_a_name);
			int idx_b = sg_scene_item_stack_index(eng, slot->scene_name, slot->source_b_name);
			if (idx_a < 0) {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_SCENE, slot->source_a_name, slot->scene_name,
					    NULL, NULL, NULL);
			} else if (idx_b < 0) {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_SCENE, slot->source_b_name, slot->scene_name,
					    NULL, NULL, NULL);
			} else {
				v->avail = true;
				mfd_msg_clear(&v->rsn);
				v->a_above_b = (idx_a > idx_b);
				snprintf(v->res_scene, sizeof(v->res_scene), "%s", slot->scene_name);
			}
		}
	} else {
		char found_sc[MFD_SOURCE_NAME_MAX] = {0};
		int idx_a = -1;
		int idx_b = -1;
		if (sg_find_scene_with_pair(eng, slot->source_a_name, slot->source_b_name, found_sc, sizeof(found_sc),
					    &idx_a, &idx_b)) {
			v->avail = true;
			mfd_msg_clear(&v->rsn);
			v->a_above_b = (idx_a > idx_b);
			snprintf(v->res_scene, sizeof(v->res_scene), "%s", found_sc);
		} else if (slot->resolved_scene_name[0] != '\0') {
			if (!mfd_engine_scene_exists(eng, slot->resolved_scene_name)) {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_SCENE_DELETED, slot->resolved_scene_name, NULL, NULL, NULL,
					    NULL);
			} else if (sg_scene_item_stack_index(eng, slot->resolved_scene_name, slot->source_a_name) < 0) {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_SCENE, slot->source_a_name,
					    slot->resolved_scene_name, NULL, NULL, NULL);
			} else {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_SCENE, slot->source_b_name,
					    slot->resolved_scene_name, NULL, NULL, NULL);
			}
		} else {
			v->avail = false;
			mfd_msg_set(&v->rsn, MFD_MSG_LAYERS_NOT_IN_SAME_SCENE, slot->source_a_name, slot->source_b_name,
				    NULL, NULL, NULL);
		}
	}
}

/* A Layer strip: its source in its own scene, or else in the scene on air, or else in any scene. */
static void verdict_single_layer(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, bool live,
				 struct slot_verdict *v)
{
	if (slot->source_a_name[0] != '\0') {
		snprintf(v->lbl, sizeof(v->lbl), "%s (Opacity)", slot->source_a_name);
	} else {
		snprintf(v->lbl, sizeof(v->lbl), "Layer Opacity");
	}
	if (slot->source_a_name[0] == '\0') {
		v->avail = false;
		mfd_msg_set(&v->rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
	} else if (!live) {
		v->avail = slot->cached_available;
		v->rsn = slot->cached_reason_msg;
	} else if (slot->scene_name[0] != '\0') {
		if (!mfd_engine_scene_exists(eng, slot->scene_name)) {
			v->avail = false;
			mfd_msg_set(&v->rsn, MFD_MSG_SCENE_DELETED, slot->scene_name, NULL, NULL, NULL, NULL);
		} else if (sg_scene_item_stack_index(eng, slot->scene_name, slot->source_a_name) < 0) {
			v->avail = false;
			mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_SCENE, slot->source_a_name, slot->scene_name, NULL,
				    NULL, NULL);
		} else {
			v->avail = true;
			mfd_msg_clear(&v->rsn);
			snprintf(v->res_scene, sizeof(v->res_scene), "%s", slot->scene_name);
		}
	} else {
		char found_sc[MFD_SOURCE_NAME_MAX] = {0};
		if (eng->cached_program_scene[0] != '\0' &&
		    sg_scene_item_stack_index(eng, eng->cached_program_scene, slot->source_a_name) >= 0) {
			/* A strip with no scene of its own acts on the scene on air when that scene
			 * holds its source (B-05): a show/hide or blend pad must change what the
			 * audience sees, not the first scene OBS happens to list. Re-checked on
			 * every Program change. */
			v->avail = true;
			mfd_msg_clear(&v->rsn);
			snprintf(v->res_scene, sizeof(v->res_scene), "%s", eng->cached_program_scene);
		} else if (mfd_engine_find_other_scene_with_layer(eng, slot->source_a_name, NULL, found_sc,
								  sizeof(found_sc))) {
			v->avail = true;
			mfd_msg_clear(&v->rsn);
			snprintf(v->res_scene, sizeof(v->res_scene), "%s", found_sc);
		} else if (slot->resolved_scene_name[0] != '\0') {
			if (!mfd_engine_scene_exists(eng, slot->resolved_scene_name)) {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_SCENE_DELETED, slot->resolved_scene_name, NULL, NULL, NULL,
					    NULL);
			} else {
				v->avail = false;
				mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_SCENE, slot->source_a_name,
					    slot->resolved_scene_name, NULL, NULL, NULL);
			}
		} else {
			v->avail = false;
			mfd_msg_set(&v->rsn, MFD_MSG_LAYER_NOT_IN_ANY_SCENE, slot->source_a_name, NULL, NULL, NULL,
				    NULL);
		}
	}
}

/* An Audio Volume strip: its source exists and has audio. */
static void verdict_audio_volume(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, bool live,
				 struct slot_verdict *v)
{
	if (slot->source_a_name[0] != '\0') {
		snprintf(v->lbl, sizeof(v->lbl), "%s (Audio)", slot->source_a_name);
	} else {
		snprintf(v->lbl, sizeof(v->lbl), "Audio Volume");
	}
	if (slot->source_a_name[0] == '\0') {
		v->avail = false;
		mfd_msg_set(&v->rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
	} else if (!live) {
		v->avail = slot->cached_available;
		v->rsn = slot->cached_reason_msg;
	} else if (!mfd_engine_source_exists(eng, slot->source_a_name)) {
		v->avail = false;
		mfd_msg_set(&v->rsn, MFD_MSG_SOURCE_DELETED, slot->source_a_name, NULL, NULL, NULL, NULL);
	} else if (!mfd_engine_source_has_audio(eng, slot->source_a_name)) {
		v->avail = false;
		mfd_msg_set(&v->rsn, MFD_MSG_NO_AUDIO, slot->source_a_name, NULL, NULL, NULL, NULL);
	} else {
		v->avail = true;
		mfd_msg_clear(&v->rsn);
	}
}

/*
 * live == false is a status read: the in-process bridge that Visual Sidechain polls on the graphics thread every
 * frame, the vendor API, a refused fader write. These run often and from any thread, so they report what
 * mfd_engine_reevaluate_slots() last found instead of looking the scene graph up (obs_get_source_by_name and
 * obs_scene_enum_items take libobs's sources, canvas and scene locks) each time. The re-check runs on every scene,
 * item, source and frontend change (see mfd_engine_report_scene_graph_change). live == true is for that re-check
 * and for checks made before a change is committed.
 */
static void evaluate_slot_preconditions(const struct mfd_engine *eng, const struct mfd_fader_slot *slot,
					char *out_label, size_t label_size, bool *out_available,
					struct mfd_msg *out_reason, char *out_resolved_scene, size_t resolved_size,
					bool *out_a_above_b, bool live)
{
	struct slot_verdict v;
	memset(&v, 0, sizeof(v));
	snprintf(v.lbl, sizeof(v.lbl), "Unassigned");
	mfd_msg_set(&v.rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
	v.a_above_b = slot ? slot->a_is_above_b : false;

	if (slot) {
		if (slot->target_type == MFD_TARGET_TWO_SOURCE_MIX || slot->target_type == MFD_TARGET_SINGLE_LAYER) {
			snprintf(v.res_scene, sizeof(v.res_scene), "%s", slot->resolved_scene_name);
		}
		if (!slot->enabled) {
			snprintf(v.lbl, sizeof(v.lbl), "Disabled");
			v.avail = false;
			mfd_msg_set(&v.rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
		} else {
			switch (slot->target_type) {
			case MFD_TARGET_UNASSIGNED:
				snprintf(v.lbl, sizeof(v.lbl), "Unassigned");
				v.avail = false;
				mfd_msg_set(&v.rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
				break;

			case MFD_TARGET_MIX_MASTER:
				snprintf(v.lbl, sizeof(v.lbl), "Whole mix");
				v.avail = true; /* Can request; the external mix provider confirms application. */
				mfd_msg_clear(&v.rsn);
				break;

			case MFD_TARGET_TBAR:
				snprintf(v.lbl, sizeof(v.lbl), "Studio Mode T-Bar");
				if (!mfd_engine_is_studio_mode_active(eng)) {
					v.avail = false;
					mfd_msg_set(&v.rsn, MFD_MSG_STUDIO_MODE_OFF, NULL, NULL, NULL, NULL, NULL);
				} else if (eng && eng->tbar_error[0] != '\0') {
					v.avail = false;
					mfd_msg_set(&v.rsn, MFD_MSG_TBAR_SLIDER_MISSING, NULL, NULL, NULL, NULL, NULL);
				} else {
					v.avail = true;
					mfd_msg_clear(&v.rsn);
				}
				break;

			case MFD_TARGET_TWO_SOURCE_MIX:
				verdict_two_source_mix(eng, slot, live, &v);
				break;

			case MFD_TARGET_SINGLE_LAYER:
				verdict_single_layer(eng, slot, live, &v);
				break;

			case MFD_TARGET_AUDIO_VOLUME:
				verdict_audio_volume(eng, slot, live, &v);
				break;

			case MFD_TARGET_FILTER_PARAM:
				if (slot->source_a_name[0] != '\0' && slot->filter_name[0] != '\0' &&
				    slot->filter_prop[0] != '\0') {
					snprintf(v.lbl, sizeof(v.lbl), "%s: %s (%s)", slot->source_a_name,
						 slot->filter_name, slot->filter_prop);
					v.avail = slot->cached_available;
					v.rsn = slot->cached_reason_msg;
				} else {
					snprintf(v.lbl, sizeof(v.lbl), "Filter Setting");
					v.avail = false;
					mfd_msg_set(&v.rsn, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
				}
				break;
			}
		}
	}

	if (out_label && label_size > 0U) {
		/* v.lbl holds names cut to 127 bytes: a long non-ASCII name can end in half a character */
		snprintf(out_label, label_size, "%s", v.lbl);
		mfd_utf8_make_valid(out_label);
	}
	if (out_available) {
		*out_available = v.avail;
	}
	if (out_reason) {
		*out_reason = v.rsn;
	}
	if (out_resolved_scene && resolved_size > 0U) {
		snprintf(out_resolved_scene, resolved_size, "%s", v.res_scene);
	}
	if (out_a_above_b) {
		*out_a_above_b = v.a_above_b;
	}
}

static bool vj_live_opacity(struct mfd_engine *eng, const char *source, float *out);

/*
 * Engine lock held. The strip's output is at v and its fader has to pick it up again from where it is: the takeover
 * starts over at v, and if the fader's position is known it is kept, with the hint pointing from it to v.
 */
static void restart_pickup_at_locked(const struct mfd_engine *eng, struct mfd_fader_slot *s, float v)
{
	bool had_raw = s->takeover.has_last_fader;
	float raw = s->last_raw_norm;
	mfd_takeover_init(&s->takeover, v, MFD_DEFAULT_PICKUP_THRESHOLD, false);
	configure_slot_takeover_mode(eng, s);
	if (had_raw) {
		s->takeover.has_last_fader = true;
		s->takeover.last_raw_fader = raw;
		s->takeover.last_fader = raw;
		float diff = raw - v;
		s->takeover.hint = fabsf(diff) <= s->takeover.pickup_threshold ? MFD_HINT_NONE
				   : diff < 0.0f                               ? MFD_HINT_MOVE_UP
									       : MFD_HINT_MOVE_DOWN;
	}
}

/*
 * B-06: a Two-Source Mix strip that was just loaded or bound has written nothing yet, and OBS has both sources at the
 * level their layer filters hold (100 % after a restart: the filter does not save its level). So the picture shows the
 * upper source. The strip starts at the end that shows the same picture, with pick-up from where its fader is,
 * instead of claiming 0 % (source A) while Program shows B. Nothing is written to OBS: the picture does not move.
 */
static void mix_start_where_obs_is_locked(struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	slot->mix_start_pending = false;
	float level_a = 1.0f;
	float level_b = 1.0f;
	(void)vj_live_opacity(eng, slot->source_a_name, &level_a);
	(void)vj_live_opacity(eng, slot->source_b_name, &level_b);
	const bool a_above = slot->a_is_above_b;
	const bool upper_shown = (a_above ? level_a : level_b) >= 0.5f;
	/* 0 % is source A, 100 % is source B */
	const float t = (a_above == upper_shown) ? 0.0f : 1.0f;

	mfd_smoother_reset(&slot->smoother, t);
	restart_pickup_at_locked(eng, slot, t);
	slot->last_applied_val = t;
	mfd_crossfade_eval_stacked(slot->curve, t, a_above, &slot->last_alpha_a, &slot->last_alpha_b);
}

void mfd_engine_reevaluate_slots(struct mfd_engine *eng, bool emit_if_changed)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->vj_refresh_pending = true;
	/* The scene graph or a strip changed: strips that follow the scene on air look again (a layer added, removed or
	 * reordered in the scene on air). The refill changes nothing when the layers are the same. */
	for (int k = 1; k <= (int)MFD_VJ_STRIPS && eng->frontend_finished_loading; ++k) {
		if (eng->fader_slots[k].auto_follow) {
			eng->vj_follow_pending = true;
			break;
		}
	}
	bool any_changed = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		bool prev_avail = slot->cached_available;
		bool prev_a_above_b = slot->a_is_above_b;
		const struct mfd_msg prev_reason = slot->cached_reason_msg;

		bool new_avail = false;
		struct mfd_msg new_reason;
		mfd_msg_clear(&new_reason);
		char new_resolved[MFD_SOURCE_NAME_MAX] = {0};
		bool new_a_above_b = slot->a_is_above_b;
		if (slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM) {
			refresh_filter_slot_metadata_locked(eng, i, &new_avail, &new_reason);
		} else {
			slot->filter_prop_valid = false;
			evaluate_slot_preconditions(eng, slot, NULL, 0U, &new_avail, &new_reason, new_resolved,
						    sizeof(new_resolved), &new_a_above_b, true);
		}

		slot->cached_available = new_avail;
		slot->cached_reason_msg = new_reason;
		mfd_msg_format_english(&new_reason, slot->cached_reason, sizeof(slot->cached_reason));
		snprintf(slot->resolved_scene_name, sizeof(slot->resolved_scene_name), "%s", new_resolved);
		slot->a_is_above_b = new_a_above_b;

		if (prev_avail && !new_avail) {
			if (slot->auto_fade_active && slot->auto_fade_is_reset_fx) {
				slot->pending_clean_apply_on_avail = true;
				slot->last_applied_val = 0.0f;
				mfd_takeover_sync_external(&slot->takeover, 0.0f);
			}
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			if (i >= 1 && i <= (int)MFD_VJ_STRIPS && (eng->vj.faded & (1U << (unsigned)(i - 1)))) {
				mfd_vj_pads_set_faded(&eng->vj, i - 1, false);
			}
			slot->has_last_written_mapped = false;
			mfd_smoother_reset(&slot->smoother, slot->last_applied_val);
			if (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_THROWING) {
				slot->tbar_state = MFD_TBAR_IDLE;
				slot->tbar_midi_active = false;
				slot->tbar_from_idle_band = false;
				slot->last_applied_val = 0.0f;
				mfd_smoother_reset(&slot->smoother, 0.0f);
			}
		} else if (!prev_avail && new_avail && slot->pending_clean_apply_on_avail &&
			   !(eng->setup_session.active && eng->setup_session.slot_index == i)) {
			slot->pending_clean_apply_on_avail = false;
			slot->has_last_written_mapped = false;
			slot->last_applied_val = 0.0f;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_engine_apply_slot_output_ex(eng, slot, 0.0f, false);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			mfd_engine_notify_fader_changed(eng, i, "api");
			any_changed = true;
		} else if (new_avail && slot->target_type == MFD_TARGET_TWO_SOURCE_MIX && slot->mix_start_pending &&
			   !(eng->setup_session.active && eng->setup_session.slot_index == i)) {
			mix_start_where_obs_is_locked(eng, slot);
			any_changed = true;
		} else if (new_avail && slot->target_type == MFD_TARGET_TWO_SOURCE_MIX && prev_avail &&
			   prev_a_above_b != new_a_above_b &&
			   !(eng->setup_session.active && eng->setup_session.slot_index == i)) {
			mfd_engine_apply_slot_output(eng, slot, slot->last_applied_val);
		}

		if (prev_avail != new_avail || prev_a_above_b != new_a_above_b ||
		    !mfd_msg_equal(&prev_reason, &new_reason)) {
			any_changed = true;
		}
	}

	/* Acquire or synchronize bypass_at_zero leases for all available filter_param slots */
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM && slot->cached_available) {
			/* Acquire lease without forcing an unsolicited enable write until a value/reset write runs */
			sync_filter_bypass_lease_ex_locked(eng, slot->source_a_name, slot->filter_name, -1, false);
		}
	}

	mfd_engine_mark_ui_dirty(eng, true);
	if (any_changed && emit_if_changed) {
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
}

/* Engine lock held. Renames every reference; no re-check (a batch of renames re-checks once, at the end). */
static bool rename_source_refs_locked(struct mfd_engine *eng, const char *prev_name, const char *new_name)
{
	if (!eng || !prev_name || prev_name[0] == '\0' || !new_name || new_name[0] == '\0' ||
	    strcmp(prev_name, new_name) == 0) {
		return false;
	}
	/* The APC40 home blends are remembered by scene + source name: a renamed scene or source keeps its home. */
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		char buf[sizeof(eng->vj_home_key[0])];
		snprintf(buf, sizeof(buf), "%s", eng->vj_home_key[i]);
		char *nl = strchr(buf, '\n');
		if (!nl) {
			continue;
		}
		*nl = '\0';
		const char *sc = strcmp(buf, prev_name) == 0 ? new_name : buf;
		const char *src = strcmp(nl + 1, prev_name) == 0 ? new_name : nl + 1;
		snprintf(eng->vj_home_key[i], sizeof(eng->vj_home_key[i]), "%s\n%s", sc, src);
	}
	bool touched = false;
	/* A renamed scene: its Scene Launch button, Program / Preview and an asked Preview keep pointing at it. */
	for (int j = 0; j < eng->cached_scene_count && j < (int)MFD_VJ_SCENES; ++j) {
		if (strcmp(eng->cached_scenes[j], prev_name) == 0) {
			snprintf(eng->cached_scenes[j], sizeof(eng->cached_scenes[j]), "%s", new_name);
			touched = true;
		}
	}
	char *const scene_names[] = {eng->cached_program_scene, eng->cached_preview_scene, eng->vj_preview_asked};
	for (size_t k = 0U; k < sizeof(scene_names) / sizeof(scene_names[0]); ++k) {
		if (strcmp(scene_names[k], prev_name) == 0) {
			snprintf(scene_names[k], MFD_SOURCE_NAME_MAX, "%s", new_name);
			touched = true;
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (l->in_use && strcmp(l->source_name, prev_name) == 0) {
			snprintf(l->source_name, sizeof(l->source_name), "%s", new_name);
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		bool slot_touched = false;
		if (strcmp(s->source_a_name, prev_name) == 0) {
			if (filter_meta_matches_slot(s)) { /* the same source under a new name */
				snprintf(s->filter_meta_source, sizeof(s->filter_meta_source), "%s", new_name);
			}
			snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", new_name);
			slot_touched = true;
		}
		if (strcmp(s->source_b_name, prev_name) == 0) {
			snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", new_name);
			slot_touched = true;
		}
		if (strcmp(s->scene_name, prev_name) == 0) {
			snprintf(s->scene_name, sizeof(s->scene_name), "%s", new_name);
			slot_touched = true;
		}
		if (strcmp(s->resolved_scene_name, prev_name) == 0) {
			snprintf(s->resolved_scene_name, sizeof(s->resolved_scene_name), "%s", new_name);
			slot_touched = true;
		}
		if (slot_touched) {
			mfd_engine_bump_slot_rev(eng, i);
			touched = true;
		}
	}
	return touched;
}

void mfd_engine_handle_source_rename(struct mfd_engine *eng, const char *prev_name, const char *new_name)
{
	if (!eng || !prev_name || prev_name[0] == '\0' || !new_name || new_name[0] == '\0' ||
	    strcmp(prev_name, new_name) == 0) {
		return;
	}
	mfd_engine_lock(eng);
	const bool touched = rename_source_refs_locked(eng, prev_name, new_name);
	mfd_engine_reevaluate_slots(eng, false);
	if (touched) {
		mfd_engine_notify_state_changed(eng);
		unlock_and_persist_global_config(eng);
		return;
	}
	mfd_engine_unlock(eng);
}

/* Engine lock held. Renames the filter's references; no re-check. */
static bool rename_filter_refs_locked(struct mfd_engine *eng, const char *parent_source_name, const char *prev_name,
				      const char *new_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !prev_name || !prev_name[0] || !new_name ||
	    !new_name[0] || strcmp(prev_name, new_name) == 0) {
		return false;
	}
	bool touched = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_filter_bypass_lease *l = &eng->filter_leases[i];
		if (l->in_use && strcmp(l->source_name, parent_source_name) == 0 &&
		    strcmp(l->filter_name, prev_name) == 0) {
			snprintf(l->filter_name, sizeof(l->filter_name), "%s", new_name);
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->target_type == MFD_TARGET_FILTER_PARAM && strcmp(s->source_a_name, parent_source_name) == 0 &&
		    strcmp(s->filter_name, prev_name) == 0) {
			if (filter_meta_matches_slot(s)) { /* the same filter under a new name: its properties hold */
				snprintf(s->filter_meta_filter, sizeof(s->filter_meta_filter), "%s", new_name);
			}
			snprintf(s->filter_name, sizeof(s->filter_name), "%s", new_name);
			mfd_engine_bump_slot_rev(eng, i);
			touched = true;
		}
	}
	return touched;
}

void mfd_engine_handle_filter_rename(struct mfd_engine *eng, const char *parent_source_name, const char *prev_name,
				     const char *new_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !prev_name || !prev_name[0] || !new_name ||
	    !new_name[0] || strcmp(prev_name, new_name) == 0) {
		return;
	}
	mfd_engine_lock(eng);
	const bool touched = rename_filter_refs_locked(eng, parent_source_name, prev_name, new_name);
	mfd_engine_reevaluate_slots(eng, false);
	if (touched) {
		mfd_engine_notify_state_changed(eng);
		unlock_and_persist_global_config(eng);
		return;
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_removed(struct mfd_engine *eng, const char *parent_source_name, const char *filter_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0]) {
		return;
	}
	mfd_engine_lock(eng);
	invalidate_filter_leases_for_removed_locked(eng, parent_source_name, filter_name);
	eng->removing_filter_host = parent_source_name;
	eng->removing_filter_name = filter_name;
	mfd_engine_reevaluate_slots(eng, true);
	eng->removing_filter_host = NULL;
	eng->removing_filter_name = NULL;
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_enable(struct mfd_engine *eng, const char *parent_source_name, const char *filter_name,
				     bool enabled)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !filter_name || !filter_name[0]) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_filter_bypass_lease *lease = find_filter_lease_locked(eng, parent_source_name, filter_name);
	if (lease) {
		if (lease->writing_enable && enabled == lease->writing_value) {
			/* the echo of Midi Fadester's own write. An operator toggle queued earlier and applied during that
			 * write (the owner-thread path drains first) carries its own value and is recorded below. */
			mfd_engine_unlock(eng);
			return;
		}
		/* Operator manually toggled filter eye icon in OBS: respect & record as new pre-bypass baseline */
		lease->pre_mfd_enabled = enabled;
		lease->last_mfd_enabled = enabled;
		lease->has_written_enable = true;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *s = &eng->fader_slots[i];
			if (s->target_type == MFD_TARGET_FILTER_PARAM &&
			    strcmp(s->source_a_name, parent_source_name) == 0 &&
			    strcmp(s->filter_name, filter_name) == 0) {
				s->filter_pre_bypass_enabled = enabled;
			}
		}
		if (obs_initialized()) {
			eng->pending_config_save = true;
		}
	}
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_settings_updated(struct mfd_engine *eng, const char *parent_source_name,
					       const char *filter_name)
{
	if (!eng || !parent_source_name || !parent_source_name[0] || !filter_name || !filter_name[0]) {
		return;
	}
	mfd_engine_lock(eng);
	obs_data_t *settings = NULL;
	bool any_synced = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled || slot->target_type != MFD_TARGET_FILTER_PARAM || !slot->cached_available ||
		    !slot->filter_prop_valid) {
			continue;
		}
		if (strcmp(slot->source_a_name, parent_source_name) != 0 ||
		    strcmp(slot->filter_name, filter_name) != 0) {
			continue;
		}
		if (slot->writing_filter_settings) {
			continue;
		}
		if (!settings) {
			settings = sg_get_filter_settings(eng, parent_source_name, filter_name);
			if (!settings) {
				break;
			}
		}
		bool has_val = obs_data_has_user_value(settings, slot->filter_prop) ||
			       obs_data_has_default_value(settings, slot->filter_prop);
		if (!has_val) {
			continue;
		}
		double live_val = slot->filter_prop_is_int ? (double)obs_data_get_int(settings, slot->filter_prop)
							   : obs_data_get_double(settings, slot->filter_prop);
		if (filter_live_value_is_ours(slot, live_val)) {
			continue;
		}
		float t_ext = mfd_filter_unmap_value(live_val, slot->filter_from_val, slot->filter_to_val);
		slot->auto_fade_active = false;
		slot->auto_fade_is_reset_fx = false;
		slot->pending_clean_apply_on_avail = false;
		slot->has_last_written_mapped = false;
		slot->last_applied_val = t_ext;
		mfd_smoother_reset(&slot->smoother, t_ext);
		mfd_takeover_sync_external(&slot->takeover, t_ext);
		slot->sec_since_last_fader_event = 1.0f;
		mfd_engine_notify_fader_changed(eng, i, "obs");
		any_synced = true;
	}
	if (settings) {
		obs_data_release(settings);
	}
	if (any_synced) {
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_handle_filter_properties_updated(struct mfd_engine *eng, const char *parent_source_name,
						 const char *filter_name)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	mark_filter_meta_stale_locked(eng, parent_source_name, filter_name);
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_get_slot_status(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, char *out_label,
				size_t label_size, bool *out_available, char *out_reason, size_t reason_size)
{
	struct mfd_msg msg;
	evaluate_slot_preconditions(eng, slot, out_label, label_size, out_available, &msg, NULL, 0U, NULL, false);
	mfd_msg_format_english(&msg, out_reason, reason_size);
}

void mfd_engine_get_slot_status_msg(const struct mfd_engine *eng, const struct mfd_fader_slot *slot, char *out_label,
				    size_t label_size, bool *out_available, char *out_reason, size_t reason_size,
				    struct mfd_msg *out_msg)
{
	struct mfd_msg msg;
	evaluate_slot_preconditions(eng, slot, out_label, label_size, out_available, &msg, NULL, 0U, NULL, false);
	mfd_msg_format_english(&msg, out_reason, reason_size);
	if (out_msg) {
		*out_msg = msg;
	}
}

void mfd_engine_notify_fader_changed(struct mfd_engine *eng, int slot_index, const char *origin)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	const char *orig = (origin && *origin) ? origin : "api";

	if (slot->sec_since_last_fader_event >= MFD_FADER_EVENT_MIN_INTERVAL_SEC) {
		slot->sec_since_last_fader_event = 0.0f;
		slot->fader_event_pending = false;
		eng->pending_fader_emits[slot_index].pending = true;
		snprintf(eng->pending_fader_emits[slot_index].slot_id,
			 sizeof(eng->pending_fader_emits[slot_index].slot_id), "%s", slot->id);
		eng->pending_fader_emits[slot_index].value = slot->last_applied_val;
		mfd_engine_format_slot_display_value(slot, slot->last_applied_val,
						     eng->pending_fader_emits[slot_index].display_value,
						     sizeof(eng->pending_fader_emits[slot_index].display_value));
		snprintf(eng->pending_fader_emits[slot_index].origin,
			 sizeof(eng->pending_fader_emits[slot_index].origin), "%s", orig);
	} else {
		slot->fader_event_pending = true;
		snprintf(slot->pending_origin, sizeof(slot->pending_origin), "%s", orig);
	}
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
}

void mfd_engine_notify_state_changed(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->pending_state_event = true;
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

bool mfd_engine_is_slot_writable(const struct mfd_engine *eng, const struct mfd_fader_slot *slot)
{
	if (!slot || !slot->enabled) {
		return false;
	}
	if (slot->target_type == MFD_TARGET_UNASSIGNED) {
		return true;
	}
	if (slot->target_type == MFD_TARGET_TBAR && eng && eng->studio_mode_override_set) {
		return eng->studio_mode_override && (eng->tbar_error[0] == '\0');
	}
	return slot->cached_available;
}

/* --- Heap-Only Atomic UI Catalog & Snapshot --- */

struct mfd_ui_catalog *mfd_ui_catalog_create(void)
{
	return (struct mfd_ui_catalog *)calloc(1U, sizeof(struct mfd_ui_catalog));
}

void mfd_ui_catalog_destroy(struct mfd_ui_catalog *cat)
{
	free(cat);
}

static int cmp_scene_entries(const void *a, const void *b)
{
	const struct mfd_ui_scene_entry *sa = (const struct mfd_ui_scene_entry *)a;
	const struct mfd_ui_scene_entry *sb = (const struct mfd_ui_scene_entry *)b;
	return ascii_stricmp(sa->name, sb->name);
}

static int cmp_audio_entries(const void *a, const void *b)
{
	const struct mfd_ui_audio_entry *aa = (const struct mfd_ui_audio_entry *)a;
	const struct mfd_ui_audio_entry *ab = (const struct mfd_ui_audio_entry *)b;
	return ascii_stricmp(aa->name, ab->name);
}

static int cmp_filter_host_entries(const void *a, const void *b)
{
	const struct mfd_ui_filter_host_summary *ha = (const struct mfd_ui_filter_host_summary *)a;
	const struct mfd_ui_filter_host_summary *hb = (const struct mfd_ui_filter_host_summary *)b;
	return ascii_stricmp(ha->name, hb->name);
}

static void count_user_filters_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	UNUSED_PARAMETER(enabled);
	if (mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	uint32_t *cnt = (uint32_t *)param;
	if (cnt) {
		++(*cnt);
	}
}

struct mfd_ui_catalog_host_ctx {
	const struct mfd_engine *eng;
	struct mfd_ui_catalog *cat;
};

static void add_ui_catalog_filter_host_cb(const char *source_name, bool is_scene, void *param)
{
	struct mfd_ui_catalog_host_ctx *ctx = (struct mfd_ui_catalog_host_ctx *)param;
	if (!ctx || !ctx->cat || is_internal_source_name(source_name)) {
		return;
	}
	for (size_t i = 0U; i < ctx->cat->filter_host_count; ++i) {
		if (strcmp(ctx->cat->filter_hosts[i].name, source_name) == 0) {
			return;
		}
	}
	if (ctx->cat->filter_host_count >= MFD_CATALOG_MAX_FILTER_HOSTS) {
		ctx->cat->filter_hosts_truncated = true;
		return;
	}
	uint32_t fcnt = 0U;
	(void)sg_enum_source_filters(ctx->eng, source_name, NULL, count_user_filters_cb, &fcnt);
	struct mfd_ui_filter_host_summary *h = &ctx->cat->filter_hosts[ctx->cat->filter_host_count++];
	memset(h, 0, sizeof(*h));
	snprintf(h->name, sizeof(h->name), "%s", source_name);
	h->is_scene = is_scene;
	h->filter_count = fcnt;
}

struct mfd_live_host_enum_ctx {
	mfd_filter_host_cb cb;
	void *cb_param;
	bool scenes_pass;
};

static bool live_enum_hosts_cb(void *param, obs_source_t *src)
{
	struct mfd_live_host_enum_ctx *ctx = (struct mfd_live_host_enum_ctx *)param;
	if (!ctx || !ctx->cb || !src || obs_source_removed(src)) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (is_internal_source_name(nm)) {
		return true;
	}
	ctx->cb(nm, ctx->scenes_pass || obs_source_is_scene(src), ctx->cb_param);
	return true;
}

static void sg_enum_filter_hosts(const struct mfd_engine *eng, mfd_filter_host_cb cb, void *cb_param)
{
	if (!cb) {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->enum_filter_hosts) {
		eng->scene_graph_ops->enum_filter_hosts(cb, cb_param, eng->scene_graph_user_data);
		return;
	}
	if (!obs_initialized()) {
		return;
	}
	struct mfd_live_host_enum_ctx sctx = {cb, cb_param, true};
	obs_enum_scenes(live_enum_hosts_cb, &sctx);
	struct mfd_live_host_enum_ctx ictx = {cb, cb_param, false};
	obs_enum_sources(live_enum_hosts_cb, &ictx);
}

/* Whether a scene item shows a picture. OBS reports every media source as video, even one playing a music file, so a
 * Media Source whose local file is an audio file counts as audio only. */
static bool source_shows_video(obs_source_t *src)
{
	uint32_t flags = obs_source_get_output_flags(src);
	if ((flags & OBS_SOURCE_VIDEO) == 0U) {
		return false;
	}
	const char *id = obs_source_get_unversioned_id(src);
	if ((flags & OBS_SOURCE_AUDIO) == 0U || !id || strcmp(id, "ffmpeg_source") != 0) {
		return true;
	}
	obs_data_t *settings = obs_source_get_settings(src);
	const char *file = settings ? obs_data_get_string(settings, "local_file") : NULL;
	const char *dot = file ? strrchr(file, '.') : NULL;
	bool audio_file = false;
	if (dot && settings && obs_data_get_bool(settings, "is_local_file")) {
		static const char *const k_audio_ext[] = {".mp3", ".mp2", ".ogg", ".oga", ".opus", ".wav", ".flac",
							  ".m4a", ".aac", ".wma", ".aif", ".aiff", ".mka", ".weba"};
		for (size_t i = 0U; i < sizeof(k_audio_ext) / sizeof(k_audio_ext[0]) && !audio_file; ++i) {
			const char *e = k_audio_ext[i];
			size_t n = strlen(e);
			if (strlen(dot) == n) {
				bool same = true;
				for (size_t c = 0U; c < n && same; ++c) {
					same = tolower((unsigned char)dot[c]) == (unsigned char)e[c];
				}
				audio_file = same;
			}
		}
	}
	obs_data_release(settings);
	return !audio_file;
}

static bool enum_catalog_scene_items_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct mfd_ui_scene_entry *se = (struct mfd_ui_scene_entry *)param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src || obs_source_removed(src)) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (!nm || !*nm || strncmp(nm, "__mfd_", 6) == 0 || strncmp(nm, "__midi_fadester_", 16) == 0) {
		return true;
	}
	for (size_t k = 0U; k < se->layer_count; ++k) {
		if (strcmp(se->layers[k], nm) == 0) {
			return true;
		}
	}
	if (se->layer_count >= MFD_CATALOG_MAX_SCENE_LAYERS) {
		se->layers_truncated = true;
		return true;
	}
	size_t idx = se->layer_count++;
	snprintf(se->layers[idx], sizeof(se->layers[idx]), "%s", nm);
	se->layer_has_audio[idx] = ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0U);
	se->layer_has_video[idx] = source_shows_video(src);
	return true;
}

static bool enum_catalog_scenes_cb(void *param, obs_source_t *scene_src)
{
	struct mfd_ui_catalog *cat = (struct mfd_ui_catalog *)param;
	if (!cat || !scene_src || obs_source_removed(scene_src) || !obs_source_is_scene(scene_src)) {
		return true;
	}
	const char *sc_name = obs_source_get_name(scene_src);
	if (!sc_name || !*sc_name) {
		return true;
	}
	for (size_t i = 0U; i < cat->scene_count; ++i) {
		if (strcmp(cat->scenes[i].name, sc_name) == 0) {
			return true;
		}
	}
	if (cat->scene_count >= MFD_CATALOG_MAX_SCENES) {
		cat->scenes_truncated = true;
		return true;
	}
	struct mfd_ui_scene_entry *se = &cat->scenes[cat->scene_count++];
	memset(se, 0, sizeof(*se));
	snprintf(se->name, sizeof(se->name), "%s", sc_name);
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		obs_scene_enum_items(scene, enum_catalog_scene_items_cb, se);
	}
	return true;
}

static bool enum_catalog_audio_cb(void *param, obs_source_t *src)
{
	struct mfd_ui_catalog *cat = (struct mfd_ui_catalog *)param;
	if (!cat || !src || obs_source_removed(src)) {
		return true;
	}
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0U) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (!nm || !*nm || strncmp(nm, "__mfd_", 6) == 0 || strncmp(nm, "__midi_fadester_", 16) == 0) {
		return true;
	}
	for (size_t i = 0U; i < cat->audio_count; ++i) {
		if (strcmp(cat->audio_sources[i].name, nm) == 0) {
			return true;
		}
	}
	if (cat->audio_count >= MFD_CATALOG_MAX_AUDIO_SOURCES) {
		cat->audio_truncated = true;
		return true;
	}
	struct mfd_ui_audio_entry *ae = &cat->audio_sources[cat->audio_count++];
	snprintf(ae->name, sizeof(ae->name), "%s", nm);
	ae->db = mfd_mul_to_db(obs_source_get_volume(src));
	return true;
}

void mfd_engine_get_ui_catalog(struct mfd_engine *eng, struct mfd_ui_catalog *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!eng) {
		return;
	}

	mfd_engine_lock(eng);
	out->config_topology_rev = eng->config_topology_rev;
	if (eng->scene_graph_ops && eng->scene_graph_ops->fill_catalog) {
		eng->scene_graph_ops->fill_catalog(out, eng->scene_graph_user_data);
		if (out->filter_host_count == 0U) {
			struct mfd_ui_catalog_host_ctx hctx = {eng, out};
			sg_enum_filter_hosts(eng, add_ui_catalog_filter_host_cb, &hctx);
		}
	} else if (obs_initialized()) {
		obs_enum_scenes(enum_catalog_scenes_cb, out);
		obs_enum_sources(enum_catalog_audio_cb, out);
		struct mfd_ui_catalog_host_ctx hctx = {eng, out};
		sg_enum_filter_hosts(eng, add_ui_catalog_filter_host_cb, &hctx);
	}

	if (out->scene_count > 1U) {
		qsort(out->scenes, out->scene_count, sizeof(out->scenes[0]), cmp_scene_entries);
	}
	if (out->audio_count > 1U) {
		qsort(out->audio_sources, out->audio_count, sizeof(out->audio_sources[0]), cmp_audio_entries);
	}
	if (out->filter_host_count > 1U) {
		qsort(out->filter_hosts, out->filter_host_count, sizeof(out->filter_hosts[0]), cmp_filter_host_entries);
	}
	mfd_engine_unlock(eng);
}

struct mfd_source_filter_catalog *mfd_source_filter_catalog_create(void)
{
	return (struct mfd_source_filter_catalog *)calloc(1U, sizeof(struct mfd_source_filter_catalog));
}

void mfd_source_filter_catalog_destroy(struct mfd_source_filter_catalog *cat)
{
	free(cat);
}

static void append_prop_to_filter_entry_cb(const struct mfd_filter_prop_info *info, void *param)
{
	struct mfd_filter_entry_info *fe = (struct mfd_filter_entry_info *)param;
	if (!fe || !info) {
		return;
	}
	if (fe->prop_count >= MFD_FILTER_MAX_PROPS) {
		fe->props_truncated = true;
		return;
	}
	fe->props[fe->prop_count++] = *info;
}

struct mfd_fill_source_filter_cat_ctx {
	const struct mfd_engine *eng;
	const char *source_name;
	struct mfd_source_filter_catalog *out;
};

static void fill_source_filter_cat_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	struct mfd_fill_source_filter_cat_ctx *ctx = (struct mfd_fill_source_filter_cat_ctx *)param;
	if (!ctx || !ctx->out || !filter_name || !filter_name[0] || mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	if (ctx->out->filter_count >= MFD_HOST_MAX_FILTERS) {
		ctx->out->filters_truncated = true;
		return;
	}
	struct mfd_filter_entry_info *fe = &ctx->out->filters[ctx->out->filter_count++];
	memset(fe, 0, sizeof(*fe));
	snprintf(fe->name, sizeof(fe->name), "%s", filter_name);
	snprintf(fe->type_id, sizeof(fe->type_id), "%s", type_id ? type_id : "");
	fe->enabled = enabled;

	obs_properties_t *props = sg_create_filter_properties(ctx->eng, ctx->source_name, filter_name);
	if (props) {
		obs_data_t *settings = sg_get_filter_settings(ctx->eng, ctx->source_name, filter_name);
		mfd_collect_filter_numeric_props(props, settings, append_prop_to_filter_entry_cb, fe);
		if (settings) {
			obs_data_release(settings);
		}
		obs_properties_destroy(props);
	}
}

bool mfd_engine_get_source_filter_catalog(struct mfd_engine *eng, const char *source_name,
					  struct mfd_source_filter_catalog *out)
{
	if (!out) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (!eng || !source_name || source_name[0] == '\0') {
		return false;
	}
	snprintf(out->source_name, sizeof(out->source_name), "%s", source_name);
	/* No engine lock (F-04): this builds the properties of every filter on the source, which runs the filters' own
	 * code and can be slow, and it reads nothing of the engine's state; the video tick must not wait for it. */
	if (!mfd_engine_source_exists(eng, source_name)) {
		out->source_exists = false;
		return false;
	}
	out->source_exists = true;
	struct mfd_fill_source_filter_cat_ctx ctx = {eng, source_name, out};
	bool is_sc = false;
	(void)sg_enum_source_filters(eng, source_name, &is_sc, fill_source_filter_cat_cb, &ctx);
	out->is_scene = is_sc || mfd_engine_scene_exists(eng, source_name);
	return true;
}

static void append_prop_to_obs_array_cb(const struct mfd_filter_prop_info *info, void *param)
{
	obs_data_array_t *arr = (obs_data_array_t *)param;
	if (!arr || !info) {
		return;
	}
	obs_data_t *pobj = obs_data_create();
	obs_data_set_string(pobj, "name", info->name);
	obs_data_set_string(pobj, "label", info->label);
	obs_data_set_string(pobj, "type", info->is_int ? "int" : "float");
	obs_data_set_double(pobj, "min", info->min_val);
	obs_data_set_double(pobj, "max", info->max_val);
	obs_data_set_double(pobj, "step", info->step);
	obs_data_set_string(pobj, "suffix", info->suffix);
	if (info->is_int) {
		obs_data_set_int(pobj, "current", (long long)llround(info->current_val));
	} else {
		obs_data_set_double(pobj, "current", info->current_val);
	}
	obs_data_array_push_back(arr, pobj);
	obs_data_release(pobj);
}

struct mfd_vendor_filter_list_ctx {
	const struct mfd_engine *eng;
	const char *source_name;
	obs_data_array_t *filters_arr;
};

static void append_vendor_filter_cb(const char *filter_name, const char *type_id, bool enabled, void *param)
{
	struct mfd_vendor_filter_list_ctx *ctx = (struct mfd_vendor_filter_list_ctx *)param;
	if (!ctx || !ctx->filters_arr || !filter_name || !filter_name[0] ||
	    mfd_is_internal_filter(filter_name, type_id)) {
		return;
	}
	obs_data_t *fobj = obs_data_create();
	obs_data_set_string(fobj, "name", filter_name);
	obs_data_set_string(fobj, "type_id", type_id ? type_id : "");
	obs_data_set_bool(fobj, "enabled", enabled);

	obs_data_array_t *parr = obs_data_array_create();
	obs_properties_t *props = sg_create_filter_properties(ctx->eng, ctx->source_name, filter_name);
	if (props) {
		obs_data_t *settings = sg_get_filter_settings(ctx->eng, ctx->source_name, filter_name);
		mfd_collect_filter_numeric_props(props, settings, append_prop_to_obs_array_cb, parr);
		if (settings) {
			obs_data_release(settings);
		}
		obs_properties_destroy(props);
	}
	obs_data_set_array(fobj, "properties", parr);
	obs_data_array_release(parr);

	obs_data_array_push_back(ctx->filters_arr, fobj);
	obs_data_release(fobj);
}

struct mfd_vendor_host_list_ctx {
	const struct mfd_engine *eng;
	obs_data_array_t *hosts_arr;
};

static bool obs_array_contains_string_field(obs_data_array_t *arr, const char *key, const char *val)
{
	if (!arr || !key || !val) {
		return false;
	}
	size_t n = obs_data_array_count(arr);
	for (size_t i = 0U; i < n; ++i) {
		obs_data_t *it = obs_data_array_item(arr, i);
		const char *s = it ? obs_data_get_string(it, key) : NULL;
		bool match = (s && strcmp(s, val) == 0);
		if (it) {
			obs_data_release(it);
		}
		if (match) {
			return true;
		}
	}
	return false;
}

static void append_vendor_host_cb(const char *source_name, bool is_scene, void *param)
{
	struct mfd_vendor_host_list_ctx *ctx = (struct mfd_vendor_host_list_ctx *)param;
	if (!ctx || !ctx->hosts_arr || is_internal_source_name(source_name)) {
		return;
	}
	if (obs_array_contains_string_field(ctx->hosts_arr, "source", source_name)) {
		return;
	}
	uint32_t fcnt = 0U;
	(void)sg_enum_source_filters(ctx->eng, source_name, NULL, count_user_filters_cb, &fcnt);
	obs_data_t *hobj = obs_data_create();
	obs_data_set_string(hobj, "source", source_name);
	obs_data_set_bool(hobj, "is_scene", is_scene);
	obs_data_set_int(hobj, "filter_count", (long long)fcnt);
	obs_data_array_push_back(ctx->hosts_arr, hobj);
	obs_data_release(hobj);
}

static const char *find_layer_opacity_slot_locked(const struct mfd_engine *eng, const char *scene_name,
						  const char *layer_name)
{
	if (!eng || !layer_name || layer_name[0] == '\0') {
		return "";
	}
	const char *any_scene_match = NULL;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (!s->enabled || s->target_type != MFD_TARGET_SINGLE_LAYER) {
			continue;
		}
		if (strcmp(s->source_a_name, layer_name) != 0) {
			continue;
		}
		if (!scene_name || scene_name[0] == '\0' || s->scene_name[0] == '\0' ||
		    strcmp(s->scene_name, scene_name) == 0) {
			return s->id;
		}
		if (!any_scene_match) {
			any_scene_match = s->id;
		}
	}
	return any_scene_match ? any_scene_match : "";
}

struct mfd_live_vendor_scene_items_ctx {
	const struct mfd_engine *eng;
	const char *scene_name;
	obs_data_array_t *layers_arr;
};

struct mfd_live_vendor_scenes_ctx {
	const struct mfd_engine *eng;
	obs_data_array_t *scenes_arr;
};

static bool append_live_vendor_scene_items_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct mfd_live_vendor_scene_items_ctx *ctx = (struct mfd_live_vendor_scene_items_ctx *)param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!ctx || !ctx->layers_arr || !src || obs_source_removed(src)) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (is_internal_source_name(nm) || obs_array_contains_string_field(ctx->layers_arr, "name", nm)) {
		return true;
	}
	bool has_internal_opacity = false;
	obs_source_t *op_flt = obs_source_get_filter_by_name(src, MFD_INTERNAL_OPACITY_FILTER_NAME);
	if (op_flt) {
		has_internal_opacity = !obs_source_removed(op_flt);
		obs_source_release(op_flt);
	}
	obs_data_t *lobj = obs_data_create();
	obs_data_set_string(lobj, "name", nm);
	obs_data_set_bool(lobj, "has_audio", (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0U);
	obs_data_set_bool(lobj, "has_internal_opacity", has_internal_opacity);
	obs_data_set_string(lobj, "opacity_slot", find_layer_opacity_slot_locked(ctx->eng, ctx->scene_name, nm));
	obs_data_array_push_back(ctx->layers_arr, lobj);
	obs_data_release(lobj);
	return true;
}

static bool append_live_vendor_scenes_cb(void *param, obs_source_t *scene_src)
{
	struct mfd_live_vendor_scenes_ctx *ctx = (struct mfd_live_vendor_scenes_ctx *)param;
	if (!ctx || !ctx->scenes_arr || !scene_src || obs_source_removed(scene_src) ||
	    !obs_source_is_scene(scene_src)) {
		return true;
	}
	const char *sc_name = obs_source_get_name(scene_src);
	if (!sc_name || !sc_name[0] || obs_array_contains_string_field(ctx->scenes_arr, "name", sc_name)) {
		return true;
	}
	obs_data_t *sobj = obs_data_create();
	obs_data_set_string(sobj, "name", sc_name);
	obs_data_array_t *layers_arr = obs_data_array_create();
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (scene) {
		struct mfd_live_vendor_scene_items_ctx ictx = {ctx->eng, sc_name, layers_arr};
		obs_scene_enum_items(scene, append_live_vendor_scene_items_cb, &ictx);
	}
	obs_data_set_array(sobj, "layers", layers_arr);
	obs_data_array_release(layers_arr);
	obs_data_array_push_back(ctx->scenes_arr, sobj);
	obs_data_release(sobj);
	return true;
}

static bool append_live_vendor_audio_cb(void *param, obs_source_t *src)
{
	obs_data_array_t *audio_arr = (obs_data_array_t *)param;
	if (!audio_arr || !src || obs_source_removed(src)) {
		return true;
	}
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0U) {
		return true;
	}
	const char *nm = obs_source_get_name(src);
	if (is_internal_source_name(nm) || obs_array_contains_string_field(audio_arr, "name", nm)) {
		return true;
	}
	obs_data_t *aobj = obs_data_create();
	obs_data_set_string(aobj, "name", nm);
	float db = mfd_mul_to_db(obs_source_get_volume(src));
	obs_data_set_double(aobj, "db", isfinite(db) ? (double)db : -120.0);
	obs_data_array_push_back(audio_arr, aobj);
	obs_data_release(aobj);
	return true;
}

bool mfd_engine_fill_vendor_catalog_msg_locked(struct mfd_engine *eng, const char *source_name,
					       obs_data_t *response_data, struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (!eng || !response_data) {
		return false;
	}

	if (source_name && source_name[0] != '\0') {
		if (!mfd_engine_source_exists(eng, source_name)) {
			mfd_msg_set(out_msg, MFD_MSG_SOURCE_NOT_FOUND, source_name, NULL, NULL, NULL, NULL);
			return false;
		}
		bool is_sc = false;
		obs_data_array_t *farr = obs_data_array_create();
		struct mfd_vendor_filter_list_ctx fctx = {eng, source_name, farr};
		(void)sg_enum_source_filters(eng, source_name, &is_sc, append_vendor_filter_cb, &fctx);
		obs_data_set_string(response_data, "source", source_name);
		obs_data_set_bool(response_data, "is_scene", is_sc || mfd_engine_scene_exists(eng, source_name));
		obs_data_set_array(response_data, "filters", farr);
		obs_data_array_release(farr);
		return true;
	}

	obs_data_array_t *scenes_arr = obs_data_array_create();
	obs_data_array_t *audio_arr = obs_data_array_create();
	obs_data_array_t *hosts_arr = obs_data_array_create();

	if (eng->scene_graph_ops && eng->scene_graph_ops->fill_catalog) {
		struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
		if (cat) {
			mfd_engine_get_ui_catalog(eng, cat);
			for (size_t i = 0U; i < cat->scene_count; ++i) {
				obs_data_t *sobj = obs_data_create();
				obs_data_set_string(sobj, "name", cat->scenes[i].name);
				obs_data_array_t *larr = obs_data_array_create();
				for (size_t j = 0U; j < cat->scenes[i].layer_count; ++j) {
					const char *lnm = cat->scenes[i].layers[j];
					obs_data_t *lobj = obs_data_create();
					obs_data_set_string(lobj, "name", lnm);
					obs_data_set_bool(lobj, "has_audio", cat->scenes[i].layer_has_audio[j]);
					obs_data_set_bool(lobj, "has_internal_opacity",
							  mfd_engine_source_has_internal_opacity(eng, lnm));
					obs_data_set_string(lobj, "opacity_slot",
							    find_layer_opacity_slot_locked(eng, cat->scenes[i].name,
											   lnm));
					obs_data_array_push_back(larr, lobj);
					obs_data_release(lobj);
				}
				obs_data_set_array(sobj, "layers", larr);
				obs_data_array_release(larr);
				obs_data_array_push_back(scenes_arr, sobj);
				obs_data_release(sobj);
			}
			for (size_t i = 0U; i < cat->audio_count; ++i) {
				obs_data_t *aobj = obs_data_create();
				obs_data_set_string(aobj, "name", cat->audio_sources[i].name);
				obs_data_set_double(
					aobj, "db",
					isfinite(cat->audio_sources[i].db) ? (double)cat->audio_sources[i].db : -120.0);
				obs_data_array_push_back(audio_arr, aobj);
				obs_data_release(aobj);
			}
			mfd_ui_catalog_destroy(cat);
		}
	} else if (obs_initialized()) {
		struct mfd_live_vendor_scenes_ctx sctx = {eng, scenes_arr};
		obs_enum_scenes(append_live_vendor_scenes_cb, &sctx);
		obs_enum_sources(append_live_vendor_audio_cb, audio_arr);
	}

	struct mfd_vendor_host_list_ctx hctx = {eng, hosts_arr};
	sg_enum_filter_hosts(eng, append_vendor_host_cb, &hctx);

	obs_data_set_array(response_data, "scenes", scenes_arr);
	obs_data_set_array(response_data, "audio_sources", audio_arr);
	obs_data_set_array(response_data, "filter_hosts", hosts_arr);
	obs_data_array_release(scenes_arr);
	obs_data_array_release(audio_arr);
	obs_data_array_release(hosts_arr);
	return true;
}

void mfd_engine_get_ui_revs(struct mfd_engine *eng, uint64_t *topology_rev, uint64_t *telemetry_rev,
			    uint64_t *desk_frame_rev)
{
	uint64_t topo = 0U, tele = 0U, frame = 0U;
	if (eng) {
		mfd_engine_lock(eng);
		topo = eng->config_topology_rev;
		tele = eng->telemetry_rev;
		frame = eng->desk_frame_rev;
		mfd_engine_unlock(eng);
	}
	if (topology_rev) {
		*topology_rev = topo;
	}
	if (telemetry_rev) {
		*telemetry_rev = tele;
	}
	if (desk_frame_rev) {
		*desk_frame_rev = frame;
	}
}

void mfd_engine_get_ui_snapshot(struct mfd_engine *eng, struct mfd_ui_snapshot *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!eng) {
		return;
	}

	mfd_engine_lock(eng);
	out->config_topology_rev = eng->config_topology_rev;
	out->telemetry_rev = eng->telemetry_rev;
	memcpy(out->slot_config_rev, eng->slot_config_rev, sizeof(out->slot_config_rev));
	snprintf(out->configured_device_name, sizeof(out->configured_device_name), "%s", eng->configured_device_name);
	snprintf(out->opened_device_name, sizeof(out->opened_device_name), "%s", eng->midi_dev.opened_name);
	out->midi_connected = eng->midi_dev.is_open;
	out->midi_error_kind = eng->midi_dev.error_kind;
	out->midi_error_operation = eng->midi_dev.error_operation;
	out->midi_error_code = eng->midi_dev.error_code;
	snprintf(out->midi_error_device_name, sizeof(out->midi_error_device_name), "%s",
		 eng->midi_dev.error_device_name);
	snprintf(out->midi_last_error, sizeof(out->midi_last_error), "%s", eng->midi_dev.last_error);
	out->active_profile = mfd_engine_get_active_profile(eng);
	const struct mfd_controller_profile_def *pdef = mfd_profile_get_def(out->active_profile);
	out->active_profile_tier = pdef ? pdef->tier : MFD_PROFILE_TIER_CUSTOM;
	snprintf(out->active_profile_name, sizeof(out->active_profile_name), "%s",
		 pdef ? pdef->slug : effective_profile_name(eng));
	out->smooth_tau_ms = eng->smooth_tau_ms;
	out->tbar_bidirectional_flip = eng->tbar_bidirectional_flip;
	out->frontend_finished_loading = eng->frontend_finished_loading;
	out->studio_mode_active = mfd_engine_is_studio_mode_active(eng);
	snprintf(out->preview_scene_name, sizeof(out->preview_scene_name), "%s", eng->cached_preview_scene);
	snprintf(out->program_scene_name, sizeof(out->program_scene_name), "%s", eng->cached_program_scene);
	out->first_run_offer_pending = eng->frontend_finished_loading && !eng->first_run_offer_dismissed &&
				       eng->apc40_controller_detected && (out->active_profile == MFD_PROFILE_APC40_MK2);
	out->stream_deck_companion_dismissed = eng->stream_deck_companion_dismissed;
	out->learning_slot_index = eng->learning_slot_index;
	out->has_last_midi = eng->has_last_midi;
	out->last_midi_decoded = eng->last_midi_decoded;
	out->last_moved_slot_index = eng->last_moved_slot_index;
	out->midi_dropped_count = mfd_midi_queue_dropped(&eng->midi_queue);
	out->setup = eng->setup_session;
	memcpy(out->slots, eng->fader_slots, sizeof(out->slots));
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		char lbl[128] = {0};
		bool avail = false;
		struct mfd_msg rsn;
		char res_sc[MFD_SOURCE_NAME_MAX] = {0};
		bool a_above = out->slots[i].a_is_above_b;
		/* live = false: the re-check keeps the cached result current on every scene-graph and frontend
		 * change, and a live look-up of every strip here would hold the engine lock (and the video tick) on
		 * every dock frame while the dialog is open (B-10). The resolved scene and layer order stay as the
		 * re-check stored them. */
		evaluate_slot_preconditions(eng, &out->slots[i], lbl, sizeof(lbl), &avail, &rsn, res_sc, sizeof(res_sc),
					    &a_above, false);
		out->slots[i].cached_available = avail;
		out->slots[i].cached_reason_msg = rsn;
		mfd_msg_format_english(&rsn, out->slots[i].cached_reason, sizeof(out->slots[i].cached_reason));
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_set_test_frontend_state(struct mfd_engine *eng, bool finished_loading, bool studio_mode,
					const char *preview_scene, const char *program_scene, bool apc40_detected)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->frontend_finished_loading = finished_loading;
	eng->studio_mode_override_set = true;
	eng->studio_mode_override = studio_mode;
	eng->cached_studio_mode = studio_mode;
	snprintf(eng->cached_preview_scene, sizeof(eng->cached_preview_scene), "%s",
		 preview_scene ? preview_scene : "");
	snprintf(eng->cached_program_scene, sizeof(eng->cached_program_scene), "%s",
		 program_scene ? program_scene : "");
	eng->apc40_controller_detected = apc40_detected;
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_request_studio_mode(struct mfd_engine *eng, bool enabled)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->scene_graph_ops && eng->scene_graph_ops->set_studio_mode) {
		eng->scene_graph_ops->set_studio_mode(enabled, eng->scene_graph_user_data);
	}
	if (eng->studio_mode_override_set || !obs_initialized()) {
		eng->studio_mode_override_set = true;
		eng->studio_mode_override = enabled;
		eng->cached_studio_mode = enabled;
		mfd_engine_reevaluate_slots(eng, true);
		mfd_engine_unlock(eng);
		return;
	}
	mfd_engine_unlock(eng);

	obs_frontend_set_preview_program_mode(enabled);
	mfd_engine_lock(eng);
	refresh_frontend_cache_on_ui_thread_locked(eng);
	mfd_engine_reevaluate_slots(eng, true);
	mfd_engine_unlock(eng);
}

/* --- Setup Session Transaction & First-Run Offer --- */

void mfd_engine_begin_setup_session(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->learning_slot_index >= 0 || eng->setup_session.learning) {
		eng->learning_slot_index = -1;
		mfd_midi_parser_cancel_learn(&eng->midi_parser);
	}

	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	slot->auto_fade_active = false;
	mfd_smoother_reset(&slot->smoother, slot->last_applied_val);

	memset(&eng->setup_session, 0, sizeof(eng->setup_session));
	eng->setup_session.active = true;
	eng->setup_session.slot_index = slot_index;
	eng->setup_session.base_slot_rev = eng->slot_config_rev[slot_index];
	eng->setup_session.candidate_addr = slot->addr;
	eng->setup_session.candidate_cal = slot->cal;
	eng->setup_session.candidate_invert = slot->invert;
	eng->setup_session.learning = false;
	eng->setup_session.candidate_raw_norm = slot->last_raw_norm;
	eng->setup_session.has_candidate_input = slot->takeover.has_last_fader;

	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_start_setup_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (!eng->setup_session.active) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->learning_slot_index = -1;
	eng->setup_session.learning = true;
	eng->setup_session.learn_auto = false;
	mfd_midi_parser_start_learn(&eng->midi_parser);
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_start_setup_learn_auto(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (!eng->setup_session.active) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->learning_slot_index = -1;
	eng->setup_session.learning = true;
	eng->setup_session.learn_auto = true;
	eng->setup_session.auto_moves = 0;
	eng->setup_session.auto_addr.type = MFD_MIDI_NONE;
	mfd_midi_parser_start_learn(&eng->midi_parser);
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

/* Learn that started by itself must not take a stray message: a pad pressed, a fader brushed, a controller that sends
 * something on its own. It takes a fader or knob that is being moved: three messages in a row from the same CC or
 * pitch bend, each with a new value. The fine half of a 14-bit pair (CC n + 32 after CC n) does not break the run.
 * Caller holds the engine lock. True when this message completes such a run. */
static bool setup_auto_learn_take_locked(struct mfd_engine *eng, const struct mfd_midi_addr *addr, int raw)
{
	struct mfd_setup_session *s = &eng->setup_session;
	if (addr->type != MFD_MIDI_CC && addr->type != MFD_MIDI_PITCH_BEND) {
		s->auto_moves = 0; /* a button */
		return false;
	}
	bool same = s->auto_moves > 0 && s->auto_addr.type == addr->type && s->auto_addr.channel == addr->channel &&
		    (addr->type == MFD_MIDI_PITCH_BEND || s->auto_addr.number == addr->number);
	if (!same && s->auto_moves > 0 && addr->type == MFD_MIDI_CC && s->auto_addr.type == MFD_MIDI_CC &&
	    s->auto_addr.channel == addr->channel && s->auto_addr.number < 32U &&
	    addr->number == (uint8_t)(s->auto_addr.number + 32U)) {
		return false; /* its fine half */
	}
	if (same) {
		if (raw != s->auto_last_raw) {
			++s->auto_moves;
		}
	} else {
		s->auto_addr = *addr;
		s->auto_moves = 1;
	}
	s->auto_last_raw = raw;
	return s->auto_moves >= 3;
}

void mfd_engine_cancel_setup_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.learning) {
		eng->setup_session.learning = false;
		eng->setup_session.learn_auto = false;
		if (eng->learning_slot_index < 0) {
			mfd_midi_parser_cancel_learn(&eng->midi_parser);
		}
		mfd_engine_mark_ui_dirty(eng, true);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_set_setup_invert(struct mfd_engine *eng, bool invert)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.active && eng->setup_session.candidate_invert != invert) {
		eng->setup_session.candidate_invert = invert;
		if (eng->setup_session.has_candidate_input) {
			eng->setup_session.candidate_raw_norm = 1.0f - eng->setup_session.candidate_raw_norm;
		}
		mfd_engine_mark_ui_dirty(eng, true);
		mfd_engine_mark_ui_dirty(eng, false);
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_cancel_setup_session(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.active) {
		if (eng->setup_session.learning && eng->learning_slot_index < 0) {
			mfd_midi_parser_cancel_learn(&eng->midi_parser);
		}
		memset(&eng->setup_session, 0, sizeof(eng->setup_session));
		mfd_engine_mark_ui_dirty(eng, true);
	}
	mfd_engine_unlock(eng);
}

static void vj_start_strip_at_locked(struct mfd_engine *eng, struct mfd_fader_slot *s, float v);
static void apply_source_opacity_and_afv(const struct mfd_engine *eng, const char *source_name, float alpha_01,
					 bool afv);

/* What a strip showed or mixed before an edit: the sources it may leave behind (B-07). */
struct mfd_layer_binding {
	enum mfd_target_type target;
	bool afv;
	char source_a[MFD_SOURCE_NAME_MAX];
	char source_b[MFD_SOURCE_NAME_MAX];
};

static void layer_binding_read(const struct mfd_fader_slot *s, struct mfd_layer_binding *out)
{
	out->target = s->enabled ? s->target_type : MFD_TARGET_UNASSIGNED;
	out->afv = s->audio_follows_video;
	snprintf(out->source_a, sizeof(out->source_a), "%s", s->source_a_name);
	snprintf(out->source_b, sizeof(out->source_b), "%s", s->source_b_name);
}

/* Whether the strip's job names this source as a layer it shows or mixes, switched on or not. */
static bool slot_names_layer(const struct mfd_fader_slot *s, const char *source)
{
	if (s->target_type == MFD_TARGET_SINGLE_LAYER) {
		return strcmp(s->source_a_name, source) == 0;
	}
	if (s->target_type == MFD_TARGET_TWO_SOURCE_MIX) {
		return strcmp(s->source_a_name, source) == 0 || strcmp(s->source_b_name, source) == 0;
	}
	return false;
}

static bool slot_drives_layer(const struct mfd_fader_slot *s, const char *source)
{
	return s->enabled && slot_names_layer(s, source);
}

/* A source no strip drives any more goes back to 100 %: left faded out, nothing could raise it again. */
static void release_layer_if_undriven_locked(struct mfd_engine *eng, const char *source, bool afv)
{
	if (!source || source[0] == '\0') {
		return;
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (slot_drives_layer(&eng->fader_slots[i], source)) {
			return;
		}
	}
	if (mfd_engine_source_exists(eng, source)) {
		apply_source_opacity_and_afv(eng, source, 1.0f, afv);
	}
}

/*
 * B-07: a strip was given another source (or another job). Call after the strip has been written and reset. The
 * sources it left go back to 100 % when no strip drives them, and a Layer Opacity strip starts at the level its new
 * source has now, with pick-up from where its fader is: the picture does not jump at the first fader move.
 *
 * A source the strip still names has not been left: switching a strip off (the Advanced tab's "On" box, set_slot
 * with "enabled": false) only parks it, and the source stays at the level it has on air (QA-A01).
 */
static void after_layer_binding_change_locked(struct mfd_engine *eng, struct mfd_fader_slot *s,
					      const struct mfd_layer_binding *before)
{
	if ((before->target == MFD_TARGET_SINGLE_LAYER || before->target == MFD_TARGET_TWO_SOURCE_MIX) &&
	    !slot_names_layer(s, before->source_a)) {
		release_layer_if_undriven_locked(eng, before->source_a, before->afv);
	}
	if (before->target == MFD_TARGET_TWO_SOURCE_MIX && !slot_names_layer(s, before->source_b)) {
		release_layer_if_undriven_locked(eng, before->source_b, before->afv);
	}
	if (s->enabled && s->target_type == MFD_TARGET_SINGLE_LAYER &&
	    (before->target != MFD_TARGET_SINGLE_LAYER || strcmp(before->source_a, s->source_a_name) != 0)) {
		float live = 1.0f;
		if (vj_live_opacity(eng, s->source_a_name, &live)) {
			vj_start_strip_at_locked(eng, s, live);
		}
	}
}

bool mfd_engine_unassign_slot_preserve_addr(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return false;
	}
	mfd_engine_lock(eng);
	if (eng->setup_session.active && eng->setup_session.slot_index == slot_index) {
		if (eng->setup_session.learning && eng->learning_slot_index < 0) {
			mfd_midi_parser_cancel_learn(&eng->midi_parser);
		}
		memset(&eng->setup_session, 0, sizeof(eng->setup_session));
	}
	mfd_engine_detach_filter_binding_locked(eng, slot_index);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	struct mfd_layer_binding before;
	layer_binding_read(s, &before);
	s->enabled = true;
	s->scene_name[0] = '\0';
	s->resolved_scene_name[0] = '\0';
	s->source_a_name[0] = '\0';
	s->source_b_name[0] = '\0';
	s->filter_name[0] = '\0';
	s->filter_prop[0] = '\0';
	s->filter_from_val = 0.0;
	s->filter_to_val = 0.0;
	s->filter_bypass_at_zero = false;
	s->filter_prop_valid = false;
	s->restore_persisted_pre_bypass = false;
	s->audio_follows_video = false;
	s->auto_follow = false;
	mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
	after_layer_binding_change_locked(eng, s, &before); /* the source it leaves is not left faded out (B-07) */
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

size_t mfd_engine_find_addr_owners(struct mfd_engine *eng, const struct mfd_midi_addr *addr, int exclude_slot,
				   int *out_slots, size_t max_slots)
{
	if (!eng || !addr || addr->type == MFD_MIDI_NONE) {
		return 0U;
	}
	mfd_engine_lock(eng);
	size_t found = 0U;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (i == exclude_slot) {
			continue;
		}
		if (midi_addr_exact_equal(&eng->fader_slots[i].addr, addr)) {
			if (out_slots && found < max_slots) {
				out_slots[found] = i;
			}
			++found;
		}
	}
	mfd_engine_unlock(eng);
	return found;
}

bool mfd_engine_validate_setup_candidate(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					 char *out_reason, size_t reason_size)
{
	struct mfd_msg msg;
	bool ok = mfd_engine_validate_setup_candidate_msg(eng, cand, &msg);
	mfd_msg_format_english(&msg, out_reason, reason_size);
	return ok;
}

static bool validate_setup_candidate_msg_locked(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
						const struct mfd_filter_meta *prefetched, struct mfd_msg *out_msg);

/* Takes the engine lock (A-04, F-14): the check reads the other strips' names and the cached OBS state, which the
 * video tick and obs-websocket threads rewrite under that lock. The lock is recursive, so a commit that validates
 * while holding it still works.
 *
 * A Filter Setting candidate needs the filter's properties, and building them runs the filter's own code. The Set up
 * sheet asks on every change of a choice, so they are read before the lock is taken (QA-A03): a slow filter then
 * costs the sheet a moment and the video tick nothing. */
bool mfd_engine_validate_setup_candidate_msg(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					     struct mfd_msg *out_msg)
{
	struct mfd_filter_meta meta;
	const struct mfd_filter_meta *prefetched = NULL;
	if (eng && cand && cand->enabled && cand->target_type == MFD_TARGET_FILTER_PARAM &&
	    cand->source_a_name[0] != '\0' && cand->filter_name[0] != '\0' && cand->filter_prop[0] != '\0' &&
	    !mfd_is_internal_filter(cand->filter_name, NULL) && filter_meta_may_read_here(eng)) {
		read_filter_meta(eng, cand->source_a_name, cand->filter_name, cand->filter_prop, &meta);
		prefetched = &meta;
	}
	struct mfd_engine *lock_eng = (struct mfd_engine *)(uintptr_t)eng;
	mfd_engine_lock(lock_eng);
	bool ok = validate_setup_candidate_msg_locked(eng, cand, prefetched, out_msg);
	mfd_engine_unlock(lock_eng);
	return ok;
}

static bool validate_setup_candidate_msg_locked(const struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
						const struct mfd_filter_meta *prefetched, struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (!eng || !cand || cand->slot_index < 0 || cand->slot_index >= MFD_MAX_FADER_SLOTS) {
		mfd_msg_set(out_msg, MFD_MSG_INVALID_STRIP, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (!cand->enabled || cand->target_type == MFD_TARGET_UNASSIGNED) {
		return true;
	}

	if (cand->target_type == MFD_TARGET_TWO_SOURCE_MIX) {
		if (cand->scene_name[0] == '\0') {
			mfd_msg_set(out_msg, MFD_MSG_CHOOSE_SCENE_TWO_LAYERS, NULL, NULL, NULL, NULL, NULL);
			return false;
		}
		if (cand->source_a_name[0] == '\0' || cand->source_b_name[0] == '\0') {
			mfd_msg_set(out_msg, MFD_MSG_NOT_SET_UP, NULL, NULL, NULL, NULL, NULL);
			return false;
		}
		if (strcmp(cand->source_a_name, cand->source_b_name) == 0) {
			mfd_msg_set(out_msg, MFD_MSG_SAME_LAYER, NULL, NULL, NULL, NULL, NULL);
			return false;
		}
	} else if (cand->target_type == MFD_TARGET_SINGLE_LAYER) {
		if (cand->scene_name[0] == '\0') {
			mfd_msg_set(out_msg, MFD_MSG_CHOOSE_SCENE_ONE_LAYER, NULL, NULL, NULL, NULL, NULL);
			return false;
		}
	} else if (cand->target_type == MFD_TARGET_FILTER_PARAM) {
		double fval = cand->filter_from_val;
		double tval = cand->filter_to_val;
		return validate_filter_param_binding(eng, cand->slot_index, cand->enabled, cand->source_a_name,
						     cand->filter_name, cand->filter_prop, true, &fval, &tval, NULL,
						     prefetched, out_msg);
	}

	struct mfd_fader_slot tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.enabled = true;
	tmp.target_type = cand->target_type;
	snprintf(tmp.scene_name, sizeof(tmp.scene_name), "%s", cand->scene_name);
	snprintf(tmp.source_a_name, sizeof(tmp.source_a_name), "%s", cand->source_a_name);
	snprintf(tmp.source_b_name, sizeof(tmp.source_b_name), "%s", cand->source_b_name);
	tmp.curve = cand->curve;
	tmp.audio_follows_video = cand->audio_follows_video;

	bool avail = false;
	struct mfd_msg rsn;
	evaluate_slot_preconditions(eng, &tmp, NULL, 0U, &avail, &rsn, NULL, 0U, NULL, true);
	if (!avail && out_msg) {
		*out_msg = rsn;
	}
	return avail;
}

bool mfd_engine_commit_setup_session(struct mfd_engine *eng, const struct mfd_setup_candidate *cand, char *out_err,
				     size_t err_size)
{
	struct mfd_msg msg;
	bool ok = mfd_engine_commit_setup_session_msg(eng, cand, &msg);
	mfd_msg_format_english(&msg, out_err, err_size);
	return ok;
}

bool mfd_engine_commit_setup_session_msg(struct mfd_engine *eng, const struct mfd_setup_candidate *cand,
					 struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (!eng || !cand || cand->slot_index < 0 || cand->slot_index >= MFD_MAX_FADER_SLOTS) {
		mfd_msg_set(out_msg, MFD_MSG_INVALID_STRIP, NULL, NULL, NULL, NULL, NULL);
		return false;
	}

	mfd_engine_lock(eng);
	if (!eng->setup_session.active || eng->setup_session.slot_index != cand->slot_index) {
		mfd_msg_set(out_msg, MFD_MSG_SETUP_NOT_ACTIVE, NULL, NULL, NULL, NULL, NULL);
		mfd_engine_unlock(eng);
		return false;
	}

	if (eng->slot_config_rev[cand->slot_index] != eng->setup_session.base_slot_rev) {
		eng->setup_session.base_slot_rev = eng->slot_config_rev[cand->slot_index];
		mfd_msg_set(out_msg, MFD_MSG_CHANGED_EXTERNALLY, NULL, NULL, NULL, NULL, NULL);
		mfd_engine_mark_ui_dirty(eng, true);
		mfd_engine_unlock(eng);
		return false;
	}

	if (!mfd_engine_validate_setup_candidate_msg(eng, cand, out_msg)) {
		mfd_engine_unlock(eng);
		return false;
	}

	if (eng->setup_session.learning && eng->learning_slot_index < 0) {
		mfd_midi_parser_cancel_learn(&eng->midi_parser);
	}

	struct mfd_midi_addr chosen_addr = eng->setup_session.candidate_addr;
	struct mfd_control_calibration chosen_cal = eng->setup_session.candidate_cal;
	bool chosen_invert = eng->setup_session.candidate_invert;
	bool had_cand_input = eng->setup_session.has_candidate_input;
	float cand_raw = eng->setup_session.candidate_raw_norm;

	if (chosen_addr.type != MFD_MIDI_NONE) {
		for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
			if (j != cand->slot_index && midi_addr_exact_equal(&eng->fader_slots[j].addr, &chosen_addr)) {
				eng->fader_slots[j].addr.type = MFD_MIDI_NONE;
				mfd_engine_bump_slot_rev(eng, j);
			}
		}
	}

	if (cand->apply_tbar_bidir) {
		mfd_engine_set_tbar_bidirectional_flip(eng, cand->tbar_bidirectional_flip);
	}

	mfd_engine_detach_filter_binding_locked(eng, cand->slot_index);

	struct mfd_fader_slot *s = &eng->fader_slots[cand->slot_index];
	struct mfd_layer_binding before;
	layer_binding_read(s, &before);
	mfd_engine_abort_tbar_for_config_change_locked(eng, s);
	s->enabled = cand->enabled;
	s->addr = chosen_addr;
	s->cal = chosen_cal;
	store_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
	s->invert = chosen_invert;
	snprintf(s->scene_name, sizeof(s->scene_name), "%s", cand->scene_name);
	if (cand->scene_name[0] == '\0') {
		s->resolved_scene_name[0] = '\0';
	}
	snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", cand->source_a_name);
	snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", cand->source_b_name);
	if (cand->target_type == MFD_TARGET_FILTER_PARAM) {
		snprintf(s->filter_name, sizeof(s->filter_name), "%s", cand->filter_name);
		snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", cand->filter_prop);
		s->filter_from_val = cand->filter_from_val;
		s->filter_to_val = cand->filter_to_val;
		s->filter_bypass_at_zero = cand->filter_bypass_at_zero;
		s->restore_persisted_pre_bypass = false;
	} else {
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
	}
	s->curve = cand->curve;
	s->audio_follows_video = cand->audio_follows_video;
	s->auto_follow = false;
	if (had_cand_input) {
		s->last_raw_norm = cand_raw;
		s->takeover.has_last_fader = true;
	}

	/* Ensure mode state resets cleanly even when re-committing the same target_type with new layers */
	enum mfd_target_type force_prev = (cand->target_type == MFD_TARGET_UNASSIGNED) ? MFD_TARGET_TBAR
										       : MFD_TARGET_UNASSIGNED;
	s->target_type = force_prev;
	mfd_engine_reset_slot_mode_state(eng, s, cand->target_type);
	after_layer_binding_change_locked(eng, s, &before);

	memset(&eng->setup_session, 0, sizeof(eng->setup_session));
	mfd_engine_bump_slot_rev(eng, cand->slot_index);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

/* OBS names its desktop audio source in its own UI language, so find it by output channel, never by name. */
static bool sg_output_source_name(const struct mfd_engine *eng, uint32_t channel, char *out, size_t out_size)
{
	out[0] = '\0';
	if (eng && eng->scene_graph_ops) {
		return eng->scene_graph_ops->get_output_source_name &&
		       eng->scene_graph_ops->get_output_source_name(channel, out, out_size,
								    eng->scene_graph_user_data) &&
		       out[0] != '\0';
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_output_source(channel);
	if (!src) {
		return false;
	}
	const char *name = obs_source_get_name(src);
	snprintf(out, out_size, "%s", name ? name : "");
	obs_source_release(src);
	return out[0] != '\0';
}

static bool sg_get_source_volume(const struct mfd_engine *eng, const char *source_name, float *out_mul)
{
	if (!source_name || source_name[0] == '\0' || !out_mul) {
		return false;
	}
	if (eng && eng->scene_graph_ops) {
		return eng->scene_graph_ops->get_volume &&
		       eng->scene_graph_ops->get_volume(source_name, out_mul, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return false;
	}
	*out_mul = obs_source_get_volume(src);
	obs_source_release(src);
	return true;
}

/* Master (slot 9) becomes the volume of OBS's Desktop Audio, when OBS has that source. Caller holds the engine lock.
 * Returns true when Master was assigned. */
static bool first_run_assign_master_locked(struct mfd_engine *eng)
{
	bool assigned = false;
	struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
	if (!cat) {
		return false;
	}
	mfd_engine_get_ui_catalog(eng, cat);
	/* Output channels 1 and 2 are OBS's Desktop Audio and Desktop Audio 2. Injected scene-graph ops without
	 * get_output_source_name (headless tests) find none, and Master stays unassigned. */
	for (uint32_t channel = 1U; channel <= 2U && !assigned; ++channel) {
		char desktop_name[MFD_SOURCE_NAME_MAX];
		if (!sg_output_source_name(eng, channel, desktop_name, sizeof(desktop_name))) {
			continue;
		}
		for (size_t i = 0U; i < cat->audio_count; ++i) {
			if (strcmp(cat->audio_sources[i].name, desktop_name) == 0) {
				struct mfd_fader_slot *master = &eng->fader_slots[9];
				master->enabled = true;
				snprintf(master->source_a_name, sizeof(master->source_a_name), "%s",
					 cat->audio_sources[i].name);
				master->source_b_name[0] = '\0';
				master->scene_name[0] = '\0';
				mfd_engine_reset_slot_mode_state(eng, master, MFD_TARGET_AUDIO_VOLUME);
				assigned = true;
				break;
			}
		}
	}
	mfd_ui_catalog_destroy(cat);
	return assigned;
}

void mfd_engine_apply_first_run_layout(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	bool pending = eng->frontend_finished_loading && !eng->first_run_offer_dismissed &&
		       !eng->first_run_layout_applied && eng->apc40_controller_detected &&
		       strcmp(effective_profile_name(eng), MFD_DEFAULT_PROFILE_NAME) == 0;
	if (!pending) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->first_run_layout_applied = true;
	/* only a Master with no job: a strip the user (or an import) already set is theirs */
	if (eng->fader_slots[9].target_type == MFD_TARGET_UNASSIGNED && first_run_assign_master_locked(eng)) {
		mfd_engine_bump_slot_rev(eng, 9);
		mfd_engine_reevaluate_slots(eng, false);
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_mark_ui_dirty(eng, false);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_acknowledge_first_run(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->first_run_offer_dismissed) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->first_run_offer_dismissed = true;
	mfd_engine_mark_ui_dirty(eng, false);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_accept_first_run_usual_layout(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	mfd_engine_cancel_setup_session(eng);
	mfd_engine_load_apc40_defaults(eng);
	(void)first_run_assign_master_locked(eng);

	eng->first_run_offer_dismissed = true;
	eng->first_run_layout_applied = true;
	/* The usual layout empties the eight strips: they fill from the scene on air again. */
	eng->vj_autofill_done = false;
	eng->vj_autofill_pending = true;
	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_decline_first_run_usual_layout(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	mfd_engine_cancel_setup_session(eng);
	mfd_engine_load_apc40_defaults(eng);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		mfd_engine_detach_filter_binding_locked(eng, i);
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		s->scene_name[0] = '\0';
		s->resolved_scene_name[0] = '\0';
		s->source_a_name[0] = '\0';
		s->source_b_name[0] = '\0';
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
		mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
	}
	eng->first_run_offer_dismissed = true;
	eng->first_run_layout_applied = true;
	eng->vj_autofill_done = true; /* the user will set the strips up: never fill them from the scene */
	eng->vj_autofill_pending = false;
	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_set_stream_deck_companion_dismissed(struct mfd_engine *eng, bool dismissed)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->stream_deck_companion_dismissed == dismissed) {
		mfd_engine_unlock(eng);
		return;
	}
	eng->stream_deck_companion_dismissed = dismissed;
	mfd_engine_mark_ui_dirty(eng, true);
	unlock_and_persist_global_config(eng);
}

/* --- Typed UI & Advanced-Tab Operations --- */

bool mfd_engine_update_slot_config_ex(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit,
				      char *out_err, size_t err_size)
{
	struct mfd_msg msg;
	bool ok = mfd_engine_update_slot_config_msg(eng, slot_index, edit, &msg);
	mfd_msg_format_english(&msg, out_err, err_size);
	return ok;
}

bool mfd_engine_update_slot_config_msg(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit,
				       struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (!eng || !edit || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		mfd_msg_set(out_msg, MFD_MSG_INVALID_STRIP, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	struct mfd_slot_config_edit kept;
	if (edit->keep_layer && s->auto_follow && edit->target_type == s->target_type) {
		/* The dialog's snapshot may predate the last scene change: keep the source the strip follows now. */
		kept = *edit;
		snprintf(kept.scene_name, sizeof(kept.scene_name), "%s", s->scene_name);
		snprintf(kept.source_a_name, sizeof(kept.source_a_name), "%s", s->source_a_name);
		snprintf(kept.source_b_name, sizeof(kept.source_b_name), "%s", s->source_b_name);
		kept.preserve_scene = false;
		edit = &kept;
	}
	/* A strip the user gives another job, source or scene stops following the scene on air (once the edit is
	 * accepted). */
	const bool stops_following = s->auto_follow &&
				     (edit->target_type != s->target_type ||
				      strcmp(edit->source_a_name, s->source_a_name) != 0 ||
				      (!edit->preserve_scene && strcmp(edit->scene_name, s->scene_name) != 0));

	bool same_filter_binding =
		(s->target_type == MFD_TARGET_FILTER_PARAM && edit->target_type == MFD_TARGET_FILTER_PARAM &&
		 strcmp(s->source_a_name, edit->source_a_name) == 0 && strcmp(s->filter_name, edit->filter_name) == 0 &&
		 strcmp(s->filter_prop, edit->filter_prop) == 0);
	double cand_from = edit->has_filter_range ? edit->filter_from_val : s->filter_from_val;
	double cand_to = edit->has_filter_range ? edit->filter_to_val : s->filter_to_val;

	if (edit->target_type == MFD_TARGET_FILTER_PARAM &&
	    (edit->source_a_name[0] != '\0' || edit->filter_name[0] != '\0' || edit->filter_prop[0] != '\0')) {
		bool full_binding_fields = (edit->source_a_name[0] != '\0' && edit->filter_name[0] != '\0' &&
					    edit->filter_prop[0] != '\0');
		if (edit->enabled && full_binding_fields) {
			bool live_exists = mfd_engine_source_exists(eng, edit->source_a_name) &&
					   sg_filter_exists(eng, edit->source_a_name, edit->filter_name, NULL);
			if (!same_filter_binding || edit->has_filter_range || live_exists) {
				bool has_ft = edit->has_filter_range || same_filter_binding;
				if (!mfd_engine_validate_filter_param_binding_msg_locked(
					    eng, slot_index, true, edit->source_a_name, edit->filter_name,
					    edit->filter_prop, has_ft, &cand_from, &cand_to, NULL, out_msg)) {
					mfd_engine_unlock(eng);
					return false;
				}
			} else {
				/* Offline bound target: still enforce reserved filter & single-writer uniqueness */
				if (mfd_is_internal_filter(edit->filter_name, NULL)) {
					mfd_msg_set(out_msg, MFD_MSG_FILTER_RESERVED, edit->filter_name, NULL, NULL,
						    NULL, NULL);
					mfd_engine_unlock(eng);
					return false;
				}
				for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
					if (j == slot_index) {
						continue;
					}
					const struct mfd_fader_slot *other = &eng->fader_slots[j];
					if (other->enabled && other->target_type == MFD_TARGET_FILTER_PARAM &&
					    strcmp(other->source_a_name, edit->source_a_name) == 0 &&
					    strcmp(other->filter_name, edit->filter_name) == 0 &&
					    strcmp(other->filter_prop, edit->filter_prop) == 0) {
						set_already_controlled_msg(eng, out_msg, edit->filter_prop,
									   edit->filter_name, j);
						mfd_engine_unlock(eng);
						return false;
					}
				}
			}
		} else if (edit->enabled && (!full_binding_fields && edit->has_filter_range)) {
			if (!mfd_engine_validate_filter_param_binding_msg_locked(
				    eng, slot_index, true, edit->source_a_name, edit->filter_name, edit->filter_prop,
				    true, &cand_from, &cand_to, NULL, out_msg)) {
				mfd_engine_unlock(eng);
				return false;
			}
		}
	}

	mfd_engine_abort_tbar_for_config_change_locked(eng, s);
	mfd_engine_detach_filter_binding_locked(eng, slot_index);

	struct mfd_layer_binding before;
	layer_binding_read(s, &before);
	s->enabled = edit->enabled;
	bool invert_changed = (s->invert != edit->invert);
	s->invert = edit->invert;
	if (invert_changed && s->takeover.has_last_fader) {
		s->last_raw_norm = 1.0f - s->last_raw_norm;
	}

	if (!edit->preserve_scene) {
		snprintf(s->scene_name, sizeof(s->scene_name), "%s", edit->scene_name);
		if (edit->scene_name[0] == '\0') {
			s->resolved_scene_name[0] = '\0';
		}
	} else if (edit->target_type == MFD_TARGET_TBAR || edit->target_type == MFD_TARGET_AUDIO_VOLUME ||
		   edit->target_type == MFD_TARGET_FILTER_PARAM || edit->target_type == MFD_TARGET_UNASSIGNED ||
		   edit->target_type == MFD_TARGET_MIX_MASTER) {
		s->scene_name[0] = '\0';
		s->resolved_scene_name[0] = '\0';
	} else if (s->scene_name[0] != '\0') {
		/* If a source changed and is not in the previously pinned scene, clear pin so auto-resolve works */
		if (edit->source_a_name[0] != '\0' &&
		    sg_scene_item_stack_index(eng, s->scene_name, edit->source_a_name) < 0) {
			s->scene_name[0] = '\0';
			s->resolved_scene_name[0] = '\0';
		} else if (edit->target_type == MFD_TARGET_TWO_SOURCE_MIX && edit->source_b_name[0] != '\0' &&
			   sg_scene_item_stack_index(eng, s->scene_name, edit->source_b_name) < 0) {
			s->scene_name[0] = '\0';
			s->resolved_scene_name[0] = '\0';
		}
	}

	bool filter_range_changed = false;
	snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", edit->source_a_name);
	snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", edit->source_b_name);
	if (edit->target_type == MFD_TARGET_FILTER_PARAM) {
		filter_range_changed = !same_filter_binding || (fabs(s->filter_from_val - cand_from) > 1e-9) ||
				       (fabs(s->filter_to_val - cand_to) > 1e-9);
		snprintf(s->filter_name, sizeof(s->filter_name), "%s", edit->filter_name);
		snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", edit->filter_prop);
		s->filter_from_val = cand_from;
		s->filter_to_val = cand_to;
		s->filter_bypass_at_zero = edit->filter_bypass_at_zero;
		s->restore_persisted_pre_bypass = false;
	} else {
		s->filter_name[0] = '\0';
		s->filter_prop[0] = '\0';
		s->filter_from_val = 0.0;
		s->filter_to_val = 0.0;
		s->filter_bypass_at_zero = false;
		s->filter_prop_valid = false;
		s->restore_persisted_pre_bypass = false;
	}
	s->curve = edit->curve;
	s->audio_follows_video = edit->audio_follows_video;
	if (stops_following) {
		s->auto_follow = false;
	}
	if (edit->target_type == MFD_TARGET_FILTER_PARAM && filter_range_changed) {
		s->target_type = MFD_TARGET_UNASSIGNED;
	}
	/* A layer or mix strip given another source starts afresh, as the Set up commit does (B-07): its level and
	 * pick-up belonged to the source it had. */
	const bool layer_sources_changed =
		(edit->target_type == MFD_TARGET_SINGLE_LAYER || edit->target_type == MFD_TARGET_TWO_SOURCE_MIX) &&
		before.target == edit->target_type &&
		(strcmp(before.source_a, s->source_a_name) != 0 ||
		 (edit->target_type == MFD_TARGET_TWO_SOURCE_MIX && strcmp(before.source_b, s->source_b_name) != 0));
	if (layer_sources_changed) {
		s->target_type = MFD_TARGET_UNASSIGNED;
	}
	mfd_engine_reset_slot_mode_state(eng, s, edit->target_type);
	after_layer_binding_change_locked(eng, s, &before);
	if (invert_changed && s->enabled && s->target_type != MFD_TARGET_TBAR &&
	    s->target_type != MFD_TARGET_UNASSIGNED && s->takeover.has_last_fader) {
		/* Reverse direction was switched: the fader now stands for the mirrored position. Its pick-up starts
		 * over at the value the strip has, so the output does not jump to the mirror at the next message. */
		restart_pickup_at_locked(eng, s, s->last_applied_val);
	}
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_reevaluate_slots(eng, false);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
	return true;
}

bool mfd_engine_update_slot_config(struct mfd_engine *eng, int slot_index, const struct mfd_slot_config_edit *edit)
{
	return mfd_engine_update_slot_config_ex(eng, slot_index, edit, NULL, 0U);
}

void mfd_engine_set_slot_label(struct mfd_engine *eng, int slot_index, const char *label)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (label && label[0] != '\0' && !mfd_profile_is_default_slot_label(slot_index, label)) {
		snprintf(s->label, sizeof(s->label), "%s", label);
	} else {
		s->label[0] = '\0';
	}
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

void mfd_engine_clear_slot_binding(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->learning_slot_index == slot_index) {
		mfd_engine_cancel_learn(eng);
	}
	eng->fader_slots[slot_index].addr.type = MFD_MIDI_NONE;
	mfd_engine_bump_slot_rev(eng, slot_index);
	mfd_engine_notify_state_changed(eng);
	unlock_and_persist_global_config(eng);
}

bool mfd_engine_set_slot_value(struct mfd_engine *eng, int slot_index, float value_01, bool release, const char *origin,
			       char *out_err, size_t err_size)
{
	struct mfd_msg msg;
	bool ok = mfd_engine_set_slot_value_msg(eng, slot_index, value_01, release, origin, &msg);
	mfd_msg_format_english(&msg, out_err, err_size);
	return ok;
}

bool mfd_engine_set_slot_value_msg(struct mfd_engine *eng, int slot_index, float value_01, bool release,
				   const char *origin, struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		mfd_msg_set(out_msg, MFD_MSG_INVALID_STRIP, NULL, NULL, NULL, NULL, NULL);
		return false;
	}

	mfd_engine_lock(eng);
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	if (eng->setup_session.active && eng->setup_session.slot_index == slot_index) {
		mfd_msg_set(out_msg, MFD_MSG_STRIP_PAUSED_IN_SETUP, NULL, NULL, NULL, NULL, NULL);
		mfd_engine_unlock(eng);
		return false;
	}
	if (!mfd_engine_is_slot_writable(eng, slot)) {
		bool avail = false;
		evaluate_slot_preconditions(eng, slot, NULL, 0U, &avail, out_msg, NULL, 0U, NULL, false);
		mfd_engine_unlock(eng);
		return false;
	}

	float val = clamp01(value_01);
	const char *orig = (origin && *origin) ? origin : "api";
	bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
			  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);
	bool prev_upper = slot->takeover.parked_at_upper_rail;

	slot->auto_fade_active = false;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	slot->tbar_midi_active = false;
	slot->tbar_from_idle_band = false;
	slot->takeover.dial_reversed = false;

	if (slot->target_type == MFD_TARGET_TBAR) {
		if (release || mfd_tbar_is_in_target_rail_zone(val, false)) {
			if (!was_parked) {
				if (slot->tbar_state == MFD_TBAR_IDLE) {
					mfd_engine_unlatch_tbar(eng);
				}
				mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		} else if (mfd_tbar_is_in_origin_rail_zone(val, false)) {
			if (slot->tbar_state == MFD_TBAR_THROWING) {
				mfd_engine_apply_tbar_abort(eng, slot);
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				mfd_engine_notify_state_changed(eng);
			} else if (was_parked) {
				mfd_engine_unlatch_tbar(eng);
				slot->tbar_state = MFD_TBAR_IDLE;
				mfd_takeover_unpark_from_api(&slot->takeover, 0.0f);
				mfd_smoother_reset(&slot->smoother, 0.0f);
				slot->last_applied_val = 0.0f;
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				mfd_engine_notify_state_changed(eng);
			}
		} else {
			if (was_parked || slot->tbar_state == MFD_TBAR_IDLE) {
				mfd_engine_unlatch_tbar(eng);
			}
			slot->tbar_state = MFD_TBAR_THROWING;
			mfd_takeover_unpark_from_api(&slot->takeover, val);
			mfd_smoother_reset(&slot->smoother, val);
			mfd_engine_apply_slot_output_ex(eng, slot, val, false);
			mfd_engine_notify_fader_changed(eng, slot_index, orig);
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	} else {
		bool at_upper = (val >= 1.0f) || (release && val >= 0.5f);
		bool at_lower = (val <= 0.0f) || (release && val < 0.5f);
		if (slot->takeover.two_rail_mode && (at_upper || at_lower)) {
			float rail_val = at_upper ? 1.0f : 0.0f;
			if (!(was_parked && prev_upper == at_upper &&
			      fabsf(slot->last_applied_val - rail_val) < 1e-5f)) {
				mfd_smoother_reset(&slot->smoother, rail_val);
				mfd_engine_apply_slot_output_ex(eng, slot, rail_val, false);
				mfd_takeover_notify_rail_completed(&slot->takeover, at_upper, rail_val);
				mfd_engine_notify_fader_changed(eng, slot_index, orig);
				mfd_engine_notify_state_changed(eng);
			}
		} else {
			mfd_takeover_unpark_from_api(&slot->takeover, val);
			mfd_smoother_reset(&slot->smoother, val);
			mfd_engine_apply_slot_output_ex(eng, slot, val, false);
			mfd_engine_notify_fader_changed(eng, slot_index, orig);
			if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
				mfd_engine_notify_state_changed(eng);
			}
		}
	}
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
	return true;
}

bool mfd_engine_finish_tbar_throw(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return false;
	}
	mfd_engine_lock(eng);
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	if (!s->enabled || s->target_type != MFD_TARGET_TBAR || s->tbar_state != MFD_TBAR_THROWING ||
	    s->takeover.state == MFD_TAKEOVER_PARKED || !mfd_engine_is_slot_writable(eng, s)) {
		mfd_engine_unlock(eng);
		return false;
	}
	s->auto_fade_active = false;
	s->tbar_midi_active = false;
	s->tbar_from_idle_band = false;
	mfd_smoother_reset(&s->smoother, 1.0f);
	mfd_engine_apply_slot_output_ex(eng, s, 1.0f, true);
	mfd_engine_notify_fader_changed(eng, slot_index, "ui");
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
	return true;
}

static void apply_source_opacity_and_afv(const struct mfd_engine *eng, const char *source_name, float alpha_01,
					 bool afv)
{
	if (!source_name || source_name[0] == '\0') {
		return;
	}
	if (eng && eng->scene_graph_ops && eng->scene_graph_ops->apply_opacity) {
		eng->scene_graph_ops->apply_opacity(source_name, alpha_01, afv, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return;
	}

	obs_source_t *src = obs_get_source_by_name(source_name);
	if (!src) {
		return;
	}

	obs_source_t *filter = mfd_ensure_layer_opacity_filter(src);
	if (filter) {
		mfd_layer_filter_set_opacity(filter, alpha_01);
		obs_source_release(filter);
	}

	if (afv) {
		uint32_t flags = obs_source_get_output_flags(src);
		if ((flags & OBS_SOURCE_AUDIO) != 0U) {
			obs_source_set_volume(src, mfd_deflection_to_mul(alpha_01));
		}
	}

	obs_source_release(src);
}

static void apply_tbar_slot_step(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val,
				 bool force_complete, bool force_abort)
{
	if (!eng || !slot || !slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
		return;
	}
	if (slot->tbar_state == MFD_TBAR_PARKED || slot->takeover.state == MFD_TAKEOVER_PARKED) {
		return;
	}

	float driver_val = force_complete ? 1.0f : (force_abort ? 0.0f : clamp01(norm_val));
	bool in_target = slot->tbar_midi_active ? mfd_tbar_is_in_target_rail_zone(slot->last_raw_norm,
										  slot->takeover.polarity_reversed)
						: mfd_tbar_is_in_target_rail_zone(driver_val, false);
	bool in_origin = slot->tbar_midi_active ? mfd_tbar_is_in_origin_rail_zone(slot->last_raw_norm,
										  slot->takeover.polarity_reversed)
						: mfd_tbar_is_in_origin_rail_zone(driver_val, false);
	bool in_start_deadband =
		(slot->tbar_midi_active && slot->tbar_state == MFD_TBAR_IDLE)
			? !mfd_tbar_is_past_start_zone(slot->last_raw_norm, slot->takeover.polarity_reversed)
			: false;

	enum mfd_tbar_state next_state = slot->tbar_state;
	bool completed = false;
	bool aborted = false;

	if (eng->tbar_driver) {
		char err_buf[256] = {0};
		bool ok = eng->tbar_driver(slot->tbar_state, driver_val, force_complete, force_abort, in_target,
					   in_origin, in_start_deadband, &next_state, &completed, &aborted, err_buf,
					   sizeof(err_buf));
		if (!ok && err_buf[0] != '\0') {
			snprintf(eng->tbar_error, sizeof(eng->tbar_error), "%s", err_buf);
		} else if (ok) {
			eng->tbar_error[0] = '\0';
		}
	} else {
		struct mfd_tbar_release_input in;
		memset(&in, 0, sizeof(in));
		in.studio_mode_active = true;
		in.slider_tbar_found = true;
		in.state = slot->tbar_state;
		in.release_latched = false;
		in.force_complete = force_complete;
		in.force_abort = force_abort;
		in.in_target_rail_zone = in_target;
		in.in_origin_rail_zone = in_origin;
		in.in_start_deadband = in_start_deadband;
		in.t_norm = driver_val;
		in.last_pos_1024 = -1;
		struct mfd_tbar_action act;
		mfd_tbar_compute_action_full(&in, &act);
		next_state = act.next_state;
		completed = act.completed_throw;
		aborted = act.aborted_throw;
	}

	if (completed || force_complete) {
		++slot->output_write_count;
		slot->tbar_state = MFD_TBAR_PARKED;
		mfd_takeover_notify_tbar_completed(&slot->takeover);
		mfd_smoother_reset(&slot->smoother, 0.0f);
		slot->last_applied_val = 0.0f;
		slot->tbar_midi_active = false;
		slot->tbar_from_idle_band = false;
	} else if (aborted || force_abort) {
		++slot->output_write_count;
		slot->tbar_state = MFD_TBAR_IDLE;
		mfd_smoother_reset(&slot->smoother, 0.0f);
		slot->last_applied_val = 0.0f;
		slot->tbar_midi_active = false;
		slot->tbar_from_idle_band = false;
		slot->takeover.target_obs = 0.0f;
		slot->takeover.last_fader = 0.0f;
		slot->takeover.rescale_origin = slot->takeover.polarity_reversed ? 1.0f : 0.0f;
		slot->takeover.has_throw_extreme = false;
	} else if (next_state == MFD_TBAR_IDLE) {
		slot->tbar_state = MFD_TBAR_IDLE;
		slot->tbar_from_idle_band = false;
		slot->last_applied_val = 0.0f;
	} else {
		++slot->output_write_count;
		slot->tbar_state = MFD_TBAR_THROWING;
		slot->last_applied_val = driver_val;
	}
}

void mfd_engine_apply_tbar_abort(struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	if (!eng || !slot || !slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
		return;
	}
	mfd_engine_lock(eng);
	if (slot->tbar_state != MFD_TBAR_THROWING) {
		slot->tbar_state = MFD_TBAR_IDLE;
		slot->tbar_from_idle_band = false;
		mfd_smoother_reset(&slot->smoother, 0.0f);
		slot->last_applied_val = 0.0f;
		mfd_engine_unlock(eng);
		return;
	}
	apply_tbar_slot_step(eng, slot, 0.0f, false, true);
	mfd_engine_unlock(eng);
}

static void apply_filter_param_output_locked(struct mfd_engine *eng, struct mfd_fader_slot *slot, float val)
{
	if (!eng || !slot || !slot->cached_available || !slot->filter_prop_valid) {
		return;
	}
	slot->last_applied_val = val;

	double mapped = mfd_filter_map_value(val, slot->filter_from_val, slot->filter_to_val, slot->filter_prop_min,
					     slot->filter_prop_max, slot->filter_prop_is_int);
	int64_t q_int = slot->filter_prop_is_int ? (int64_t)llround(mapped) : 0;
	double q_flt = mapped;
	bool should_write = mfd_filter_should_write_mapped(slot->has_last_written_mapped, slot->filter_prop_is_int,
							   slot->last_written_int, slot->last_written_float, mapped,
							   slot->filter_from_val, slot->filter_to_val, val);
	if (should_write) {
		obs_data_t *settings = sg_get_filter_settings(eng, slot->source_a_name, slot->filter_name);
		if (settings) {
			if (slot->filter_prop_is_int) {
				obs_data_set_int(settings, slot->filter_prop, (long long)q_int);
			} else {
				obs_data_set_double(settings, slot->filter_prop, q_flt);
			}
			slot->writing_filter_settings = true;
			sg_update_filter_settings(eng, slot->source_a_name, slot->filter_name, settings);
			slot->writing_filter_settings = false;
			obs_data_release(settings);
			slot->has_last_written_mapped = true;
			slot->last_written_int = q_int;
			slot->last_written_float = q_flt;
			++slot->output_write_count;
		}
	}

	sync_filter_bypass_lease_ex_locked(eng, slot->source_a_name, slot->filter_name, -1, true);
}

void mfd_engine_apply_slot_output_ex(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val,
				     bool force_release)
{
	if (!eng || !slot || !slot->enabled) {
		return;
	}
	mfd_engine_lock(eng);
	if (!mfd_engine_is_slot_writable(eng, slot)) {
		mfd_engine_unlock(eng);
		return;
	}

	float val = clamp01(norm_val);

	switch (slot->target_type) {
	case MFD_TARGET_UNASSIGNED:
		slot->last_applied_val = val;
		break;

	case MFD_TARGET_TBAR:
		apply_tbar_slot_step(eng, slot, val, force_release, false);
		break;

	case MFD_TARGET_SINGLE_LAYER:
		slot->last_applied_val = val;
		slot->last_alpha_a = val;
		slot->last_alpha_b = 0.0f;
		++slot->output_write_count;
		/* A held APC40 flash pad keeps the layer at 100 %; the strip's value carries on underneath. */
		if (!slot->vj_flash_held) {
			apply_source_opacity_and_afv(eng, slot->source_a_name, val, slot->audio_follows_video);
		}
		break;

	case MFD_TARGET_TWO_SOURCE_MIX: {
		float alpha_a = 1.0f;
		float alpha_b = 0.0f;
		mfd_crossfade_eval_stacked(slot->curve, val, slot->a_is_above_b, &alpha_a, &alpha_b);
		slot->last_applied_val = val;
		slot->last_alpha_a = alpha_a;
		slot->last_alpha_b = alpha_b;
		++slot->output_write_count;
		apply_source_opacity_and_afv(eng, slot->source_a_name, alpha_a, slot->audio_follows_video);
		apply_source_opacity_and_afv(eng, slot->source_b_name, alpha_b, slot->audio_follows_video);
		break;
	}

	case MFD_TARGET_AUDIO_VOLUME:
		slot->last_applied_val = val;
		slot->last_audio_mul = mfd_deflection_to_mul(val);
		slot->last_audio_db = mfd_deflection_to_db(val);
		++slot->output_write_count;
		if (eng->scene_graph_ops && eng->scene_graph_ops->apply_volume && slot->source_a_name[0] != '\0') {
			eng->scene_graph_ops->apply_volume(slot->source_a_name, slot->last_audio_mul,
							   eng->scene_graph_user_data);
		}
		if (slot->source_a_name[0] != '\0' && obs_initialized()) {
			obs_source_t *src = obs_get_source_by_name(slot->source_a_name);
			if (src) {
				uint32_t flags = obs_source_get_output_flags(src);
				if ((flags & OBS_SOURCE_AUDIO) != 0U) {
					obs_source_set_volume(src, slot->last_audio_mul);
				}
				obs_source_release(src);
			}
		}
		break;

	case MFD_TARGET_MIX_MASTER: {
		int index = (int)(slot - eng->fader_slots);
		slot->last_applied_val = val; /* Local control position; provider replies carry the applied gain. */
		slot->mix_pending = true;
		slot->mix_pending_db = fmaxf(-100.0f, mfd_deflection_to_db(val));
		slot->mix_pending_rev = eng->slot_config_rev[index];
		break;
	}
	case MFD_TARGET_FILTER_PARAM:
		apply_filter_param_output_locked(eng, slot, val);
		break;
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_sync_filter_slot_from_live_locked(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	struct mfd_fader_slot *slot = &eng->fader_slots[slot_index];
	if (!slot->enabled || slot->target_type != MFD_TARGET_FILTER_PARAM || !slot->cached_available ||
	    !slot->filter_prop_valid || slot->pending_clean_apply_on_avail) {
		return;
	}
	float live_value = query_live_filter_unmapped_deflection_locked(eng, slot);
	slot->last_applied_val = live_value;
	slot->has_last_written_mapped = false;
	mfd_smoother_reset(&slot->smoother, live_value);
	mfd_takeover_sync_external(&slot->takeover, live_value);
}

bool mfd_slot_is_reset_fx_eligible(const struct mfd_fader_slot *slot)
{
	return slot && slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM;
}

bool mfd_engine_reset_fx(struct mfd_engine *eng, int ms, int *out_eligible_count, int *out_reset_count,
			 int *out_queued_unavailable_count, char *out_err, size_t err_size)
{
	struct mfd_msg msg;
	bool ok = mfd_engine_reset_fx_msg(eng, ms, out_eligible_count, out_reset_count, out_queued_unavailable_count,
					  &msg);
	mfd_msg_format_english(&msg, out_err, err_size);
	return ok;
}

bool mfd_engine_reset_fx_msg(struct mfd_engine *eng, int ms, int *out_eligible_count, int *out_reset_count,
			     int *out_queued_unavailable_count, struct mfd_msg *out_msg)
{
	mfd_msg_clear(out_msg);
	if (out_eligible_count) {
		*out_eligible_count = 0;
	}
	if (out_reset_count) {
		*out_reset_count = 0;
	}
	if (out_queued_unavailable_count) {
		*out_queued_unavailable_count = 0;
	}
	if (!eng) {
		mfd_msg_set(out_msg, MFD_MSG_ENGINE_NOT_INITIALIZED, NULL, NULL, NULL, NULL, NULL);
		return false;
	}
	if (ms != 0 && (ms < 50 || ms > 20000)) {
		mfd_msg_set(out_msg, MFD_MSG_INVALID_FADE_MS, NULL, NULL, NULL, NULL, NULL);
		return false;
	}

	mfd_engine_lock(eng);
	int eligible = 0;
	int reset_cnt = 0;
	int queued_cnt = 0;

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!mfd_slot_is_reset_fx_eligible(slot)) {
			continue;
		}
		++eligible;

		bool paused_in_setup = (eng->setup_session.active && eng->setup_session.slot_index == i);
		if (paused_in_setup || !mfd_engine_is_slot_writable(eng, slot)) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->has_last_written_mapped = false;
			slot->pending_clean_apply_on_avail = true;
			slot->last_applied_val = 0.0f;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			mfd_engine_notify_fader_changed(eng, i, "api");
			++queued_cnt;
			continue;
		}

		slot->pending_clean_apply_on_avail = false;
		if (ms == 0 || slot->last_applied_val <= 0.0005f) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->has_last_written_mapped = false;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_engine_apply_slot_output_ex(eng, slot, 0.0f, false);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			mfd_engine_notify_fader_changed(eng, i, "api");
			++reset_cnt;
		} else {
			slot->auto_fade_from = slot->last_applied_val;
			slot->auto_fade_to = 0.0f;
			slot->auto_fade_duration_sec = (float)ms / 1000.0f;
			slot->auto_fade_elapsed_sec = 0.0f;
			slot->auto_fade_active = true;
			slot->auto_fade_is_reset_fx = true;
			if (slot->takeover.state == MFD_TAKEOVER_PARKED) {
				mfd_takeover_unpark_from_api(&slot->takeover, slot->auto_fade_from);
			}
			++reset_cnt;
		}
	}

	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_notify_state_changed(eng);
	if (out_eligible_count) {
		*out_eligible_count = eligible;
	}
	if (out_reset_count) {
		*out_reset_count = reset_cnt;
	}
	if (out_queued_unavailable_count) {
		*out_queued_unavailable_count = queued_cnt;
	}
	mfd_engine_unlock(eng);
	return true;
}

void mfd_engine_apply_slot_output(struct mfd_engine *eng, struct mfd_fader_slot *slot, float norm_val)
{
	mfd_engine_apply_slot_output_ex(eng, slot, norm_val, false);
}

void mfd_engine_on_external_tbar_reset(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	bool changed = false;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled || slot->target_type != MFD_TARGET_TBAR) {
			continue;
		}
		if (slot->tbar_state == MFD_TBAR_THROWING) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->pending_clean_apply_on_avail = false;
			slot->tbar_state = MFD_TBAR_IDLE;
			slot->tbar_midi_active = false;
			slot->tbar_from_idle_band = false;
			slot->last_applied_val = 0.0f;
			mfd_smoother_reset(&slot->smoother, 0.0f);
			mfd_takeover_sync_external(&slot->takeover, 0.0f);
			changed = true;
		}
	}
	if (changed) {
		mfd_engine_unlatch_tbar(eng);
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
}

static float normalize_decoded_sample(struct mfd_engine *eng, struct mfd_midi_addr *slot_addr,
				      struct mfd_control_calibration *cal, bool invert,
				      const struct mfd_midi_decoded *dec)
{
	if (!eng || !slot_addr || !cal || !dec) {
		return 0.0f;
	}
	if (dec->addr.type == MFD_MIDI_CC || dec->addr.type == MFD_MIDI_PITCH_BEND) {
		if (dec->addr.type == MFD_MIDI_CC && dec->addr.is_14bit && !slot_addr->is_14bit) {
			int prev_min = cal->cal_min;
			int prev_max = cal->cal_max;
			slot_addr->is_14bit = true;
			if (!mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), slot_addr,
								     cal)) {
				if (prev_min == MFD_CAL_DEFAULT_MIN && prev_max == MFD_CAL_DEFAULT_MAX_7BIT) {
					mfd_calibration_init(cal, true);
				} else {
					mfd_calibration_set(cal, true, prev_min * 128, (prev_max * 128) + 127);
				}
			}
		}
		int raw = (int)dec->raw_value;
		if (dec->addr.type == MFD_MIDI_CC && !dec->addr.is_14bit && slot_addr->is_14bit) {
			raw <<= 7; /* High byte alone on a 14-bit binding: scale it into the 14-bit range. */
		}
		if (mfd_calibration_observe_and_widen(cal, raw)) {
			store_persisted_calibration(eng, effective_profile_name(eng), slot_addr, cal);
			if (obs_initialized()) {
				eng->pending_config_save = true;
			}
			mfd_engine_notify_state_changed(eng);
		}
		return mfd_calibration_normalize(cal, raw, invert);
	}
	return invert ? (1.0f - dec->normalized) : dec->normalized;
}

/* CC 32..63 bound as their own 7-bit control is never read as the low half of a 14-bit pair, even on a strip that is
 * switched off. */
static void sync_parser_lsb_independence_locked(struct mfd_engine *eng)
{
	/* The table depends only on the desk, the layout and the bound addresses: work those out (19 small compares)
	 * and leave the table alone while they are what it was built from (B-11). 0xFFFF is "no CC here". */
	const bool all_independent = (eng->midi_dev.is_open && mfd_is_apc40_mk2_name(eng->midi_dev.opened_name)) ||
				     mfd_engine_get_active_profile(eng) != MFD_PROFILE_GENERIC;
	uint16_t key[MFD_MAX_FADER_SLOTS + 2];
	key[0] = all_independent ? 1U : 0U;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_midi_addr *a = &eng->fader_slots[i].addr;
		const bool own = a->type == MFD_MIDI_CC && !a->is_14bit && a->number >= 32U && a->number < 64U;
		key[i + 1] = own ? (uint16_t)(((uint16_t)a->channel << 8) | a->number) : (uint16_t)0xFFFFU;
	}
	{
		const struct mfd_midi_addr *c = &eng->setup_session.candidate_addr;
		const bool own = eng->setup_session.active && c->type == MFD_MIDI_CC && !c->is_14bit &&
				 c->number >= 32U && c->number < 64U;
		key[MFD_MAX_FADER_SLOTS + 1] = own ? (uint16_t)(((uint16_t)c->channel << 8) | c->number)
						   : (uint16_t)0xFFFFU;
	}
	if (eng->midi_parser.lsb_table_built && memcmp(key, eng->lsb_sync_key, sizeof(key)) == 0) {
		return;
	}
	memcpy(eng->lsb_sync_key, key, sizeof(key));

	mfd_midi_parser_clear_lsb_independent(&eng->midi_parser);
	eng->midi_parser.lsb_table_built = true;
	/* The APC40 mkII sends only 7-bit CCs, but its controls sit on 14-bit LSB numbers: Cue Level (CC 47) is the
	 * crossfader's (CC 15) partner and the track knobs (CC 48..55) the Device knobs' (CC 16..23). Turning one within
	 * the pairing window of the other must not merge them, or Cue Level would drive the live T-bar.
	 *
	 * The same holds for every desk this plugin has a layout for (G-02): none of them sends 14-bit CC pairs, and
	 * on several a second control sits 32 numbers above a fader (the nanoKONTROL2's S buttons above its sliders,
	 * the MIDImix's knob rows). Only the Generic layout, where the desk is unknown, pairs CC n with CC n+32, and
	 * there the parser asks for a run of pairs first (MFD_MIDI_14BIT_PAIR_REPEATS). */
	if (all_independent) {
		for (uint8_t ch = 0; ch < 16U; ++ch) {
			for (uint8_t cc = 32U; cc < 64U; ++cc) {
				mfd_midi_parser_set_lsb_independent(&eng->midi_parser, ch, cc, true);
			}
		}
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_midi_addr *a = &eng->fader_slots[i].addr;
		if (a->type == MFD_MIDI_CC && !a->is_14bit && a->number >= 32U && a->number < 64U) {
			mfd_midi_parser_set_lsb_independent(&eng->midi_parser, a->channel, a->number, true);
		}
	}
	const struct mfd_midi_addr *c = &eng->setup_session.candidate_addr;
	if (eng->setup_session.active && c->type == MFD_MIDI_CC && !c->is_14bit && c->number >= 32U &&
	    c->number < 64U) {
		mfd_midi_parser_set_lsb_independent(&eng->midi_parser, c->channel, c->number, true);
	}
}

/* A volume moved without the fader (OBS mixer, script, scene collection): pick up instead of jumping. With two Volume
 * slots on one source, one fader's write is an outside change to the other, which then waits to pick up. */
static void sync_volume_slots_from_live_locked(struct mfd_engine *eng)
{
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (!s->enabled || !s->cached_available || s->target_type != MFD_TARGET_AUDIO_VOLUME ||
		    s->source_a_name[0] == '\0') {
			continue;
		}
		float live = 0.0f;
		if (!sg_get_source_volume(eng, s->source_a_name, &live) || fabsf(live - s->last_audio_mul) <= 1e-5f) {
			continue;
		}
		float t = mfd_mul_to_deflection(live);
		s->auto_fade_active = false;
		s->last_applied_val = t;
		s->last_audio_mul = live;
		s->last_audio_db = mfd_deflection_to_db(t);
		mfd_smoother_reset(&s->smoother, t);
		mfd_takeover_sync_external(&s->takeover, t);
		mfd_engine_notify_fader_changed(eng, i, "obs");
		mfd_engine_mark_ui_dirty(eng, false);
	}
}

/* ---- APC40 mkII VJ pads (core/vj-pads.h) ------------------------------------------------------------------- */

#define MFD_VJ_DEFAULT_FADE_MS 1000

/* The APC40 mkII's endless knobs that send steps, not a position: Tempo (CC 13) and Cue Level (CC 47), channel 1. */
static bool apc40_relative_knob(const struct mfd_engine *eng, const struct mfd_midi_addr *a)
{
	return eng->midi_dev.is_open && mfd_is_apc40_mk2_name(eng->midi_dev.opened_name) && a->type == MFD_MIDI_CC &&
	       a->channel == 0U && (a->number == MFD_VJ_CC_TEMPO || a->number == MFD_VJ_CC_CUE_LEVEL);
}

static int vj_effective_fade_ms(const struct mfd_engine *eng)
{
	int ms = eng ? eng->vj_fade_ms : 0;
	return (ms < 50 || ms > 20000) ? MFD_VJ_DEFAULT_FADE_MS : ms;
}

/* The pads belong to the APC40 mkII layout; another profile's pads mean other things. */
static bool vj_active(const struct mfd_engine *eng)
{
	/* Only with an APC40 mkII open: the default profile is the APC40's, but another desk on it keeps its notes. */
	return eng && !eng->vj_disabled && eng->midi_dev.is_open && mfd_is_apc40_mk2_name(eng->midi_dev.opened_name) &&
	       strcmp(effective_profile_name(eng), MFD_DEFAULT_PROFILE_NAME) == 0;
}

/* Strip k (0..7) is fader_(k+1), slot k + 1 (slot 0 is the crossfader). */
static struct mfd_fader_slot *vj_slot(struct mfd_engine *eng, int strip)
{
	return (strip >= 0 && strip < (int)MFD_VJ_STRIPS) ? &eng->fader_slots[strip + 1] : NULL;
}

/* The scene item a strip's pads act on: a Layer Opacity strip's source, a Two-Source Mix strip's upper source. */
static bool vj_strip_layer(struct mfd_engine *eng, int strip, const char **out_scene, const char **out_source)
{
	struct mfd_fader_slot *slot = vj_slot(eng, strip);
	if (!slot || !slot->enabled) {
		return false;
	}
	const char *source = NULL;
	if (slot->target_type == MFD_TARGET_SINGLE_LAYER) {
		source = slot->source_a_name;
	} else if (slot->target_type == MFD_TARGET_TWO_SOURCE_MIX) {
		source = slot->a_is_above_b ? slot->source_a_name : slot->source_b_name;
	} else {
		return false;
	}
	const char *scene = slot->scene_name[0] != '\0' ? slot->scene_name : slot->resolved_scene_name;
	if (!source || source[0] == '\0' || !scene || scene[0] == '\0') {
		return false;
	}
	*out_scene = scene;
	*out_source = source;
	return true;
}

static const enum obs_blending_type k_vj_obs_blend[MFD_VJ_BLEND_COUNT] = {
	OBS_BLEND_NORMAL,   OBS_BLEND_LIGHTEN,  OBS_BLEND_SCREEN, OBS_BLEND_ADDITIVE,
	OBS_BLEND_MULTIPLY, OBS_BLEND_SUBTRACT, OBS_BLEND_DARKEN,
};

static bool vj_blend_from_obs(int obs_mode, enum mfd_vj_blend *out)
{
	for (int i = 0; i < (int)MFD_VJ_BLEND_COUNT; ++i) {
		if ((int)k_vj_obs_blend[i] == obs_mode) {
			*out = (enum mfd_vj_blend)i;
			return true;
		}
	}
	return false;
}

struct vj_blend_find {
	const char *source;
	bool set;
	enum obs_blending_type mode;
	bool found;
	enum obs_blending_type read;
};

/* obs_scene_enum_items holds the scene's lock and a reference on each item, so an item removed on another thread
 * cannot go away under us (obs_scene_find_source returns an unreferenced item). Items come bottom first: the last
 * match is the topmost copy, the one the layer-order checks use. */
static bool vj_blend_enum_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct vj_blend_find *f = param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = src ? obs_source_get_name(src) : NULL;
	if (name && strcmp(name, f->source) == 0) {
		f->found = true;
		f->read = obs_sceneitem_get_blending_mode(item);
		if (f->set) {
			obs_sceneitem_set_blending_mode(item, f->mode);
		}
	}
	return true;
}

/* With Studio Mode's Duplicate Scene option (on by default) Program plays a private copy of the scene taken at the
 * Take: its items hold their own visibility and blend, so a desk write to the public scene alone would change the
 * LEDs and the Preview but not what the audience sees. Returns that copy (a new reference) when the output's
 * transition shows a scene of this name that is not the public one; NULL otherwise. */
static obs_source_t *vj_program_copy_of(obs_source_t *scene_src)
{
	obs_source_t *out = obs_get_output_source(0);
	if (!out) {
		return NULL;
	}
	obs_source_t *active =
		obs_source_get_type(out) == OBS_SOURCE_TYPE_TRANSITION ? obs_transition_get_active_source(out) : NULL;
	obs_source_release(out);
	if (active && (active == scene_src || !obs_scene_from_source(active) ||
		       strcmp(obs_source_get_name(active), obs_source_get_name(scene_src)) != 0)) {
		obs_source_release(active);
		active = NULL;
	}
	return active;
}

/* Reads (set == false) or writes a scene item's blend mode. The video tick holds no scene lock (the layer-order
 * reads take the same lock from here). */
static bool vj_blend_io(struct mfd_engine *eng, const char *scene, const char *source, bool set,
			enum mfd_vj_blend *mode)
{
	if (eng->scene_graph_ops &&
	    (set ? eng->scene_graph_ops->set_blend_mode != NULL : eng->scene_graph_ops->get_blend_mode != NULL)) {
		if (set) {
			return eng->scene_graph_ops->set_blend_mode(scene, source, (int)k_vj_obs_blend[*mode],
								    eng->scene_graph_user_data);
		}
		int got = 0;
		return eng->scene_graph_ops->get_blend_mode(scene, source, &got, eng->scene_graph_user_data) &&
		       vj_blend_from_obs(got, mode);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *scene_src = obs_get_source_by_name(scene);
	if (!scene_src) {
		return false;
	}
	struct vj_blend_find f = {source, set, k_vj_obs_blend[*mode], false, OBS_BLEND_NORMAL};
	obs_scene_t *sc = obs_scene_from_source(scene_src);
	if (sc) {
		/* A write gives every copy of the source in the scene the mode, as the opacity filter (on the source)
		 * already reaches every copy; a read reports the topmost copy. */
		obs_scene_enum_items(sc, vj_blend_enum_cb, &f);
		obs_source_t *copy = set && f.found ? vj_program_copy_of(scene_src) : NULL;
		if (copy) {
			/* the copy on air too; its own read is not used (the LEDs follow the public scene) */
			struct vj_blend_find c = {source, true, k_vj_obs_blend[*mode], false, OBS_BLEND_NORMAL};
			obs_scene_enum_items(obs_scene_from_source(copy), vj_blend_enum_cb, &c);
			obs_source_release(copy);
		}
	}
	obs_source_release(scene_src);
	if (!f.found) {
		return false;
	}
	return set ? true : vj_blend_from_obs((int)f.read, mode);
}

/* A line for the log once per strip and reason, not on every press. */
static void vj_say_once(struct mfd_engine *eng, int strip, const char *msg)
{
	if (strip < 0 || strip >= (int)MFD_VJ_STRIPS || strcmp(eng->vj_said[strip], msg) == 0) {
		return;
	}
	snprintf(eng->vj_said[strip], sizeof(eng->vj_said[strip]), "%s", msg);
	obs_log(LOG_INFO, "APC40 pads, strip %d: %s", strip + 1, msg);
}

/* The same start as the obs-websocket auto_fade request, for a Layer Opacity strip. */
static void vj_start_fade_locked(struct mfd_engine *eng, struct mfd_fader_slot *slot, float to_val, float seconds)
{
	bool was_parked = slot->takeover.state == MFD_TAKEOVER_PARKED;
	slot->auto_fade_from = slot->last_applied_val;
	slot->auto_fade_to = clamp01(to_val);
	slot->auto_fade_duration_sec = seconds >= 0.0f ? seconds : (float)vj_effective_fade_ms(eng) / 1000.0f;
	slot->auto_fade_elapsed_sec = 0.0f;
	slot->auto_fade_active = true;
	slot->auto_fade_is_reset_fx = false;
	slot->pending_clean_apply_on_avail = false;
	if (was_parked) {
		mfd_takeover_unpark_from_api(&slot->takeover, slot->auto_fade_from);
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_mark_ui_dirty(eng, false);
}

static bool vj_fadeable(struct mfd_engine *eng, struct mfd_fader_slot *slot)
{
	return slot && slot->enabled && slot->target_type == MFD_TARGET_SINGLE_LAYER &&
	       mfd_engine_is_slot_writable(eng, slot);
}

static void vj_fade_out_locked(struct mfd_engine *eng, int strip)
{
	struct mfd_fader_slot *slot = vj_slot(eng, strip);
	if (!vj_fadeable(eng, slot) || slot->last_applied_val <= 0.0005f) {
		return;
	}
	eng->vj_fade_restore[strip] = slot->last_applied_val;
	vj_start_fade_locked(eng, slot, 0.0f, -1.0f);
	mfd_vj_pads_set_faded(&eng->vj, strip, true);
}

static void vj_set_blend_locked(struct mfd_engine *eng, int s, enum mfd_vj_blend b)
{
	const char *scene = NULL, *source = NULL;
	if (!vj_strip_layer(eng, s, &scene, &source)) {
		vj_say_once(eng, s, "no Layer Opacity or Two-Source Mix layer on this strip, so no blend mode");
		mfd_vj_pads_set_has_layer(&eng->vj, s, false);
		return;
	}
	if (!vj_blend_io(eng, scene, source, true, &b)) {
		vj_say_once(eng, s, "its layer is not in the scene; blend mode not changed");
		enum mfd_vj_blend now = MFD_VJ_BLEND_NORMAL;
		vj_blend_io(eng, scene, source, false, &now);
		mfd_vj_pads_set_current(&eng->vj, s, now);
		return;
	}
	obs_log(LOG_INFO, "APC40 pads: %s blend %s", source, mfd_vj_blend_name(b));
	mfd_vj_pads_set_current(&eng->vj, s, b);
	eng->vj_state_dirty = true;
}

/* Queues a desk command for OBS's UI thread (mfd_engine_flush_pending_ui_work). */
static bool vj_queue_strip_cmd_locked(struct mfd_engine *eng, enum mfd_vj_command_kind kind, const char *scene,
				      const char *source, int arg, int strip)
{
	/* Panic toggles at run time: a second press while one waits would undo it, so it is not queued twice (pressing
	 * again after it ran restores, as on the desk). */
	if (kind == MFD_VJ_CMD_VS_PANIC) {
		for (int i = 0; i < eng->vj_cmd_count; ++i) {
			if (eng->vj_cmds[i].kind == MFD_VJ_CMD_VS_PANIC) {
				return true;
			}
		}
	}
	/* The last two places are kept for Panic (Record), so a burst of other presses never drops it. */
	int room = kind == MFD_VJ_CMD_VS_PANIC ? MFD_VJ_CMD_MAX : MFD_VJ_CMD_MAX - 2;
	if (eng->vj_cmd_count >= room) {
		obs_log(LOG_WARNING, "APC40: too many desk presses at once; one was dropped");
		++eng->vj_cmd_drops;
		return false;
	}
	struct mfd_vj_command *c = &eng->vj_cmds[eng->vj_cmd_count++];
	memset(c, 0, sizeof(*c));
	c->kind = kind;
	c->arg = arg;
	c->strip = strip;
	snprintf(c->scene, sizeof(c->scene), "%s", scene ? scene : "");
	snprintf(c->source, sizeof(c->source), "%s", source ? source : "");
	mfd_engine_mark_ui_dirty(eng, false);
	return true;
}

static bool vj_queue_cmd_locked(struct mfd_engine *eng, enum mfd_vj_command_kind kind, const char *scene,
				const char *source, int arg)
{
	return vj_queue_strip_cmd_locked(eng, kind, scene, source, arg, -1);
}

/* A crossfader (T-bar) throw is under way. A T-bar at rest after a throw is parked at its end and does not count. */
static bool vj_tbar_busy_locked(const struct mfd_engine *eng)
{
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->enabled && s->target_type == MFD_TARGET_TBAR && s->tbar_state == MFD_TBAR_THROWING) {
			return true;
		}
	}
	return false;
}

/* Holds strip s's layer at a level (flash 1, blackout 0) over its fader, which carries on underneath. */
static bool vj_hold_on_locked(struct mfd_engine *eng, int s, float level, const char *why)
{
	struct mfd_fader_slot *slot = vj_slot(eng, s);
	if (!vj_fadeable(eng, slot)) {
		vj_say_once(eng, s, why);
		return false;
	}
	slot->vj_flash_held = true;
	apply_source_opacity_and_afv(eng, slot->source_a_name, level, slot->audio_follows_video);
	eng->vj_state_dirty = true;
	return true;
}

/* Lets go of a held flash or blackout: the layer goes back to its fader's level. */
static void vj_hold_off_locked(struct mfd_engine *eng, int s)
{
	struct mfd_fader_slot *slot = vj_slot(eng, s);
	if (slot && slot->vj_flash_held) {
		slot->vj_flash_held = false;
		apply_source_opacity_and_afv(eng, slot->source_a_name, slot->last_applied_val,
					     slot->audio_follows_video);
		eng->vj_state_dirty = true;
	}
}

/* The scene in Preview as the desk sees it: the one a press just asked for, while that command waits for the UI
 * thread, else the one OBS shows. */
static const char *vj_preview_locked(const struct mfd_engine *eng)
{
	return eng->vj_preview_asked[0] != '\0' ? eng->vj_preview_asked : eng->cached_preview_scene;
}

/* Preview to Program with the current transition. Studio Mode only. */
static void vj_take_locked(struct mfd_engine *eng)
{
	const char *preview = vj_preview_locked(eng);
	if (!mfd_engine_is_studio_mode_active(eng) || preview[0] == '\0') {
		return;
	}
	if (strcmp(preview, eng->cached_program_scene) == 0) {
		obs_log(LOG_INFO, "APC40: \"%s\" is in Preview and on air already, so there is nothing to fade to",
			preview);
		return;
	}
	if (vj_tbar_busy_locked(eng)) {
		obs_log(LOG_INFO, "APC40: a crossfader throw is under way, so the scene buttons wait for it");
	} else {
		/* names the scene meant, checked again on the UI thread before it goes on air */
		vj_queue_cmd_locked(eng, MFD_VJ_CMD_TAKE, preview, NULL, 0);
	}
}

/* A top-row scene fade (pad 0..7) or, pad -1, a cut: Preview to Program at that length. Studio Mode only. */
static void vj_scene_fade_locked(struct mfd_engine *eng, int pad)
{
	const char *preview = vj_preview_locked(eng);
	if (!mfd_engine_is_studio_mode_active(eng) || preview[0] == '\0' ||
	    strcmp(preview, eng->cached_program_scene) == 0) {
		return; /* nothing waiting in Preview: nothing to fade to */
	}
	if (vj_tbar_busy_locked(eng)) {
		obs_log(LOG_INFO, "APC40: a crossfader throw is under way, so the scene fades wait for it");
		return;
	}
	vj_queue_cmd_locked(eng, MFD_VJ_CMD_SCENE_FADE, preview, NULL, pad);
}

/* Scene Launch j: into Preview; pressed again while in Preview, taken. With Shift, or outside Studio Mode, straight
 * to Program. Both use the current transition: Midi Fadester never changes the user's transition. */
static void vj_scene_locked(struct mfd_engine *eng, int j, bool direct)
{
	if (j < 0 || j >= eng->cached_scene_count || j >= (int)MFD_VJ_SCENES) {
		return;
	}
	const char *name = eng->cached_scenes[j];
	if (strcmp(name, eng->cached_program_scene) == 0) {
		/* already on air: putting it in Preview too would leave nothing to fade to */
		obs_log(LOG_INFO, "APC40: \"%s\" is on air already, so its Scene Launch button does nothing", name);
		return;
	}
	if (vj_tbar_busy_locked(eng)) {
		obs_log(LOG_INFO, "APC40: a crossfader throw is under way, so the scene buttons wait for it");
	} else if (direct || !mfd_engine_is_studio_mode_active(eng)) {
		vj_queue_cmd_locked(eng, MFD_VJ_CMD_PROGRAM, name, NULL, 0);
	} else if (strcmp(name, vj_preview_locked(eng)) == 0 && strcmp(name, eng->cached_program_scene) != 0) {
		vj_take_locked(eng);
	} else {
		if (vj_queue_cmd_locked(eng, MFD_VJ_CMD_PREVIEW, name, NULL, 0)) {
			snprintf(eng->vj_preview_asked, sizeof(eng->vj_preview_asked), "%s", name);
		}
	}
}

static void vj_run_locked(struct mfd_engine *eng, const struct mfd_vj_action *a)
{
	int s = a->strip;
	switch (a->kind) {
	case MFD_VJ_ACTION_SET_BLEND:
		vj_set_blend_locked(eng, s, a->blend);
		break;
	case MFD_VJ_ACTION_FLASH_ON:
		/* An override on top of the strip: its value, smoothing, fade and takeover carry on underneath. */
		if (!vj_hold_on_locked(eng, s, 1.0f, "flash needs a Layer Opacity strip")) {
			eng->vj.flashing &= (uint8_t) ~(1U << (unsigned)s);
		}
		break;
	case MFD_VJ_ACTION_BLACKOUT_ON:
		if (!vj_hold_on_locked(eng, s, 0.0f, "blackout needs a Layer Opacity strip")) {
			eng->vj.blackout &= (uint8_t) ~(1U << (unsigned)s);
		}
		break;
	case MFD_VJ_ACTION_FLASH_OFF:
	case MFD_VJ_ACTION_BLACKOUT_OFF:
		vj_hold_off_locked(eng, s);
		break;
	case MFD_VJ_ACTION_SNAP: {
		struct mfd_fader_slot *slot = vj_slot(eng, s);
		if (!vj_fadeable(eng, slot)) {
			vj_say_once(eng, s, "snap needs a Layer Opacity strip");
			break;
		}
		if (slot->last_applied_val > 0.0005f) {
			eng->vj_fade_restore[s] = slot->last_applied_val;
			vj_start_fade_locked(eng, slot, 0.0f, 0.0f);
			mfd_vj_pads_set_faded(&eng->vj, s, true);
		} else {
			float back = eng->vj_fade_restore[s] > 0.001f ? eng->vj_fade_restore[s] : 1.0f;
			vj_start_fade_locked(eng, slot, back, 0.0f);
			mfd_vj_pads_set_faded(&eng->vj, s, false);
		}
		eng->vj_state_dirty = true;
		break;
	}
	case MFD_VJ_ACTION_SHOW_TOGGLE: {
		const char *scene = NULL, *source = NULL;
		if (vj_strip_layer(eng, s, &scene, &source)) {
			bool hidden = (eng->vj.hidden & (1U << (unsigned)s)) != 0;
			if (vj_queue_strip_cmd_locked(eng, MFD_VJ_CMD_SET_VISIBLE, scene, source, hidden ? 1 : 0, s)) {
				mfd_vj_pads_set_hidden(&eng->vj, s, !hidden);
				eng->vj_eye_hold |= (uint8_t)(1U << (unsigned)s);
				obs_log(LOG_INFO, "APC40 pads: %s %s", source, hidden ? "shown" : "hidden");
				eng->vj_state_dirty = true;
			}
		}
		break;
	}
	case MFD_VJ_ACTION_WAKE: {
		const char *scene = NULL, *source = NULL;
		if (!vj_strip_layer(eng, s, &scene, &source)) {
			break;
		}
		if (vj_queue_strip_cmd_locked(eng, MFD_VJ_CMD_SET_VISIBLE, scene, source, 1, s)) {
			mfd_vj_pads_set_hidden(&eng->vj, s, false);
			eng->vj_eye_hold |= (uint8_t)(1U << (unsigned)s);
		}
		eng->vj.flashing &= (uint8_t) ~(1U << (unsigned)s);
		eng->vj.blackout &= (uint8_t) ~(1U << (unsigned)s);
		vj_hold_off_locked(eng, s);
		vj_set_blend_locked(eng, s, eng->vj.home[s]); /* its own blend in the scene */
		struct mfd_fader_slot *slot = vj_slot(eng, s);
		if (vj_fadeable(eng, slot)) {
			vj_start_fade_locked(eng, slot, 1.0f, 0.0f);
			mfd_vj_pads_set_faded(&eng->vj, s, false);
		}
		obs_log(LOG_INFO, "APC40 pads: %s woken: shown, 100 %%, its own blend", source);
		eng->vj_state_dirty = true;
		break;
	}
	case MFD_VJ_ACTION_PIN_TOGGLE: {
		struct mfd_fader_slot *slot = vj_slot(eng, s);
		if (!slot) {
			break;
		}
		if (slot->auto_follow && !(eng->vj.has_layer & (1U << (unsigned)s))) {
			vj_say_once(eng, s, "no source on this column, so there is nothing to pin");
			break;
		}
		if (slot->auto_follow) {
			slot->auto_follow = false;
			obs_log(LOG_INFO, "APC40 pads: column %d pinned to %s", s + 1,
				slot->source_a_name[0] != '\0' ? slot->source_a_name : "no source");
		} else if (slot->target_type == MFD_TARGET_UNASSIGNED || slot->target_type == MFD_TARGET_SINGLE_LAYER) {
			slot->auto_follow = true;
			eng->vj_follow_pending = true;
			obs_log(LOG_INFO, "APC40 pads: column %d follows the scene on air again", s + 1);
		} else {
			vj_say_once(eng, s, "this strip has a job of its own, so it does not follow the scene");
			break;
		}
		mfd_vj_pads_set_pinned(&eng->vj, s,
				       !slot->auto_follow && (eng->vj.has_layer & (1U << (unsigned)s)) != 0);
		if (obs_initialized()) {
			eng->pending_config_save = true;
		}
		mfd_engine_bump_slot_rev(eng, s + 1);
		eng->vj_state_dirty = true;
		break;
	}
	case MFD_VJ_ACTION_COLUMN_CLEAN:
		eng->vj.flashing &= (uint8_t) ~(1U << (unsigned)s);
		eng->vj.blackout &= (uint8_t) ~(1U << (unsigned)s);
		vj_hold_off_locked(eng, s);
		vj_set_blend_locked(eng, s, eng->vj.home[s]); /* its own blend in the scene */
		eng->vj_state_dirty = true;
		break;
	case MFD_VJ_ACTION_SCENE_PICK:
		vj_scene_locked(eng, s, false);
		break;
	case MFD_VJ_ACTION_SCENE_PROGRAM:
		vj_scene_locked(eng, s, true);
		break;
	case MFD_VJ_ACTION_SCENE_FADE:
		vj_scene_fade_locked(eng, s);
		break;
	case MFD_VJ_ACTION_SCENE_CUT:
		vj_scene_fade_locked(eng, -1);
		break;
	case MFD_VJ_ACTION_BLACKOUT_TOGGLE:
		mfd_blackout_toggle(eng->cached_transition_ms);
		eng->vj_state_dirty = true;
		mfd_engine_mark_ui_dirty(eng, false); /* the UI thread puts the black on air, dock shown or not */
		break;
	case MFD_VJ_ACTION_VS_PANIC:
		vj_queue_cmd_locked(eng, MFD_VJ_CMD_VS_PANIC, NULL, NULL, 0);
		break;
	case MFD_VJ_ACTION_VS_TAP:
		vj_queue_cmd_locked(eng, MFD_VJ_CMD_VS_TAP, NULL, NULL, 0);
		break;
	case MFD_VJ_ACTION_VS_LOCK:
		vj_queue_cmd_locked(eng, MFD_VJ_CMD_VS_LOCK, NULL, NULL, 0);
		break;
	case MFD_VJ_ACTION_VS_RESYNC:
		vj_queue_cmd_locked(eng, MFD_VJ_CMD_VS_RESYNC, NULL, NULL, 0);
		break;
	case MFD_VJ_ACTION_CUE_STEPS: {
		long ms = (long)eng->vj_cue_ms + (long)a->steps * 50L;
		eng->vj_cue_ms = (int)(ms > 20000L ? 20000L : ms < -20000L ? -20000L : ms);
		mfd_engine_mark_ui_dirty(eng, false);
		break;
	}
	case MFD_VJ_ACTION_FADE_STEPS: {
		int ms = vj_effective_fade_ms(eng) + a->steps * 50;
		eng->vj_fade_ms = ms < 50 ? 50 : ms > 20000 ? 20000 : ms;
		if (obs_initialized()) {
			eng->pending_config_save = true;
		}
		mfd_engine_mark_ui_dirty(eng, true);
		mfd_engine_notify_state_changed(eng);
		break;
	}
	case MFD_VJ_ACTION_FADE_TOGGLE: {
		struct mfd_fader_slot *slot = vj_slot(eng, s);
		if (!vj_fadeable(eng, slot)) {
			vj_say_once(eng, s, "fades need a Layer Opacity strip");
			break;
		}
		if (eng->vj.faded & (1U << (unsigned)s)) {
			float back = eng->vj_fade_restore[s] > 0.001f ? eng->vj_fade_restore[s] : 1.0f;
			vj_start_fade_locked(eng, slot, back, -1.0f);
			mfd_vj_pads_set_faded(&eng->vj, s, false);
		} else if (slot->last_applied_val > 0.0005f) {
			vj_fade_out_locked(eng, s);
		} else {
			vj_start_fade_locked(eng, slot, 1.0f, -1.0f);
		}
		eng->vj_state_dirty = true;
		break;
	}
	case MFD_VJ_ACTION_CLEAN:
		mfd_engine_reset_fx(eng, 0, NULL, NULL, NULL, NULL, 0);
		for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
			const char *scene = NULL, *source = NULL;
			enum mfd_vj_blend b = eng->vj.home[i];
			/* Only a punched strip is written (its blend was read at this press): an untouched layer, and every
			 * other copy of its source, keep what the scene has. */
			if (!(eng->vj_punched & (1U << (unsigned)i)) || !vj_strip_layer(eng, i, &scene, &source)) {
				continue;
			}
			if (vj_blend_io(eng, scene, source, true, &b) || vj_blend_io(eng, scene, source, false, &b)) {
				mfd_vj_pads_set_current(&eng->vj, i, b); /* written, or what OBS still has */
			}
		}
		obs_log(LOG_INFO, "APC40 pads: Clean / All FX Off, every source back to its own blend");
		eng->vj_refresh_pending = true; /* a source not found keeps its blend: show what OBS has */
		eng->vj_state_dirty = true;
		break;
	case MFD_VJ_ACTION_FADE_ALL_OUT:
		for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
			if (!(eng->vj.faded & (1U << (unsigned)i))) {
				vj_fade_out_locked(eng, i);
			}
		}
		obs_log(LOG_INFO, "APC40 pads: every layer strip fading out");
		eng->vj_state_dirty = true;
		break;
	case MFD_VJ_ACTION_SPEED_STEPS: {
		long total = (long)eng->vj_speed_steps + (long)a->steps;
		eng->vj_speed_steps = (int)(total > MFD_VJ_SPEED_MAX_STEPS    ? MFD_VJ_SPEED_MAX_STEPS
					    : total < -MFD_VJ_SPEED_MAX_STEPS ? -MFD_VJ_SPEED_MAX_STEPS
									      : total);
		break;
	}
	case MFD_VJ_ACTION_SPEED_RESET:
		eng->vj_speed_steps = 0; /* steps turned before Tap Tempo are overtaken by it */
		eng->vj_speed_actions &= ~MFD_VJ_SPEED_SET_BIT;
		eng->vj_speed_actions |= MFD_VJ_SPEED_RESET_BIT;
		break;
	case MFD_VJ_ACTION_PLAY_PAUSE:
		eng->vj_speed_actions ^= MFD_VJ_SPEED_PLAY_BIT; /* two presses before they are sent cancel out */
		break;
	case MFD_VJ_ACTION_NONE:
	default:
		break;
	}
}

/* Sends a pressed button's light again on the next tick, in case the desk changed it by itself on the press or the
 * release. */
static void vj_repaint_note_locked(struct mfd_engine *eng, uint8_t channel, uint8_t note)
{
	struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
	size_t n = mfd_vj_pads_leds(&eng->vj, leds, MFD_VJ_MAX_LEDS);
	for (size_t i = 0; i < n; ++i) {
		if (leds[i].note == note &&
		    (leds[i].rgb || leds[i].note != MFD_VJ_NOTE_CLIP_STOP || leds[i].channel == channel)) {
			eng->vj_led_sent[i] = 0xFFFFU;
		}
	}
}

/* A note on / note off for the VJ pads, unless a learned strip owns that address. True when it was one. Runs after
 * MIDI Learn, and never while learning or while Set up edits a strip. */
static void vj_sync_blend_locked(struct mfd_engine *eng, int i);

static bool vj_feed_locked(struct mfd_engine *eng, const struct mfd_midi_decoded *dec)
{
	/* A held flash pad is always let go, whatever changed while it was held (Learn, Set up, the pads turned off). */
	if (dec->addr.type == MFD_MIDI_NOTE_OFF && dec->addr.channel == 0U &&
	    dec->addr.number >= MFD_VJ_NOTE_ROW_FLASH && dec->addr.number < MFD_VJ_NOTE_ROW_FLASH + MFD_VJ_STRIPS &&
	    ((eng->vj.flashing | eng->vj.blackout) & (1U << (unsigned)(dec->addr.number - MFD_VJ_NOTE_ROW_FLASH)))) {
		struct mfd_vj_action off[MFD_VJ_MAX_ACTIONS];
		size_t n_off = 0;
		mfd_vj_pads_feed(&eng->vj, false, 0U, (uint8_t)dec->addr.number, off, MFD_VJ_MAX_ACTIONS, &n_off);
		for (size_t i = 0; i < n_off; ++i) {
			vj_run_locked(eng, &off[i]);
		}
		vj_repaint_note_locked(eng, dec->addr.channel, (uint8_t)dec->addr.number);
		return true;
	}
	if (dec->addr.type == MFD_MIDI_NOTE_OFF && dec->addr.channel == 0U && dec->addr.number == MFD_VJ_NOTE_SHIFT &&
	    eng->vj.shift) {
		eng->vj.shift = false; /* a held Shift is always let go too */
		return vj_active(eng);
	}
	bool is_note = dec->addr.type == MFD_MIDI_NOTE_ON || dec->addr.type == MFD_MIDI_NOTE_OFF;
	bool is_knob = dec->addr.type == MFD_MIDI_CC && !dec->addr.is_14bit &&
		       mfd_vj_pads_is_knob(dec->addr.channel, (uint8_t)dec->addr.number);
	if (!vj_active(eng) || eng->setup_session.active || eng->learning_slot_index >= 0 || (!is_note && !is_knob)) {
		return false;
	}
	struct mfd_midi_addr owner = dec->addr;
	if (is_note) {
		owner.type = MFD_MIDI_NOTE_ON; /* a strip learned on this pad's note on owns its note off too */
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (eng->fader_slots[i].enabled && mfd_midi_addr_matches(&eng->fader_slots[i].addr, &owner)) {
			return false;
		}
	}
	struct mfd_vj_action acts[MFD_VJ_MAX_ACTIONS];
	size_t n = 0;
	/* Only a press that works with blends (a bottom-row pad, Clip Stop, Clean) reads them from OBS first: each read
	 * looks the scene up, so Shift, the scene buttons and the other rows skip it. */
	uint8_t note = (uint8_t)dec->addr.number;
	if (is_note && dec->addr.type == MFD_MIDI_NOTE_ON &&
	    (note < MFD_VJ_NOTE_ROW_FLASH || note == MFD_VJ_NOTE_CLIP_STOP || note == MFD_VJ_NOTE_STOP_ALL)) {
		eng->vj_punched = 0U;
		for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
			vj_sync_blend_locked(eng, i);
			if (eng->vj.current[i] != eng->vj.home[i]) {
				eng->vj_punched |=
					(uint8_t)(1U << (unsigned)i); /* before the feed sets them home (Clean) */
			}
		}
	}
	bool fed = is_note ? mfd_vj_pads_feed(&eng->vj, dec->addr.type == MFD_MIDI_NOTE_ON, dec->addr.channel,
					      (uint8_t)dec->addr.number, acts, MFD_VJ_MAX_ACTIONS, &n)
			   : mfd_vj_pads_feed_cc(&eng->vj, dec->addr.channel, (uint8_t)dec->addr.number,
						 (uint8_t)(dec->raw_value & 0x7FU), acts, MFD_VJ_MAX_ACTIONS, &n);
	if (!fed) {
		if (is_note && dec->addr.type == MFD_MIDI_NOTE_ON && dec->addr.channel < 16U &&
		    dec->addr.number < 128U) {
			/* once per button: which APC40 buttons send a plain press but have no job (to pick free ones) */
			static uint8_t logged[16][16];
			uint8_t *byte = &logged[dec->addr.channel][dec->addr.number / 8U];
			uint8_t bit = (uint8_t)(1U << (dec->addr.number % 8U));
			if (!(*byte & bit)) {
				*byte |= bit;
				obs_log(LOG_INFO, "APC40: button note %u on channel %u has no job",
					(unsigned)dec->addr.number, (unsigned)dec->addr.channel + 1U);
			}
		}
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		vj_run_locked(eng, &acts[i]);
	}
	if (is_note) {
		vj_repaint_note_locked(eng, dec->addr.channel, (uint8_t)dec->addr.number);
	}
	mfd_engine_mark_ui_dirty(eng, false);
	return true;
}

struct vj_visible_find {
	const char *source;
	bool found;
	bool visible;
};

static bool vj_visible_enum_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct vj_visible_find *f = param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = src ? obs_source_get_name(src) : NULL;
	if (name && strcmp(name, f->source) == 0) {
		f->found = true;
		f->visible = obs_sceneitem_visible(item); /* the topmost copy, as for the blend mode */
	}
	return true;
}

/* Whether a source's eye is on in a scene. False when the item is not found. */
static bool vj_item_visible(struct mfd_engine *eng, const char *scene, const char *source, bool *out_visible)
{
	if (eng->scene_graph_ops) {
		return eng->scene_graph_ops->get_item_visible &&
		       eng->scene_graph_ops->get_item_visible(scene, source, out_visible, eng->scene_graph_user_data);
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *scene_src = obs_get_source_by_name(scene);
	if (!scene_src) {
		return false;
	}
	struct vj_visible_find f = {source, false, true};
	obs_scene_t *sc = obs_scene_from_source(scene_src);
	if (sc) {
		obs_scene_enum_items(sc, vj_visible_enum_cb, &f);
	}
	obs_source_release(scene_src);
	if (f.found) {
		*out_visible = f.visible;
	}
	return f.found;
}

/* What the lights show besides the blends: hidden, at 0 %, pinned, the scenes, Session and Record. read_eyes re-reads
 * each source's eye from OBS (on a refresh: a scene change, an item_visible signal, a show / hide that has run). */
static void vj_status_locked(struct mfd_engine *eng, bool read_eyes)
{
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		struct mfd_fader_slot *slot = vj_slot(eng, i);
		bool has = slot && (eng->vj.has_layer & (1U << (unsigned)i)) != 0;
		mfd_vj_pads_set_pinned(&eng->vj, i, has && !slot->auto_follow);
		mfd_vj_pads_set_zero(&eng->vj, i,
				     has && slot->target_type == MFD_TARGET_SINGLE_LAYER && !slot->vj_flash_held &&
					     slot->last_applied_val <= 0.0005f);
		if (!has) {
			mfd_vj_pads_set_hidden(&eng->vj, i, false);
		} else if (read_eyes && !(eng->vj_eye_hold & (1U << (unsigned)i))) {
			const char *scene = NULL, *source = NULL;
			bool visible = true;
			if (vj_strip_layer(eng, i, &scene, &source) && vj_item_visible(eng, scene, source, &visible)) {
				mfd_vj_pads_set_hidden(&eng->vj, i, !visible);
			}
		}
	}
	bool studio = mfd_engine_is_studio_mode_active(eng);
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		enum mfd_vj_scene_state st = MFD_VJ_SCENE_NONE;
		if (j < eng->cached_scene_count) {
			const char *nm = eng->cached_scenes[j];
			st = strcmp(nm, eng->cached_program_scene) == 0             ? MFD_VJ_SCENE_PROGRAM
			     : studio && strcmp(nm, eng->cached_preview_scene) == 0 ? MFD_VJ_SCENE_PREVIEW
										    : MFD_VJ_SCENE_IDLE;
		}
		mfd_vj_pads_set_scene(&eng->vj, j, st);
	}
	eng->vj.take_ready = studio && eng->cached_preview_scene[0] != '\0' &&
			     strcmp(eng->cached_preview_scene, eng->cached_program_scene) != 0;
	/* The Record lamp is on or off only: half a second on, half off while Visual Sidechain is panicked. */
	eng->vj.vs_panic = eng->vs_panic && fmodf(eng->vj_blink_sec, 1.0f) < 0.5f;
	eng->vj.blackout_lamp = mfd_blackout_is_on() && fmodf(eng->vj_blink_sec, 1.0f) < 0.5f;
}

/* Whether a strip other than `except` shows the source a home key names. */
static bool vj_key_shared_locked(const struct mfd_engine *eng, int except, const char *key)
{
	if (!key || key[0] == '\0') {
		return false; /* no source: nothing to share */
	}
	for (int j = 0; j < (int)MFD_VJ_STRIPS; ++j) {
		if (j != except && strcmp(eng->vj_home_key[j], key) == 0) {
			return true;
		}
	}
	return false;
}

/* Puts the source a home key names ("scene\nsource") back to a blend. */
static void vj_restore_key_locked(struct mfd_engine *eng, const char *key, enum mfd_vj_blend home)
{
	char buf[sizeof(eng->vj_home_key[0])];
	snprintf(buf, sizeof(buf), "%s", key);
	char *nl = strchr(buf, '\n');
	if (!nl) {
		return;
	}
	*nl = '\0';
	enum mfd_vj_blend b = home;
	vj_blend_io(eng, buf, nl + 1, true, &b); /* the source may be gone: nothing to put back then */
}

/* Every punched source back to its own blend (pads off, the desk gone, a scene collection change, OBS closing). */
static void vj_restore_all_punches_locked(struct mfd_engine *eng)
{
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		if (eng->vj_home_key[i][0] != '\0' && eng->vj.current[i] != eng->vj.home[i]) {
			vj_restore_key_locked(eng, eng->vj_home_key[i], eng->vj.home[i]);
			mfd_vj_pads_set_current(&eng->vj, i, eng->vj.home[i]);
		}
	}
}

/* Punched sources saved with the scene collection that just loaded go back to their own blends. */
static void vj_restore_loaded_punches_locked(struct mfd_engine *eng)
{
	for (int k = 0; k < eng->vj_load_restore_count; ++k) {
		vj_restore_key_locked(eng, eng->vj_load_restore_key[k], eng->vj_load_restore_home[k]);
	}
	eng->vj_load_restore_count = 0;
}

/* One strip's blend read from OBS just before a desk press acts on it: a change made with the mouse (OBS reports no
 * signal for a blend change) is the source's own blend from now on. */
static void vj_sync_blend_locked(struct mfd_engine *eng, int i)
{
	const char *scene = NULL, *source = NULL;
	enum mfd_vj_blend b = MFD_VJ_BLEND_NORMAL;
	if (!(eng->vj.has_layer & (1U << (unsigned)i)) || !vj_strip_layer(eng, i, &scene, &source) ||
	    !vj_blend_io(eng, scene, source, false, &b)) {
		return;
	}
	char key[sizeof(eng->vj_home_key[0])];
	snprintf(key, sizeof(key), "%s\n%s", scene, source);
	if (strcmp(key, eng->vj_home_key[i]) != 0) {
		/* Not read since the strip took this source (a press right after a scene or collection change): learn it
		 * now, as the refresh would, so the press and a later save know the source's own blend. */
		enum mfd_vj_blend home = b;
		for (int j = 0; j < (int)MFD_VJ_STRIPS; ++j) {
			if (j != i && strcmp(eng->vj_home_key[j], key) == 0) {
				home = eng->vj.home[j];
				break;
			}
		}
		snprintf(eng->vj_home_key[i], sizeof(eng->vj_home_key[i]), "%s", key);
		mfd_vj_pads_set_home(&eng->vj, i, home);
	} else if (b != eng->vj.current[i] && !vj_key_shared_locked(eng, i, key)) {
		mfd_vj_pads_set_home(&eng->vj, i, b);
	}
	mfd_vj_pads_set_current(&eng->vj, i, b);
}

/* Each strip's layer and its blend mode as OBS has them now (scene change, graph change, a strip edited). */
static void vj_refresh_locked(struct mfd_engine *eng)
{
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		const char *scene = NULL, *source = NULL;
		bool has = vj_strip_layer(eng, i, &scene, &source);
		char key[sizeof(eng->vj_home_key[0])];
		key[0] = '\0';
		if (has) {
			snprintf(key, sizeof(key), "%s\n%s", scene, source);
		}
		/* The strip no longer shows the source it had (edited, cleared, switched off, re-pointed, by any path): a
		 * punch it left there goes back to the source's own blend, unless another strip still shows that source. */
		if (eng->vj_home_key[i][0] != '\0' && strcmp(key, eng->vj_home_key[i]) != 0 &&
		    eng->vj.current[i] != eng->vj.home[i] && !vj_key_shared_locked(eng, i, eng->vj_home_key[i])) {
			vj_restore_key_locked(eng, eng->vj_home_key[i], eng->vj.home[i]);
		}
		mfd_vj_pads_set_has_layer(&eng->vj, i, has);
		enum mfd_vj_blend b = MFD_VJ_BLEND_NORMAL;
		if (!has) {
			mfd_vj_pads_set_current(&eng->vj, i, MFD_VJ_BLEND_NORMAL);
			mfd_vj_pads_set_home(&eng->vj, i, MFD_VJ_BLEND_NORMAL);
			eng->vj_home_key[i][0] = '\0';
			continue;
		}
		const enum mfd_vj_blend before = eng->vj.current[i];
		bool read = vj_blend_io(eng, scene, source, false, &b);
		if (!read) {
			continue; /* not readable now (loading, briefly missing): keep what is known, read it next time */
		}
		if (strcmp(key, eng->vj_home_key[i]) != 0) {
			/* A new source on this strip: its blend in the scene now is the one Clean goes back to, unless another
			 * strip shows it already (it may have punched it): then that strip knows its own blend. */
			enum mfd_vj_blend home = b;
			for (int j = 0; j < (int)MFD_VJ_STRIPS; ++j) {
				if (j != i && strcmp(eng->vj_home_key[j], key) == 0) {
					home = eng->vj.home[j];
					break;
				}
			}
			snprintf(eng->vj_home_key[i], sizeof(eng->vj_home_key[i]), "%s", key);
			mfd_vj_pads_set_home(&eng->vj, i, home);
		} else if (b != before && !vj_key_shared_locked(eng, i, key)) {
			/* Changed outside Midi Fadester (the mouse, a script): that is the source's blend now. Another strip
			 * on the same source may have punched it: then this strip only shows it. */
			mfd_vj_pads_set_home(&eng->vj, i, b);
		}
		mfd_vj_pads_set_current(&eng->vj, i, b);
	}
	vj_status_locked(eng, true);
}

/* Keeps the pad state true to the strips: a strip raised by hand (or by obs-websocket) after a pad faded it out is
 * no longer faded out; a flash on a strip that is no longer a Layer Opacity strip, or whose pad is no longer held, ends. */
static void vj_tidy_locked(struct mfd_engine *eng)
{
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		struct mfd_fader_slot *slot = vj_slot(eng, i);
		unsigned b = 1U << (unsigned)i;
		if (!slot) {
			continue;
		}
		if ((eng->vj.faded & b) && !slot->auto_fade_active && slot->last_applied_val > 0.0005f) {
			mfd_vj_pads_set_faded(&eng->vj, i, false);
			eng->vj_state_dirty = true;
		}
		if (slot->vj_flash_held && (!((eng->vj.flashing | eng->vj.blackout) & b) || !vj_fadeable(eng, slot))) {
			slot->vj_flash_held = false;
			eng->vj.flashing &= (uint8_t)~b;
			eng->vj.blackout &= (uint8_t)~b;
			apply_source_opacity_and_afv(eng, slot->source_a_name, slot->last_applied_val,
						     slot->audio_follows_video);
			eng->vj_state_dirty = true;
		}
	}
}

/* Sends the LEDs that changed (a dirty table, so a quiet tick sends nothing). */
static void vj_flush_leds_locked(struct mfd_engine *eng)
{
	if (!vj_active(eng) || !mfd_winmm_has_output(&eng->midi_dev) ||
	    os_atomic_load_bool(&eng->midi_dev.device_closed_unexpectedly)) {
		return;
	}
	struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
	size_t n = mfd_vj_pads_leds(&eng->vj, leds, MFD_VJ_MAX_LEDS);
	unsigned sent = 0U;
	for (size_t i = 0; i < n && sent < 48U; ++i) {
		uint16_t key = (uint16_t)(((unsigned)leds[i].channel << 8) | leds[i].velocity);
		if (eng->vj_led_sent[i] == key) {
			continue;
		}
		/* A pulse or blink runs between the pad's primary colour (channel 1) and this one: the dim end goes first,
		 * so the pad stays lit if the desk does not animate it. */
		bool animated = leds[i].rgb && leds[i].channel != 0U;
		if (animated && !mfd_winmm_send_short(&eng->midi_dev, 0x90U, leds[i].note, leds[i].primary)) {
			break;
		}
		if (!mfd_winmm_send_short(&eng->midi_dev, (uint8_t)(0x90U | leds[i].channel), leds[i].note,
					  leds[i].velocity)) {
			break; /* the desk is going away: try again next tick, not 55 times now */
		}
		eng->vj_led_sent[i] = key;
		sent += animated ? 2U : 1U;
	}
}

/* Turns off every LED the pads lit (pads turned off, OBS closing). */
static void vj_leds_off_locked(struct mfd_engine *eng)
{
	if (mfd_winmm_has_output(&eng->midi_dev)) {
		struct mfd_vj_led leds[MFD_VJ_MAX_LEDS];
		size_t n = mfd_vj_pads_leds(&eng->vj, leds, MFD_VJ_MAX_LEDS);
		for (size_t i = 0; i < n; ++i) {
			uint16_t was = eng->vj_led_sent[i];
			if (was != 0xFFFFU && (was & 0xFFU) != 0U) {
				/* An RGB pad goes off (and stops animating) on channel 1; a lamp on its own channel. */
				uint8_t ch = leds[i].rgb ? 0U : (uint8_t)(was >> 8);
				mfd_winmm_send_short(&eng->midi_dev, (uint8_t)(0x90U | ch), leds[i].note, 0U);
			}
		}
	}
	memset(eng->vj_led_sent, 255, sizeof(eng->vj_led_sent));
}

/* The controller went away: let go of held pads (a flash goes back to the fader's level). */
static void vj_release_all_locked(struct mfd_engine *eng)
{
	struct mfd_vj_action acts[MFD_VJ_MAX_ACTIONS];
	size_t n = mfd_vj_pads_release_all(&eng->vj, acts, MFD_VJ_MAX_ACTIONS);
	for (size_t i = 0; i < n; ++i) {
		vj_run_locked(eng, &acts[i]);
	}
	memset(eng->vj_led_sent, 255, sizeof(eng->vj_led_sent));
	vj_hold_epoch_sync_locked(eng);
}

void mfd_engine_set_vj_buttons(struct mfd_engine *eng, bool enabled, int fade_ms)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	bool was = vj_active(eng);
	eng->vj_disabled = !enabled;
	eng->vj_fade_ms = (fade_ms < 50 || fade_ms > 20000) ? MFD_VJ_DEFAULT_FADE_MS : fade_ms;
	if (was && !vj_active(eng)) {
		vj_leds_off_locked(eng); /* first: it only turns off what the table says is lit */
		vj_release_all_locked(eng);
		vj_restore_all_punches_locked(eng);
	} else if (!was && vj_active(eng)) {
		memset(eng->vj_led_sent, 255, sizeof(eng->vj_led_sent));
		if (eng->frontend_finished_loading && !eng->vj_autofill_done) {
			eng->vj_autofill_pending = true;
		}
		eng->vj_follow_pending = true; /* Program may have changed while the pads were off */
		mfd_engine_mark_ui_dirty(eng, false);
	}
	eng->vj_was_active = vj_active(eng);
	eng->vj_refresh_pending = true;
	if (obs_initialized()) {
		eng->pending_config_save = true;
	}
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
}

bool mfd_engine_vj_buttons_enabled(const struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	struct mfd_engine *lock_eng = (struct mfd_engine *)(uintptr_t)eng;
	mfd_engine_lock(lock_eng);
	bool on = !eng->vj_disabled;
	mfd_engine_unlock(lock_eng);
	return on;
}

void mfd_engine_set_desk_video_controls(struct mfd_engine *eng, bool shown)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->desk_video_controls = shown;
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
}

bool mfd_engine_desk_video_controls(const struct mfd_engine *eng)
{
	return eng && eng->desk_video_controls;
}

bool mfd_engine_desk_first_show_due(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	mfd_engine_lock(eng);
	bool due = !eng->desk_dock_shown && eng->frontend_finished_loading && eng->midi_dev.is_open &&
		   mfd_is_apc40_mk2_name(eng->midi_dev.opened_name);
	mfd_engine_unlock(eng);
	return due;
}

void mfd_engine_desk_first_show_done(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	bool changed = !eng->desk_dock_shown;
	eng->desk_dock_shown = true;
	mfd_engine_unlock(eng);
	if (changed) {
		mfd_engine_save_global_config(eng);
	}
}

int mfd_engine_vj_fade_ms(const struct mfd_engine *eng)
{
	if (!eng) {
		return vj_effective_fade_ms(eng);
	}
	struct mfd_engine *lock_eng = (struct mfd_engine *)(uintptr_t)eng;
	mfd_engine_lock(lock_eng);
	int ms = vj_effective_fade_ms(eng);
	mfd_engine_unlock(lock_eng);
	return ms;
}

/* --- Strips that fill themselves from the scene on air --- */

#define MFD_VJ_LAYER_MAX 32

struct vj_layer_list {
	int count;
	char names[MFD_VJ_LAYER_MAX][MFD_SOURCE_NAME_MAX];
};

static void vj_layer_add(struct vj_layer_list *l, const char *name)
{
	if (!name || name[0] == '\0' || strncmp(name, "__mfd_", 6) == 0 || strncmp(name, "__midi_fadester_", 16) == 0 ||
	    l->count >= MFD_VJ_LAYER_MAX) {
		return;
	}
	for (int i = 0; i < l->count; ++i) {
		if (strcmp(l->names[i], name) == 0) {
			return;
		}
	}
	snprintf(l->names[l->count], sizeof(l->names[l->count]), "%s", name);
	++l->count;
}

static bool vj_collect_enum_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src && !obs_source_removed(src) && source_shows_video(src)) {
		vj_layer_add(param, obs_source_get_name(src));
	}
	return true;
}

/* A scene's video sources, bottom first (hidden ones too: row 4 shows them and can bring them in). False when the
 * scene is not there. */
static bool vj_collect_layers(struct mfd_engine *eng, const char *scene, struct vj_layer_list *out)
{
	memset(out, 0, sizeof(*out));
	if (eng->scene_graph_ops) {
		if (!eng->scene_graph_ops->fill_catalog) {
			return false;
		}
		struct mfd_ui_catalog *cat = mfd_ui_catalog_create();
		if (!cat) {
			return false;
		}
		eng->scene_graph_ops->fill_catalog(cat, eng->scene_graph_user_data);
		bool found = false;
		for (size_t i = 0U; i < cat->scene_count && !found; ++i) {
			if (strcmp(cat->scenes[i].name, scene) == 0) {
				found = true;
				for (size_t k = 0U; k < cat->scenes[i].layer_count; ++k) {
					if (cat->scenes[i].layer_has_video[k]) {
						vj_layer_add(out, cat->scenes[i].layers[k]);
					}
				}
			}
		}
		mfd_ui_catalog_destroy(cat);
		return found;
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *scene_src = obs_get_source_by_name(scene);
	if (!scene_src) {
		return false;
	}
	obs_scene_t *sc = obs_scene_from_source(scene_src);
	if (sc) {
		obs_scene_enum_items(sc, vj_collect_enum_cb, out);
	}
	obs_source_release(scene_src);
	return sc != NULL;
}

/* The level Midi Fadester's own opacity filter has on a source now, if it has one. */
static bool vj_live_opacity(struct mfd_engine *eng, const char *source, float *out)
{
	if (!source || source[0] == '\0') {
		return false;
	}
	if (eng->scene_graph_ops) {
		float v = 0.0f;
		if (eng->scene_graph_ops->get_opacity &&
		    eng->scene_graph_ops->get_opacity(source, &v, eng->scene_graph_user_data) && v >= 0.0f &&
		    v <= 1.0f) {
			*out = v;
			return true;
		}
		return false;
	}
	if (!obs_initialized()) {
		return false;
	}
	obs_source_t *src = obs_get_source_by_name(source);
	if (!src) {
		return false;
	}
	bool ok = false;
	obs_source_t *flt = obs_source_get_filter_by_name(src, MFD_INTERNAL_OPACITY_FILTER_NAME);
	if (flt) {
		float v = mfd_layer_filter_get_opacity(flt);
		if (v >= 0.0f && v <= 1.0f) {
			*out = v;
			ok = true;
		}
		obs_source_release(flt);
	}
	obs_source_release(src);
	return ok;
}

/* A strip taking a layer that is at some level already starts there, with pick-up from where its fader is. */
static void vj_start_strip_at_locked(struct mfd_engine *eng, struct mfd_fader_slot *s, float v)
{
	mfd_smoother_reset(&s->smoother, v);
	restart_pickup_at_locked(eng, s, v);
	s->last_applied_val = v;
	s->last_alpha_a = v;
}

/* Points strip k (1..8) at a source in a scene, or at nothing, as a strip that follows the scene. What the pads held
 * on the old source is let go first, so it is not left at 100 % or 0 %. Returns whether anything changed. */
/* Strip s is about to leave its source: a punched blend goes back to the source's own, so the scene it stays in looks
 * as it was built, and the next strip to take that source reads its true blend. */
static void vj_leave_source_locked(struct mfd_engine *eng, int s)
{
	const char *scene = NULL, *source = NULL;
	if (eng->vj.current[s] != eng->vj.home[s] && !vj_key_shared_locked(eng, s, eng->vj_home_key[s]) &&
	    vj_strip_layer(eng, s, &scene, &source)) {
		enum mfd_vj_blend b = eng->vj.home[s];
		if (vj_blend_io(eng, scene, source, true, &b)) {
			mfd_vj_pads_set_current(&eng->vj, s, b);
		}
	}
	eng->vj_home_key[s][0] = '\0'; /* the next source's blend is read afresh */
}

/* A strip's flash / blackout hold changed: its epoch moves on, so a dock release of an earlier hold does nothing. */
static void vj_hold_epoch_sync_locked(struct mfd_engine *eng)
{
	uint8_t now = (uint8_t)(eng->vj.flashing | eng->vj.blackout);
	uint8_t changed = (uint8_t)(now ^ eng->vj_hold_seen);
	for (int i = 0; i < (int)MFD_VJ_STRIPS && changed != 0U; ++i) {
		if (changed & (1U << (unsigned)i)) {
			++eng->vj_hold_epoch[i];
		}
	}
	eng->vj_hold_seen = now;
}

void mfd_engine_vj_let_go_locked(struct mfd_engine *eng, int slot_index)
{
	int strip = slot_index - 1;
	if (strip < 0 || strip >= (int)MFD_VJ_STRIPS) {
		return;
	}
	struct mfd_fader_slot *s = &eng->fader_slots[slot_index];
	uint8_t b = (uint8_t)(1U << (unsigned)strip);
	vj_leave_source_locked(eng, strip);
	if (s->vj_flash_held) {
		/* on the source it held, before the strip points elsewhere */
		s->vj_flash_held = false;
		apply_source_opacity_and_afv(eng, s->source_a_name, s->last_applied_val, s->audio_follows_video);
	}
	eng->vj.flashing &= (uint8_t)~b;
	eng->vj.blackout &= (uint8_t)~b;
	vj_hold_epoch_sync_locked(eng);
	eng->vj_state_dirty = true;
}

static bool vj_point_strip_locked(struct mfd_engine *eng, int k, const char *scene, const char *source)
{
	struct mfd_fader_slot *s = &eng->fader_slots[k];
	bool was_auto = s->auto_follow;
	s->auto_follow = true;
	if (source && s->enabled && s->target_type == MFD_TARGET_SINGLE_LAYER &&
	    strcmp(s->source_a_name, source) == 0 && strcmp(s->scene_name, scene) == 0) {
		return !was_auto;
	}
	if (!source && s->target_type == MFD_TARGET_UNASSIGNED) {
		return !was_auto;
	}
	if (source && s->enabled && s->target_type == MFD_TARGET_SINGLE_LAYER &&
	    strcmp(s->source_a_name, source) == 0) {
		/* The same source in the scene now on air: its level, fade, flash and pick-up carry on. */
		vj_leave_source_locked(eng, k - 1);
		snprintf(s->scene_name, sizeof(s->scene_name), "%s", scene);
		s->resolved_scene_name[0] = '\0';
		return true;
	}
	int strip = k - 1;
	uint8_t b = (uint8_t)(1U << (unsigned)strip);
	vj_leave_source_locked(eng, strip);
	if (s->vj_flash_held) {
		s->vj_flash_held = false;
		apply_source_opacity_and_afv(eng, s->source_a_name, s->last_applied_val, s->audio_follows_video);
	}
	eng->vj.flashing &= (uint8_t)~b;
	eng->vj.blackout &= (uint8_t)~b;
	mfd_vj_pads_set_faded(&eng->vj, strip, false);
	mfd_vj_pads_set_hidden(&eng->vj, strip, false);
	eng->vj_eye_hold &= (uint8_t)~b;
	eng->vj_fade_restore[strip] = 0.0f;
	eng->vj_said[strip][0] = '\0';
	s->auto_fade_active = false;
	s->auto_fade_is_reset_fx = false;
	mfd_engine_detach_filter_binding_locked(eng, k);
	s->enabled = true;
	s->source_b_name[0] = '\0';
	s->resolved_scene_name[0] = '\0';
	clear_slot_filter_fields(s);
	s->audio_follows_video = false;
	if (!source) {
		s->scene_name[0] = '\0';
		s->source_a_name[0] = '\0';
		mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
		return true;
	}
	snprintf(s->scene_name, sizeof(s->scene_name), "%s", scene);
	snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", source);
	mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_UNASSIGNED);
	mfd_engine_reset_slot_mode_state(eng, s, MFD_TARGET_SINGLE_LAYER);
	float live = 1.0f;
	if (vj_live_opacity(eng, source, &live)) {
		vj_start_strip_at_locked(eng, s, live);
	}
	return true;
}

/* Whether a strip that does not follow the scene already drives this source as a layer. */
static bool vj_source_kept_locked(const struct mfd_engine *eng, const bool *following, const char *name)
{
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		if ((i >= 1 && i <= (int)MFD_VJ_STRIPS && following[i]) || !s->enabled ||
		    (s->target_type != MFD_TARGET_SINGLE_LAYER && s->target_type != MFD_TARGET_TWO_SOURCE_MIX)) {
			continue;
		}
		if (strcmp(s->source_a_name, name) == 0 || strcmp(s->source_b_name, name) == 0) {
			return true;
		}
	}
	return false;
}

static int vj_fill_locked(struct mfd_engine *eng, const char *scene_arg, bool include_empty, bool replace_layers,
			  enum mfd_fill_status *status, char *out_scene, size_t out_scene_size, bool *changed)
{
	char scene[MFD_SOURCE_NAME_MAX];
	snprintf(scene, sizeof(scene), "%s", scene_arg && scene_arg[0] != '\0' ? scene_arg : eng->cached_program_scene);
	if (out_scene && out_scene_size > 0U) {
		snprintf(out_scene, out_scene_size, "%s", scene);
	}
	struct vj_layer_list *layers = calloc(1, sizeof(*layers));
	if (!layers) {
		*status = MFD_FILL_NO_SCENE;
		return 0;
	}
	if (scene[0] == '\0' || !vj_collect_layers(eng, scene, layers)) {
		*status = MFD_FILL_NO_SCENE;
		free(layers);
		return 0;
	}
	enum mfd_controller_profile prof = mfd_engine_get_active_profile(eng);
	bool following[MFD_VJ_STRIPS + 1] = {false};
	int eligible = 0;
	for (int k = 1; k <= (int)MFD_VJ_STRIPS; ++k) {
		const struct mfd_fader_slot *s = &eng->fader_slots[k];
		if (!mfd_profile_has_physical_slot(prof, k) ||
		    (eng->setup_session.active && eng->setup_session.slot_index == k)) {
			continue;
		}
		if (!s->enabled) {
			continue; /* switched off by the user: left alone */
		}
		if (s->auto_follow || (include_empty && s->target_type == MFD_TARGET_UNASSIGNED) ||
		    (replace_layers && s->target_type == MFD_TARGET_SINGLE_LAYER)) {
			following[k] = true;
			++eligible;
		}
	}
	if (eligible == 0) {
		*status = MFD_FILL_NO_EMPTY_STRIPS;
		free(layers);
		return 0;
	}
	int next = 0, placed = 0;
	for (int k = 1; k <= (int)MFD_VJ_STRIPS; ++k) {
		if (!following[k]) {
			continue;
		}
		while (next < layers->count && vj_source_kept_locked(eng, following, layers->names[next])) {
			++next;
		}
		const char *pick = next < layers->count ? layers->names[next++] : NULL;
		if (vj_point_strip_locked(eng, k, scene, pick)) {
			*changed = true;
		}
		if (pick) {
			++placed;
		}
	}
	free(layers);
	*status = placed > 0 ? MFD_FILL_OK : MFD_FILL_NO_VIDEO_LAYERS;
	return placed;
}

int mfd_engine_fill_strips_from_scene(struct mfd_engine *eng, const char *scene, bool include_empty,
				      bool replace_layers, enum mfd_fill_status *out_status, char *out_scene,
				      size_t out_scene_size)
{
	enum mfd_fill_status st = MFD_FILL_NO_SCENE;
	if (!eng) {
		if (out_status) {
			*out_status = st;
		}
		return 0;
	}
	mfd_engine_lock(eng);
	bool changed = false;
	int placed =
		vj_fill_locked(eng, scene, include_empty, replace_layers, &st, out_scene, out_scene_size, &changed);
	vj_hold_epoch_sync_locked(eng); /* a strip that moved let go of its hold */
	if (out_status) {
		*out_status = st;
	}
	if (!changed) {
		mfd_engine_unlock(eng);
		return placed;
	}
	mfd_engine_bump_slot_rev(eng, -1);
	/* The re-check of the strips this fill just set asks for no follow of its own: a fill from a named scene would
	 * be put straight back to the scene on air. A follow already asked for (a scene change) still runs. */
	bool follow_was = eng->vj_follow_pending;
	mfd_engine_reevaluate_slots(eng, false);
	eng->vj_follow_pending = follow_was;
	vj_refresh_locked(eng);
	mfd_engine_notify_state_changed(eng);
	if (obs_initialized()) {
		eng->pending_config_save = true; /* saved by the UI thread, never from obs-websocket's */
	}
	mfd_engine_unlock(eng);
	return placed;
}

/* The first start with an APC40: eight empty strips fill from the scene on air, once. */
static void vj_try_autofill(struct mfd_engine *eng)
{
	mfd_engine_lock(eng);
	bool go = eng->frontend_finished_loading && vj_active(eng) && !eng->vj_autofill_done;
	bool set_up = false;
	for (int k = 1; go && k <= (int)MFD_VJ_STRIPS; ++k) {
		const struct mfd_fader_slot *s = &eng->fader_slots[k];
		if (s->target_type != MFD_TARGET_UNASSIGNED || s->auto_follow) {
			set_up = true; /* someone has set the strips up already */
		}
	}
	if (go && set_up) {
		/* Never fill them later, even if every strip is emptied by hand one day. */
		eng->vj_autofill_done = true;
		if (obs_initialized()) {
			eng->pending_config_save = true;
		}
		go = false;
	}
	mfd_engine_unlock(eng);
	if (!go) {
		return;
	}
	enum mfd_fill_status st = MFD_FILL_NO_SCENE;
	char scene[MFD_SOURCE_NAME_MAX] = {0};
	int n = mfd_engine_fill_strips_from_scene(eng, NULL, true, false, &st, scene, sizeof(scene));
	if (n > 0) {
		mfd_engine_lock(eng);
		eng->vj_autofill_done = true;
		obs_log(LOG_INFO, "APC40: faders 1-8 filled from %s (%d sources); they follow the scene on air", scene,
			n);
		if (obs_initialized()) {
			eng->pending_config_save = true;
		}
		mfd_engine_mark_ui_dirty(eng, true);
		mfd_engine_unlock(eng);
	}
}

/* Program changed: strips that follow the scene move to its sources, after any crossfader throw. */
static void vj_follow_program(struct mfd_engine *eng)
{
	mfd_engine_lock(eng);
	bool any = false;
	for (int k = 1; k <= (int)MFD_VJ_STRIPS && vj_active(eng); ++k) {
		any = any || eng->fader_slots[k].auto_follow;
	}
	bool later = any && vj_tbar_busy_locked(eng);
	if (later) {
		eng->vj_follow_pending = true;
	}
	mfd_engine_unlock(eng);
	if (any && !later) {
		mfd_engine_fill_strips_from_scene(eng, NULL, false, false, NULL, NULL, 0U);
	}
}

struct vj_set_visible {
	const char *source;
	bool visible;
};

/* Every copy of the source in the scene, as the eye is read from the topmost copy. */
static bool vj_set_visible_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct vj_set_visible *v = param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = src ? obs_source_get_name(src) : NULL;
	if (name && strcmp(name, v->source) == 0) {
		obs_sceneitem_set_visible(item, v->visible);
	}
	return true;
}

/* Carries out a desk command in the UI thread, with no lock held. */
static void vj_run_command(struct mfd_engine *eng, const struct mfd_vj_command *cmd)
{
	if (eng->scene_graph_ops) {
		if (eng->scene_graph_ops->run_vj_command) {
			eng->scene_graph_ops->run_vj_command(cmd, eng->scene_graph_user_data);
		}
		return;
	}
	if (!obs_initialized()) {
		return;
	}
	switch (cmd->kind) {
	case MFD_VJ_CMD_PREVIEW:
	case MFD_VJ_CMD_PROGRAM: {
		/* checked again here: by now OBS may have put this scene on air (a fast double press, a click in OBS) */
		obs_source_t *on_air = obs_frontend_get_current_scene();
		bool already = on_air && strcmp(obs_source_get_name(on_air), cmd->scene) == 0;
		obs_source_release(on_air);
		if (already) {
			obs_log(LOG_INFO, "APC40: \"%s\" is on air already, so it is left as it is", cmd->scene);
			break;
		}
		obs_source_t *src = obs_get_source_by_name(cmd->scene);
		if (src) {
			if (cmd->kind == MFD_VJ_CMD_PREVIEW) {
				obs_frontend_set_current_preview_scene(src);
			} else {
				obs_frontend_set_current_scene(src); /* with the current transition */
			}
			obs_source_release(src);
		}
		break;
	}
	case MFD_VJ_CMD_TAKE: {
		/* Only the scene the press meant goes on air: if Preview holds another by now, that scene goes to
		 * Preview instead, and nothing unexpected reaches the audience. */
		obs_source_t *on_air = obs_frontend_get_current_scene();
		bool already = on_air && cmd->scene[0] != '\0' && strcmp(obs_source_get_name(on_air), cmd->scene) == 0;
		obs_source_release(on_air);
		if (already) { /* a second fast press: the first one has put it on air already */
			obs_log(LOG_INFO, "APC40: \"%s\" is on air already, so there is nothing to fade to",
				cmd->scene);
			break;
		}
		obs_source_t *prev = obs_frontend_get_current_preview_scene();
		bool meant = cmd->scene[0] == '\0' || (prev && strcmp(obs_source_get_name(prev), cmd->scene) == 0);
		obs_source_release(prev);
		if (meant) {
			obs_frontend_preview_program_trigger_transition();
			break;
		}
		obs_source_t *want = obs_get_source_by_name(cmd->scene);
		if (want) {
			obs_log(LOG_INFO, "APC40: Preview changed before the take, so \"%s\" goes to Preview instead",
				cmd->scene);
			obs_frontend_set_current_preview_scene(want);
			obs_source_release(want);
		}
		break;
	}
	case MFD_VJ_CMD_SCENE_FADE: {
		obs_source_t *on_air = obs_frontend_get_current_scene();
		bool already = on_air && strcmp(obs_source_get_name(on_air), cmd->scene) == 0;
		obs_source_release(on_air);
		obs_source_t *prev = obs_frontend_get_current_preview_scene();
		bool meant = prev && strcmp(obs_source_get_name(prev), cmd->scene) == 0;
		obs_source_release(prev);
		if (already || !meant) {
			break; /* on air already, or Preview holds another scene by now: nothing unexpected goes out */
		}
		double bpm = VS_TEMPO_DEFAULT_BPM; /* Visual Sidechain's tempo when it knows one */
		proc_handler_t *ph = obs_get_proc_handler();
		if (ph) {
			calldata_t cd;
			calldata_init(&cd);
			if (proc_handler_call(ph, "visual_sidechain_get_signals", &cd) && calldata_bool(&cd, "ok")) {
				double b = calldata_float(&cd, "bpm");
				if (isfinite(b) && b >= VS_TEMPO_MIN_BPM && b <= VS_TEMPO_MAX_BPM) {
					bpm = b;
				}
			}
			calldata_free(&cd);
		}
		int ms = 50; /* a cut: as short as OBS allows */
		if (cmd->arg >= 1 && cmd->arg < (int)MFD_VJ_STRIPS) {
			double beats = 32.0 / (double)(1 << (cmd->arg - 1)); /* pad 1: 8 bars .. pad 7: 1/2 beat */
			ms = (int)lround(beats * 60000.0 / bpm);
		}
		ms = ms < 50 ? 50 : ms > 20000 ? 20000 : ms; /* OBS's own limits */
		/* the user's transition length is theirs: this fade's length is set, the fade starts with it (OBS keeps
		 * the length a transition started with), then the user's length is put back */
		int user_ms = obs_frontend_get_transition_duration();
		obs_frontend_set_transition_duration(ms);
		obs_frontend_preview_program_trigger_transition();
		obs_frontend_set_transition_duration(user_ms);
		obs_log(LOG_INFO, "APC40: scene fade to \"%s\" over %d ms (%s, %.0f BPM)", cmd->scene, ms,
			cmd->arg < 0 ? "cut" : "top row", bpm);
		break;
	}
	case MFD_VJ_CMD_SET_VISIBLE: {
		obs_source_t *scene_src = obs_get_source_by_name(cmd->scene);
		obs_scene_t *sc = scene_src ? obs_scene_from_source(scene_src) : NULL;
		if (sc) {
			struct vj_set_visible set = {cmd->source, cmd->arg != 0};
			obs_scene_enum_items(sc, vj_set_visible_cb, &set);
			obs_source_t *copy =
				vj_program_copy_of(scene_src); /* the copy on air, see vj_program_copy_of */
			if (copy) {
				obs_scene_enum_items(obs_scene_from_source(copy), vj_set_visible_cb, &set);
				obs_source_release(copy);
			}
		}
		obs_source_release(scene_src);
		break;
	}
	case MFD_VJ_CMD_TRANSITION: {
		int ms = obs_frontend_get_transition_duration() + cmd->arg;
		ms = ms < 50 ? 50 : ms > 20000 ? 20000 : ms;
		obs_frontend_set_transition_duration(ms);
		break;
	}
	case MFD_VJ_CMD_VS_PANIC:
	case MFD_VJ_CMD_VS_TAP:
	case MFD_VJ_CMD_VS_LOCK:
	case MFD_VJ_CMD_VS_RESYNC: {
		bool panic = false;
		bool answered = mfd_vendor_call_visual_sidechain_desk(cmd->kind == MFD_VJ_CMD_VS_PANIC ? "panic"
								      : cmd->kind == MFD_VJ_CMD_VS_TAP ? "tap_tempo"
								      : cmd->kind == MFD_VJ_CMD_VS_LOCK
									      ? "lock_bpm"
									      : "resync_downbeat",
								      &panic);
		mfd_engine_vs_result(eng, false, cmd->kind, answered, panic);
		break;
	}
	default:
		break;
	}
}

void mfd_engine_set_test_scene_list(struct mfd_engine *eng, const char *const *names, int count)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->scene_list_override_set = true;
	eng->cached_scene_count = 0;
	for (int i = 0; names && i < count && i < (int)MFD_VJ_SCENES; ++i) {
		snprintf(eng->cached_scenes[i], sizeof(eng->cached_scenes[i]), "%s", names[i] ? names[i] : "");
		eng->cached_scene_count = i + 1;
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_vs_result(struct mfd_engine *eng, bool from_poll, enum mfd_vj_command_kind kind, bool answered,
			  bool panic)
{
	if (!eng) {
		return;
	}
	if (!answered && !from_poll) {
		obs_log(LOG_INFO, "APC40: Visual Sidechain did not answer (is it installed, with obs-websocket?)");
	}
	mfd_engine_lock(eng);
	bool notify = false;
	if (from_poll) {
		if (answered != eng->vs_panic_known) {
			eng->vs_panic_known = answered; /* a first "not panicked" answer counts too */
			notify = true;
		}
		if (answered && eng->vs_last_failed) {
			eng->vs_last_failed = false; /* it answers again */
			notify = true;
		}
	} else {
		notify = eng->vs_last_failed != !answered;
		eng->vs_last_failed = !answered;
		if (answered && kind == MFD_VJ_CMD_VS_PANIC && !eng->vs_panic_known) {
			eng->vs_panic_known = true;
			notify = true;
		}
	}
	bool set = answered && kind == MFD_VJ_CMD_VS_PANIC && (!from_poll || panic != eng->vs_panic);
	if (notify) {
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
	if (set) {
		mfd_engine_set_vs_panic(eng, panic);
	}
}

void mfd_engine_set_vs_panic(struct mfd_engine *eng, bool panic)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->vs_panic = panic;
	eng->vj.vs_panic = panic;
	eng->vj_blink_sec = 0.0f;
	obs_log(LOG_INFO, "APC40: Visual Sidechain %s",
		panic ? "panicked (Record blinks; press again to restore)" : "restored");
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
}

/* --- Fadester Desk dock --- */

/* Whether a fader learned this MIDI note address (as vj_feed_locked's ownership check). */
static bool desk_owned_locked(const struct mfd_engine *eng, uint8_t channel, uint8_t note)
{
	struct mfd_midi_addr a = {0};
	a.type = MFD_MIDI_NOTE_ON;
	a.channel = channel;
	a.number = note;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (eng->fader_slots[i].enabled && mfd_midi_addr_matches(&eng->fader_slots[i].addr, &a)) {
			return true;
		}
	}
	return false;
}

void mfd_engine_get_desk_view(struct mfd_engine *eng, bool dock_shift, struct mfd_desk_view *out)
{
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	out->finished_loading = eng->frontend_finished_loading;
	out->desk_connected = eng->midi_dev.is_open && mfd_is_apc40_mk2_name(eng->midi_dev.opened_name);
	out->pads = vj_active(eng)         ? MFD_DESK_PADS_ON
		    : eng->vj_disabled     ? MFD_DESK_PADS_BUTTONS_OFF
		    : !out->desk_connected ? MFD_DESK_PADS_NO_APC40
					   : MFD_DESK_PADS_OTHER_PROFILE;
	out->learn_or_setup = eng->setup_session.active || eng->learning_slot_index >= 0;
	out->studio_mode = mfd_engine_is_studio_mode_active(eng);
	out->take_ready = out->studio_mode && eng->cached_preview_scene[0] != '\0' &&
			  strcmp(eng->cached_preview_scene, eng->cached_program_scene) != 0;
	out->tbar_busy = vj_tbar_busy_locked(eng);
	out->desk_shift = eng->vj.shift;
	out->fade_ms = mfd_engine_vj_fade_ms(eng);
	snprintf(out->program, sizeof(out->program), "%s", eng->cached_program_scene);
	snprintf(out->preview, sizeof(out->preview), "%s", out->studio_mode ? eng->cached_preview_scene : "");
	out->scene_count = eng->cached_scene_count < (int)MFD_VJ_SCENES ? eng->cached_scene_count : (int)MFD_VJ_SCENES;
	struct mfd_vj_pads pads = eng->vj; /* a copy: the dock's Shift changes only what it is shown */
	pads.shift = pads.shift || dock_shift;
	for (int j = 0; j < (int)MFD_VJ_SCENES; ++j) {
		enum mfd_vj_scene_state st = MFD_VJ_SCENE_NONE;
		if (j < out->scene_count) {
			snprintf(out->scenes[j], sizeof(out->scenes[j]), "%s", eng->cached_scenes[j]);
			st = !strcmp(eng->cached_scenes[j], eng->cached_program_scene) ? MFD_VJ_SCENE_PROGRAM
			     : out->studio_mode && !strcmp(eng->cached_scenes[j], eng->cached_preview_scene)
				     ? MFD_VJ_SCENE_PREVIEW
				     : MFD_VJ_SCENE_IDLE;
		}
		pads.scene[j] = st; /* worked out here: the pads' own copy is only kept while they are on */
		out->scene_owned[j] = desk_owned_locked(eng, 0U, (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)j));
	}
	pads.take_ready = out->take_ready;
	pads.vs_panic = eng->vj.vs_panic;
	out->stop_all_owned = desk_owned_locked(eng, 0U, MFD_VJ_NOTE_STOP_ALL);
	out->session_owned = desk_owned_locked(eng, 0U, MFD_VJ_NOTE_SESSION);
	static const uint8_t k_rows[MFD_DESK_CLIP_STOP] = {MFD_VJ_NOTE_ROW_BLEND, MFD_VJ_NOTE_ROW_FLASH,
							   MFD_VJ_NOTE_ROW_FADE, MFD_VJ_NOTE_ROW_SHOW,
							   MFD_VJ_NOTE_ROW_RESTART};
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		const struct mfd_fader_slot *sl = &eng->fader_slots[i + 1];
		struct mfd_desk_strip *st = &out->strips[i];
		const char *scene = NULL, *source = NULL;
		/* from the strip itself: the pads' bits are stale while the pads are off */
		st->has_layer = vj_strip_layer(eng, i, &scene, &source);
		if (st->has_layer) {
			snprintf(st->scene, sizeof(st->scene), "%s", scene);
			snprintf(st->source, sizeof(st->source), "%s", source);
		} else if (sl->enabled && sl->target_type != MFD_TARGET_UNASSIGNED) {
			snprintf(st->label, sizeof(st->label), "%s",
				 sl->source_a_name); /* audio or a filter's source */
		}
		if (out->pads != MFD_DESK_PADS_ON) {
			pads.has_layer = (uint8_t)(st->has_layer ? pads.has_layer | (1U << (unsigned)i)
								 : pads.has_layer & ~(1U << (unsigned)i));
		}
		bool layer_fader = st->has_layer && sl->enabled && sl->target_type == MFD_TARGET_SINGLE_LAYER;
		st->single_layer = layer_fader && mfd_engine_is_slot_writable(eng, sl); /* as vj_fadeable */
		st->source_missing = layer_fader && !st->single_layer;
		st->level = sl->last_applied_val;
		/* as the settings dialog's strip card (mfd_ui_format_strip_presentation): only once the fader's position
		 * has been seen */
		st->pickup = sl->enabled && sl->target_type != MFD_TARGET_UNASSIGNED && sl->takeover.has_last_fader &&
			     sl->takeover.state == MFD_TAKEOVER_WAITING && sl->takeover.hint != MFD_HINT_NONE;
		if (st->pickup) {
			bool rev = sl->takeover.polarity_reversed;
			st->pickup_target = rev ? 1.0f - sl->takeover.target_obs : sl->takeover.target_obs;
			st->pickup_up = rev ? sl->takeover.hint == MFD_HINT_MOVE_DOWN
					    : sl->takeover.hint == MFD_HINT_MOVE_UP;
		}
		st->follows = sl->auto_follow;
		/* Shift + top pad: pins a strip on a single layer, unpins a pinned one (even empty) (vj_run_locked). */
		st->can_pin_toggle = (sl->enabled && sl->target_type == MFD_TARGET_SINGLE_LAYER &&
				      (!sl->auto_follow || st->has_layer)) ||
				     (!sl->auto_follow && sl->target_type == MFD_TARGET_UNASSIGNED);
		for (int c = 0; c < (int)MFD_DESK_CLIP_STOP; ++c) {
			st->owned[c] = desk_owned_locked(eng, 0U, (uint8_t)(k_rows[c] + (unsigned)i));
		}
		st->owned[MFD_DESK_CLIP_STOP] = desk_owned_locked(eng, (uint8_t)i, MFD_VJ_NOTE_CLIP_STOP);
	}
	out->led_count = mfd_vj_pads_leds(&pads, out->leds, MFD_VJ_MAX_LEDS);
	out->crossfader = eng->fader_slots[0].last_applied_val;
	out->vs_panic = eng->vs_panic;
	out->blackout = mfd_blackout_is_on();
	out->blackout_failure = (int)mfd_blackout_last_failure();
	out->video_controls = eng->desk_video_controls;
	out->vs_panic_known = eng->vs_panic_known;
	out->vs_last_failed = eng->vs_last_failed;
	mfd_engine_unlock(eng);
}

enum mfd_desk_press_result mfd_engine_desk_press(struct mfd_engine *eng, uint8_t channel, uint8_t note, bool on,
						 bool shift, uint32_t *hold_epoch)
{
	if (!eng || note == MFD_VJ_NOTE_SHIFT || note > 127U || channel > 15U ||
	    !mfd_vj_pads_is_button(channel, note)) {
		return MFD_DESK_REJECTED;
	}
	mfd_engine_lock(eng);
	vj_hold_epoch_sync_locked(eng); /* holds the desk changed since the last tick */
	int strip = note >= MFD_VJ_NOTE_ROW_FLASH && note < MFD_VJ_NOTE_ROW_FLASH + MFD_VJ_STRIPS && channel == 0U
			    ? (int)(note - MFD_VJ_NOTE_ROW_FLASH)
			    : -1;
	uint8_t held_before = (uint8_t)(eng->vj.flashing | eng->vj.blackout);
	if (!on && strip >= 0) {
		/* only the hold this dock press started: not one the desk started since */
		bool same = hold_epoch && (held_before & (1U << (unsigned)strip)) &&
			    eng->vj_hold_epoch[strip] == *hold_epoch;
		if (!same) {
			mfd_engine_unlock(eng);
			return MFD_DESK_REJECTED;
		}
	}
	struct mfd_midi_decoded dec;
	memset(&dec, 0, sizeof(dec));
	dec.addr.type = on ? MFD_MIDI_NOTE_ON : MFD_MIDI_NOTE_OFF;
	dec.addr.channel = channel;
	dec.addr.number = note;
	dec.raw_value = on ? 127U : 0U;
	dec.normalized = on ? 1.0f : 0.0f;
	bool desk_shift = eng->vj.shift;
	uint32_t drops = eng->vj_cmd_drops;
	eng->vj.shift = shift || desk_shift; /* Shift held on the desk shifts a dock click too, as the view shows */
	bool fed = vj_feed_locked(eng, &dec) && eng->vj_cmd_drops == drops; /* a dropped command did nothing */
	eng->vj.shift = desk_shift; /* the desk's own Shift, whatever the dock had */
	vj_hold_epoch_sync_locked(eng);
	enum mfd_desk_press_result r = fed ? MFD_DESK_ACCEPTED : MFD_DESK_REJECTED;
	if (fed && on && strip >= 0 && !(held_before & (1U << (unsigned)strip)) &&
	    ((eng->vj.flashing | eng->vj.blackout) & (1U << (unsigned)strip))) {
		r = MFD_DESK_HOLD_STARTED;
		if (hold_epoch) {
			*hold_epoch = eng->vj_hold_epoch[strip];
		}
	}
	mfd_engine_unlock(eng);
	return r;
}

bool mfd_engine_desk_vs(struct mfd_engine *eng, enum mfd_vj_command_kind kind)
{
	if (!eng || (kind != MFD_VJ_CMD_VS_PANIC && kind != MFD_VJ_CMD_VS_TAP && kind != MFD_VJ_CMD_VS_LOCK &&
		     kind != MFD_VJ_CMD_VS_RESYNC)) {
		return false;
	}
	mfd_engine_lock(eng);
	bool ok = vj_queue_cmd_locked(eng, kind, NULL, NULL, 0);
	mfd_engine_unlock(eng);
	return ok;
}

enum mfd_desk_press_result mfd_engine_desk_scene(struct mfd_engine *eng, int scene, bool direct)
{
	if (!eng || scene < 0 || scene >= (int)MFD_VJ_SCENES) {
		return MFD_DESK_REJECTED;
	}
	mfd_engine_lock(eng);
	if (!eng->frontend_finished_loading) { /* on every path, the pads' own too (QA-S01) */
		mfd_engine_unlock(eng);
		return MFD_DESK_REJECTED;
	}
	bool pads_act = vj_active(eng) && !eng->setup_session.active && eng->learning_slot_index < 0;
	if (pads_act) {
		/* the desk's own button: Shift held on the desk, a strip that learned it and its light all as before */
		mfd_engine_unlock(eng);
		return mfd_engine_desk_press(eng, 0U, (uint8_t)(MFD_VJ_NOTE_SCENE + (unsigned)scene), true, direct,
					     NULL);
	}
	enum mfd_desk_press_result r = MFD_DESK_REJECTED;
	if (scene < eng->cached_scene_count && !vj_tbar_busy_locked(eng)) {
		uint32_t drops = eng->vj_cmd_drops;
		vj_scene_locked(eng, scene, direct);
		if (eng->vj_cmd_drops == drops) { /* a dropped command did nothing */
			r = MFD_DESK_ACCEPTED;
		}
		mfd_engine_mark_ui_dirty(eng, false); /* under the lock: the unlock wakes the UI thread */
	}
	mfd_engine_unlock(eng);
	return r;
}

void mfd_engine_desk_fade(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	vj_take_locked(eng);
	mfd_engine_mark_ui_dirty(eng, false); /* under the lock: the unlock wakes the UI thread */
	mfd_engine_unlock(eng);
}

void mfd_engine_desk_blackout(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	mfd_blackout_toggle(eng->cached_transition_ms); /* lock order: engine, then the blackout's own */
	eng->vj_state_dirty = true;
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
}

void mfd_engine_desk_vjster(struct mfd_engine *eng, int steps, bool reset, bool play_pause)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (reset) {
		eng->vj_speed_steps = 0; /* as Tap Tempo: steps and a preset waiting before it are dropped */
		eng->vj_speed_actions = (eng->vj_speed_actions & ~MFD_VJ_SPEED_SET_BIT) | MFD_VJ_SPEED_RESET_BIT;
	}
	if (play_pause) {
		eng->vj_speed_actions |= MFD_VJ_SPEED_PLAY_BIT;
	}
	if (steps != 0 && steps >= -200 && steps <= 200) {
		int sum = eng->vj_speed_steps + steps; /* bounded like the APC40's Tempo knob */
		eng->vj_speed_steps = sum < -MFD_VJ_SPEED_MAX_STEPS  ? -MFD_VJ_SPEED_MAX_STEPS
				      : sum > MFD_VJ_SPEED_MAX_STEPS ? MFD_VJ_SPEED_MAX_STEPS
								     : sum;
	}
	mfd_engine_unlock(eng); /* the unlock sends them (mfd_engine_unlock), never with the lock held */
}

void mfd_engine_desk_vjster_rate(struct mfd_engine *eng, double rate)
{
	if (!eng || !(rate >= 0.25 && rate <= 4.0)) {
		return;
	}
	mfd_engine_lock(eng);
	eng->vj_speed_steps = 0; /* the preset is where the video should be: steps and a 1x before it are overtaken */
	eng->vj_speed_actions = (eng->vj_speed_actions & ~MFD_VJ_SPEED_RESET_BIT) | MFD_VJ_SPEED_SET_BIT;
	eng->vj_speed_set_rate = rate;
	mfd_engine_unlock(eng); /* sent at the unlock, with no lock held */
}

void mfd_engine_set_desk_visible(struct mfd_engine *eng, bool visible)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	if (visible && !eng->desk_visible) {
		eng->vj_vs_poll_pending = true; /* the panic state at once, not in up to 2 s */
		eng->vj_vs_poll_sec = 0.0f;
		mfd_engine_mark_ui_dirty(eng, false);
	}
	eng->desk_visible = visible;
	mfd_engine_unlock(eng);
}

void mfd_engine_tick(struct mfd_engine *eng, float seconds)
{
	if (!eng) {
		return;
	}

	/* The video tick runs under libobs's draw-callbacks lock only, never a source or scene lock. */
	mfd_engine_process_scene_graph_changes(eng);

	mfd_engine_lock(eng);

	if (midi_closed_unexpectedly_locked(eng)) {
		bool first = !eng->midi_disconnect_handled;
		abort_open_tbar_slots_on_midi_disconnect_locked(eng);
		if (first) {
			vj_release_all_locked(eng);
		}
		mfd_engine_mark_ui_dirty(eng, true);
	}
	{
		/* The pads stopped applying (profile changed, the desk changed): lights off, held pads let go. */
		bool active = vj_active(eng);
		if (eng->vj_was_active && !active) {
			vj_leds_off_locked(eng);
			vj_release_all_locked(eng);
			vj_restore_all_punches_locked(eng); /* nothing can put them back while the pads are off */
			eng->vj_follow_pending = false;
		} else if (!eng->vj_was_active && active) {
			/* The pads start applying (the desk opened, the pads turned on): its LEDs are unknown, repaint all.
			 * Empty strips may fill from the scene on air now (in the UI thread). */
			memset(eng->vj_led_sent, 255, sizeof(eng->vj_led_sent));
			eng->vj_refresh_pending = true;
			if (eng->frontend_finished_loading && !eng->vj_autofill_done) {
				eng->vj_autofill_pending = true;
			}
			eng->vj_follow_pending = true;  /* Program may have changed while the pads were off */
			eng->vj_vs_poll_pending = true; /* Record lights from Visual Sidechain's panic state at once */
			eng->vj_vs_poll_sec = 0.0f;
			mfd_engine_mark_ui_dirty(eng, false);
		}
		eng->vj_was_active = active;
	}
	if (eng->vj_refresh_pending && vj_active(eng)) {
		eng->vj_refresh_pending = false;
		vj_refresh_locked(eng);
	} else if (vj_active(eng)) {
		vj_status_locked(eng, false);
	}
	if (vj_active(eng) || eng->desk_visible) {
		eng->vj_vs_poll_sec += seconds;
		if (eng->vj_vs_poll_sec >= 2.0f) {
			eng->vj_vs_poll_sec = 0.0f;
			eng->vj_vs_poll_pending = true; /* a panic from a hotkey or a script lights Record too */
			mfd_engine_mark_ui_dirty(eng, false);
		}
	}
	eng->vj_blink_sec += seconds;
	if (eng->vj_blink_sec >= 3600.0f) {
		eng->vj_blink_sec -= 3600.0f; /* a whole number of blinks: no jump in the phase */
	}
	if (eng->vj_follow_pending && !vj_tbar_busy_locked(eng)) {
		mfd_engine_mark_ui_dirty(eng, false); /* the throw is over: let the UI thread move the strips */
	}
	sync_volume_slots_from_live_locked(eng);

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		eng->fader_slots[i].sec_since_last_fader_event += seconds;
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->mix_sent_ns && os_gettime_ns() - s->mix_sent_ns > 10000000000ULL) {
			s->mix_sent_ns = 0;
			s->mix_request_id[0] = '\0';
			s->mix_delivery = MFD_MIX_NO_REPLY;
			eng->mix_provider_id[0] = '\0';
			eng->last_mix_sent_id[0] = '\0';
			eng->last_mix_sent_ns = 0;
			eng->mix_provider_conflict = false;
			mfd_engine_mark_ui_dirty(eng, false);
		}
	}

	sync_parser_lsb_independence_locked(eng);
	vj_hold_epoch_sync_locked(eng); /* holds the UI thread let go of since the last tick, before the desk presses */
	struct mfd_midi_raw_event raw;
	while (mfd_midi_queue_pop(&eng->midi_queue, &raw)) {
		struct mfd_midi_decoded dec;
		if (!mfd_midi_parser_feed_at(&eng->midi_parser, raw.raw_msg, raw.timestamp_ms, &dec)) {
			continue;
		}
		eng->last_midi_decoded = dec;
		eng->has_last_midi = true;
		mfd_engine_mark_ui_dirty(eng, false);

		struct mfd_midi_addr learned;
		bool got_learned = mfd_midi_parser_pop_learned(&eng->midi_parser, &learned);
		if (got_learned && apc40_relative_knob(eng, &learned)) {
			/* The APC40's Tempo and Cue Level knobs are endless and send steps, not a position: bound to a
			 * strip they would throw it between 0 and 100 %. Keep listening for another control. */
			mfd_midi_parser_start_learn(&eng->midi_parser);
		} else if (got_learned && eng->setup_session.active && eng->setup_session.learning &&
			   eng->setup_session.learn_auto &&
			   !setup_auto_learn_take_locked(eng, &learned, (int)dec.raw_value)) {
			mfd_midi_parser_start_learn(
				&eng->midi_parser); /* not a control being moved yet: keep listening */
		} else if (got_learned) {
			if (eng->setup_session.active && eng->setup_session.learning) {
				eng->setup_session.learn_auto = false;
				eng->setup_session.candidate_addr = learned;
				mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), &learned,
									&eng->setup_session.candidate_cal);
				eng->setup_session.learning = false;
				mfd_engine_mark_ui_dirty(eng, true);
			} else if (eng->learning_slot_index >= 0 && eng->learning_slot_index < MFD_MAX_FADER_SLOTS) {
				int lidx = eng->learning_slot_index;
				struct mfd_fader_slot *ls = &eng->fader_slots[lidx];
				ls->addr = learned;
				/* One control drives one strip: take the address away from any other strip, as Set up does. */
				for (int j = 0; j < MFD_MAX_FADER_SLOTS; ++j) {
					if (j != lidx && midi_addr_exact_equal(&eng->fader_slots[j].addr, &learned)) {
						eng->fader_slots[j].addr.type = MFD_MIDI_NONE;
						mfd_engine_bump_slot_rev(eng, j);
					}
				}
				mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), &ls->addr,
									&ls->cal);
				ls->enabled = true;
				eng->learning_slot_index = -1;
				mfd_engine_bump_slot_rev(eng, lidx);
				if (obs_initialized()) {
					eng->pending_config_save = true;
				}
				obs_log(LOG_INFO, "MIDI Learn bound slot '%s' to Ch %u Type %d Num %u", ls->id,
					(unsigned)(learned.channel + 1U), (int)learned.type, (unsigned)learned.number);
				mfd_engine_notify_state_changed(eng);
			}
			eng->learning_slot_index = -1;
		}

		bool vj_fed = vj_feed_locked(eng, &dec);
		vj_hold_epoch_sync_locked(eng);
		if (vj_fed) {
			continue;
		}

		if (dec.addr.type == MFD_MIDI_NOTE_OFF) {
			continue;
		}

		/* Track last-moved strip for Zone 3 status line */
		int matched_slot = -1;
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			if (eng->fader_slots[i].enabled &&
			    mfd_midi_addr_matches(&eng->fader_slots[i].addr, &dec.addr)) {
				matched_slot = i;
				break;
			}
		}
		eng->last_moved_slot_index = matched_slot;

		/* Update Step 3 candidate meter if setup session is active */
		if (eng->setup_session.active && mfd_midi_addr_matches(&eng->setup_session.candidate_addr, &dec.addr)) {
			eng->setup_session.candidate_raw_norm = normalize_decoded_sample(
				eng, &eng->setup_session.candidate_addr, &eng->setup_session.candidate_cal,
				eng->setup_session.candidate_invert, &dec);
			eng->setup_session.has_candidate_input = true;
			mfd_engine_mark_ui_dirty(eng, false);
		}

		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *slot = &eng->fader_slots[i];
			if (!slot->enabled) {
				continue;
			}
			if (!mfd_midi_addr_matches(&slot->addr, &dec.addr)) {
				continue;
			}

			float norm = normalize_decoded_sample(eng, &slot->addr, &slot->cal, slot->invert, &dec);
			slot->last_raw_norm = norm;
			mfd_engine_mark_ui_dirty(eng, false);

			if (eng->setup_session.active && eng->setup_session.slot_index == i) {
				/* Broadcast-safe preview: live OBS writes paused while Set up is editing this strip */
				continue;
			}
			if (!mfd_engine_is_slot_writable(eng, slot)) {
				continue;
			}

			if (slot->target_type == MFD_TARGET_MIX_MASTER) {
				/* Absolute master: report calibrated travel, independent of a source's pickup value. */
				slot->auto_fade_active = false;
				mfd_takeover_unpark_from_api(&slot->takeover, norm);
				mfd_smoother_reset(&slot->smoother, norm);
				mfd_engine_apply_slot_output(eng, slot, norm);
				mfd_engine_notify_fader_changed(eng, i, "midi");
				continue;
			}
			float effective = slot->last_applied_val;
			bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED) ||
					  (slot->target_type == MFD_TARGET_TBAR && slot->tbar_state == MFD_TBAR_PARKED);
			if (!mfd_takeover_step(&slot->takeover, norm, &effective)) {
				if (slot->target_type == MFD_TARGET_TBAR && was_parked &&
				    slot->takeover.state == MFD_TAKEOVER_WAITING) {
					slot->tbar_state = MFD_TBAR_IDLE;
					slot->tbar_midi_active = false;
					slot->tbar_from_idle_band = false;
					slot->last_applied_val = 0.0f;
					mfd_smoother_reset(&slot->smoother, 0.0f);
					mfd_engine_unlatch_tbar(eng);
					mfd_engine_notify_state_changed(eng);
				}
				continue;
			}

			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			slot->pending_clean_apply_on_avail = false;

			if (slot->target_type == MFD_TARGET_TBAR) {
				slot->tbar_midi_active = true;
				if (was_parked && slot->takeover.state != MFD_TAKEOVER_PARKED) {
					mfd_engine_unlatch_tbar(eng);
					slot->tbar_state = MFD_TBAR_THROWING;
					slot->tbar_from_idle_band = false;
				}

				bool pol_rev = slot->takeover.polarity_reversed;
				bool in_target = mfd_tbar_is_in_target_rail_zone(norm, pol_rev);
				bool in_origin = mfd_tbar_is_in_origin_rail_zone(norm, pol_rev);
				bool past_start = mfd_tbar_is_past_start_zone(norm, pol_rev);

				if (in_target) {
					if (slot->tbar_state == MFD_TBAR_IDLE) {
						mfd_engine_unlatch_tbar(eng);
					}
					apply_tbar_slot_step(eng, slot, 1.0f, false, false);
					mfd_engine_notify_fader_changed(eng, i, "midi");
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				} else if (in_origin) {
					if (slot->tbar_state == MFD_TBAR_THROWING) {
						apply_tbar_slot_step(eng, slot, 0.0f, false, false);
						mfd_engine_notify_fader_changed(eng, i, "midi");
					} else {
						slot->tbar_state = MFD_TBAR_IDLE;
						slot->tbar_from_idle_band = false;
						mfd_smoother_reset(&slot->smoother, 0.0f);
						slot->last_applied_val = 0.0f;
					}
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				} else if (slot->tbar_state == MFD_TBAR_IDLE && !past_start) {
					/*
					 * 6% origin-rail start band (R-TBAR-SM / D-8):
					 * In Idle, positions inside (0.03, 0.06] normal (CC 4..7) or [0.94, 0.97)
					 * reversed remain Idle with zero OBS writes.
					 */
					slot->tbar_from_idle_band = false;
					mfd_smoother_reset(&slot->smoother, 0.0f);
					slot->last_applied_val = 0.0f;
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				} else {
					bool started_from_idle = (slot->tbar_state == MFD_TBAR_IDLE);
					if (started_from_idle) {
						mfd_engine_unlatch_tbar(eng);
						slot->tbar_from_idle_band = true;
					}
					float t_target = slot->tbar_from_idle_band
								 ? mfd_tbar_rescale_from_start_zone(norm, pol_rev)
								 : effective;
					mfd_smoother_set_target(&slot->smoother, t_target);
					if (started_from_idle || !mfd_smoother_is_active(&slot->smoother)) {
						apply_tbar_slot_step(eng, slot, slot->smoother.current, false, false);
						mfd_engine_notify_fader_changed(eng, i, "midi");
					}
					if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
						mfd_engine_notify_state_changed(eng);
					}
				}
			} else {
				mfd_smoother_set_target(&slot->smoother, effective);
				if (!mfd_smoother_is_active(&slot->smoother)) {
					mfd_engine_apply_slot_output(eng, slot, slot->smoother.current);
					mfd_engine_notify_fader_changed(eng, i, "midi");
				}
				if (was_parked != (slot->takeover.state == MFD_TAKEOVER_PARKED)) {
					mfd_engine_notify_state_changed(eng);
				}
			}
		}
	}

	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled) {
			continue;
		}
		if (eng->setup_session.active && eng->setup_session.slot_index == i) {
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			mfd_smoother_reset(&slot->smoother, slot->last_applied_val);
			continue;
		}
		if (!mfd_engine_is_slot_writable(eng, slot)) {
			if (slot->auto_fade_active && slot->auto_fade_is_reset_fx) {
				slot->pending_clean_apply_on_avail = true;
				slot->last_applied_val = 0.0f;
				mfd_takeover_sync_external(&slot->takeover, 0.0f);
			}
			slot->auto_fade_active = false;
			slot->auto_fade_is_reset_fx = false;
			mfd_smoother_reset(&slot->smoother, slot->last_applied_val);
		} else if (slot->auto_fade_active) {
			slot->auto_fade_elapsed_sec += seconds;
			float progress = (slot->auto_fade_duration_sec > 0.0f)
						 ? clamp01(slot->auto_fade_elapsed_sec / slot->auto_fade_duration_sec)
						 : 1.0f;
			float v = slot->auto_fade_from + (slot->auto_fade_to - slot->auto_fade_from) * progress;
			mfd_smoother_reset(&slot->smoother, v);

			if (slot->target_type == MFD_TARGET_TBAR) {
				slot->tbar_midi_active = false;
				slot->tbar_from_idle_band = false;
				if (slot->auto_fade_to <= MFD_TBAR_RAIL_ZONE &&
				    (progress >= 1.0f || v <= MFD_TBAR_RAIL_ZONE)) {
					slot->auto_fade_active = false;
					slot->auto_fade_is_reset_fx = false;
					if (slot->tbar_state == MFD_TBAR_THROWING) {
						mfd_engine_apply_tbar_abort(eng, slot);
					} else {
						slot->tbar_state = MFD_TBAR_IDLE;
						slot->last_applied_val = 0.0f;
					}
					mfd_engine_notify_state_changed(eng);
				} else if (v >= (1.0f - MFD_TBAR_RAIL_ZONE - 1e-6f) ||
					   (progress >= 1.0f &&
					    slot->auto_fade_to >= (1.0f - MFD_TBAR_RAIL_ZONE - 1e-6f))) {
					if (slot->tbar_state == MFD_TBAR_IDLE) {
						mfd_engine_unlatch_tbar(eng);
					}
					mfd_engine_apply_slot_output_ex(eng, slot, 1.0f, true);
					slot->auto_fade_active = false;
					slot->auto_fade_is_reset_fx = false;
					mfd_engine_notify_state_changed(eng);
				} else {
					if (slot->tbar_state == MFD_TBAR_IDLE) {
						mfd_engine_unlatch_tbar(eng);
						slot->tbar_state = MFD_TBAR_THROWING;
					}
					mfd_engine_apply_slot_output(eng, slot, v);
					if (progress >= 1.0f) {
						slot->auto_fade_active = false;
						slot->auto_fade_is_reset_fx = false;
						mfd_takeover_unpark_from_api(&slot->takeover, v);
					}
				}
			} else {
				if (progress >= 1.0f) {
					bool was_reset_fx = slot->auto_fade_is_reset_fx;
					slot->auto_fade_active = false;
					slot->auto_fade_is_reset_fx = false;
					if (was_reset_fx) {
						slot->has_last_written_mapped = false;
						mfd_engine_apply_slot_output(eng, slot, 0.0f);
						mfd_smoother_reset(&slot->smoother, 0.0f);
						mfd_takeover_sync_external(&slot->takeover, 0.0f);
						mfd_engine_notify_state_changed(eng);
					} else if (slot->auto_fade_to >= MFD_TWO_RAIL_UPPER_PARK_THRESHOLD) {
						mfd_engine_apply_slot_output(eng, slot, slot->auto_fade_to);
						mfd_takeover_notify_rail_completed(&slot->takeover, true,
										   slot->auto_fade_to);
						mfd_smoother_reset(&slot->smoother, slot->auto_fade_to);
						mfd_engine_notify_state_changed(eng);
					} else if (slot->auto_fade_to <= MFD_TWO_RAIL_LOWER_PARK_THRESHOLD) {
						mfd_engine_apply_slot_output(eng, slot, slot->auto_fade_to);
						mfd_takeover_notify_rail_completed(&slot->takeover, false,
										   slot->auto_fade_to);
						mfd_smoother_reset(&slot->smoother, slot->auto_fade_to);
						mfd_engine_notify_state_changed(eng);
					} else {
						mfd_engine_apply_slot_output(eng, slot, v);
						mfd_takeover_unpark_from_api(&slot->takeover, v);
					}
				} else {
					mfd_engine_apply_slot_output(eng, slot, v);
				}
			}
			mfd_engine_notify_fader_changed(eng, i, "auto_fade");
		} else if (mfd_smoother_is_active(&slot->smoother)) {
			bool was_parked = (slot->takeover.state == MFD_TAKEOVER_PARKED);
			float smoothed = mfd_smoother_tick(&slot->smoother, seconds);
			mfd_engine_apply_slot_output(eng, slot, smoothed);
			if (!was_parked && slot->takeover.state == MFD_TAKEOVER_PARKED) {
				mfd_engine_notify_state_changed(eng);
			}
			mfd_engine_notify_fader_changed(eng, i, "midi");
		}

		if (slot->fader_event_pending && slot->sec_since_last_fader_event >= MFD_FADER_EVENT_MIN_INTERVAL_SEC) {
			slot->sec_since_last_fader_event = 0.0f;
			slot->fader_event_pending = false;
			eng->pending_fader_emits[i].pending = true;
			snprintf(eng->pending_fader_emits[i].slot_id, sizeof(eng->pending_fader_emits[i].slot_id), "%s",
				 slot->id);
			eng->pending_fader_emits[i].value = slot->last_applied_val;
			mfd_engine_format_slot_display_value(slot, slot->last_applied_val,
							     eng->pending_fader_emits[i].display_value,
							     sizeof(eng->pending_fader_emits[i].display_value));
			snprintf(eng->pending_fader_emits[i].origin, sizeof(eng->pending_fader_emits[i].origin), "%s",
				 slot->pending_origin);
		}
	}

	eng->mix_send_wait = fmaxf(0.0f, eng->mix_send_wait - seconds);
	int mix_count = 0;
	int mix_slots[MFD_MAX_FADER_SLOTS];
	uint64_t mix_revs[MFD_MAX_FADER_SLOTS];
	float mix_dbs[MFD_MAX_FADER_SLOTS];
	if (eng->mix_send_wait <= 0.0f) {
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			struct mfd_fader_slot *ms = &eng->fader_slots[i];
			if (!ms->mix_pending) {
				continue;
			}
			ms->mix_pending = false;
			mix_slots[mix_count] = i;
			mix_revs[mix_count] = ms->mix_pending_rev;
			mix_dbs[mix_count] = ms->mix_pending_db;
			++mix_count;
		}
		if (mix_count > 0) {
			eng->mix_send_wait = 0.05f;
		}
	}
	vj_tidy_locked(eng);
	vj_hold_epoch_sync_locked(eng);
	vj_flush_leds_locked(eng);
	if (eng->tbar_deferred && eng->tbar_deferred(false)) {
		/* A T-bar command is parked behind a transition that is still ending (C-07): the tick is the clock that
		 * asks the UI thread to try it again, about every 25 ms. No timer. */
		eng->tbar_deferred_wait_sec += seconds;
		if (eng->tbar_deferred_wait_sec >= MFD_TBAR_DEFERRED_RETRY_SEC) {
			eng->tbar_deferred_wait_sec = 0.0f;
			eng->tbar_deferred_due = true;
			eng->pending_ui_notify = true;
		}
	} else {
		eng->tbar_deferred_wait_sec = 0.0f;
	}
	if (eng->desk_visible) {
		/* The dock repaints on OBS's frame clock (no timers): about 30 times a second, for its pulsing pads and
		 * Visual Sidechain's beat. */
		eng->desk_frame_sec += seconds;
		if (eng->desk_frame_sec >= 1.0f / 30.0f) {
			eng->desk_frame_sec = 0.0f;
			++eng->desk_frame_rev; /* the dock only: the settings window does not refresh on this (F-08) */
			eng->pending_ui_notify = true;
		}
	}
	if (eng->vj_state_dirty) {
		eng->vj_state_dirty = false;
		mfd_engine_notify_state_changed(eng);
	}
	mfd_engine_unlock(eng);
	for (int k = 0; k < mix_count; ++k) {
		mfd_vendor_send_mix_set(eng, mix_slots[k], mix_revs[k], mix_dbs[k]);
	}
	mfd_blackout_tick(seconds); /* no engine lock: it updates its own source */
}

static void mfd_engine_video_tick(void *param, float seconds)
{
	struct mfd_engine *eng = param;
	if (!eng || !eng->initialized) {
		return;
	}
	mfd_engine_tick(eng, seconds);
}

struct mfd_tracked_signal_entry {
	struct mfd_engine *eng;
	obs_source_t *key_ptr;
	obs_weak_source_t *weak_src;
	obs_weak_source_t *weak_parent;
	bool is_filter;
	bool is_scene;
	struct mfd_tracked_signal_entry *next;
};

static void on_obs_scene_signal(void *data, calldata_t *cd);
static void on_obs_host_filter_add(void *data, calldata_t *cd);
static void on_obs_host_filter_remove(void *data, calldata_t *cd);
static void on_obs_filter_rename(void *data, calldata_t *cd);
static void on_obs_filter_update_properties(void *data, calldata_t *cd);
static void on_obs_filter_enable(void *data, calldata_t *cd);
static void on_obs_filter_update(void *data, calldata_t *cd);

static struct mfd_tracked_signal_entry *find_tracked_signal_entry_locked(const struct mfd_engine *eng,
									 const obs_source_t *src)
{
	if (!eng || !src) {
		return NULL;
	}
	for (struct mfd_tracked_signal_entry *e = eng->tracked_signals_head; e != NULL; e = e->next) {
		if (e->key_ptr == src) {
			return e;
		}
	}
	return NULL;
}

static void disconnect_tracked_entry_signals(struct mfd_tracked_signal_entry *entry, signal_handler_t *sh)
{
	if (!entry || !sh) {
		return;
	}
	struct mfd_engine *eng = entry->eng;
	if (entry->is_filter) {
		signal_handler_disconnect(sh, "rename", on_obs_filter_rename, eng);
		signal_handler_disconnect(sh, "update_properties", on_obs_filter_update_properties, eng);
		signal_handler_disconnect(sh, "enable", on_obs_filter_enable, eng);
		signal_handler_disconnect(sh, "update", on_obs_filter_update, eng);
	} else {
		signal_handler_disconnect(sh, "filter_add", on_obs_host_filter_add, eng);
		signal_handler_disconnect(sh, "filter_remove", on_obs_host_filter_remove, eng);
		if (entry->is_scene) {
			signal_handler_disconnect(sh, "item_add", on_obs_scene_signal, eng);
			signal_handler_disconnect(sh, "item_remove", on_obs_scene_signal, eng);
			signal_handler_disconnect(sh, "reorder", on_obs_scene_signal, eng);
			signal_handler_disconnect(sh, "item_visible", on_obs_scene_signal, eng);
		}
	}
}

static void free_tracked_signal_entry(struct mfd_tracked_signal_entry *entry)
{
	if (!entry) {
		return;
	}
	if (entry->weak_src) {
		obs_weak_source_release(entry->weak_src);
	}
	if (entry->weak_parent) {
		obs_weak_source_release(entry->weak_parent);
	}
	free(entry);
}

static void disconnect_all_tracked_signals_locked(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	while (eng->tracked_signals_head != NULL) {
		struct mfd_tracked_signal_entry *cur = eng->tracked_signals_head;
		eng->tracked_signals_head = cur->next;
		obs_source_t *live = (obs_initialized() && cur->weak_src) ? obs_weak_source_get_source(cur->weak_src)
									  : NULL;
		if (live) {
			disconnect_tracked_entry_signals(cur, obs_source_get_signal_handler(live));
			obs_source_release(live);
		}
		/* A source past its last reference keeps no entry: its callbacks carry the engine, not the entry. */
		free_tracked_signal_entry(cur);
	}
}

/* Drops tracked entries for sources that were removed or destroyed, and for filters no longer on their host.
 *
 * The strong references the walk takes are let go only after it: the last release of a source destroys it on this
 * thread, libobs then emits filter_remove, our handler applies the change in place (we hold the recursive engine
 * lock) and asks for another prune, which would free the entry this walk stands on (A-01). A prune asked for while
 * one is walking only sets a flag, and the walk runs again. */
static void prune_tracked_signals_locked(struct mfd_engine *eng)
{
	if (!eng || !obs_initialized()) {
		return;
	}
	if (eng->pruning_tracked_signals) {
		eng->prune_tracked_again = true;
		return;
	}
	eng->pruning_tracked_signals = true;
	do {
		eng->prune_tracked_again = false;
		DARRAY(obs_source_t *) held;
		da_init(held);
		struct mfd_tracked_signal_entry **pp = &eng->tracked_signals_head;
		while (*pp != NULL) {
			struct mfd_tracked_signal_entry *cur = *pp;
			obs_source_t *src = cur->weak_src ? obs_weak_source_get_source(cur->weak_src) : NULL;
			if (src) {
				da_push_back(held, &src);
			}
			bool keep = src && !obs_source_removed(src);
			if (keep && cur->is_filter) {
				obs_source_t *parent = cur->weak_parent ? obs_weak_source_get_source(cur->weak_parent)
									: NULL;
				if (parent) {
					da_push_back(held, &parent);
				}
				keep = parent && !obs_source_removed(parent) && obs_filter_get_parent(src) == parent;
			}
			if (keep) {
				pp = &cur->next;
				continue;
			}
			*pp = cur->next;
			if (src) {
				disconnect_tracked_entry_signals(cur, obs_source_get_signal_handler(src));
			}
			free_tracked_signal_entry(cur);
		}
		for (size_t k = 0; k < held.num; ++k) {
			obs_source_release(held.array[k]);
		}
		da_free(held);
	} while (eng->prune_tracked_again);
	eng->pruning_tracked_signals = false;
}

static void connect_filter_source_signals_locked(struct mfd_engine *eng, obs_source_t *host_src, obs_source_t *flt)
{
	if (!eng || !host_src || !flt || obs_source_removed(host_src) || obs_source_removed(flt)) {
		return;
	}
	if (obs_source_get_type(flt) != OBS_SOURCE_TYPE_FILTER) {
		return;
	}
	const char *fname = obs_source_get_name(flt);
	const char *fid = obs_source_get_id(flt);
	if (mfd_is_internal_filter(fname, fid)) {
		return;
	}
	if (find_tracked_signal_entry_locked(eng, flt) != NULL) {
		return;
	}
	signal_handler_t *sh = obs_source_get_signal_handler(flt);
	if (!sh) {
		return;
	}
	struct mfd_tracked_signal_entry *entry = (struct mfd_tracked_signal_entry *)calloc(1U, sizeof(*entry));
	if (!entry) {
		return;
	}
	entry->eng = eng;
	entry->key_ptr = flt;
	entry->weak_src = obs_source_get_weak_source(flt);
	entry->weak_parent = obs_source_get_weak_source(host_src);
	entry->is_filter = true;
	entry->is_scene = false;
	entry->next = eng->tracked_signals_head;
	eng->tracked_signals_head = entry;

	signal_handler_connect(sh, "rename", on_obs_filter_rename, eng);
	signal_handler_connect(sh, "update_properties", on_obs_filter_update_properties, eng);
	signal_handler_connect(sh, "enable", on_obs_filter_enable, eng);
	signal_handler_connect(sh, "update", on_obs_filter_update, eng);
}

static void enum_connect_host_filters_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	connect_filter_source_signals_locked((struct mfd_engine *)param, parent, child);
}

static void connect_host_source_signals_locked(struct mfd_engine *eng, obs_source_t *src)
{
	if (!eng || !src || obs_source_removed(src)) {
		return;
	}
	enum obs_source_type stype = obs_source_get_type(src);
	if (stype != OBS_SOURCE_TYPE_INPUT && stype != OBS_SOURCE_TYPE_SCENE) {
		return;
	}
	const char *sname = obs_source_get_name(src);
	if (!sname || sname[0] == '\0') {
		return;
	}
	if (find_tracked_signal_entry_locked(eng, src) == NULL) {
		signal_handler_t *sh = obs_source_get_signal_handler(src);
		if (sh) {
			struct mfd_tracked_signal_entry *entry =
				(struct mfd_tracked_signal_entry *)calloc(1U, sizeof(*entry));
			if (entry) {
				entry->eng = eng;
				entry->key_ptr = src;
				entry->weak_src = obs_source_get_weak_source(src);
				entry->weak_parent = NULL;
				entry->is_filter = false;
				entry->is_scene = (stype == OBS_SOURCE_TYPE_SCENE) || obs_source_is_scene(src);
				entry->next = eng->tracked_signals_head;
				eng->tracked_signals_head = entry;

				signal_handler_connect(sh, "filter_add", on_obs_host_filter_add, eng);
				signal_handler_connect(sh, "filter_remove", on_obs_host_filter_remove, eng);
				if (entry->is_scene) {
					signal_handler_connect(sh, "item_add", on_obs_scene_signal, eng);
					signal_handler_connect(sh, "item_remove", on_obs_scene_signal, eng);
					signal_handler_connect(sh, "reorder", on_obs_scene_signal, eng);
					signal_handler_connect(sh, "item_visible", on_obs_scene_signal, eng);
				}
			}
		}
	}
	obs_source_enum_filters(src, enum_connect_host_filters_cb, eng);
}

static bool enum_connect_sources_cb(void *param, obs_source_t *src)
{
	connect_host_source_signals_locked((struct mfd_engine *)param, src);
	return true;
}

static void connect_all_live_source_and_filter_signals(struct mfd_engine *eng)
{
	if (!eng || !obs_initialized()) {
		return;
	}
	mfd_engine_lock(eng);
	if (eng->signals_connected) {
		obs_enum_sources(enum_connect_sources_cb, eng);
		obs_enum_scenes(enum_connect_sources_cb, eng);
	}
	mfd_engine_unlock(eng);
}

static void graph_lock(struct mfd_engine *eng)
{
	if (eng->graph_mutex_initialized) {
		pthread_mutex_lock(&eng->graph_mutex);
	}
}

static void graph_unlock(struct mfd_engine *eng)
{
	if (eng->graph_mutex_initialized) {
		pthread_mutex_unlock(&eng->graph_mutex);
	}
}

void mfd_engine_discard_scene_graph_changes(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	graph_lock(eng);
	struct mfd_graph_change *items = eng->graph_items;
	eng->graph_items = NULL;
	eng->graph_count = 0U;
	eng->graph_cap = 0U;
	eng->graph_rescan = false;
	eng->graph_recheck = false;
	eng->graph_overflowed = false;
	os_atomic_store_long(&eng->graph_pending, 0);
	graph_unlock(eng);
	bfree(items);
}

/*
 * Engine lock held. Applies one name-bearing change to Midi Fadester's own state only: no re-check here (the caller
 * re-checks once, after the whole batch, against the scene graph as it now is).
 */
static void apply_named_change_locked(struct mfd_engine *eng, const struct mfd_graph_change *ch, bool *renamed,
				      bool *recheck, bool *prune)
{
	switch (ch->kind) {
	case MFD_GRAPH_RESCAN:
	case MFD_GRAPH_RECHECK:
		*recheck = true;
		break;
	case MFD_GRAPH_FILTER_PROPS:
		mark_filter_meta_stale_locked(eng, NULL, NULL);
		*recheck = true;
		break;
	case MFD_GRAPH_SOURCE_GONE:
		*prune = true;
		if (!ch->flag) {
			if (ch->a[0] != '\0') {
				invalidate_filter_leases_for_removed_locked(eng, ch->a, NULL);
			}
			*recheck = true;
		}
		break;
	case MFD_GRAPH_SOURCE_RENAMED:
		if (rename_source_refs_locked(eng, ch->a, ch->b)) {
			*renamed = true;
		}
		*recheck = true;
		break;
	case MFD_GRAPH_FILTER_REMOVED:
		*prune = true;
		if (ch->a[0] != '\0') {
			invalidate_filter_leases_for_removed_locked(eng, ch->a, ch->b[0] != '\0' ? ch->b : NULL);
			/* a filter of the same name may be added in the same batch: it is another filter */
			mark_filter_meta_stale_locked(eng, ch->a, ch->b[0] != '\0' ? ch->b : NULL);
		}
		*recheck = true;
		break;
	case MFD_GRAPH_FILTER_RENAMED:
		if (rename_filter_refs_locked(eng, ch->a, ch->b, ch->c)) {
			*renamed = true;
		}
		*recheck = true;
		break;
	case MFD_GRAPH_FILTER_ENABLED:
		mfd_engine_handle_filter_enable(eng, ch->a, ch->b, ch->flag);
		break;
	case MFD_GRAPH_FILTER_SETTINGS:
		mfd_engine_handle_filter_settings_updated(eng, ch->a, ch->b);
		break;
	}
}

/* Engine lock held. Re-checks once after a batch (or one owner-thread change). */
static void finish_changes_locked(struct mfd_engine *eng, bool renamed, bool recheck, bool prune, bool rescan)
{
	if (prune || rescan) {
		prune_tracked_signals_locked(eng);
	}
	if (rescan) {
		connect_all_live_source_and_filter_signals(eng); /* a no-op once exit or shutdown disconnected */
		recheck = true;
	}
	if (recheck || renamed) {
		mfd_engine_reevaluate_slots(eng, true);
	}
	if (renamed) {
		mfd_engine_notify_state_changed(eng);
	}
}

/* Releases the engine lock. A rename is saved to the global config by the UI thread's flush, never here: a drain may
 * run on the graphics thread. */
static void unlock_after_changes(struct mfd_engine *eng, bool renamed)
{
	if (renamed && obs_initialized()) {
		eng->pending_config_save = true;
	}
	mfd_engine_unlock(eng);
}

/* allow_rescan is false on the owner-thread path: connecting signals there would enumerate sources inside whatever
 * libobs call this thread is in; the rescan stays queued for the next drain. */
static size_t drain_scene_graph_changes(struct mfd_engine *eng, bool allow_rescan)
{
	if (os_atomic_load_long(&eng->graph_pending) == 0) {
		return 0U; /* nothing queued: an idle video tick takes no lock */
	}
	/*
	 * The engine lock first, then the batch: drains (the video tick, frontend events, obs-websocket requests) then
	 * run one at a time, each on everything queued before it, so a later batch can never be applied before an
	 * earlier one (A->B then B->C). Order: engine lock, then graph_mutex, as on the owner-thread path; the signal
	 * callbacks take graph_mutex only.
	 */
	mfd_engine_lock(eng);
	graph_lock(eng);
	struct mfd_graph_change *items = eng->graph_items;
	const size_t count = eng->graph_count;
	const bool rescan = allow_rescan && eng->graph_rescan;
	const bool recheck = eng->graph_recheck || (!allow_rescan && eng->graph_rescan);
	const bool overflowed = eng->graph_overflowed;
	eng->graph_items = NULL;
	eng->graph_count = 0U;
	eng->graph_cap = 0U;
	eng->graph_recheck = false;
	eng->graph_overflowed = false;
	if (allow_rescan) {
		eng->graph_rescan = false;
	}
	os_atomic_store_long(&eng->graph_pending, eng->graph_rescan ? 1 : 0);
	graph_unlock(eng);
	if (!eng->initialized || (count == 0U && !rescan && !recheck)) {
		/* nothing to do, or shut down: nothing may be connected or changed any more */
		mfd_engine_unlock(eng);
		bfree(items);
		return 0U;
	}
	if (overflowed) {
		obs_log(LOG_WARNING, "More than %d filter changes queued at once; rescanning sources",
			MFD_GRAPH_CHANGE_MAX);
	}
	const bool was_draining = eng->graph_draining;
	eng->graph_draining = true;
	bool renamed = false;
	bool need_recheck = recheck;
	bool prune = false;
	for (size_t i = 0U; i < count; ++i) {
		apply_named_change_locked(eng, &items[i], &renamed, &need_recheck, &prune);
	}
	finish_changes_locked(eng, renamed, need_recheck, prune, rescan);
	eng->graph_draining = was_draining;
	unlock_after_changes(eng, renamed);
	bfree(items);
	return count + (rescan ? 1U : 0U) + (recheck ? 1U : 0U);
}

size_t mfd_engine_process_scene_graph_changes(struct mfd_engine *eng)
{
	if (!eng) {
		return 0U;
	}
	return drain_scene_graph_changes(eng, true);
}

static void fill_graph_change(struct mfd_graph_change *ch, enum mfd_graph_change_kind kind, const char *a,
			      const char *b, const char *c, bool flag)
{
	ch->kind = kind;
	ch->flag = flag;
	snprintf(ch->a, sizeof(ch->a), "%s", a ? a : "");
	snprintf(ch->b, sizeof(ch->b), "%s", b ? b : "");
	snprintf(ch->c, sizeof(ch->c), "%s", c ? c : "");
}

static bool graph_kind_is_droppable(enum mfd_graph_change_kind kind)
{
	return kind == MFD_GRAPH_FILTER_SETTINGS; /* an operator's enable toggle is never dropped */
}

static bool graph_kind_is_foldable(enum mfd_graph_change_kind kind)
{
	return kind == MFD_GRAPH_SOURCE_GONE || kind == MFD_GRAPH_FILTER_SETTINGS || kind == MFD_GRAPH_FILTER_ENABLED ||
	       kind == MFD_GRAPH_FILTER_PROPS;
}

/* A rename or removal: an update queued after it may concern a different source or filter of the same name. */
static bool graph_kind_is_fold_barrier(enum mfd_graph_change_kind kind)
{
	return kind == MFD_GRAPH_SOURCE_RENAMED || kind == MFD_GRAPH_FILTER_RENAMED ||
	       kind == MFD_GRAPH_FILTER_REMOVED || kind == MFD_GRAPH_SOURCE_GONE;
}

/*
 * graph_mutex held. An entry this change can replace: the same removed source, or the same filter's settings or
 * enable state (the last value wins). Searched from the tail and never past a rename or removal, so a change never
 * moves ahead of one queued before it (enable A, rename A->B, rename C->A, enable A keeps all four).
 */
static struct mfd_graph_change *find_foldable_graph_change(struct mfd_engine *eng, enum mfd_graph_change_kind kind,
							   const char *a, const char *b, bool flag)
{
	if (!graph_kind_is_foldable(kind)) {
		return NULL;
	}
	const char *na = a ? a : "";
	const char *nb = b ? b : "";
	for (size_t i = eng->graph_count; i > 0U; --i) {
		struct mfd_graph_change *q = &eng->graph_items[i - 1U];
		const bool same_key = q->kind == kind && strncmp(q->a, na, sizeof(q->a) - 1U) == 0 &&
				      (kind == MFD_GRAPH_SOURCE_GONE ? (q->flag == flag)
								     : (strncmp(q->b, nb, sizeof(q->b) - 1U) == 0));
		if (same_key) {
			return q;
		}
		if (graph_kind_is_fold_barrier(q->kind)) {
			return NULL;
		}
	}
	return NULL;
}

static void enqueue_graph_change(struct mfd_engine *eng, enum mfd_graph_change_kind kind, const char *a, const char *b,
				 const char *c, bool flag)
{
	graph_lock(eng);
	if (kind == MFD_GRAPH_RESCAN) {
		eng->graph_rescan = true;
	} else if (kind == MFD_GRAPH_RECHECK) {
		eng->graph_recheck = true;
	} else {
		struct mfd_graph_change *same = find_foldable_graph_change(eng, kind, a, b, flag);
		if (same) {
			same->flag =
				flag; /* remove and destroy report one source once; a filter keeps its last value */
		} else if (graph_kind_is_droppable(kind) && eng->graph_count >= MFD_GRAPH_CHANGE_MAX) {
			/* Too many filters changed at once: a full rescan and re-check stand in for the dropped ones */
			eng->graph_rescan = true;
			eng->graph_recheck = true;
			eng->graph_overflowed = true;
		} else {
			if (eng->graph_count == eng->graph_cap) {
				/* renames, removals and removed filters grow past MFD_GRAPH_CHANGE_MAX: losing one would leave
				 * a slot naming a source that no longer exists */
				const size_t cap = eng->graph_cap ? eng->graph_cap * 2U : 64U;
				eng->graph_items = brealloc(eng->graph_items, cap * sizeof(*eng->graph_items));
				eng->graph_cap = cap;
			}
			fill_graph_change(&eng->graph_items[eng->graph_count++], kind, a, b, c, flag);
		}
	}
	os_atomic_store_long(&eng->graph_pending, 1);
	graph_unlock(eng);
}

void mfd_engine_report_scene_graph_change(struct mfd_engine *eng, enum mfd_graph_change_kind kind, const char *a,
					  const char *b, const char *c, bool flag)
{
	if (!eng) {
		return;
	}
	if (!mfd_engine_held_by_current_thread(eng)) {
		enqueue_graph_change(eng, kind, a, b, c, flag);
		return;
	}
	/*
	 * Midi Fadester's own call made libobs emit this (a bypass write, a filter it added): this thread already holds
	 * the engine lock, so applying now waits for nothing and keeps the engine -> libobs order. Its write guards
	 * (writing_enable, writing_filter_settings) are set right now, so it must be applied now, not queued. Changes
	 * queued earlier go first, unless this thread is draining them already.
	 */
	if (kind == MFD_GRAPH_RESCAN) {
		enqueue_graph_change(eng, kind, a, b, c, flag);
		return;
	}
	if (!eng->graph_draining) {
		drain_scene_graph_changes(eng, false);
	}
	struct mfd_graph_change *ch = bzalloc(sizeof(*ch));
	fill_graph_change(ch, kind, a, b, c, flag);
	bool renamed = false;
	bool recheck = false;
	bool prune = false;
	mfd_engine_lock(eng);
	const bool was_draining = eng->graph_draining;
	eng->graph_draining = true; /* echoes of this change's own writes apply in place, not after a new drain */
	apply_named_change_locked(eng, ch, &renamed, &recheck, &prune);
	finish_changes_locked(eng, renamed, recheck, prune, false);
	eng->graph_draining = was_draining;
	unlock_after_changes(eng, renamed);
	bfree(ch);
}

/*
 * libobs signal callbacks. libobs may hold its own locks while it emits these (the sources mutex during a removal,
 * a scene's full lock for item_remove), and an engine-lock holder looks the scene graph up through those same locks.
 * So none of these waits for the engine lock: each reports the change, which is queued unless this thread already
 * holds the engine lock. Their data is the engine, never a tracking entry, so a late signal from a source that is
 * being destroyed touches nothing that has been freed.
 */
static struct mfd_engine *engine_from_signal_data(void *data)
{
	struct mfd_engine *eng = (struct mfd_engine *)data;
	return (eng && eng->initialized) ? eng : NULL;
}

/* The filter a filter signal is about, and its host's name; false when either is gone. */
static bool filter_signal_names(calldata_t *cd, char *out_host, size_t host_size, char *out_filter, size_t filter_size)
{
	obs_source_t *flt = cd ? (obs_source_t *)calldata_ptr(cd, "source") : NULL;
	obs_source_t *host = flt ? obs_filter_get_parent(flt) : NULL;
	const char *hname = host ? obs_source_get_name(host) : NULL;
	const char *fname = flt ? obs_source_get_name(flt) : NULL;
	if (!hname || hname[0] == '\0' || !fname || fname[0] == '\0') {
		return false;
	}
	snprintf(out_host, host_size, "%s", hname);
	snprintf(out_filter, filter_size, "%s", fname);
	return true;
}

static void on_obs_scene_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (eng) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_RECHECK, NULL, NULL, NULL, false);
	}
}

static void on_obs_host_filter_add(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (!eng) {
		return;
	}
	obs_source_t *flt = cd ? (obs_source_t *)calldata_ptr(cd, "filter") : NULL;
	if (flt && mfd_is_internal_filter(obs_source_get_name(flt), obs_source_get_id(flt))) {
		return;
	}
	/* a filter of a bound name may have come (back): its properties are read again (A-03) */
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_PROPS, NULL, NULL, NULL, false);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_RESCAN, NULL, NULL, NULL, false);
}

static void on_obs_host_filter_remove(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (!eng) {
		return;
	}
	obs_source_t *host = cd ? (obs_source_t *)calldata_ptr(cd, "source") : NULL;
	obs_source_t *flt = cd ? (obs_source_t *)calldata_ptr(cd, "filter") : NULL;
	const char *fname = flt ? obs_source_get_name(flt) : NULL;
	if (flt && mfd_is_internal_filter(fname, obs_source_get_id(flt))) {
		return;
	}
	const char *hname = host ? obs_source_get_name(host) : NULL;
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_REMOVED, hname, fname, NULL, false);
}

static void on_obs_filter_rename(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (!eng || !cd) {
		return;
	}
	obs_source_t *flt = (obs_source_t *)calldata_ptr(cd, "source");
	obs_source_t *host = flt ? obs_filter_get_parent(flt) : NULL;
	const char *hname = host ? obs_source_get_name(host) : NULL;
	if (hname && hname[0] != '\0') {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_RENAMED, hname,
						     calldata_string(cd, "prev_name"), calldata_string(cd, "new_name"),
						     false);
	}
}

static void on_obs_filter_update_properties(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (eng) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_PROPS, NULL, NULL, NULL, false);
	}
}

static void on_obs_filter_enable(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	char hname[MFD_SOURCE_NAME_MAX] = {0};
	char fname[MFD_SOURCE_NAME_MAX] = {0};
	if (eng && filter_signal_names(cd, hname, sizeof(hname), fname, sizeof(fname))) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_ENABLED, hname, fname, NULL,
						     calldata_bool(cd, "enabled"));
	}
}

static void on_obs_filter_update(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	char hname[MFD_SOURCE_NAME_MAX] = {0};
	char fname[MFD_SOURCE_NAME_MAX] = {0};
	if (eng && filter_signal_names(cd, hname, sizeof(hname), fname, sizeof(fname))) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_FILTER_SETTINGS, hname, fname, NULL, false);
	}
}

static void on_obs_source_create(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	obs_source_t *src = cd ? (obs_source_t *)calldata_ptr(cd, "source") : NULL;
	if (!eng || !src) {
		return;
	}
	enum obs_source_type stype = obs_source_get_type(src);
	if (stype != OBS_SOURCE_TYPE_INPUT && stype != OBS_SOURCE_TYPE_SCENE) {
		return;
	}
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_RESCAN, NULL, NULL, NULL, false);
}

static void on_obs_source_remove_or_destroy(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (!eng) {
		return;
	}
	obs_source_t *src = cd ? (obs_source_t *)calldata_ptr(cd, "source") : NULL;
	if (!src) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_RECHECK, NULL, NULL, NULL, false);
		return;
	}
	enum obs_source_type stype = obs_source_get_type(src);
	const bool filter_or_transition = (stype == OBS_SOURCE_TYPE_FILTER || stype == OBS_SOURCE_TYPE_TRANSITION);
	mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_SOURCE_GONE, obs_source_get_name(src), NULL, NULL,
					     filter_or_transition);
}

static void on_obs_source_rename(void *data, calldata_t *cd)
{
	struct mfd_engine *eng = engine_from_signal_data(data);
	if (eng && cd) {
		mfd_engine_report_scene_graph_change(eng, MFD_GRAPH_SOURCE_RENAMED, calldata_string(cd, "prev_name"),
						     calldata_string(cd, "new_name"), NULL, false);
	}
}

void mfd_engine_connect_signal_callbacks_for_test(struct mfd_engine *eng, signal_handler_t *sh)
{
	if (!eng || !sh) {
		return;
	}
	static const char *const decls[] = {
		"void source_create(ptr source)",
		"void source_remove(ptr source)",
		"void source_destroy(ptr source)",
		"void source_rename(ptr source, string new_name, string prev_name)",
		"void item_add(ptr scene, ptr item)",
		"void item_remove(ptr scene, ptr item)",
		"void reorder(ptr scene)",
		"void filter_add(ptr source, ptr filter)",
		"void filter_remove(ptr source, ptr filter)",
		"void rename(ptr source, string new_name, string prev_name)",
		"void update_properties(ptr source)",
		"void enable(ptr source, bool enabled)",
		"void update(ptr source)",
	};
	for (size_t i = 0U; i < sizeof(decls) / sizeof(decls[0]); ++i) {
		signal_handler_add(sh, decls[i]);
	}
	signal_handler_connect(sh, "source_create", on_obs_source_create, eng);
	signal_handler_connect(sh, "source_remove", on_obs_source_remove_or_destroy, eng);
	signal_handler_connect(sh, "source_destroy", on_obs_source_remove_or_destroy, eng);
	signal_handler_connect(sh, "source_rename", on_obs_source_rename, eng);
	signal_handler_connect(sh, "item_add", on_obs_scene_signal, eng);
	signal_handler_connect(sh, "item_remove", on_obs_scene_signal, eng);
	signal_handler_connect(sh, "reorder", on_obs_scene_signal, eng);
	signal_handler_connect(sh, "item_visible", on_obs_scene_signal, eng);
	signal_handler_connect(sh, "filter_add", on_obs_host_filter_add, eng);
	signal_handler_connect(sh, "filter_remove", on_obs_host_filter_remove, eng);
	signal_handler_connect(sh, "rename", on_obs_filter_rename, eng);
	signal_handler_connect(sh, "update_properties", on_obs_filter_update_properties, eng);
	signal_handler_connect(sh, "enable", on_obs_filter_enable, eng);
	signal_handler_connect(sh, "update", on_obs_filter_update, eng);
}

static void connect_obs_signals(struct mfd_engine *eng)
{
	if (!eng || eng->signals_connected || !obs_initialized()) {
		return;
	}
	mfd_engine_lock(eng);
	eng->signals_connected = true; /* before connecting: connect_all_live_source_and_filter_signals requires it */
	signal_handler_t *sh = obs_get_signal_handler();
	if (sh) {
		signal_handler_connect(sh, "source_create", on_obs_source_create, eng);
		signal_handler_connect(sh, "source_remove", on_obs_source_remove_or_destroy, eng);
		signal_handler_connect(sh, "source_destroy", on_obs_source_remove_or_destroy, eng);
		signal_handler_connect(sh, "source_rename", on_obs_source_rename, eng);
	}
	connect_all_live_source_and_filter_signals(eng);
	mfd_engine_unlock(eng);
}

static void disconnect_obs_signals(struct mfd_engine *eng)
{
	if (!eng || !eng->signals_connected) {
		return;
	}
	mfd_engine_lock(eng);
	eng->signals_connected = false;
	if (obs_initialized()) {
		signal_handler_t *sh = obs_get_signal_handler();
		if (sh) {
			signal_handler_disconnect(sh, "source_create", on_obs_source_create, eng);
			signal_handler_disconnect(sh, "source_remove", on_obs_source_remove_or_destroy, eng);
			signal_handler_disconnect(sh, "source_destroy", on_obs_source_remove_or_destroy, eng);
			signal_handler_disconnect(sh, "source_rename", on_obs_source_rename, eng);
		}
	}
	disconnect_all_tracked_signals_locked(eng);
	mfd_engine_unlock(eng);
}

static void sync_slots_on_scene_change(struct mfd_engine *eng, bool reset_tbar)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	refresh_frontend_cache_on_ui_thread_locked(eng);
	vj_status_locked(eng, false); /* the scene lights and get_state's scenes, at once */
	if (reset_tbar) {
		mfd_engine_unlatch_tbar(eng);
	}
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (!slot->enabled) {
			continue;
		}
		if (slot->target_type == MFD_TARGET_TBAR) {
			if (reset_tbar) {
				slot->auto_fade_active = false;
				slot->auto_fade_is_reset_fx = false;
				slot->pending_clean_apply_on_avail = false;
				slot->tbar_state = MFD_TBAR_IDLE;
				slot->tbar_midi_active = false;
				slot->tbar_from_idle_band = false;
				slot->last_applied_val = 0.0f;
				mfd_smoother_reset(&slot->smoother, 0.0f);
				mfd_takeover_sync_external(&slot->takeover, 0.0f);
			}
		}
		/* An audio strip is not synced here: OBS's volume is the smoothed value, which trails a fader that is
		 * moving, so a scene change in the middle of a move (a fade ending is one) would drop the fader out of
		 * pick-up. The tick already notices a volume changed from outside (sync_volume_slots_from_live_locked). */
	}
	mfd_engine_reevaluate_slots(eng, false);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *slot = &eng->fader_slots[i];
		if (slot->enabled && slot->target_type == MFD_TARGET_FILTER_PARAM && slot->cached_available &&
		    !slot->pending_clean_apply_on_avail && !slot->auto_fade_active) {
			/* Only a value somebody else wrote is taken over: a knob that is moving, or one whose setting
			 * is still what it wrote, keeps its pick-up (a coarse whole-number setting would otherwise
			 * differ from the knob by more than the pick-up window and freeze it). */
			double live_val = 0.0;
			if (mfd_smoother_is_active(&slot->smoother) || !slot->filter_prop_valid ||
			    !query_live_filter_value_locked(eng, slot, &live_val) ||
			    filter_live_value_is_ours(slot, live_val) ||
			    fabs(slot->filter_to_val - slot->filter_from_val) < MFD_FILTER_MIN_RANGE_SPAN) {
				continue;
			}
			float t_live = mfd_filter_unmap_value(live_val, slot->filter_from_val, slot->filter_to_val);
			slot->last_applied_val = t_live;
			slot->has_last_written_mapped = false;
			mfd_smoother_reset(&slot->smoother, t_live);
			mfd_takeover_sync_external(&slot->takeover, t_live);
		}
	}
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
}

static void on_save_callback(obs_data_t *save_data, bool saving, void *private_data);

static void on_frontend_event(enum obs_frontend_event event, void *private_data)
{
	struct mfd_engine *eng = private_data;
	if (!eng) {
		return;
	}
	struct mfd_midi_port_list loaded_ports;
	loaded_ports.count = 0U;
	loaded_ports.valid = false;

	/* Frontend events run on the UI thread with no libobs lock held: a safe place to catch up. */
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		/* Changes queued for the scene collection that is going away name sources that no longer matter. */
		mfd_engine_discard_scene_graph_changes(eng);
	} else {
		mfd_engine_process_scene_graph_changes(eng);
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || event == OBS_FRONTEND_EVENT_EXIT) {
		mfd_blackout_reset(); /* OBS clears its output channels with the collection: start clear */
	}

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		midi_port_list_read(&loaded_ports); /* before the lock (A-02) */
		mfd_engine_lock(eng);
		vj_restore_loaded_punches_locked(eng);
		eng->frontend_finished_loading = true;
		mfd_engine_mark_ui_dirty(eng, false); /* the UI thread looks once more: the dock's first show (U1-01) */
		refresh_apc40_detected_locked(eng, &loaded_ports);
		connect_all_live_source_and_filter_signals(eng);
		sync_slots_on_scene_change(eng, false);
		mfd_engine_unlock(eng);
		vj_try_autofill(eng);
		vj_follow_program(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		/* The leases were cleared at SCENE_COLLECTION_CLEANUP: the ones the load callback made since then
		 * carry the saved on/off state of "bypass at 0 %" filters and must stay. */
		mfd_engine_lock(eng);
		vj_restore_loaded_punches_locked(eng);
		disconnect_all_tracked_signals_locked(eng);
		connect_all_live_source_and_filter_signals(eng);
		sync_slots_on_scene_change(eng, false);
		mfd_engine_unlock(eng);
		vj_follow_program(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		mfd_engine_lock(eng);
		connect_all_live_source_and_filter_signals(eng);
		sync_slots_on_scene_change(eng, false);
		mfd_engine_unlock(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		sync_slots_on_scene_change(eng, false);
		vj_follow_program(eng); /* strips that follow the scene on air move to its sources */
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		sync_slots_on_scene_change(eng, false);
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		sync_slots_on_scene_change(eng, true);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		mfd_engine_lock(eng);
		vj_restore_all_punches_locked(eng); /* while the collection's sources still exist */
		for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
			eng->vj_home_key[i][0] = '\0';
		}
		mfd_engine_unlock(eng);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		/* After OBS saved the old collection and released its sources, before the next one loads: the old
		 * filters' leases go now, so the load callback's leases for the new collection survive (B-03). */
		mfd_engine_lock(eng);
		memset(eng->filter_leases, 0, sizeof(eng->filter_leases));
		mfd_engine_unlock(eng);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		mfd_engine_lock(eng);
		vj_restore_all_punches_locked(eng); /* punched blends are the desk's: the saved scenes keep their own */
		/* Usually finds no lease: OBS cleared the scene data (SCENE_COLLECTION_CLEANUP) before EXIT, and the
		 * saved strips carry each filter's own on/off state for the next start. */
		mfd_engine_restore_all_bypass_leases(eng);
		disconnect_obs_signals(eng);
		mfd_engine_unlock(eng);
		/* OBS takes its frontend callbacks away right after this event, before modules unload: remove ours
		 * now (removing the running one is safe, OBS walks the list from the end) (A-06). */
		if (!eng->frontend_callbacks_removed) {
			eng->frontend_callbacks_removed = true;
			obs_frontend_remove_save_callback(on_save_callback, eng);
			obs_frontend_remove_event_callback(on_frontend_event, eng);
		}
		break;
	default:
		break;
	}
}

void mfd_engine_frontend_event_for_test(struct mfd_engine *eng, int event)
{
	on_frontend_event((enum obs_frontend_event)event, eng);
}

static void serialize_slots(struct mfd_engine *eng, obs_data_t *root);
static void deserialize_slots(struct mfd_engine *eng, obs_data_t *root, bool keep_user_midi);

void mfd_engine_serialize_slots_for_test(struct mfd_engine *eng, obs_data_t *root)
{
	mfd_engine_lock(eng);
	serialize_slots(eng, root);
	mfd_engine_unlock(eng);
}

void mfd_engine_deserialize_slots_for_test(struct mfd_engine *eng, obs_data_t *root)
{
	mfd_engine_lock(eng);
	deserialize_slots(eng, root, false);
	mfd_engine_unlock(eng);
}

static void serialize_calibrations(const struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_CAL_ENTRIES; ++i) {
		const struct mfd_persisted_cal_entry *e = &eng->cal_entries[i];
		if (!e->in_use) {
			continue;
		}
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "profile", e->profile_name);
		obs_data_set_int(item, "midi_type", (int)e->addr.type);
		obs_data_set_int(item, "midi_channel", (int)e->addr.channel);
		obs_data_set_int(item, "midi_number", (int)e->addr.number);
		obs_data_set_bool(item, "midi_14bit", e->addr.is_14bit);
		obs_data_set_int(item, "cal_min", e->cal.cal_min);
		obs_data_set_int(item, "cal_max", e->cal.cal_max);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "control_calibrations", arr);
	obs_data_array_release(arr);
}

static void deserialize_calibrations(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_get_array(root, "control_calibrations");
	if (!arr) {
		return;
	}
	memset(eng->cal_entries, 0, sizeof(eng->cal_entries)); /* Import replaces the learned ranges */
	size_t count = obs_data_array_count(arr);
	for (size_t i = 0U; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		if (!item) {
			continue;
		}
		const char *prof = obs_data_get_string(item, "profile");
		struct mfd_midi_addr addr;
		memset(&addr, 0, sizeof(addr));
		addr.type = (enum mfd_midi_msg_type)obs_data_get_int(item, "midi_type");
		addr.channel = (uint8_t)(obs_data_get_int(item, "midi_channel") & 0x0F);
		addr.number = (uint8_t)(obs_data_get_int(item, "midi_number") & 0x7F);
		addr.is_14bit = obs_data_get_bool(item, "midi_14bit");
		struct mfd_control_calibration cal;
		mfd_calibration_set(&cal, slot_addr_is_14bit(&addr), (int)obs_data_get_int(item, "cal_min"),
				    (int)obs_data_get_int(item, "cal_max"));
		store_persisted_calibration(eng, prof, &addr, &cal);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void serialize_slots(struct mfd_engine *eng, obs_data_t *root)
{
	obs_data_array_t *arr = obs_data_array_create();
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		const struct mfd_fader_slot *s = &eng->fader_slots[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", s->id);
		obs_data_set_string(item, "kind", s->kind);
		obs_data_set_bool(item, "enabled", s->enabled);
		char english_label[MFD_SLOT_LABEL_MAX];
		mfd_engine_slot_english_label(eng, i, english_label, sizeof(english_label));
		obs_data_set_string(item, "label", english_label);
		obs_data_set_int(item, "midi_type", (int)s->addr.type);
		obs_data_set_int(item, "midi_channel", (int)s->addr.channel);
		obs_data_set_int(item, "midi_number", (int)s->addr.number);
		obs_data_set_bool(item, "midi_14bit", s->addr.is_14bit);
		obs_data_set_int(item, "cal_min", s->cal.cal_min);
		obs_data_set_int(item, "cal_max", s->cal.cal_max);
		obs_data_set_int(item, "target_type", (int)s->target_type);
		obs_data_set_string(item, "scene", s->scene_name);
		obs_data_set_string(item, "source_a", s->source_a_name);
		obs_data_set_string(item, "source_b", s->source_b_name);
		obs_data_set_string(item, "filter_name", s->filter_name);
		obs_data_set_string(item, "filter_prop", s->filter_prop);
		obs_data_set_double(item, "from_val", s->filter_from_val);
		obs_data_set_double(item, "to_val", s->filter_to_val);
		obs_data_set_bool(item, "bypass_at_zero", s->filter_bypass_at_zero);
		if (s->target_type == MFD_TARGET_FILTER_PARAM && s->filter_bypass_at_zero &&
		    (find_filter_lease_locked(eng, s->source_a_name, s->filter_name) != NULL ||
		     s->restore_persisted_pre_bypass)) {
			obs_data_set_bool(item, "filter_pre_bypass_enabled", s->filter_pre_bypass_enabled);
		}
		obs_data_set_int(item, "curve", (int)s->curve);
		obs_data_set_bool(item, "afv", s->audio_follows_video);
		obs_data_set_bool(item, "invert", s->invert);
		obs_data_set_bool(item, "auto_follow", s->auto_follow);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(root, "slots", arr);
	obs_data_array_release(arr);
}

static void clear_slot_filter_fields(struct mfd_fader_slot *s)
{
	if (!s) {
		return;
	}
	s->filter_name[0] = '\0';
	s->filter_prop[0] = '\0';
	s->filter_from_val = 0.0;
	s->filter_to_val = 0.0;
	s->filter_bypass_at_zero = false;
	s->filter_pre_bypass_enabled = true;
	s->restore_persisted_pre_bypass = false;
	s->filter_prop_valid = false;
}

/*
 * keep_user_midi (B-04): a scene collection is loading and this installation has its own settings file. Which control
 * drives each strip, its 14-bit flag and its learned range belong to the user (README: "kept per user"), so they stay
 * as they are; the collection gives only each strip's job. Without it (the settings file itself, Import, or a
 * collection read before any settings file exists) the addresses in the data are taken.
 */
static void deserialize_slots(struct mfd_engine *eng, obs_data_t *root, bool keep_user_midi)
{
	obs_data_array_t *arr = obs_data_get_array(root, "slots");
	if (!arr) {
		return;
	}

	struct mfd_midi_addr user_addr[MFD_MAX_FADER_SLOTS];
	struct mfd_control_calibration user_cal[MFD_MAX_FADER_SLOTS];
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		user_addr[i] = eng->fader_slots[i].addr;
		user_cal[i] = eng->fader_slots[i].cal;
	}

	/* Pre-seed 0..17 from active profile defaults (detaching any active filter bypass leases first) */
	mfd_engine_load_profile_defaults(eng, mfd_engine_get_active_profile(eng));
	if (keep_user_midi) {
		for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
			eng->fader_slots[i].addr = user_addr[i];
			eng->fader_slots[i].cal = user_cal[i];
		}
	}

	bool seen_slot[MFD_MAX_FADER_SLOTS];
	memset(seen_slot, 0, sizeof(seen_slot));

	size_t count = obs_data_array_count(arr);
	if (count > 256U) {
		count = 256U;
	}
	for (size_t i = 0U; i < count; ++i) {
		obs_data_t *item = obs_data_array_item(arr, i);
		if (!item) {
			continue;
		}
		int slot_idx = -1;
		const char *item_id = obs_data_get_string(item, "id");
		if (item_id && item_id[0] != '\0') {
			slot_idx = mfd_slot_index_for_id(item_id);
			if (slot_idx < 0 || slot_idx >= MFD_MAX_FADER_SLOTS) {
				obs_log(LOG_WARNING, "Ignoring unknown persisted slot id '%s' at index %u", item_id,
					(unsigned)i);
				obs_data_release(item);
				continue;
			}
		} else if (i < (size_t)MFD_MAX_FADER_SLOTS) {
			slot_idx = (int)i;
		} else {
			obs_data_release(item);
			continue;
		}

		if (seen_slot[slot_idx]) {
			obs_log(LOG_WARNING, "Ignoring duplicate persisted slot entry for '%s'",
				mfd_slot_id_for_index(slot_idx));
			obs_data_release(item);
			continue;
		}
		seen_slot[slot_idx] = true;

		struct mfd_fader_slot *s = &eng->fader_slots[slot_idx];
		snprintf(s->id, sizeof(s->id), "%s", mfd_slot_id_for_index(slot_idx));
		snprintf(s->kind, sizeof(s->kind), "%s", mfd_slot_kind_for_index(slot_idx));
		s->enabled = obs_data_get_bool(item, "enabled");
		const char *lbl = obs_data_get_string(item, "label");
		if (lbl && *lbl) {
			/* A stored default label (any profile's English default) becomes empty, so it is shown translated. */
			if (mfd_profile_is_default_slot_label(slot_idx, lbl)) {
				s->label[0] = '\0';
			} else {
				snprintf(s->label, sizeof(s->label), "%s", lbl);
			}
		}

		if (keep_user_midi) {
			/* the address and range restored above stay; the data's own are not read */
		} else if (obs_data_has_user_value(item, "midi_type")) {
			long long raw_mtype = obs_data_get_int(item, "midi_type");
			if (raw_mtype < (long long)MFD_MIDI_NONE || raw_mtype > (long long)MFD_MIDI_PITCH_BEND) {
				obs_log(LOG_WARNING, "Slot '%s' had invalid midi_type %lld; resetting MIDI binding",
					s->id, raw_mtype);
				s->addr.type = MFD_MIDI_NONE;
			} else {
				s->addr.type = (enum mfd_midi_msg_type)raw_mtype;
			}
			s->addr.channel = (uint8_t)(obs_data_get_int(item, "midi_channel") & 0x0F);
			s->addr.number = (uint8_t)(obs_data_get_int(item, "midi_number") & 0x7F);
			s->addr.is_14bit = obs_data_get_bool(item, "midi_14bit");
		}
		if (keep_user_midi) {
			/* kept */
		} else if (obs_data_has_user_value(item, "cal_min") && obs_data_has_user_value(item, "cal_max")) {
			mfd_calibration_set(&s->cal, slot_addr_is_14bit(&s->addr),
					    (int)obs_data_get_int(item, "cal_min"),
					    (int)obs_data_get_int(item, "cal_max"));
			store_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
		} else {
			mfd_engine_lookup_persisted_calibration(eng, effective_profile_name(eng), &s->addr, &s->cal);
		}

		if (obs_data_has_user_value(item, "target_type")) {
			long long raw_ttype = obs_data_get_int(item, "target_type");
			if (raw_ttype < (long long)MFD_TARGET_UNASSIGNED ||
			    raw_ttype > (long long)MFD_TARGET_MIX_MASTER) {
				obs_log(LOG_WARNING,
					"Slot '%s' had invalid target_type %lld; downgrading to unassigned", s->id,
					raw_ttype);
				s->target_type = MFD_TARGET_UNASSIGNED;
			} else {
				s->target_type = (enum mfd_target_type)raw_ttype;
			}
		} else if (obs_data_has_user_value(item, "target_mode")) {
			enum mfd_target_type parsed_tm = MFD_TARGET_UNASSIGNED;
			const char *tm_str = obs_data_get_string(item, "target_mode");
			if (mfd_target_mode_from_string(tm_str, &parsed_tm)) {
				s->target_type = parsed_tm;
			} else {
				obs_log(LOG_WARNING,
					"Slot '%s' had invalid target_mode '%s'; downgrading to unassigned", s->id,
					tm_str ? tm_str : "");
				s->target_type = MFD_TARGET_UNASSIGNED;
			}
		} else {
			s->target_type = MFD_TARGET_UNASSIGNED;
		}

		const char *sc = obs_data_get_string(item, "scene");
		const char *sa = obs_data_get_string(item, "source_a");
		const char *sb = obs_data_get_string(item, "source_b");
		const char *fn = obs_data_get_string(item, "filter_name");
		const char *fp = obs_data_get_string(item, "filter_prop");
		snprintf(s->scene_name, sizeof(s->scene_name), "%s", sc ? sc : "");
		snprintf(s->source_a_name, sizeof(s->source_a_name), "%s", sa ? sa : "");
		snprintf(s->source_b_name, sizeof(s->source_b_name), "%s", sb ? sb : "");
		snprintf(s->filter_name, sizeof(s->filter_name), "%s", fn ? fn : "");
		snprintf(s->filter_prop, sizeof(s->filter_prop), "%s", fp ? fp : "");
		s->filter_from_val = obs_data_get_double(item, "from_val");
		s->filter_to_val = obs_data_get_double(item, "to_val");
		s->filter_bypass_at_zero = obs_data_get_bool(item, "bypass_at_zero");
		if (obs_data_has_user_value(item, "filter_pre_bypass_enabled")) {
			s->filter_pre_bypass_enabled = obs_data_get_bool(item, "filter_pre_bypass_enabled");
			s->restore_persisted_pre_bypass = true;
		} else {
			s->filter_pre_bypass_enabled = true;
			s->restore_persisted_pre_bypass = false;
		}

		if (s->target_type == MFD_TARGET_FILTER_PARAM) {
			if (s->source_a_name[0] == '\0' || s->filter_name[0] == '\0' || s->filter_prop[0] == '\0' ||
			    mfd_is_internal_filter(s->filter_name, NULL) || !isfinite(s->filter_from_val) ||
			    !isfinite(s->filter_to_val) ||
			    fabs(s->filter_to_val - s->filter_from_val) < MFD_FILTER_MIN_RANGE_SPAN) {
				obs_log(LOG_WARNING,
					"Slot '%s' had malformed filter_param fields; downgrading to unassigned",
					s->id);
				clear_slot_filter_fields(s);
				s->target_type = MFD_TARGET_UNASSIGNED;
			}
		} else {
			clear_slot_filter_fields(s);
			if (s->target_type != MFD_TARGET_TBAR && s->target_type != MFD_TARGET_MIX_MASTER &&
			    s->source_a_name[0] == '\0' && s->source_b_name[0] == '\0') {
				s->target_type = MFD_TARGET_UNASSIGNED;
			}
		}

		long long raw_curve = obs_data_get_int(item, "curve");
		if (raw_curve < (long long)MFD_CURVE_DIP_FREE || raw_curve > (long long)MFD_CURVE_CUT) {
			s->curve = MFD_CURVE_DIP_FREE;
		} else {
			s->curve = (enum mfd_crossfade_curve)raw_curve;
		}
		s->audio_follows_video = obs_data_get_bool(item, "afv");
		s->invert = obs_data_get_bool(item, "invert");
		/* Only strips 1..8 follow the scene on air, and only as Show or hide a layer (or empty) strips. */
		s->auto_follow = obs_data_get_bool(item, "auto_follow") && slot_idx >= 1 &&
				 slot_idx <= (int)MFD_VJ_STRIPS &&
				 (s->target_type == MFD_TARGET_UNASSIGNED || s->target_type == MFD_TARGET_SINGLE_LAYER);
		obs_data_release(item);
	}
	obs_data_array_release(arr);

	/* Deterministic 0..17 single-writer resolution (first valid owner wins) + runtime init */
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		struct mfd_fader_slot *s = &eng->fader_slots[i];
		if (s->enabled && s->target_type == MFD_TARGET_FILTER_PARAM) {
			for (int j = 0; j < i; ++j) {
				const struct mfd_fader_slot *earlier = &eng->fader_slots[j];
				if (earlier->enabled && earlier->target_type == MFD_TARGET_FILTER_PARAM &&
				    strcmp(earlier->source_a_name, s->source_a_name) == 0 &&
				    strcmp(earlier->filter_name, s->filter_name) == 0 &&
				    strcmp(earlier->filter_prop, s->filter_prop) == 0) {
					obs_log(LOG_WARNING,
						"Persisted slot '%s' conflicted with earlier owner '%s' on (%s, %s, %s); downgrading '%s' to unassigned",
						s->id, earlier->id, s->source_a_name, s->filter_name, s->filter_prop,
						s->id);
					clear_slot_filter_fields(s);
					s->target_type = MFD_TARGET_UNASSIGNED;
					break;
				}
			}
		}

		float init_val = 0.0f;
		if (s->target_type == MFD_TARGET_SINGLE_LAYER || s->target_type == MFD_TARGET_AUDIO_VOLUME) {
			init_val = 1.0f;
		} else if (s->target_type == MFD_TARGET_FILTER_PARAM) {
			init_val = query_live_filter_unmapped_deflection_locked(eng, s);
		}
		init_slot_runtime(eng, s, init_val);
	}

	mfd_engine_bump_slot_rev(eng, -1);
	mfd_engine_reevaluate_slots(eng, false);
}

static void on_save_callback(obs_data_t *save_data, bool saving, void *private_data)
{
	struct mfd_engine *eng = private_data;
	if (!eng || !save_data) {
		return;
	}

	mfd_engine_lock(eng);
	if (saving) {
#ifdef MFD_ENABLE_SELF_TEST
		/* only a build with the self-test harness looks at this (H-02): an ordinary build always saves */
		const char *self_test_env = getenv("MFD_SELF_TEST");
		if (self_test_env && strcmp(self_test_env, "1") == 0) {
			mfd_engine_unlock(eng);
			return;
		}
#endif
		obs_data_t *obj = obs_data_create();
		serialize_slots(eng, obj);
		/* Sources punched by the desk right now: the scene saved with them holds the punch, so the source's own
		 * blend goes with it, to be put back when the collection loads again. */
		obs_data_array_t *punches = obs_data_array_create();
		for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
			if (eng->vj_home_key[i][0] != '\0' && eng->vj.current[i] != eng->vj.home[i]) {
				obs_data_t *p = obs_data_create();
				obs_data_set_string(p, "key", eng->vj_home_key[i]);
				obs_data_set_string(p, "home", mfd_vj_blend_id(eng->vj.home[i]));
				obs_data_array_push_back(punches, p);
				obs_data_release(p);
			}
		}
		obs_data_set_array(obj, "vj_punches", punches);
		obs_data_array_release(punches);
		obs_data_set_obj(save_data, MFD_SAVE_DATA_KEY, obj);
		obs_data_release(obj);
	} else {
		obs_data_t *obj = obs_data_get_obj(save_data, MFD_SAVE_DATA_KEY);
		eng->vj_load_restore_count = 0;
		obs_data_array_t *punches = obj ? obs_data_get_array(obj, "vj_punches") : NULL;
		for (size_t k = 0;
		     punches && k < obs_data_array_count(punches) && eng->vj_load_restore_count < (int)MFD_VJ_STRIPS;
		     ++k) {
			obs_data_t *p = obs_data_array_item(punches, k);
			enum mfd_vj_blend home = MFD_VJ_BLEND_NORMAL;
			const char *key = p ? obs_data_get_string(p, "key") : NULL;
			if (key && strchr(key, '\n') && mfd_vj_blend_from_id(obs_data_get_string(p, "home"), &home)) {
				int n = eng->vj_load_restore_count++;
				snprintf(eng->vj_load_restore_key[n], sizeof(eng->vj_load_restore_key[n]), "%s", key);
				eng->vj_load_restore_home[n] = home;
			}
			obs_data_release(p);
		}
		obs_data_array_release(punches);
		if (obj) {
			deserialize_slots(eng, obj, eng->user_midi_layout_known);
			if (slots_differ_from_apc40_defaults(eng)) {
				eng->first_run_offer_dismissed = true;
			}
			obs_data_release(obj);
			mfd_engine_notify_state_changed(eng);
		}
	}
	mfd_engine_unlock(eng);
}

void mfd_engine_collection_save_callback_for_test(struct mfd_engine *eng, obs_data_t *save_data, bool saving)
{
	on_save_callback(save_data, saving, eng);
}

bool mfd_engine_export_config_file(struct mfd_engine *eng, const char *filepath)
{
	if (!eng || !filepath || filepath[0] == '\0') {
		return false;
	}
	mfd_engine_lock(eng);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "device_name", eng->configured_device_name);
	obs_data_set_string(data, "active_profile", effective_profile_name(eng));
	obs_data_set_double(data, "smooth_tau_ms", (double)eng->smooth_tau_ms);
	obs_data_set_bool(data, "tbar_bidirectional_flip", eng->tbar_bidirectional_flip);
	obs_data_set_bool(data, "first_run_offer_dismissed", eng->first_run_offer_dismissed);
	obs_data_set_bool(data, "first_run_layout_applied", eng->first_run_layout_applied);
	obs_data_set_bool(data, "stream_deck_companion_dismissed", eng->stream_deck_companion_dismissed);
	obs_data_set_bool(data, "vj_buttons_enabled", !eng->vj_disabled);
	obs_data_set_bool(data, "desk_video_controls", eng->desk_video_controls);
	obs_data_set_bool(data, "desk_dock_shown", eng->desk_dock_shown);
	obs_data_set_int(data, "vj_fade_ms", vj_effective_fade_ms(eng));
	obs_data_set_bool(data, "vj_autofill_done", eng->vj_autofill_done);
	obs_data_array_t *punch = obs_data_array_create();
	for (int i = 0; i < (int)MFD_VJ_STRIPS; ++i) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "blend", mfd_vj_blend_id(eng->vj.punch[i]));
		obs_data_array_push_back(punch, item);
		obs_data_release(item);
	}
	obs_data_set_array(data, "vj_punch", punch);
	obs_data_array_release(punch);
	serialize_calibrations(eng, data);
	serialize_slots(eng, data);
	mfd_engine_unlock(eng);

	bool ok = obs_data_save_json_safe(data, filepath, "tmp", "bak");
	obs_data_release(data);
	return ok;
}

/* R-FEEL-1: the smoothing time constant is 0-250 ms (the dialog's range); a hand-edited file may hold anything. */
static float clamp_smooth_tau_ms(float tau_ms)
{
	if (!isfinite(tau_ms) || tau_ms <= 0.0f) {
		return 0.0f;
	}
	return (tau_ms > 250.0f) ? 250.0f : tau_ms;
}

/* user_file: a file the user picked in Import. It is read without the ".bak" recovery (which would rename a
 * neighbouring "<file>.bak" over the user's own file when it does not parse), must look like a Midi Fadester
 * configuration, and leaves the port and the one-time notices alone when it does not name them. */
static bool import_config_file_impl(struct mfd_engine *eng, const char *filepath, bool user_file)
{
	if (!eng || !filepath || filepath[0] == '\0') {
		return false;
	}
	obs_data_t *data = user_file ? obs_data_create_from_json_file(filepath)
				     : obs_data_create_from_json_file_safe(filepath, "bak");
	if (!data) {
		return false;
	}
	if (user_file && !obs_data_has_user_value(data, "slots") && !obs_data_has_user_value(data, "device_name") &&
	    !obs_data_has_user_value(data, "active_profile")) {
		obs_data_release(data);
		return false;
	}
	struct mfd_midi_port_list ports;
	midi_port_list_read(&ports); /* before the lock (A-02) */
	mfd_engine_lock(eng);
	cancel_row_learn_locked(eng);
	const char *dev = obs_data_get_string(data, "device_name");
	if (dev && (!user_file || dev[0] != '\0')) {
		snprintf(eng->configured_device_name, sizeof(eng->configured_device_name), "%s", dev);
	}
	const char *prof = obs_data_get_string(data, "active_profile");
	if (prof && prof[0] != '\0') {
		snprintf(eng->active_profile_name, sizeof(eng->active_profile_name), "%s", prof);
	}
	if (obs_data_has_user_value(data, "smooth_tau_ms")) {
		eng->smooth_tau_ms = clamp_smooth_tau_ms((float)obs_data_get_double(data, "smooth_tau_ms"));
	}
	if (obs_data_has_user_value(data, "tbar_bidirectional_flip")) {
		eng->tbar_bidirectional_flip = obs_data_get_bool(data, "tbar_bidirectional_flip");
	}
	if (obs_data_has_user_value(data, "first_run_offer_dismissed")) {
		eng->first_run_offer_dismissed = obs_data_get_bool(data, "first_run_offer_dismissed");
	} else if (!user_file) {
		/* Upgrading from 0.1.x global.json without first_run_offer_dismissed: never prompt to reset */
		eng->first_run_offer_dismissed = true;
	}
	if (user_file) {
		/* A file the user picked in Import is a set-up they chose: whatever it says (an export from a new install
		 * says "not yet"), the first-run layout is never applied on top of it, so Import never gives Master a job
		 * by surprise. */
		eng->first_run_layout_applied = true;
	} else if (obs_data_has_user_value(data, "first_run_layout_applied")) {
		eng->first_run_layout_applied = obs_data_get_bool(data, "first_run_layout_applied");
	} else {
		/* The plugin's own settings from before this key: this is not a first run, whatever the old offer's
		 * state. An update never gives Master a job by itself, in the middle of someone's show set-up; the
		 * notice still shows while the old offer was never answered, and leaves Master out of its sentence. */
		eng->first_run_layout_applied = true;
	}
	if (obs_data_has_user_value(data, "stream_deck_companion_dismissed")) {
		eng->stream_deck_companion_dismissed = obs_data_get_bool(data, "stream_deck_companion_dismissed");
	} else if (!user_file) {
		/* Existing users have not seen the companion card, so show it once after upgrading. */
		eng->stream_deck_companion_dismissed = false;
	}
	if (obs_data_has_user_value(data, "vj_buttons_enabled")) {
		eng->vj_disabled = !obs_data_get_bool(data, "vj_buttons_enabled");
	}
	if (obs_data_has_user_value(data, "desk_video_controls")) {
		eng->desk_video_controls = obs_data_get_bool(data, "desk_video_controls");
	} else if (!user_file) {
		/* The plugin's own settings from before this key (0.9.0 and older showed the rows always): keep them, so
		 * an update never takes a control away from a dock in use. A new install has no file and starts without. */
		eng->desk_video_controls = true;
	}
	if (user_file) {
		/* Import never opens the dock by surprise: a file without the key counts as shown, and a file that says
		 * "not shown yet" does not undo a dock this installation has already shown. */
		if (!obs_data_has_user_value(data, "desk_dock_shown") || obs_data_get_bool(data, "desk_dock_shown")) {
			eng->desk_dock_shown = true;
		}
	} else if (obs_data_has_user_value(data, "desk_dock_shown")) {
		eng->desk_dock_shown = obs_data_get_bool(data, "desk_dock_shown");
	} else {
		/* settings from before this key: the user has lived with the dock where it is; never open it for them */
		eng->desk_dock_shown = true;
	}
	if (obs_data_has_user_value(data, "vj_fade_ms")) {
		long long ms = obs_data_get_int(data, "vj_fade_ms");
		eng->vj_fade_ms = (ms < 50 || ms > 20000) ? 0 : (int)ms;
	}
	if (user_file) {
		/* A layout imported by hand keeps its empty strips empty. */
		eng->vj_autofill_done = true;
	} else if (obs_data_has_user_value(data, "vj_autofill_done")) {
		eng->vj_autofill_done = obs_data_get_bool(data, "vj_autofill_done");
	}
	obs_data_array_t *punch = obs_data_get_array(data, "vj_punch");
	if (punch) {
		for (size_t i = 0; i < obs_data_array_count(punch) && i < MFD_VJ_STRIPS; ++i) {
			obs_data_t *item = obs_data_array_item(punch, i);
			enum mfd_vj_blend b;
			if (item && mfd_vj_blend_from_id(obs_data_get_string(item, "blend"), &b)) {
				mfd_vj_pads_set_punch(&eng->vj, (int)i, b);
			}
			obs_data_release(item);
		}
		obs_data_array_release(punch);
	}
	deserialize_calibrations(eng, data);
	deserialize_slots(eng, data, false);
	if (obs_data_has_user_value(data, "slots")) {
		eng->user_midi_layout_known =
			true; /* from here a scene collection no longer changes addresses (B-04) */
	}
	refresh_apc40_detected_locked(eng, &ports);
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
	obs_data_release(data);
	return true;
}

bool mfd_engine_import_config_file(struct mfd_engine *eng, const char *filepath)
{
	return import_config_file_impl(eng, filepath, false);
}

/* Dialog Import: load the file, then open the port it names. Layout and bindings stay as imported. */
bool mfd_engine_import_config_file_and_reopen(struct mfd_engine *eng, const char *filepath)
{
	if (!import_config_file_impl(eng, filepath, true)) {
		return false;
	}
	mfd_engine_lock(eng);
	const bool same_port = eng->midi_dev.is_open && eng->configured_device_name[0] != '\0' &&
			       strcmp(eng->configured_device_name, eng->midi_dev.opened_name) == 0;
	if (same_port) {
		/* No reopen: the imported layout still starts with clean pairing history. */
		mfd_midi_parser_reset_cc_state(&eng->midi_parser);
	}
	mfd_engine_unlock(eng);
	if (!same_port) {
		(void)mfd_engine_reconnect_midi(eng, NULL); /* NULL keeps the imported configured_device_name */
	}
	return true;
}

bool mfd_engine_save_global_config(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	if (!obs_initialized()) {
		return true;
	}
	char *dir_path = obs_module_config_path("");
	if (dir_path) {
		os_mkdirs(dir_path);
		bfree(dir_path);
	}
	char *cfg_path = obs_module_config_path("global.json");
	if (!cfg_path) {
		return false;
	}
	bool ok = mfd_engine_export_config_file(eng, cfg_path);
	bfree(cfg_path);
	if (ok) {
		mfd_engine_lock(eng);
		if (eng->frontend_finished_loading) {
			/* The settings file now holds this session's addresses. Only once OBS has loaded: an installation
			 * with no settings file yet first reads its addresses from the scene collection (B-04). */
			eng->user_midi_layout_known = true;
		}
		mfd_engine_unlock(eng);
	}
	return ok;
}

static bool load_global_config_if_present(struct mfd_engine *eng)
{
	if (!eng) {
		return false;
	}
	char *cfg_path = obs_module_config_path("global.json");
	if (!cfg_path) {
		return false;
	}
	bool loaded = false;
	if (os_file_exists(cfg_path)) {
		loaded = mfd_engine_import_config_file(eng, cfg_path);
	}
	bfree(cfg_path);
	return loaded;
}

bool mfd_engine_reconnect_midi(struct mfd_engine *eng, const char *device_name)
{
	if (!eng) {
		return false;
	}
	/* The port is closed and opened with no engine lock held (A-02): the video tick does not wait for the MIDI
	 * driver. Call with no engine lock held. */
	struct mfd_winmm_device old;
	struct mfd_winmm_device fresh;
	struct mfd_midi_port_list ports;
	char name[MFD_MIDI_DEV_NAME_MAX];

	pthread_mutex_lock(&g_midi_io_mutex);
	mfd_engine_lock(eng);
	if (device_name) {
		snprintf(eng->configured_device_name, sizeof(eng->configured_device_name), "%s", device_name);
	}
	snprintf(name, sizeof(name), "%s", eng->configured_device_name);
	midi_detach_handles_locked(eng, &old);
	mfd_engine_unlock(eng);

	bool ok = midi_reopen_unlocked(eng, &old, name, &fresh);
	midi_port_list_read(&ports);

	mfd_engine_lock(eng);
	midi_install_device_locked(eng, &fresh);
	midi_io_done_locked(eng);
	mfd_midi_parser_reset_cc_state(&eng->midi_parser);
	if (ok) {
		eng->midi_disconnect_handled = false;
	}
	refresh_apc40_detected_locked(eng, &ports);
	mfd_engine_notify_state_changed(eng);
	mfd_engine_unlock(eng);
	pthread_mutex_unlock(&g_midi_io_mutex);
	return ok;
}

void mfd_engine_set_smooth_tau_ms(struct mfd_engine *eng, float tau_ms)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->smooth_tau_ms = clamp_smooth_tau_ms(tau_ms);
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		mfd_smoother_set_tau_ms(&eng->fader_slots[i].smoother, eng->smooth_tau_ms);
	}
	/* a value, not a change of what the strips are: the settings window refreshes without rebuilding its Advanced
	 * table on every step of the Smoothing box (F-06) */
	mfd_engine_mark_ui_dirty(eng, false);
	mfd_engine_unlock(eng);
}

void mfd_engine_set_tbar_bidirectional_flip(struct mfd_engine *eng, bool flip)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	eng->tbar_bidirectional_flip = flip;
	for (int i = 0; i < MFD_MAX_FADER_SLOTS; ++i) {
		if (eng->fader_slots[i].target_type == MFD_TARGET_TBAR) {
			eng->fader_slots[i].takeover.bidirectional_flip = flip;
		}
	}
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_start_learn(struct mfd_engine *eng, int slot_index)
{
	if (!eng || slot_index < 0 || slot_index >= MFD_MAX_FADER_SLOTS) {
		return;
	}
	mfd_engine_lock(eng);
	eng->setup_session.learning = false;
	eng->learning_slot_index = slot_index;
	mfd_midi_parser_start_learn(&eng->midi_parser);
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_cancel_learn(struct mfd_engine *eng)
{
	if (!eng) {
		return;
	}
	mfd_engine_lock(eng);
	cancel_row_learn_locked(eng);
	mfd_engine_mark_ui_dirty(eng, true);
	mfd_engine_unlock(eng);
}

void mfd_engine_poll_midi_health_on_ui_thread(struct mfd_engine *eng)
{
	if (!eng || !eng->initialized) {
		return;
	}

	/*
	 * Everything that asks the MIDI driver (listing the ports, closing, opening) runs with no engine lock held
	 * (A-02), so the video tick never waits for it; the engine lock is taken only to read and store state. Call
	 * with no engine lock held.
	 */
	struct mfd_winmm_device work;
	struct mfd_winmm_device fresh;
	struct mfd_midi_port_list ports;
	char name[MFD_MIDI_DEV_NAME_MAX];

	pthread_mutex_lock(&g_midi_io_mutex);
	mfd_engine_lock(eng);
	work = eng->midi_dev;
	snprintf(name, sizeof(name), "%s", eng->configured_device_name);
	mfd_engine_unlock(eng);

	if (work.is_open) {
		bool healthy = mfd_winmm_is_present(&work) &&
			       !os_atomic_load_bool(&eng->midi_dev.device_closed_unexpectedly);
		if (!healthy) {
			midi_port_list_read(&ports);
			mfd_engine_lock(eng);
			midi_detach_handles_locked(eng, &work);
			eng->midi_dev.is_open = false;
			eng->midi_dev.out_name[0] = '\0';
			eng->midi_dev.error_kind = MFD_WINMM_ERR_DISCONNECTED;
			eng->midi_dev.error_operation = MFD_WINMM_OP_NONE;
			eng->midi_dev.error_code = 0U;
			snprintf(eng->midi_dev.error_device_name, sizeof(eng->midi_dev.error_device_name), "%s",
				 work.opened_name);
			snprintf(eng->midi_dev.last_error, sizeof(eng->midi_dev.last_error),
				 "MIDI device '%s' was disconnected. Reconnect USB cable and click Reconnect.",
				 work.opened_name);
			obs_log(LOG_WARNING, "%s", eng->midi_dev.last_error);
			abort_open_tbar_slots_on_midi_disconnect_locked(eng);
			vj_release_all_locked(eng);
			refresh_apc40_detected_locked(eng, &ports);
			mfd_engine_notify_state_changed(eng);
			mfd_engine_unlock(eng);
			mfd_winmm_close(&work);
			os_atomic_store_bool(&eng->midi_dev.device_closed_unexpectedly, false);
			mfd_engine_lock(eng);
			midi_io_done_locked(eng);
			mfd_engine_unlock(eng);
		}
		pthread_mutex_unlock(&g_midi_io_mutex);
		return;
	}

	/* Event-driven hot-plug rescan on WM_DEVICECHANGE / Reconnect (R-MIDI-2). The rescan always reads the list,
	 * also before OBS is up. */
	ports.valid = true;
	ports.count = mfd_winmm_enumerate(ports.list, MFD_MAX_MIDI_DEVICES);

	bool candidate_present = false;
	if (ports.count == 0U) {
		candidate_present = false;
	} else if (name[0] != '\0') {
		for (size_t i = 0U; i < ports.count; ++i) {
			if (strcmp(ports.list[i].name, name) == 0) {
				candidate_present = true;
				break;
			}
		}
	} else {
		for (size_t i = 0U; i < ports.count; ++i) {
			if (ports.list[i].is_apc40_mk2) {
				candidate_present = true;
				break;
			}
		}
		if (!candidate_present && work.error_kind != MFD_WINMM_ERR_BUSY_ALLOCATED) {
			candidate_present = true;
		}
	}

	bool opened = false;
	if (candidate_present) {
		work.handle = NULL; /* nothing was open: nothing to close */
		work.out_handle = NULL;
		opened = midi_reopen_unlocked(eng, &work, name, &fresh);
	}

	mfd_engine_lock(eng);
	if (candidate_present) {
		midi_install_device_locked(eng, &fresh); /* also a failed open: its error is what the UI shows */
		mfd_midi_parser_reset_cc_state(&eng->midi_parser);
	}
	if (opened) {
		eng->midi_disconnect_handled = false;
		mfd_engine_notify_state_changed(eng);
	}
	ports.valid = obs_initialized(); /* as before: the "an APC40 is plugged in" flag is only kept once OBS is up */
	refresh_apc40_detected_locked(eng, &ports);
	mfd_engine_unlock(eng);
	pthread_mutex_unlock(&g_midi_io_mutex);
}

bool mfd_engine_init(void)
{
	mfd_tbar_apply_fn saved_tbar_driver = g_engine.tbar_driver;
	mfd_tbar_unlatch_fn saved_tbar_unlatch = g_engine.tbar_unlatch;
	mfd_tbar_deferred_fn saved_tbar_deferred = g_engine.tbar_deferred;
	memset(&g_engine, 0, sizeof(g_engine));
	if (!mfd_engine_init_mutex(&g_engine)) {
		return false;
	}
	g_engine.tbar_driver = saved_tbar_driver;
	g_engine.tbar_unlatch = saved_tbar_unlatch;
	g_engine.tbar_deferred = saved_tbar_deferred;
	mfd_midi_queue_init(&g_engine.midi_queue);
	mfd_midi_parser_init(&g_engine.midi_parser);
	mfd_winmm_init(&g_engine.midi_dev, &g_engine.midi_queue);

	snprintf(g_engine.active_profile_name, sizeof(g_engine.active_profile_name), "%s", MFD_DEFAULT_PROFILE_NAME);
	g_engine.smooth_tau_ms = MFD_DEFAULT_SMOOTH_TAU_MS;
	g_engine.tbar_bidirectional_flip = true;
	g_engine.learning_slot_index = -1;
	g_engine.last_moved_slot_index = -1;
	g_engine.first_run_offer_dismissed = false;

	mfd_engine_load_apc40_defaults(&g_engine);
	bool had_global_cfg = load_global_config_if_present(&g_engine);

	if (mfd_winmm_open_by_name(&g_engine.midi_dev, g_engine.configured_device_name)) {
		g_engine.midi_disconnect_handled = false;
	}
	if (!had_global_cfg && g_engine.midi_dev.is_open) {
		enum mfd_controller_profile detected = mfd_profile_match_device_name(g_engine.midi_dev.opened_name);
		if (detected != MFD_PROFILE_GENERIC && detected != MFD_PROFILE_APC40_MK2) {
			mfd_engine_load_profile_defaults(&g_engine, detected);
		}
	}
	{
		struct mfd_midi_port_list init_ports;
		midi_port_list_read(&init_ports);
		refresh_apc40_detected_locked(&g_engine, &init_ports);
	}
	refresh_frontend_cache_on_ui_thread_locked(&g_engine);

	connect_obs_signals(&g_engine);
	mfd_engine_reevaluate_slots(&g_engine, false);
	obs_frontend_add_event_callback(on_frontend_event, &g_engine);
	obs_frontend_add_save_callback(on_save_callback, &g_engine);
	mfd_blackout_init();
	obs_add_tick_callback(mfd_engine_video_tick, &g_engine);

	g_engine.initialized = true;
	return true;
}

void mfd_engine_shutdown(void)
{
	if (!g_engine.initialized) {
		return;
	}
	mfd_engine_lock(&g_engine);
	g_engine.initialized = false;
	g_engine.ui_notify_fn = NULL;
	g_engine.ui_notify_user_data = NULL;
	mfd_engine_cancel_setup_session(&g_engine);
	mfd_engine_restore_all_bypass_leases(&g_engine);
	disconnect_obs_signals(&g_engine);
	/* Exit already disconnected the signals; whatever got connected since must not outlive the module. */
	disconnect_all_tracked_signals_locked(&g_engine);
	mfd_engine_discard_scene_graph_changes(&g_engine);
	mfd_engine_unlock(&g_engine);

	obs_remove_tick_callback(mfd_engine_video_tick, &g_engine);
	mfd_blackout_shutdown();
	if (!g_engine.frontend_callbacks_removed) { /* already gone when OBS sent EXIT */
		g_engine.frontend_callbacks_removed = true;
		obs_frontend_remove_save_callback(on_save_callback, &g_engine);
		obs_frontend_remove_event_callback(on_frontend_event, &g_engine);
	}

	mfd_engine_save_global_config(&g_engine);
	mfd_engine_lock(&g_engine);
	vj_leds_off_locked(&g_engine);
	mfd_engine_unlock(&g_engine);
	mfd_winmm_close(&g_engine.midi_dev);
	mfd_engine_destroy_mutex(&g_engine);
}
