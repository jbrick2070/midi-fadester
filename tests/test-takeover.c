/*
 * Midi Fadester — test-takeover
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/takeover.h"
#include <math.h>
#include <stdio.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	struct mfd_takeover t;
	float out = -1.0f;

	/* 1. OBS target is at 0.70; physical fader starts at 0.10 -> waits until crossing 0.70 */
	mfd_takeover_init(&t, 0.70f, 0.015f, false);
	REQUIRE(!mfd_takeover_step(&t, 0.10f, &out));
	REQUIRE(fabsf(out - 0.70f) < 1e-6f);
	REQUIRE(t.state == MFD_TAKEOVER_WAITING);
	REQUIRE(t.hint == MFD_HINT_MOVE_UP);

	REQUIRE(!mfd_takeover_step(&t, 0.45f, &out));
	REQUIRE(fabsf(out - 0.70f) < 1e-6f);

	/* Crosses 0.70 upward (0.45 -> 0.75) -> engages! */
	REQUIRE(mfd_takeover_step(&t, 0.75f, &out));
	REQUIRE(fabsf(out - 0.75f) < 1e-6f);
	REQUIRE(t.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(t.hint == MFD_HINT_NONE);

	/* Tracks while engaged */
	REQUIRE(mfd_takeover_step(&t, 0.30f, &out));
	REQUIRE(fabsf(out - 0.30f) < 1e-6f);

	/* 2. External OBS change to 0.10 re-arms WAITING with MOVE_DOWN hint */
	mfd_takeover_sync_external(&t, 0.10f);
	REQUIRE(t.state == MFD_TAKEOVER_WAITING);
	REQUIRE(t.hint == MFD_HINT_MOVE_DOWN);
	REQUIRE(!mfd_takeover_step(&t, 0.25f, &out));
	REQUIRE(fabsf(out - 0.10f) < 1e-6f);

	/* Crosses 0.10 downward (0.25 -> 0.08) -> engages! */
	REQUIRE(mfd_takeover_step(&t, 0.08f, &out));
	REQUIRE(fabsf(out - 0.08f) < 1e-6f);
	REQUIRE(t.state == MFD_TAKEOVER_ENGAGED);

	/* 3. Bidirectional T-Bar throw flip: 0 -> 1 completes, then 1 -> 0 drives 0 -> 1 */
	struct mfd_takeover tbar;
	mfd_takeover_init(&tbar, 0.0f, 0.015f, true);
	REQUIRE(mfd_takeover_step(&tbar, 0.0f, &out));
	REQUIRE(mfd_takeover_step(&tbar, 1.0f, &out));
	REQUIRE(fabsf(out - 1.0f) < 1e-6f);

	mfd_takeover_notify_tbar_completed(&tbar);
	REQUIRE(tbar.polarity_reversed);
	REQUIRE(tbar.state == MFD_TAKEOVER_ENGAGED);

	/* Physical fader now moves back from 1.0 -> 0.25 -> effective T-bar moves 0.0 -> 0.75 */
	REQUIRE(mfd_takeover_step(&tbar, 0.25f, &out));
	REQUIRE(fabsf(out - 0.75f) < 1e-6f);

	printf("test-takeover: PASS\n");
	return 0;
}
