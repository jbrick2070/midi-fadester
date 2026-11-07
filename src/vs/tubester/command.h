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
char *tub_command_tape(uint64_t gen, const char *file_url, double start_seconds, bool loop);
char *tub_command_rate(double rate, bool tape_pitch);
char *tub_command_seek(double seconds);
char *tub_command_play(void);
char *tub_command_pause(void);

/* True when text the writer produced may be spliced into page code: it is one object, and it holds no raw U+2028,
 * U+2029, CR or LF (JSON escapes are fine). It does not parse the text, so it is a guard on the writer's output, not a
 * validator for arbitrary strings. */
bool tub_command_is_safe(const char *json);

#ifdef __cplusplus
}
#endif
