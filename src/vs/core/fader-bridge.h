/*
 * Visual Sidechain - Pure-C Fader Slot & In-Process Signal Bridge
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "analyzer.h"

#include <callback/proc.h>
#include <callback/signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VS_FADER_SLOT_NONE (-1)
#define VS_FADER_SLOT_COUNT 10
#define VS_SHADER_PARAM_FADER_SLOTS 2
#define VS_SIGNALS_MAX_EVENTS_PER_SEC 30.0f
#define VS_SIGNALS_MIN_INTERVAL_SEC (1.0f / VS_SIGNALS_MAX_EVENTS_PER_SEC)

enum vs_fader_slot_index {
	VS_FADER_CROSSFADER = 0,
	VS_FADER_1 = 1,
	VS_FADER_2 = 2,
	VS_FADER_3 = 3,
	VS_FADER_4 = 4,
	VS_FADER_5 = 5,
	VS_FADER_6 = 6,
	VS_FADER_7 = 7,
	VS_FADER_8 = 8,
	VS_FADER_MASTER = 9,
};

const char *vs_fader_slot_id(int slot_idx);
int vs_fader_slot_from_name(const char *name);

void vs_fader_bridge_reset(void);
void vs_fader_bridge_attach(proc_handler_t *ph, signal_handler_t *sh);
void vs_fader_bridge_detach(void);

bool vs_fader_bridge_is_midi_fadester_loaded(void);
bool vs_fader_bridge_poll_once(void);

bool vs_fader_bridge_set_slot(int slot_idx, float value);
bool vs_fader_bridge_set_slot_by_name(const char *name, float value);
float vs_fader_bridge_get_slot(int slot_idx);
/* Copies a coherent snapshot of all slots under one bridge lock. Unloaded slots read zero. */
void vs_fader_bridge_get_slots(float out_values[VS_FADER_SLOT_COUNT]);

/*
 * Resolves an optional Fadester scale factor in [0.0f, 1.0f]:
 * - Returns 1.0f when `slot_idx == VS_FADER_SLOT_NONE` (< 0, i.e. fader unbound).
 * - Returns `vs_fader_bridge_get_slot(slot_idx)` (0.0f if Midi Fadester is not
 *   loaded, or [0.0f, 1.0f] live fader value) when 0 <= slot_idx < VS_FADER_SLOT_COUNT.
 * - Returns 0.0f for out-of-range positive indices.
 */
float vs_fader_bridge_resolve_scale(int slot_idx);

/*
 * Scales all reactive energy/transient fields of `in_frame` by `clamp01(reactivity_scale)`
 * into `out_frame` while preserving pitch/tempo metadata (`spectral_centroid`, `estimated_bpm`).
 */
void vs_fader_bridge_scale_audio_frame(const struct vs_audio_frame *in_frame, float reactivity_scale,
				       struct vs_audio_frame *out_frame);

/*
 * Evaluates a shader float or integer uniform optionally bound to a Fadester slot.
 * When `slot_idx < 0` (`VS_FADER_SLOT_NONE`), returns `slider_val` unchanged.
 * When `slot_idx >= 0`, maps the fader value [0, 1] linearly across [min_val, max_val]
 * (supporting inverted ranges where min_val > max_val) and clamps to the shader's
 * annotated hard [hard_min, hard_max] limits when `has_hard_min` / `has_hard_max` are set.
 */
float vs_fader_bridge_eval_float_param(int slot_idx, float slider_val, double min_val, double max_val,
				       bool has_hard_min, double hard_min, bool has_hard_max, double hard_max);
int vs_fader_bridge_eval_int_param(int slot_idx, int slider_val, double min_val, double max_val, int step,
				   bool has_hard_min, double hard_min, bool has_hard_max, double hard_max);
float vs_fader_bridge_map_float_param(float value, double min_val, double max_val, bool has_hard_min, double hard_min,
				      bool has_hard_max, double hard_max);
int vs_fader_bridge_map_int_param(float value, double min_val, double max_val, int step, bool has_hard_min,
				  double hard_min, bool has_hard_max, double hard_max);

void vs_fader_bridge_publish_signals(const struct vs_audio_frame *frame);
void vs_fader_bridge_get_signals(struct vs_audio_frame *out_frame);

/*
 * Coalesces outbound `visual_sidechain_signals` and `signals_changed` events
 * to at most 30 events/sec (minimum interval 33.33 ms) and suppresses duplicate
 * emissions when all primary signals are unchanged within epsilon (1e-3).
 */
bool vs_fader_bridge_should_emit_signals(float dt_sec, const struct vs_audio_frame *frame);

/*
 * Runs once per video tick: publishes the latest audio frame, performs at most
 * one round-robin poll of `midi_fadester_get_slot_value` on the attached
 * proc_handler per distinct non-zero `video_frame_ts`, prioritizes Track 1
 * publishers over secondary taps, and emits the coalesced
 * `visual_sidechain_signals` signal at <= 30 Hz (returning true when an
 * emission occurred). Passing `video_frame_ts == 0` treats every call as a
 * distinct frame for standalone unit tests.
 */
bool vs_fader_bridge_on_frame_tick(uint64_t video_frame_ts, float dt_sec, const struct vs_audio_frame *frame,
				   bool is_primary_track);
bool vs_fader_bridge_on_tick(float dt_sec, const struct vs_audio_frame *frame);

/*
 * Panic & Restore hotkey state and parameter-slot helpers:
 * "Visual Sidechain: Panic" immediately sets every strobe/flash/modulation
 * amount to 0.0 (saving pre-panic values once); "Visual Sidechain: Restore"
 * restores them.
 */
struct vs_panic_param_slot {
	char name[64];
	double current_value;
	double saved_value;
	bool saved_valid;
};

bool vs_fader_bridge_is_strobe_or_flash_param(const char *param_name);
void vs_fader_bridge_trigger_panic(void);
void vs_fader_bridge_trigger_restore(void);
bool vs_fader_bridge_is_panic_active(void);
bool vs_fader_bridge_should_shader_passthrough(bool filter_panic_passthrough, bool has_compiled_effect);
void vs_fader_bridge_apply_panic_to_slots(struct vs_panic_param_slot *slots, size_t count);
void vs_fader_bridge_apply_restore_to_slots(struct vs_panic_param_slot *slots, size_t count);
void vs_fader_bridge_apply_panic_to_shader(bool *filter_panic_passthrough, struct vs_panic_param_slot *slots,
					   size_t count);
void vs_fader_bridge_apply_restore_to_shader(bool *filter_panic_passthrough, struct vs_panic_param_slot *slots,
					     size_t count);

#ifdef __cplusplus
}
#endif
