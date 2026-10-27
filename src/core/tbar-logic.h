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

#define MFD_TBAR_RANGE_MAX 1023

struct mfd_tbar_action {
	bool allow_frontend_calls;
	bool report_missing_slider_error;
	int target_pos_1024;
	bool call_set_tbar;
	bool call_release_tbar;
};

/*
 * Pure-C decision function for driving OBS 33's Studio Mode T-bar safely:
 * - Guards against OBSBasic::tBar uninitialized/dangling pointer crash when studio_mode_active is false.
 * - Reports a loud error if studio_mode_active is true but the Qt "slider-tbar" QSlider is missing.
 * - Maps normalized [0.0f, 1.0f] to [0, 1023] and triggers release_tbar() once upon reaching 1023.
 */
void mfd_tbar_compute_action(bool studio_mode_active, bool slider_tbar_found, float t_norm, int last_pos_1024,
			     struct mfd_tbar_action *out_action);

bool mfd_should_show_studio_mode_note(bool studio_mode_active, size_t active_tbar_slot_count);

#ifdef __cplusplus
}
#endif
