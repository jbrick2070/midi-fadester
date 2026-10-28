/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_TBAR_PRECISION 1024
#define MFD_TBAR_RANGE_MAX 1023
#define MFD_TBAR_CLAMP (MFD_TBAR_PRECISION / 10)
/*
 * OBS's minimum slider position for OBSBasic::TBarReleased() to finish rather than ignore/cancel
 * ((1023 - val) <= 102 ==> val >= 921, ~90.03%). This is OBS's minimum for a release, NOT our trigger.
 */
#define MFD_TBAR_OBS_MIN_RELEASE_POS (MFD_TBAR_RANGE_MAX - MFD_TBAR_CLAMP)
#define MFD_TBAR_COMPLETE_POS MFD_TBAR_OBS_MIN_RELEASE_POS

/*
 * Calibrated 3% rail zone threshold (R-TBAR-SM, zero timers anywhere):
 * - Lower rail zone: x <= 0.03 (CC 0..3 under default [0, 127])
 * - Upper rail zone: x >= 0.97 (CC 124..127 under default [0, 127]; CC 122..125 with 125 = 1.0 under [0, 125])
 */
#define MFD_TBAR_RAIL_ZONE 0.03f

#define MFD_CAL_DEFAULT_MIN 0
#define MFD_CAL_DEFAULT_MAX_7BIT 127
#define MFD_CAL_DEFAULT_MAX_14BIT 16383

struct mfd_control_calibration {
	int cal_min;
	int cal_max;
};

void mfd_calibration_init(struct mfd_control_calibration *cal, bool is_14bit);
void mfd_calibration_set(struct mfd_control_calibration *cal, bool is_14bit, int cal_min, int cal_max);
bool mfd_calibration_observe_and_widen(struct mfd_control_calibration *cal, int raw_value);
float mfd_calibration_normalize(const struct mfd_control_calibration *cal, int raw_value, bool invert);

/*
 * Explicit timer-free Studio Mode T-bar state machine states (R-TBAR-SM Section 5.1.1):
 *   Idle     -> No active manual throw (t = 0, release_latched = false, parked = false)
 *   Throwing -> Active manual transition in (0.03, 0.97)
 *   Parked   -> Throw completed at target rail (release_latched = true, parked = true)
 */
enum mfd_tbar_state {
	MFD_TBAR_IDLE = 0,
	MFD_TBAR_THROWING = 1,
	MFD_TBAR_PARKED = 2,
};

struct mfd_tbar_release_input {
	bool studio_mode_active;
	bool slider_tbar_found;
	enum mfd_tbar_state state;
	bool release_latched;
	bool force_complete;
	bool force_abort;
	bool in_target_rail_zone;
	bool in_origin_rail_zone;
	float t_norm;
	int last_pos_1024;
};

struct mfd_tbar_action {
	bool allow_frontend_calls;
	bool report_missing_slider_error;
	enum mfd_tbar_state next_state;
	int target_pos_1024;
	bool call_set_tbar;
	bool call_release_tbar;
	bool completed_throw;
	bool aborted_throw;
};

bool mfd_tbar_is_in_target_rail_zone(float x_cal, bool polarity_reversed);
bool mfd_tbar_is_in_origin_rail_zone(float x_cal, bool polarity_reversed);
bool mfd_tbar_is_in_rail_zone(float x_cal, bool polarity_reversed);

/*
 * Timer-free pure-C decision function implementing the Section 5.1.1 (R-TBAR-SM) T-bar state machine:
 * - Guards against OBSBasic::tBar uninitialized/dangling pointer crash when studio_mode_active is false.
 * - Reports a loud error if studio_mode_active is true but the Qt "slider-tbar" QSlider is missing.
 * - Idle: writes nothing inside the origin 3% rail zone; enters Throwing on (0.03, 0.97); completes to Parked if
 *   jumping directly into the target 3% rail zone or on force_complete.
 * - Throwing: drives pos in [1, 1022] linearly with zero idle/stall timers; completes (pos = 1023 + release once ->
 *   Parked) only in the target 3% calibrated rail zone or on force_complete; aborts (pos = 0 + release once -> Idle,
 *   no polarity flip, no park) when returning into the origin 3% calibrated rail zone or on force_abort.
 * - Parked: writes nothing and never calls release until an explicit unpark leaves Parked.
 */
void mfd_tbar_compute_action_full(const struct mfd_tbar_release_input *in, struct mfd_tbar_action *out_action);

void mfd_tbar_compute_action_ex(bool studio_mode_active, bool slider_tbar_found, bool release_latched, float t_norm,
				int last_pos_1024, struct mfd_tbar_action *out_action);

void mfd_tbar_compute_action(bool studio_mode_active, bool slider_tbar_found, float t_norm, int last_pos_1024,
			     struct mfd_tbar_action *out_action);

bool mfd_should_show_studio_mode_note(bool studio_mode_active, size_t active_tbar_slot_count);

#ifdef __cplusplus
}
#endif
