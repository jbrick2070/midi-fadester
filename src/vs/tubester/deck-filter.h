/*
 * Tubester - the Deck filter: a Browser source becomes a tape deck (REQUIREMENTS section 3)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-data.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Setting names are stable from 0.1 (R-INT-3): other tools write them over obs-websocket. */
#define TUB_SETTING_TAPE "tape"
#define TUB_SETTING_SPEED "speed_percent"
#define TUB_SETTING_TAPE_PITCH "tape_pitch"
#define TUB_SETTING_LOOP "loop"
#define TUB_SETTING_START "start_seconds"
/* the transport (R-CTL-4, D-11): true plays, false pauses */
#define TUB_SETTING_PLAYING "playing"
/* Speed Fader (R-SPD-5): a Fadester slot index 0 to 17 (crossfader, faders 1 to 8, master, FX 1 to 8), or -1 for none,
 * and the speeds at the bottom and the top of the fader */
#define TUB_SETTING_SPEED_FADER "speed_fader_slot"
#define TUB_SETTING_SPEED_FADER_MIN "speed_fader_min_percent"
#define TUB_SETTING_SPEED_FADER_MAX "speed_fader_max_percent"
#define TUB_SPEED_FADER_MIN_DEFAULT 50
#define TUB_SPEED_FADER_MAX_DEFAULT 200
/* Written by the deck itself at each save, not a setting to change: { source_uuid, <hotkey name>: [bindings] }, a copy
 * of the deck's hotkey bindings so they survive a removal and an Undo */
#define TUB_SETTING_HOTKEY_COPY "hotkey_bindings"

#define TUB_DECK_FILTER_ID "tubester_deck_filter"

void tub_register_deck_filter(void);

/* Fills out with { success, api_version: 1, decks: [ { source, filter, deck_id, role, state, tape, speed_percent,
 * effective_speed_percent, speed_fader: { slot, slot_id, min_percent, max_percent, live }, tape_pitch, playing, loop,
 * start_seconds, linked, reported?: { current_tape, rate, position, duration, paused, ended, ready_state, error,
 * no_picture } } ] }: every Tape Deck (up to 64) as its page last reported it (R-INT-4). speed_percent is the Speed
 * setting; effective_speed_percent is the speed the deck asks the page for, the Speed Fader's while it is live.
 * speed_fader.slot is -1 and slot_id "" when no fader is bound; live is true while the bound fader drives the speed
 * (Midi Fadester loaded). `reported` is present only while `linked` is true; `current_tape` is false while it describes
 * an earlier tape. Any thread. */
void tub_decks_describe(obs_data_t *out);

#ifdef __cplusplus
}
#endif
