/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "core/tbar-logic.h"

#ifdef __cplusplus
extern "C" {
#endif

void mfd_tbar_init(void);
void mfd_tbar_shutdown(void);

/*
 * Safe, timer-free Studio Mode T-bar driver callback registered with mfd_engine (R-TBAR-SM).
 * Returns false and populates out_err if Studio Mode is active but "slider-tbar" is missing.
 */
bool mfd_tbar_driver_apply(enum mfd_tbar_state state, float t_norm, bool force_complete, bool force_abort,
			   bool in_target_rail_zone, bool in_origin_rail_zone, bool in_start_deadband,
			   enum mfd_tbar_state *out_next_state, bool *out_completed_throw, bool *out_aborted_throw,
			   char *out_err, size_t err_size);

/*
 * Resets the per-transition T-bar release latch when a new throw starts or unparks.
 */
void mfd_tbar_reset_release_latch(void);
bool mfd_tbar_is_release_latched(void);

/*
 * Returns true if Studio Mode is currently active in OBS.
 */
bool mfd_tbar_is_studio_mode_active(void);

#ifdef __cplusplus
}
#endif
