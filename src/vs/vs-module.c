/*
Visual Sidechain, part of Midi Fadester since 0.9.0
Copyright (C) 2026 Jeffrey A. Brick
SPDX-License-Identifier: GPL-2.0-or-later

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <string.h>

#include "core/tempo.h"
#include "obs/modulator-filter.h"
#include "obs/shader-filter.h"
#include "obs/vendor-api.h"
#include "tubester/deck-filter.h"
#include "tubester/listener.h"
#include "vs-module.h"
#include <stdio.h>

static obs_hotkey_id g_panic_hotkey_id = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_restore_hotkey_id = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_tap_tempo_hotkey_id = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_lock_bpm_hotkey_id = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id g_resync_downbeat_hotkey_id = OBS_INVALID_HOTKEY_ID;

/* The walk only collects weak references while obs_enum_all_sources holds the sources lock; the filters are acted
 * on after it has returned. Acting inside it would take each filter's own mutex and the graphics lock under the
 * sources lock, and libobs can take those in the other order (a source destroyed while rendering takes the sources
 * lock inside the graphics lock). */
struct vs_filter_walk {
	obs_weak_source_t **items;
	size_t num;
	size_t cap;
};

static bool collect_vs_filters_cb(void *param, obs_source_t *source)
{
	struct vs_filter_walk *walk = param;
	const char *id = source ? obs_source_get_unversioned_id(source) : NULL;
	if (!id ||
	    (strcmp(id, "visual_sidechain_shader_filter") != 0 && strcmp(id, "visual_sidechain_modulator_filter") != 0))
		return true;
	if (walk->num == walk->cap) {
		walk->cap = walk->cap ? walk->cap * 2 : 16;
		walk->items = brealloc(walk->items, walk->cap * sizeof(*walk->items));
	}
	walk->items[walk->num++] = obs_source_get_weak_source(source);
	return true;
}

static void walk_vs_filters(bool panic)
{
	struct vs_filter_walk walk = {0};
	obs_enum_all_sources(collect_vs_filters_cb, &walk);
	for (size_t i = 0; i < walk.num; i++) {
		obs_source_t *source = obs_weak_source_get_source(walk.items[i]);
		obs_weak_source_release(walk.items[i]);
		if (!source)
			continue;
		const char *id = obs_source_get_unversioned_id(source);
		const bool shader = id && strcmp(id, "visual_sidechain_shader_filter") == 0;
		if (panic && shader)
			vs_shader_filter_apply_panic(source);
		else if (panic)
			vs_modulator_filter_apply_panic(source);
		else if (shader)
			vs_shader_filter_apply_restore(source);
		else
			vs_modulator_filter_apply_restore(source);
		obs_source_release(source);
	}
	bfree(walk.items);
}

/* Lock order. libobs calls the hotkey callbacks below with its hotkey lock held (obs->hotkeys.mutex), and
 * obs_enum_all_sources takes the sources lock (obs->data.sources_mutex). The frontend's scene-collection save takes
 * them the other way round: OBSBasic::Save -> obs_save_sources_filtered holds the sources lock for its whole source
 * loop, and obs_save_source -> obs_hotkeys_save_source takes the hotkey lock for every source and filter. A Panic or
 * Restore from obs-websocket's thread (TriggerHotkeyByName) during a save would therefore deadlock OBS. So the
 * callbacks only set or clear the panic flag, which at once makes every Shader Filter pass through and stops every
 * Audio Modulator on its next tick, and these tasks walk the sources on the UI thread. Posted from obs-websocket's
 * thread, a task runs after the hotkey lock is released; from a key press, which the frontend runs on the UI thread,
 * it runs in place, on the save's own thread, so a walk and a save never overlap. */
static void panic_walk_task(void *param)
{
	const bool emit_event = param != NULL;
	/* A Restore since this task was queued wins: the walk always ends matching the flag. */
	if (vs_fader_bridge_is_panic_active()) {
		walk_vs_filters(true);
		obs_log(LOG_INFO,
			"Panic triggered: shader filters set to pass-through and all strobe, flash, and modulation amounts zeroed");
	}
	if (emit_event)
		vs_vendor_api_emit_safety_changed();
}

static void restore_walk_task(void *param)
{
	const bool emit_event = param != NULL;
	/* A Panic since this task was queued wins: the walk always ends matching the flag. */
	if (!vs_fader_bridge_is_panic_active()) {
		walk_vs_filters(false);
		obs_log(LOG_INFO,
			"Restore triggered: shader filters restored from pass-through and pre-panic amounts restored");
	}
	if (emit_event)
		vs_vendor_api_emit_safety_changed();
}

static void schedule_safety_walk(bool panic)
{
	obs_queue_task(OBS_TASK_UI, panic ? panic_walk_task : restore_walk_task, NULL, false);
}

static void on_panic_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	vs_fader_bridge_trigger_panic();
	obs_queue_task(OBS_TASK_UI, panic_walk_task, (void *)(uintptr_t)1, false);
}

static void on_restore_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	vs_fader_bridge_trigger_restore();
	obs_queue_task(OBS_TASK_UI, restore_walk_task, (void *)(uintptr_t)1, false);
}

static void tempo_emit_task(void *param)
{
	UNUSED_PARAMETER(param);
	struct vs_tempo_snapshot snap;
	vs_tempo_global_get_snapshot(&snap);
	vs_fader_bridge_apply_tempo_snapshot(&snap);
	vs_vendor_api_emit_signals_changed();
}

static void on_tap_tempo_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct vs_tempo_snapshot snap;
	if (vs_tempo_global_tap((double)os_gettime_ns() * 1e-9, &snap))
		obs_queue_task(OBS_TASK_UI, tempo_emit_task, NULL, false);
}

static void on_lock_bpm_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct vs_tempo_snapshot snap;
	vs_tempo_global_toggle_locked(&snap);
	obs_queue_task(OBS_TASK_UI, tempo_emit_task, NULL, false);
}

static void on_resync_downbeat_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct vs_tempo_snapshot snap;
	vs_tempo_global_resync_downbeat(&snap);
	obs_queue_task(OBS_TASK_UI, tempo_emit_task, NULL, false);
}

/* Settings > Hotkeys saves every frontend hotkey into the profile's [Hotkeys] section by name, as
 * {"bindings":[...]}, but OBS reads back only its own frontend hotkeys (OBSBasic::CreateHotkeys). Without this a key
 * set for Panic worked until OBS restarted, then did nothing. Read ours from the same place, the same way, once the
 * frontend has loaded and again whenever the profile changes. obs_hotkey_load clears the old bindings first, so a
 * profile with no entry leaves the hotkey unbound, as OBS does for its own. */
static void load_frontend_hotkey(config_t *profile, obs_hotkey_id id, const char *name)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return;
	const char *json = profile ? config_get_string(profile, "Hotkeys", name) : NULL;
	obs_data_t *data = (json && *json) ? obs_data_create_from_json(json) : NULL;
	obs_data_array_t *bindings = data ? obs_data_get_array(data, "bindings") : NULL;
	obs_hotkey_load(id, bindings);
	obs_data_array_release(bindings);
	obs_data_release(data);
}

static void load_frontend_hotkeys(void)
{
	config_t *profile = obs_frontend_get_profile_config();
	load_frontend_hotkey(profile, g_panic_hotkey_id, "visual_sidechain_panic");
	load_frontend_hotkey(profile, g_restore_hotkey_id, "visual_sidechain_restore");
	load_frontend_hotkey(profile, g_tap_tempo_hotkey_id, "visual_sidechain_tap_tempo");
	load_frontend_hotkey(profile, g_lock_bpm_hotkey_id, "visual_sidechain_lock_bpm");
	load_frontend_hotkey(profile, g_resync_downbeat_hotkey_id, "visual_sidechain_resync_downbeat");
}

static void on_frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_PROFILE_CHANGED)
		load_frontend_hotkeys();
}

static bool g_vs_active;

bool vs_module_load(void)
{
	/* Visual Sidechain lives inside Midi Fadester since 0.9.0. A copy of the old plugin would register the same
	 * filters, hotkeys, procs and vendor and act on these filters with its own state (and its own struct layouts), so
	 * nothing of Visual Sidechain is registered twice: if the old plugin is already loaded, this part stays off and
	 * says where it is; otherwise the old plugin is told not to load at all. */
	obs_module_t *old = obs_get_module("visual-sidechain");
	if (old) {
		const char *path = obs_get_module_binary_path(old);
		vs_module_set_conflict_path(path ? path : "visual-sidechain");
		obs_log(LOG_ERROR,
			"Visual Sidechain is part of Midi Fadester now, but the old Visual Sidechain plugin is installed (%s): "
			"its effects run from the old copy. Remove it and restart OBS.",
			vs_module_conflict_path());
		g_vs_active = false;
		return true;
	}
	obs_add_disabled_module("visual-sidechain");
	g_vs_active = true;
	vs_vendor_api_set_safety_walk_scheduler(schedule_safety_walk);
	vs_register_modulator_filter();
	vs_register_shader_filter();
	tub_register_deck_filter();
	g_panic_hotkey_id = obs_hotkey_register_frontend("visual_sidechain_panic", obs_module_text("Hotkey.Panic"),
							 on_panic_hotkey, NULL);
	g_restore_hotkey_id = obs_hotkey_register_frontend("visual_sidechain_restore",
							   obs_module_text("Hotkey.Restore"), on_restore_hotkey, NULL);
	g_tap_tempo_hotkey_id = obs_hotkey_register_frontend(
		"visual_sidechain_tap_tempo", obs_module_text("Hotkey.TapTempo"), on_tap_tempo_hotkey, NULL);
	g_lock_bpm_hotkey_id = obs_hotkey_register_frontend(
		"visual_sidechain_lock_bpm", obs_module_text("Hotkey.LockBpm"), on_lock_bpm_hotkey, NULL);
	g_resync_downbeat_hotkey_id = obs_hotkey_register_frontend("visual_sidechain_resync_downbeat",
								   obs_module_text("Hotkey.ResyncDownbeat"),
								   on_resync_downbeat_hotkey, NULL);
	obs_frontend_add_event_callback(on_frontend_event, NULL);
	obs_log(LOG_INFO, "Visual Sidechain effects loaded (part of Midi Fadester %s)", PLUGIN_VERSION);
	return true;
}

void vs_module_post_load(void)
{
	if (!g_vs_active)
		return;
	vs_vendor_api_register();
}

void vs_module_unload(void)
{
	if (!g_vs_active)
		return;
	g_vs_active = false;
	obs_frontend_remove_event_callback(on_frontend_event, NULL);
	if (g_panic_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_panic_hotkey_id);
		g_panic_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	if (g_restore_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_restore_hotkey_id);
		g_restore_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	if (g_tap_tempo_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_tap_tempo_hotkey_id);
		g_tap_tempo_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	if (g_lock_bpm_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_lock_bpm_hotkey_id);
		g_lock_bpm_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	if (g_resync_downbeat_hotkey_id != OBS_INVALID_HOTKEY_ID) {
		obs_hotkey_unregister(g_resync_downbeat_hotkey_id);
		g_resync_downbeat_hotkey_id = OBS_INVALID_HOTKEY_ID;
	}
	vs_vendor_api_shutdown(); /* unregisters the vendor requests before anything they use goes away */
	vs_fader_bridge_detach();
	/* Closes the report socket and joins its thread. Decks are usually gone by now (the frontend clears its scenes
	 * first), but libobs unloads modules before it destroys any source still alive, so a deck may be destroyed
	 * after this; it then unregisters under the listener's lock, which is never destroyed, and finds nothing. */
	tub_listener_stop();
	obs_log(LOG_INFO, "Visual Sidechain effects unloaded");
}
