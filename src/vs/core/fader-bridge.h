/*
 * Visual Sidechain - Pure-C Fader Slot & In-Process Signal Bridge
 * Copyright (C) 2026
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

#ifdef __cplusplus
extern "C" {
#endif

#define VS_FADER_SLOT_COUNT 10
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
 * proc_handler, and emits the coalesced `visual_sidechain_signals` signal on
 * the attached signal_handler (returning true when a 30 Hz emission occurred).
 */
bool vs_fader_bridge_on_tick(float dt_sec, const struct vs_audio_frame *frame);

#ifdef __cplusplus
}
#endif
