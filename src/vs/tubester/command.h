/*
 * Tubester - commands to the deck page, written as JSON for javascript_event
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one event name every deck page listens for. obs-browser splices it unescaped into page code, so it is a constant
 * and never comes from settings (R-CTL-2). */
#define TUB_EVENT_NAME "tubester"

/* Each returns a JSON object as a string the caller frees with bfree, or NULL when an argument cannot be sent. The
 * text is checked before it is returned: obs-browser splices it into page code, so it holds no raw line terminator. */
char *tub_command_hello(const char *deck_id, int port, const char *token);
/* Loads a tape at start_seconds; it plays at once when playing is true, else it waits paused there, so a page that
 * reloads keeps a paused deck paused (R-CTL-4). */
char *tub_command_tape(uint64_t gen, const char *file_url, double start_seconds, bool loop, bool playing);
/* Takes the tape out: the page empties its player, so the deck reads Ready with no tape. It carries a new generation,
 * like a tape command, so reports about the old tape stop counting at once. */
char *tub_command_eject(uint64_t gen);
char *tub_command_rate(double rate, bool tape_pitch);
/* Changes looping on the tape that is loaded. The tape command carries loop for a new load; this one is for a change
 * in between, so the tape is not reloaded. */
char *tub_command_loop(bool loop);
/* A seek that reaches a tape still loading is kept until its length is known, then made. */
char *tub_command_seek(double seconds);
/* Plays the loaded tape. A tape that has ended starts again from start_seconds rather than from 0 (D-11). */
char *tub_command_play(double start_seconds);
char *tub_command_pause(void);

/* True when text the writer produced may be spliced into page code: it is one object, and it holds no raw U+2028,
 * U+2029, CR or LF (JSON escapes are fine). It does not parse the text, so it is a guard on the writer's output, not a
 * validator for arbitrary strings. */
bool tub_command_is_safe(const char *json);

#ifdef __cplusplus
}
#endif
