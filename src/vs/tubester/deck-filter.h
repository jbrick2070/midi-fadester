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

#define TUB_DECK_FILTER_ID "tubester_deck_filter"

void tub_register_deck_filter(void);

/* Fills out with { success, api_version: 1, decks: [ { source, filter, deck_id, role, state, tape, speed_percent,
 * tape_pitch, loop, start_seconds, linked, reported?: { current_tape, rate, position, duration, paused, ended,
 * ready_state, error, no_picture } } ] }: every Tape Deck (up to 64) as its page last reported it (R-INT-4).
 * `reported` is present only while `linked` is true; `current_tape` is false while it describes an earlier tape.
 * Any thread. */
void tub_decks_describe(obs_data_t *out);

#ifdef __cplusplus
}
#endif
