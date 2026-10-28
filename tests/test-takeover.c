/*
 * Midi Fadester — test-takeover
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/takeover.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

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

	/*
	 * 3. Parked state machine, completion peak tracking, 8% hysteresis, and unpark rescaling:
	 * Case 3a: Full throw 0 -> 1.0 (completing at 0.90), tracking peak to 1.0, 8% hysteresis, then 1.0 -> 0.
	 */
	struct mfd_takeover tbar;
	mfd_takeover_init(&tbar, 0.0f, 0.015f, true);
	REQUIRE(mfd_takeover_step(&tbar, 0.0f, &out));
	REQUIRE(mfd_takeover_step(&tbar, 0.90f, &out));
	REQUIRE(fabsf(out - 0.90f) < 1e-6f);

	/* Throw completes at 90% -> enters PARKED without flipping polarity yet */
	mfd_takeover_notify_tbar_completed(&tbar);
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);
	REQUIRE(tbar.parked_at_upper_rail);
	REQUIRE(!tbar.polarity_reversed);
	REQUIRE(fabsf(tbar.parked_peak - 0.90f) < 1e-6f);

	/* Remaining travel 0.90 -> 0.96 -> 1.00 updates parked_peak with zero slider writes (returns false) */
	REQUIRE(!mfd_takeover_step(&tbar, 0.96f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);
	REQUIRE(fabsf(tbar.parked_peak - 0.96f) < 1e-6f);
	REQUIRE(!mfd_takeover_step(&tbar, 1.00f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);
	REQUIRE(fabsf(tbar.parked_peak - 1.00f) < 1e-6f);

	/* Rail jitter and retreat <= 8% (1.00 -> 0.97 -> 0.99 -> 0.925, retreat = 0.075 <= 0.08) stay PARKED */
	REQUIRE(!mfd_takeover_step(&tbar, 0.97f, &out));
	REQUIRE(!mfd_takeover_step(&tbar, 0.99f, &out));
	REQUIRE(!mfd_takeover_step(&tbar, 0.925f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);
	REQUIRE(!tbar.polarity_reversed);

	/* Retreat > 8% from peak (1.00 -> 0.90, retreat = 0.10 > 0.08) unparks, flips polarity, rescales to 0% */
	REQUIRE(mfd_takeover_step(&tbar, 0.90f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(tbar.polarity_reversed);
	REQUIRE(fabsf(tbar.rescale_origin - 0.90f) < 1e-6f);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);

	/* Halfway from unpark point (0.90) to opposite physical rail (0.0) -> raw = 0.45 -> effective = 0.50 */
	REQUIRE(mfd_takeover_step(&tbar, 0.45f, &out));
	REQUIRE(fabsf(out - 0.50f) < 1e-5f);

	/* Opposite physical rail (0.0) -> effective = 1.00 */
	REQUIRE(mfd_takeover_step(&tbar, 0.0f, &out));
	REQUIRE(fabsf(out - 1.00f) < 1e-5f);

	/*
	 * Case 3b: Throw that completes at 90% and stops at 95% (0.95 peak), then comes back.
	 */
	mfd_takeover_init(&tbar, 0.0f, 0.015f, true);
	REQUIRE(mfd_takeover_step(&tbar, 0.0f, &out));
	REQUIRE(mfd_takeover_step(&tbar, 0.90f, &out));
	mfd_takeover_notify_tbar_completed(&tbar);
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);

	/* Fader continues to 0.95 and stops -> peak is 0.95, no writes */
	REQUIRE(!mfd_takeover_step(&tbar, 0.95f, &out));
	REQUIRE(fabsf(tbar.parked_peak - 0.95f) < 1e-6f);

	/* Retreat <= 8% from 0.95 peak (0.88, retreat = 0.07 <= 0.08) stays PARKED */
	REQUIRE(!mfd_takeover_step(&tbar, 0.88f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);

	/* Retreat > 8% from 0.95 peak (0.86, retreat = 0.09 > 0.08) unparks at 0%, rescales [0.86 -> 0.0] to [0 -> 1] */
	REQUIRE(mfd_takeover_step(&tbar, 0.86f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(tbar.polarity_reversed);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);
	REQUIRE(mfd_takeover_step(&tbar, 0.43f, &out));
	REQUIRE(fabsf(out - 0.50f) < 1e-5f);

	/* Return throw reaches 90% at raw = 0.086, completes, tracks lower peak to 0.02, unparks at > 8% (0.11) */
	REQUIRE(mfd_takeover_step(&tbar, 0.086f, &out));
	REQUIRE(fabsf(out - 0.90f) < 1e-5f);
	mfd_takeover_notify_tbar_completed(&tbar);
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);
	REQUIRE(!tbar.parked_at_upper_rail);
	REQUIRE(!mfd_takeover_step(&tbar, 0.02f, &out));
	REQUIRE(fabsf(tbar.parked_peak - 0.02f) < 1e-6f);
	REQUIRE(!mfd_takeover_step(&tbar, 0.095f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_PARKED);
	REQUIRE(mfd_takeover_step(&tbar, 0.11f, &out));
	REQUIRE(tbar.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(!tbar.polarity_reversed);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);
	REQUIRE(mfd_takeover_step(&tbar, 1.0f, &out));
	REQUIRE(fabsf(out - 1.0f) < 1e-5f);

	/*
	 * Case 3c: Two-source crossfade rail mapping (mfd_takeover_throw_to_mix_value):
	 * At upper rail unpark (polarity_reversed=true, throw_progress=0.0), mix stays 1.0 (Source B, no jump)
	 * and reaches 0.0 (Source A) at physical rail 0.0.
	 */
	tbar.polarity_reversed = true;
	REQUIRE(fabsf(mfd_takeover_throw_to_mix_value(&tbar, 0.0f) - 1.0f) < 1e-6f);
	REQUIRE(fabsf(mfd_takeover_throw_to_mix_value(&tbar, 0.5f) - 0.5f) < 1e-6f);
	REQUIRE(fabsf(mfd_takeover_throw_to_mix_value(&tbar, 1.0f) - 0.0f) < 1e-6f);
	REQUIRE(fabsf(mfd_takeover_mix_to_throw_progress(&tbar, 0.10f) - 0.90f) < 1e-6f);

	/*
	 * Case 3d: Directional two-rail mode (two-source crossfade, single-layer opacity, audio volume):
	 * Output stays strictly linear to the physical end (92% -> 92%, 95% -> 95%, 97% -> 97%, 100% -> 100%;
	 * no 90%/10% snap!). Parking engages only inside the 3% rail zones (>= 97% or <= 3%), advances linearly
	 * with new extremes toward 100%/0%, absorbs <= 8% rail jitter with zero writes, and unparks + rescales
	 * after > 8% retreat with zero jump.
	 */
	struct mfd_takeover two_rail;
	mfd_takeover_init(&two_rail, 0.0f, 0.015f, false);
	mfd_takeover_set_two_rail_mode(&two_rail, true);
	REQUIRE(two_rail.two_rail_mode);
	REQUIRE(mfd_takeover_step(&two_rail, 0.0f, &out));
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(!two_rail.armed_lower_rail);
	REQUIRE(two_rail.armed_upper_rail);

	/* 90%, 92%, 95% stay strictly linear and ENGAGED (no T-bar 90% snap!) */
	REQUIRE(mfd_takeover_step(&two_rail, 0.50f, &out));
	REQUIRE(two_rail.armed_lower_rail);
	REQUIRE(mfd_takeover_step(&two_rail, 0.90f, &out));
	REQUIRE(fabsf(out - 0.90f) < 1e-6f);
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(mfd_takeover_step(&two_rail, 0.92f, &out));
	REQUIRE(fabsf(out - 0.92f) < 1e-6f);
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(mfd_takeover_step(&two_rail, 0.95f, &out));
	REQUIRE(fabsf(out - 0.95f) < 1e-6f);
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);

	/* Entering >= 97% (0.97) parks WITHOUT snapping (output is exact linear 0.97) */
	REQUIRE(mfd_takeover_step(&two_rail, 0.97f, &out));
	REQUIRE(fabsf(out - 0.97f) < 1e-6f);
	REQUIRE(two_rail.state == MFD_TAKEOVER_PARKED);
	REQUIRE(two_rail.parked_at_upper_rail);

	/* Continuing to physical stop 0.985 -> 1.00 advances linearly to 1.00; rail jitter <= 8% produces zero writes */
	REQUIRE(mfd_takeover_step(&two_rail, 0.985f, &out));
	REQUIRE(fabsf(out - 0.985f) < 1e-6f);
	REQUIRE(mfd_takeover_step(&two_rail, 1.00f, &out));
	REQUIRE(fabsf(out - 1.00f) < 1e-6f);
	REQUIRE(fabsf(two_rail.parked_peak - 1.00f) < 1e-6f);
	REQUIRE(!mfd_takeover_step(&two_rail, 0.96f, &out));
	REQUIRE(!mfd_takeover_step(&two_rail, 0.99f, &out));
	REQUIRE(!mfd_takeover_step(&two_rail, 0.93f, &out));
	REQUIRE(!mfd_takeover_step(&two_rail, 1.00f, &out));
	REQUIRE(two_rail.state == MFD_TAKEOVER_PARKED);

	/* Retreat > 8% from peak (1.00 -> 0.90) unparks at 1.0f (zero jump!) and rescales [0.0, 0.90] -> [0.0, 1.0] */
	REQUIRE(mfd_takeover_step(&two_rail, 0.90f, &out));
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(fabsf(out - 1.0f) < 1e-6f);

	/* Halfway down (raw = 0.45) -> effective = 0.50; 10% (raw = 0.09) -> effective = 0.10 (NOT parked!) */
	REQUIRE(mfd_takeover_step(&two_rail, 0.45f, &out));
	REQUIRE(fabsf(out - 0.50f) < 1e-5f);
	REQUIRE(two_rail.armed_upper_rail);
	REQUIRE(mfd_takeover_step(&two_rail, 0.09f, &out));
	REQUIRE(fabsf(out - 0.10f) < 1e-5f);
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);

	/* Enter <= 3% lower rail zone (raw = 0.027 -> eff = 0.03 <= 0.03) -> parks at linear 0.03, then reaches 0.00 */
	REQUIRE(mfd_takeover_step(&two_rail, 0.027f, &out));
	REQUIRE(fabsf(out - 0.03f) < 1e-5f);
	REQUIRE(two_rail.state == MFD_TAKEOVER_PARKED);
	REQUIRE(!two_rail.parked_at_upper_rail);
	REQUIRE(mfd_takeover_step(&two_rail, 0.00f, &out));
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);
	REQUIRE(fabsf(two_rail.parked_peak - 0.00f) < 1e-6f);

	/* Bottom-rail jitter within 8% (0.05, 0.01, 0.07, 0.00) produces zero writes */
	REQUIRE(!mfd_takeover_step(&two_rail, 0.05f, &out));
	REQUIRE(!mfd_takeover_step(&two_rail, 0.01f, &out));
	REQUIRE(!mfd_takeover_step(&two_rail, 0.07f, &out));
	REQUIRE(!mfd_takeover_step(&two_rail, 0.00f, &out));
	REQUIRE(two_rail.state == MFD_TAKEOVER_PARKED);

	/* Advance > 8% from bottom peak (0.00 -> 0.10) unparks at 0.0f (zero jump!) and rescales [0.10, 1.00] -> [0.0, 1.0] */
	REQUIRE(mfd_takeover_step(&two_rail, 0.10f, &out));
	REQUIRE(two_rail.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);
	REQUIRE(mfd_takeover_step(&two_rail, 0.55f, &out));
	REQUIRE(fabsf(out - 0.50f) < 1e-5f);

	/* 4. Output bar formatter shows ↑/↓ arrow while waiting, then live value once engaged (Item 2) */
	char bar_text[128] = {0};
	struct mfd_takeover t_ui;
	mfd_takeover_init(&t_ui, 0.80f, 0.015f, false);
	REQUIRE(!mfd_takeover_step(&t_ui, 0.20f, &out));
	int pct = mfd_takeover_format_output_bar(&t_ui, MFD_TARGET_SINGLE_LAYER, out, 0.80f, 0.0f, 0.0f, bar_text,
						 sizeof(bar_text));
	REQUIRE(pct == 80);
	REQUIRE(strcmp(bar_text, "\xE2\x86\x91 80%") == 0);

	REQUIRE(mfd_takeover_step(&t_ui, 0.85f, &out));
	pct = mfd_takeover_format_output_bar(&t_ui, MFD_TARGET_SINGLE_LAYER, out, 0.85f, 0.0f, 0.0f, bar_text,
					     sizeof(bar_text));
	REQUIRE(pct == 85);
	REQUIRE(strcmp(bar_text, "85%") == 0);

	mfd_takeover_sync_external(&t_ui, 0.20f);
	REQUIRE(!mfd_takeover_step(&t_ui, 0.60f, &out));
	pct = mfd_takeover_format_output_bar(&t_ui, MFD_TARGET_SINGLE_LAYER, out, 0.20f, 0.0f, 0.0f, bar_text,
					     sizeof(bar_text));
	REQUIRE(pct == 20);
	REQUIRE(strcmp(bar_text, "\xE2\x86\x93 20%") == 0);

	/*
	 * 5. Fast throw bumper-bounce peak preservation & 30-step 127/126/127 rail jitter (R-FADE-1 / R-FEEL-4):
	 * If a fast physical throw hits CC 127 (1.00) and bounces slightly to CC 126 (126/127) before the
	 * 30 ms smoother crosses 90% and calls mfd_takeover_notify_tbar_completed(), parked_peak MUST preserve
	 * the true raw extreme 1.00, and 30 steps of 127/126/127 rail jitter MUST produce zero writes.
	 */
	struct mfd_takeover t_jitter;
	mfd_takeover_init(&t_jitter, 0.0f, 0.015f, true);
	REQUIRE(mfd_takeover_step(&t_jitter, 0.0f, &out));
	REQUIRE(mfd_takeover_step(&t_jitter, 1.0f, &out));
	REQUIRE(mfd_takeover_step(&t_jitter, 126.0f / 127.0f, &out));
	mfd_takeover_notify_tbar_completed(&t_jitter);
	REQUIRE(t_jitter.state == MFD_TAKEOVER_PARKED);
	REQUIRE(fabsf(t_jitter.parked_peak - 1.0f) < 1e-6f);

	for (int step = 0; step < 30; ++step) {
		float raw_cc = ((step % 2) == 0) ? (126.0f / 127.0f) : 1.0f;
		REQUIRE(!mfd_takeover_step(&t_jitter, raw_cc, &out));
		REQUIRE(t_jitter.state == MFD_TAKEOVER_PARKED);
	}

	/*
	 * 6. Unidirectional T-bar (bidirectional_flip = false, two_rail_mode = false) unpark (R-TBAR-SM):
	 *    After completing at the upper rail (1.0), retreating > 8% (e.g. to 0.85) MUST enter WAITING
	 *    for return to 0.0 (MOVE_DOWN, returns false, out = 0.0) rather than jumping the T-bar to 0.85!
	 *    Once the fader reaches the <= 3% origin rail zone (e.g. 0.02), it engages cleanly for the next throw.
	 */
	struct mfd_takeover t_unidir;
	mfd_takeover_init(&t_unidir, 0.0f, 0.015f, false);
	REQUIRE(mfd_takeover_step(&t_unidir, 0.0f, &out));
	REQUIRE(mfd_takeover_step(&t_unidir, 0.98f, &out));
	mfd_takeover_notify_tbar_completed(&t_unidir);
	REQUIRE(t_unidir.state == MFD_TAKEOVER_PARKED);
	REQUIRE(!mfd_takeover_step(&t_unidir, 1.0f, &out));

	REQUIRE(!mfd_takeover_step(&t_unidir, 0.85f, &out));
	REQUIRE(t_unidir.state == MFD_TAKEOVER_WAITING);
	REQUIRE(t_unidir.hint == MFD_HINT_MOVE_DOWN);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);

	REQUIRE(!mfd_takeover_step(&t_unidir, 0.40f, &out));
	REQUIRE(t_unidir.state == MFD_TAKEOVER_WAITING);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);

	/* Reaches <= 3% origin rail zone (0.02) -> engages cleanly at 0.02 */
	REQUIRE(mfd_takeover_step(&t_unidir, 0.02f, &out));
	REQUIRE(t_unidir.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(fabsf(out - 0.02f) < 1e-6f);

	/*
	 * 7. Fresh bidirectional T-bar starting physically at the upper rail (>= 0.97) on startup:
	 *    Auto-selects reversed polarity (rescale_origin = 1.0, out = 0.0) so a downward throw 1.0 -> 0.0
	 *    drives 0.0 -> 1.0 instead of insta-completing on the first MIDI packet!
	 */
	struct mfd_takeover t_top_start;
	mfd_takeover_init(&t_top_start, 0.0f, 0.015f, true);
	REQUIRE(mfd_takeover_step(&t_top_start, 1.0f, &out));
	REQUIRE(t_top_start.state == MFD_TAKEOVER_ENGAGED);
	REQUIRE(t_top_start.polarity_reversed);
	REQUIRE(fabsf(t_top_start.rescale_origin - 1.0f) < 1e-6f);
	REQUIRE(fabsf(out - 0.0f) < 1e-6f);
	REQUIRE(mfd_takeover_step(&t_top_start, 0.5f, &out));
	REQUIRE(fabsf(out - 0.5f) < 1e-6f);

	/*
	 * 8. Exact Calibrated Output Readout (R-FEEL-2 / D-12):
	 *    CC 51 of [0, 127] (51/127 = 0.4015748) MUST always format and return 40% (never 33% from an unpark
	 *    rescale), and must keep moving with calibrated_norm even while !available or WAITING.
	 */
	const float cc51_norm = 51.0f / 127.0f;
	REQUIRE(mfd_calibrated_percent(cc51_norm) == 40);
	REQUIRE(mfd_calibrated_percent(0.0f) == 0);
	REQUIRE(mfd_calibrated_percent(1.0f) == 100);

	/* Advance two_rail from bottom-parked unpark rescale [0.10, 1.00] to CC 51: effective is ~0.335 (34%),
	 * yet calibrated output bar percentage MUST still equal 40%! */
	struct mfd_takeover t_rescaled;
	mfd_takeover_init(&t_rescaled, 0.0f, 0.015f, false);
	mfd_takeover_set_two_rail_mode(&t_rescaled, true);
	mfd_takeover_notify_rail_completed(&t_rescaled, false, 0.0f);
	REQUIRE(mfd_takeover_step(&t_rescaled, 0.10f,
				  &out)); /* unparks at 0.10 -> rescales [0.10, 1.00] -> [0.0, 1.0] */
	REQUIRE(mfd_takeover_step(&t_rescaled, cc51_norm, &out));
	REQUIRE(fabsf(out - ((cc51_norm - 0.10f) / 0.90f)) < 1e-5f); /* effective ~ 0.3351 (33.5%) */
	pct = mfd_takeover_format_calibrated_output_bar(&t_rescaled, MFD_TARGET_TWO_SOURCE_MIX, cc51_norm, true, "",
							1.0f, out, 0.0f, bar_text, sizeof(bar_text));
	REQUIRE(pct == 40);
	REQUIRE(strncmp(bar_text, "40% ", 4) == 0);

	/* While !available ("Studio Mode is off."), calibrated bar still reports 40% at CC 51 */
	pct = mfd_takeover_format_calibrated_output_bar(&t_rescaled, MFD_TARGET_TBAR, cc51_norm, false,
							"Studio Mode is off.", 1.0f, 0.0f, 0.0f, bar_text,
							sizeof(bar_text));
	REQUIRE(pct == 40);
	REQUIRE(strstr(bar_text, "40%") != NULL);
	REQUIRE(strstr(bar_text, "Studio Mode is off.") != NULL);

	printf("test-takeover: PASS\n");
	return 0;
}
