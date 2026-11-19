/*
 * Tubester - the loopback listener that receives the deck pages' reports
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
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One listener for the whole plugin, on 127.0.0.1 and a port the system picks, held exclusively (R-LNK-2). It starts
 * with the first deck that registers and stops at module unload. Pages POST their reports as text/plain JSON to
 * /report; each report names its deck, and the deck's callback runs on the listener thread with the parsed report,
 * under the listener's lock: a callback must not call back into the listener. Reports for a deck nobody registered,
 * and bodies that are not reports, are dropped without a word. One request may take at most 1.5 s, so a slow client
 * cannot hold the other decks' reports or the shutdown. Every function here may be called from any thread. */

typedef void (*tub_report_fn)(void *deck, obs_data_t *report, uint64_t now_ns);

/* Registers a deck under its id and starts the listener when it is not running. False when the listener cannot start
 * or the id is taken. */
bool tub_listener_register(const char *deck_id, tub_report_fn fn, void *deck);

/* After this returns, no callback for deck runs or is running. */
void tub_listener_unregister(void *deck);

/* The port in use, or 0 while the listener is not running. */
int tub_listener_port(void);

/* Stops the thread and closes the socket, waiting at most a fraction of a second. Decks still registered are
 * forgotten; unregistering them afterwards is harmless, and a later register starts the listener again. */
void tub_listener_stop(void);

#ifdef __cplusplus
}
#endif
