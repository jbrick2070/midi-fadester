/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/tbar-logic.h"
#include <math.h>
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
	struct mfd_control_calibration cal;

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
	ASSERT_TRUE(act.next_state == MFD_TBAR_IDLE);

	/*
	 * 2. Studio Mode ON, but Qt "slider-tbar" missing (future OBS version breakage check):
	 * Must flag report_missing_slider_error = true and block unsafe calls.
	 */
	mfd_tbar_compute_action(true, false, 0.5f, 0, &act);
	ASSERT_TRUE(!act.allow_frontend_calls);
	ASSERT_TRUE(act.report_missing_slider_error);
	ASSERT_TRUE(!act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);
	ASSERT_TRUE(act.next_state == MFD_TBAR_IDLE);

	/*
	 * 3. Calibrated control range [cal_min, cal_max] & auto-widening (R-MIDI-5):
	 *    - Default 7-bit [0, 127] and 14-bit [0, 16383].
	 *    - Short-travel fader calibrated to [0, 125] reaches x_cal = 1.0 at CC 125 (rail zone CC 122..125).
	 *    - If raw value exceeds stored extremes (e.g. CC 127), observe_and_widen widens to [0, 127] once.
	 */
	mfd_calibration_init(&cal, false);
	ASSERT_TRUE(cal.cal_min == 0 && cal.cal_max == 127);
	ASSERT_TRUE(fabsf(mfd_calibration_normalize(&cal, 0, false) - 0.0f) < 1e-6f);
	ASSERT_TRUE(fabsf(mfd_calibration_normalize(&cal, 127, false) - 1.0f) < 1e-6f);

	mfd_calibration_init(&cal, true);
	ASSERT_TRUE(cal.cal_min == 0 && cal.cal_max == 16383);
	ASSERT_TRUE(fabsf(mfd_calibration_normalize(&cal, 16383, false) - 1.0f) < 1e-6f);

	mfd_calibration_set(&cal, false, 0, 125);
	ASSERT_TRUE(cal.cal_min == 0 && cal.cal_max == 125);
	ASSERT_TRUE(fabsf(mfd_calibration_normalize(&cal, 125, false) - 1.0f) < 1e-6f);
	ASSERT_TRUE(!mfd_tbar_is_in_target_rail_zone(mfd_calibration_normalize(&cal, 121, false), false));
	ASSERT_TRUE(mfd_tbar_is_in_target_rail_zone(mfd_calibration_normalize(&cal, 122, false), false));
	ASSERT_TRUE(mfd_tbar_is_in_target_rail_zone(mfd_calibration_normalize(&cal, 125, false), false));

	ASSERT_TRUE(!mfd_calibration_observe_and_widen(&cal, 125));
	ASSERT_TRUE(mfd_calibration_observe_and_widen(&cal, 127));
	ASSERT_TRUE(cal.cal_min == 0 && cal.cal_max == 127);
	ASSERT_TRUE(!mfd_calibration_observe_and_widen(&cal, 127));

	/*
	 * 4. Disjoint 3% origin & target rail zones across all 128 7-bit CC steps in both polarities:
	 *    - Normal polarity: origin CC 0..3, throwing CC 4..123, target CC 124..127.
	 *    - Reversed polarity: origin CC 124..127, throwing CC 4..123, target CC 0..3.
	 */
	for (int cc = 0; cc <= 127; ++cc) {
		float x_cal = (float)cc / 127.0f;
		ASSERT_TRUE(mfd_tbar_is_in_origin_rail_zone(x_cal, false) == (cc <= 3));
		ASSERT_TRUE(mfd_tbar_is_in_target_rail_zone(x_cal, false) == (cc >= 124));
		ASSERT_TRUE(mfd_tbar_is_in_rail_zone(x_cal, false) == (cc >= 124));

		ASSERT_TRUE(mfd_tbar_is_in_origin_rail_zone(x_cal, true) == (cc >= 124));
		ASSERT_TRUE(mfd_tbar_is_in_target_rail_zone(x_cal, true) == (cc <= 3));
		ASSERT_TRUE(mfd_tbar_is_in_rail_zone(x_cal, true) == (cc <= 3));
	}

	/*
	 * 5. Idle state (R-TBAR-SM Section 5.1.1):
	 *    - Staying inside the origin 3% rail zone (CC 0..3) produces 0 writes, 0 releases, stays Idle.
	 *    - Leaving origin rail into (0.03, 0.97) enters Throwing and writes pos in [1, 1022].
	 */
	for (int cc = 0; cc <= 3; ++cc) {
		float x = (float)cc / 127.0f;
		memset(&in, 0, sizeof(in));
		in.studio_mode_active = true;
		in.slider_tbar_found = true;
		in.state = MFD_TBAR_IDLE;
		in.in_origin_rail_zone = mfd_tbar_is_in_origin_rail_zone(x, false);
		in.in_target_rail_zone = mfd_tbar_is_in_target_rail_zone(x, false);
		in.t_norm = x;
		in.last_pos_1024 = 0;
		mfd_tbar_compute_action_full(&in, &act);
		ASSERT_TRUE(act.allow_frontend_calls);
		ASSERT_TRUE(act.next_state == MFD_TBAR_IDLE);
		ASSERT_TRUE(!act.call_set_tbar);
		ASSERT_TRUE(!act.call_release_tbar);
		ASSERT_TRUE(!act.completed_throw);
		ASSERT_TRUE(!act.aborted_throw);
	}

	/* Idle -> Throwing on mid-throw t = 0.5 -> pos = 512 */
	mfd_tbar_compute_action(true, true, 0.5f, 0, &act);
	ASSERT_TRUE(act.allow_frontend_calls);
	ASSERT_TRUE(!act.report_missing_slider_error);
	ASSERT_TRUE(act.next_state == MFD_TBAR_THROWING);
	ASSERT_TRUE(act.target_pos_1024 == 512);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(!act.call_release_tbar);

	/*
	 * 6. Throwing state (zero timers anywhere!):
	 *    Do NOT release at OBS's 90% minimum (pos = 921), CC 116 (~91.34%, pos = 934),
	 *    CC 119 (~93.70%, pos = 959), or CC 123 (~96.85%, pos = 991), regardless of how long
	 *    the fader pauses there!
	 */
	ASSERT_TRUE(MFD_TBAR_OBS_MIN_RELEASE_POS == 921);

	const int hold_ccs[] = {4, 60, 115, 116, 118, 119, 121, 123};
	int last_pos = 0;
	for (size_t i = 0U; i < sizeof(hold_ccs) / sizeof(hold_ccs[0]); ++i) {
		float x = (float)hold_ccs[i] / 127.0f;
		memset(&in, 0, sizeof(in));
		in.studio_mode_active = true;
		in.slider_tbar_found = true;
		in.state = MFD_TBAR_THROWING;
		in.in_origin_rail_zone = mfd_tbar_is_in_origin_rail_zone(x, false);
		in.in_target_rail_zone = mfd_tbar_is_in_target_rail_zone(x, false);
		in.t_norm = x;
		in.last_pos_1024 = last_pos;
		mfd_tbar_compute_action_full(&in, &act);
		ASSERT_TRUE(act.next_state == MFD_TBAR_THROWING);
		ASSERT_TRUE(act.target_pos_1024 >= 1 && act.target_pos_1024 <= 1022);
		ASSERT_TRUE(act.call_set_tbar);
		ASSERT_TRUE(!act.call_release_tbar);
		ASSERT_TRUE(!act.completed_throw);
		ASSERT_TRUE(!act.aborted_throw);
		last_pos = act.target_pos_1024;

		/* Holding steady at the same position produces 0 duplicate writes and 0 releases */
		in.last_pos_1024 = last_pos;
		mfd_tbar_compute_action_full(&in, &act);
		ASSERT_TRUE(act.next_state == MFD_TBAR_THROWING);
		ASSERT_TRUE(!act.call_set_tbar);
		ASSERT_TRUE(!act.call_release_tbar);
	}

	/*
	 * 7. Throwing -> Idle (Aborted throw on return into origin 3% rail zone, R-TBAR-SM):
	 *    Writes pos = 0, calls release once (aborted_throw = true, completed_throw = false),
	 *    and enters Idle so subsequent origin-rail CCs (2, 1, 0) write nothing.
	 */
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.state = MFD_TBAR_THROWING;
	in.in_origin_rail_zone = true; /* CC 3 <= 0.03 */
	in.in_target_rail_zone = false;
	in.t_norm = 3.0f / 127.0f;
	in.last_pos_1024 = 483; /* from CC 60 */
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.next_state == MFD_TBAR_IDLE);
	ASSERT_TRUE(act.target_pos_1024 == 0);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);
	ASSERT_TRUE(act.aborted_throw);
	ASSERT_TRUE(!act.completed_throw);

	/* Subsequent CCs 2, 1, 0 in Idle produce zero writes and zero releases */
	for (int cc = 2; cc >= 0; --cc) {
		in.state = act.next_state;
		in.t_norm = (float)cc / 127.0f;
		in.last_pos_1024 = 0;
		mfd_tbar_compute_action_full(&in, &act);
		ASSERT_TRUE(act.next_state == MFD_TBAR_IDLE);
		ASSERT_TRUE(!act.call_set_tbar);
		ASSERT_TRUE(!act.call_release_tbar);
	}

	/*
	 * 8. Throwing -> Parked (Completed throw on entering target 3% rail zone, CC >= 124):
	 *    Writes pos = 1023, calls release once (completed_throw = true, aborted_throw = false),
	 *    and enters Parked.
	 */
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.state = MFD_TBAR_THROWING;
	in.in_origin_rail_zone = false;
	in.in_target_rail_zone = true; /* CC 124 >= 0.97 */
	in.t_norm = 124.0f / 127.0f;
	in.last_pos_1024 = 991;
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.next_state == MFD_TBAR_PARKED);
	ASSERT_TRUE(act.target_pos_1024 == 1023);
	ASSERT_TRUE(act.call_set_tbar);
	ASSERT_TRUE(act.call_release_tbar);
	ASSERT_TRUE(act.completed_throw);
	ASSERT_TRUE(!act.aborted_throw);

	/*
	 * 9. Parked state & per-transition release latch (R-TBAR-SM / R-FADE-1 / R-FEEL-4):
	 * While in MFD_TBAR_PARKED or release_latched is true, zero slider writes and zero releases
	 * are permitted regardless of t_norm (0.0 .. 1.0) or last_pos_1024.
	 */
	const float test_vals[] = {0.0f, 0.02f, 0.05f, 0.50f, 0.89f, 0.90f, 0.95f, 0.9764f, 0.992f, 1.0f};
	const int test_last_pos[] = {0, 12, 512, 910, 921, 991, 1023};
	for (size_t vi = 0U; vi < sizeof(test_vals) / sizeof(test_vals[0]); ++vi) {
		for (size_t pi = 0U; pi < sizeof(test_last_pos) / sizeof(test_last_pos[0]); ++pi) {
			mfd_tbar_compute_action_ex(true, true, true, test_vals[vi], test_last_pos[pi], &act);
			ASSERT_TRUE(act.allow_frontend_calls);
			ASSERT_TRUE(!act.report_missing_slider_error);
			ASSERT_TRUE(act.next_state == MFD_TBAR_PARKED);
			ASSERT_TRUE(!act.call_set_tbar);
			ASSERT_TRUE(!act.call_release_tbar);
		}
	}

	/*
	 * 10. Amber Studio Mode note banner visibility (Item 3):
	 * Shown only when Studio Mode is OFF and at least one T-bar slot is enabled; hidden when Studio Mode is ON.
	 */
	ASSERT_TRUE(mfd_should_show_studio_mode_note(false, 1U));
	ASSERT_TRUE(!mfd_should_show_studio_mode_note(true, 1U));
	ASSERT_TRUE(!mfd_should_show_studio_mode_note(false, 0U));

	/*
	 * 11. 0.1.3 Bottom-Rail 6% Start Band & Hysteresis (R-TBAR-SM / D-8):
	 *     - Throw starts from Idle only above x = 0.06 (CC >= 8 under [0, 127] normal polarity;
	 *       CC <= 119 under [0, 127] reversed polarity), with throw progress t rescaled to 0.0 at 0.06 / 0.94.
	 *     - CC 3,4,3,4,3,4 five times (30 steps) in Idle -> 0 transitions, 0 writes, 0 releases.
	 *     - Mutation comparison: without in_start_deadband (0.03 start threshold), CC 4 starts a transition.
	 *     - CC 0 -> 8 -> 3 -> 1 start (Idle -> Throwing at CC 8) and 1 cancel (Throwing -> Idle at CC 3).
	 *     - Retreating into (0.03, 0.06] (e.g. CC 5) while already Throwing stays Throwing (pos = 1, 0 cancels).
	 */
	for (int cc = 0; cc <= 127; ++cc) {
		float x_cal = (float)cc / 127.0f;
		ASSERT_TRUE(mfd_tbar_is_past_start_zone(x_cal, false) == (cc >= 8));
		ASSERT_TRUE(mfd_tbar_is_past_start_zone(x_cal, true) == (cc <= 119));
	}
	ASSERT_TRUE(fabsf(mfd_tbar_rescale_from_start_zone(0.06f, false) - 0.0f) < 1e-6f);
	ASSERT_TRUE(fabsf(mfd_tbar_rescale_from_start_zone(0.03f, false) - 0.0f) < 1e-6f);
	ASSERT_TRUE(fabsf(mfd_tbar_rescale_from_start_zone(1.00f, false) - 1.0f) < 1e-6f);
	ASSERT_TRUE(fabsf(mfd_tbar_rescale_from_start_zone(0.94f, true) - 0.0f) < 1e-6f);
	ASSERT_TRUE(fabsf(mfd_tbar_rescale_from_start_zone(0.97f, true) - 0.0f) < 1e-6f);
	ASSERT_TRUE(fabsf(mfd_tbar_rescale_from_start_zone(0.00f, true) - 1.0f) < 1e-6f);

	/* CC 3,4,3,4,3,4 five times from Idle -> 0 transitions */
	enum mfd_tbar_state sm_state = MFD_TBAR_IDLE;
	int sm_last_pos = 0;
	int sm_starts = 0;
	int sm_writes = 0;
	int sm_releases = 0;
	for (int rep = 0; rep < 5; ++rep) {
		const int osc[] = {3, 4, 3, 4, 3, 4};
		for (size_t oi = 0U; oi < sizeof(osc) / sizeof(osc[0]); ++oi) {
			float x = (float)osc[oi] / 127.0f;
			memset(&in, 0, sizeof(in));
			in.studio_mode_active = true;
			in.slider_tbar_found = true;
			in.state = sm_state;
			in.in_origin_rail_zone = mfd_tbar_is_in_origin_rail_zone(x, false);
			in.in_target_rail_zone = mfd_tbar_is_in_target_rail_zone(x, false);
			in.in_start_deadband = !mfd_tbar_is_past_start_zone(x, false);
			in.t_norm = mfd_tbar_rescale_from_start_zone(x, false);
			in.last_pos_1024 = sm_last_pos;
			mfd_tbar_compute_action_full(&in, &act);
			if (sm_state == MFD_TBAR_IDLE && act.next_state == MFD_TBAR_THROWING) {
				++sm_starts;
			}
			if (act.call_set_tbar) {
				++sm_writes;
				sm_last_pos = act.target_pos_1024;
			}
			if (act.call_release_tbar) {
				++sm_releases;
				sm_last_pos = 0;
			}
			sm_state = act.next_state;
		}
	}
	ASSERT_TRUE(sm_starts == 0);
	ASSERT_TRUE(sm_writes == 0);
	ASSERT_TRUE(sm_releases == 0);
	ASSERT_TRUE(sm_state == MFD_TBAR_IDLE);

	/* Mutation verification: old 3% start threshold (in_start_deadband = false) fails on CC 4 */
	memset(&in, 0, sizeof(in));
	in.studio_mode_active = true;
	in.slider_tbar_found = true;
	in.state = MFD_TBAR_IDLE;
	in.in_origin_rail_zone = mfd_tbar_is_in_origin_rail_zone(4.0f / 127.0f, false);
	in.in_target_rail_zone = false;
	in.in_start_deadband = false;
	in.t_norm = 4.0f / 127.0f;
	in.last_pos_1024 = 0;
	mfd_tbar_compute_action_full(&in, &act);
	ASSERT_TRUE(act.next_state == MFD_TBAR_THROWING && act.call_set_tbar);

	/* CC 0 -> 8 -> 5 (stays Throwing in deadband!) -> 3 (aborts once to Idle) */
	const int seq_abort[] = {0, 8, 5, 3};
	sm_state = MFD_TBAR_IDLE;
	sm_last_pos = 0;
	sm_starts = 0;
	int sm_cancels = 0;
	for (size_t si = 0U; si < sizeof(seq_abort) / sizeof(seq_abort[0]); ++si) {
		float x = (float)seq_abort[si] / 127.0f;
		memset(&in, 0, sizeof(in));
		in.studio_mode_active = true;
		in.slider_tbar_found = true;
		in.state = sm_state;
		in.in_origin_rail_zone = mfd_tbar_is_in_origin_rail_zone(x, false);
		in.in_target_rail_zone = mfd_tbar_is_in_target_rail_zone(x, false);
		in.in_start_deadband = !mfd_tbar_is_past_start_zone(x, false);
		in.t_norm = mfd_tbar_rescale_from_start_zone(x, false);
		in.last_pos_1024 = sm_last_pos;
		mfd_tbar_compute_action_full(&in, &act);
		if (sm_state == MFD_TBAR_IDLE && act.next_state == MFD_TBAR_THROWING) {
			++sm_starts;
		}
		if (act.aborted_throw) {
			++sm_cancels;
		}
		if (seq_abort[si] == 5) {
			ASSERT_TRUE(act.next_state == MFD_TBAR_THROWING);
			ASSERT_TRUE(!act.call_release_tbar);
		}
		sm_last_pos = act.call_release_tbar ? 0 : act.target_pos_1024;
		sm_state = act.next_state;
	}
	ASSERT_TRUE(sm_starts == 1);
	ASSERT_TRUE(sm_cancels == 1);
	ASSERT_TRUE(sm_state == MFD_TBAR_IDLE);

	printf("test-tbar-logic: PASS\n");
	return 0;
}
