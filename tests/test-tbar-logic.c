/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/tbar-logic.h"
#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(cond)                                                               \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	struct mfd_tbar_action act;
	struct mfd_tbar_release_input in;

	/*
	 * 1. Studio Mode OFF guard:
	 * Must never call obs_frontend_get_tbar_position / set_tbar_position / release_tbar
	 * when Studio Mode is inactive (OBSBasic::tBar is uninitialized or dangling).
	 */
	mfd_tbar_compute_action(false, false, 0.5f, 0, &act);
	ASSERT_TRUE(!act.allow_frontend_calls);
	ASSERT_TRUE(!act.report_missing_slider_error);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 2. Studio Mode ON, but Qt "slider-tbar" missing (future OBS version breakage check):
	 * Must flag report_missing_slider_error = true and block unsafe calls.
	 */
	mfd_tbar_compute_action(true, false, 0.5f, 0, &act);
	ASSERT_TRUE(!act.allow_frontend_calls);
	ASSERT_TRUE(act.report_missing_slider_error);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 3. Studio Mode ON and "slider-tbar" found: mid-throw at t = 0.5 -> pos = 512
	 */
	mfd_tbar_compute_action(true, true, 0.5f, 0, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(!act.report_missing_slider_error);
	ASSERT_TRUE(act.target_pos_1024 == 512);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 4. 3% rail zone classification across all 128 7-bit CC steps in both polarities:
	 *    - Normal polarity: CC 0..123 -> false, CC 124..127 -> true.
	 *    - Reversed polarity: CC 4..127 -> false, CC 0..3 -> true.
	 */
	for (int cc = 0; cc <= 127; ++cc) {
		float raw_norm = (float)cc / 127.0f;
		ASSERT_TRUE(mfd_tbar_is_in_rail_zone(raw_norm, false) == (cc >= 124));
		ASSERT_TRUE(mfd_tbar_is_in_rail_zone(raw_norm, true) == (cc <= 3));
	}

	/*
	 * 5. Do NOT release at OBS's 90% minimum (pos = 921), CC 119 (~93.7%, pos = 959), or CC 123 (~96.85%, pos = 991):
	 *    Keep driving the T-bar linearly all the way through 90..96.85% while the fader is moving.
	 */
	ASSERT_TRUE(MFD_TBAR_OBS_MIN_RELEASE_POS == 921);

	mfd_tbar_compute_action(true, true, 0.90f, 910, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 921);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	mfd_tbar_compute_action(true, true, 119.0f / 127.0f, 921, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 959);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	mfd_tbar_compute_action(true, true, 123.0f / 127.0f, 959, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 991);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/* Holding steady at pos = 959 without idle >= 150 ms produces no duplicate write and no release */
	mfd_tbar_compute_action(true, true, 119.0f / 127.0f, 959, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 6. Primary release trigger: entering the 3% rail zone (CC >= 124 -> pos = 999 .. CC 127 -> pos = 1023).
	 *    Also verify that during a fast throw where raw CC is already >= 124 (in_rail_zone = true) while the
	 *    30 ms smoother is still catching up at 92% (pos = 941 < 992, smoother_settled = false), it keeps
	 *    driving linearly until the smoothed value enters the 3% rail zone (pos >= 992) or settles.
	 */
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.in_rail_zone = true;
	in.smoother_settled = false;
	in.t_norm = 0.92f;
	in.last_pos_1024 = 900;
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.target_pos_1024 == 941);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	mfd_tbar_compute_action(true, true, 124.0f / 127.0f, 991, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 999);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);

	mfd_tbar_compute_action(true, true, 1.0f, 999, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(act.target_pos_1024 == 1023);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);

	/*
	 * 7. Fallback release trigger for short-travel faders:
	 *    Fader stopped moving for >= 150 ms while above 90% (pos >= 921) triggers release_tbar;
	 *    below 90% (e.g. 88%, pos = 900 < 921), holding indefinitely never triggers release_tbar.
	 */
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.in_rail_zone = false;
	in.smoother_settled = true;
	in.t_norm = 121.0f / 127.0f; /* 95.28%, pos = 975 */
	in.last_pos_1024 = 975;
	in.fader_idle_sec = 0.140f;
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.target_pos_1024 == 975);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	in.fader_idle_sec = 0.150f;
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.target_pos_1024 == 975);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);

	in.t_norm = 0.88f; /* 88%, pos = 900 < 921 */
	in.last_pos_1024 = 900;
	in.fader_idle_sec = 5.0f;
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.target_pos_1024 == 900);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 8. Per-transition release latch (R-FADE-1 / R-FEEL-4):
	 * While release_latched is true, zero slider writes and zero releases are permitted
	 * regardless of t_norm (0.0 .. 1.0) or last_pos_1024, until an explicit unpark clears the latch.
	 */
	const float test_vals[] = {0.0f, 0.05f, 0.50f, 0.89f, 0.90f, 0.95f, 0.9764f, 0.992f, 1.0f};
	const int test_last_pos[] = {0, 12, 512, 910, 921, 991, 1023};
	for (size_t vi = 0U; vi < sizeof(test_vals) / sizeof(test_vals[0]); ++vi) {
		for (size_t pi = 0U; pi < sizeof(test_last_pos) / sizeof(test_last_pos[0]); ++pi) {
			mfd_tbar_compute_action_ex(true, true, true, test_vals[vi], test_last_pos[pi], &act);
			ASSERT_TRUE(act.allow_frontend_calls);
			ASSERT_TRUE(!act.report_missing_slider_error);
			ASSERT_TRUE(!act.call_set_tbar);
			ASSERT_TRUE(!act.call_release_tbar);
		}
	}

	/* Once unpark clears release_latched (false), a new throw writes linearly and releases in the 3% rail zone */
	mfd_tbar_compute_action_ex(true, true, false, 0.25f, 0, &act);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);
	mfd_tbar_compute_action_ex(true, true, false, 0.94f, act.target_pos_1024, &act);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);
	mfd_tbar_compute_action_ex(true, true, false, 124.0f / 127.0f, act.target_pos_1024, &act);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);

	/*
	 * 9. Amber Studio Mode note banner visibility (Item 3):
	 * Shown only when Studio Mode is OFF and at least one T-bar slot is enabled; hidden when Studio Mode is ON.
	 */
	ASSERT_TRUE(mfd_should_show_studio_mode_note(false, 1U));
	ASSERT_TRUE(!mfd_should_show_studio_mode_note(true, 1U));
	ASSERT_TRUE(!mfd_should_show_studio_mode_note(false, 0U));

	printf("test-tbar-logic: PASS\n");
	return 0;
}
