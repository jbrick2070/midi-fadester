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
#define MFD_TBAR_MIN_RELEASE_NORM 0.90f

/*
 * Primary release trigger: physical fader enters the 3% rail zone
 * (CC >= 124 in normal polarity: 124/127 = 0.9764 >= 0.97; CC <= 3 in reversed polarity: 3/127 = 0.0236 <= 0.03).
 */
#define MFD_TBAR_RAIL_ZONE 0.03f
#define MFD_TBAR_RAIL_RELEASE_POS 992

/*
 * Fallback release trigger for short-travel faders that cannot reach the 3% rail zone:
 * fader has stopped moving for >= 150 ms while above 90% (pos >= 921).
 */
#define MFD_TBAR_FALLBACK_STALL_SEC 0.150f

struct mfd_tbar_release_input {
	bool studio_mode_active;
	bool slider_tbar_found;
	bool release_latched;
	bool force_release;
	bool in_rail_zone;
	bool smoother_settled;
	float t_norm;
	float fader_idle_sec;
	int last_pos_1024;
};

struct mfd_tbar_action {
	bool allow_frontend_calls;
	bool report_missing_slider_error;
	int target_pos_1024;
	bool call_set_tbar;
	bool call_release_tbar;
};

bool mfd_tbar_is_in_rail_zone(float raw_fader_norm, bool polarity_reversed);

/*
 * Pure-C decision function for driving OBS 33's Studio Mode T-bar safely:
 * - Guards against OBSBasic::tBar uninitialized/dangling pointer crash when studio_mode_active is false.
 * - Reports a loud error if studio_mode_active is true but the Qt "slider-tbar" QSlider is missing.
 * - Enforces a per-transition release latch: when release_latched is true, zero slider writes and zero releases
 *   are permitted until an explicit unpark clears the latch.
 * - Maps normalized [0.0f, 1.0f] linearly to [0, 1023] all the way to 100% (CC 127 = 1.0 -> 1023), releasing
 *   only when entering the 3% rail zone (CC >= 124 or CC <= 3 under reversed polarity), on explicit force_release,
 *   or when the fader has stopped moving for >= 150 ms while above OBS's 90% minimum (pos >= 921).
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
