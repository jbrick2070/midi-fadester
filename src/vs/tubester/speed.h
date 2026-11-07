/*
 * Tubester - deck speed: range, ladder steps, and merging fast control input
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

/* Speed in percent of normal. 1600 stays out because it froze the player (REQUIREMENTS F-5, R-SPD-4). */
#define TUB_SPEED_MIN_PCT 25
#define TUB_SPEED_MAX_PCT 800
#define TUB_SPEED_NORMAL_PCT 100

/* Fast input from a Knob is merged so at most one change reaches the player per interval (R-SPD-3). */
#define TUB_COALESCE_INTERVAL_NS 50000000ULL

int tub_speed_clamp_pct(int pct);
double tub_speed_pct_to_rate(int pct);

/* The speed a fader at value (0 to 1) asks for, between min_pct at 0 and max_pct at 1 on a ratio scale: equal fader
 * travel is an equal speed ratio, so the middle of 50 to 200 is 100 (R-SPD-5). Both ends are clamped to the speed
 * range first; min above max turns the fader round. A value outside 0 to 1, or not a number, counts as the nearer end
 * (not a number as 0). */
int tub_speed_from_fader(float value, int min_pct, int max_pct);

/* The next ladder value above (direction > 0) or below (direction < 0) the given percent. The ladder is 25, 50, 75,
 * 100, 125, 150, 200, 300, 400, 600, 800. A value between rungs moves to the nearest rung in that direction; the ends
 * stay put. direction 0 returns the clamped value. */
int tub_speed_step_pct(int pct, int direction);

/* Last value wins. Offer every change; it says whether to send now. A change held back is returned by flush once the
 * interval has passed, so the final Knob position always arrives. */
struct tub_coalesce {
	uint64_t last_sent_ns;
	int pending_pct;
	bool has_pending;
	bool ever_sent;
};

void tub_coalesce_init(struct tub_coalesce *c);
/* True: send pct now. False: it is held as pending (replacing any earlier pending value). */
bool tub_coalesce_offer(struct tub_coalesce *c, int pct, uint64_t now_ns);
/* True with *pct set when a pending value is due. */
bool tub_coalesce_flush(struct tub_coalesce *c, uint64_t now_ns, int *pct);
/* A value sent without an offer (a forced resend of the latest speed): it is this interval's send, and it replaces any
 * pending value. */
void tub_coalesce_note_sent(struct tub_coalesce *c, uint64_t now_ns);

#ifdef __cplusplus
}
#endif
