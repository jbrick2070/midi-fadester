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

#include "tubester/speed.h"

#include <math.h>
#include <string.h>

static const int ladder[] = {25, 50, 75, 100, 125, 150, 200, 300, 400, 600, 800};
#define LADDER_COUNT (sizeof(ladder) / sizeof(ladder[0]))

int tub_speed_clamp_pct(int pct)
{
	if (pct < TUB_SPEED_MIN_PCT)
		return TUB_SPEED_MIN_PCT;
	if (pct > TUB_SPEED_MAX_PCT)
		return TUB_SPEED_MAX_PCT;
	return pct;
}

int tub_speed_pct_from_setting(long long value)
{
	if (value < TUB_SPEED_MIN_PCT)
		return TUB_SPEED_MIN_PCT;
	if (value > TUB_SPEED_MAX_PCT)
		return TUB_SPEED_MAX_PCT;
	return (int)value;
}

double tub_speed_pct_to_rate(int pct)
{
	return (double)tub_speed_clamp_pct(pct) / 100.0;
}

int tub_speed_from_fader(float value, int min_pct, int max_pct)
{
	double v = (double)value;
	if (!(v > 0.0)) /* also not a number */
		v = 0.0;
	else if (v > 1.0)
		v = 1.0;
	double lo = (double)tub_speed_clamp_pct(min_pct);
	double hi = (double)tub_speed_clamp_pct(max_pct);
	/* both ends are at least 25, so the ratio is finite and positive */
	return tub_speed_clamp_pct((int)lround(lo * pow(hi / lo, v)));
}

int tub_speed_step_pct(int pct, int direction)
{
	pct = tub_speed_clamp_pct(pct);
	if (direction > 0) {
		for (size_t i = 0; i < LADDER_COUNT; i++) {
			if (ladder[i] > pct)
				return ladder[i];
		}
		return TUB_SPEED_MAX_PCT;
	}
	if (direction < 0) {
		for (size_t i = LADDER_COUNT; i-- > 0;) {
			if (ladder[i] < pct)
				return ladder[i];
		}
		return TUB_SPEED_MIN_PCT;
	}
	return pct;
}

void tub_coalesce_init(struct tub_coalesce *c)
{
	memset(c, 0, sizeof(*c));
}

static bool interval_passed(const struct tub_coalesce *c, uint64_t now_ns)
{
	return !c->ever_sent || now_ns < c->last_sent_ns || now_ns - c->last_sent_ns >= TUB_COALESCE_INTERVAL_NS;
}

bool tub_coalesce_offer(struct tub_coalesce *c, int pct, uint64_t now_ns)
{
	if (interval_passed(c, now_ns)) {
		c->last_sent_ns = now_ns;
		c->ever_sent = true;
		c->has_pending = false;
		return true;
	}
	c->pending_pct = pct;
	c->has_pending = true;
	return false;
}

void tub_coalesce_note_sent(struct tub_coalesce *c, uint64_t now_ns)
{
	c->last_sent_ns = now_ns;
	c->ever_sent = true;
	c->has_pending = false;
}

bool tub_coalesce_flush(struct tub_coalesce *c, uint64_t now_ns, int *pct)
{
	if (!c->has_pending || !interval_passed(c, now_ns))
		return false;
	*pct = c->pending_pct;
	c->has_pending = false;
	c->last_sent_ns = now_ns;
	return true;
}
