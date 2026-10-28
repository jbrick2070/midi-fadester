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

void mfd_tbar_init(void);
void mfd_tbar_shutdown(void);

/*
 * Safe Studio Mode T-bar driver callback registered with mfd_engine.
 * Returns false and populates out_err if Studio Mode is active but "slider-tbar" is missing.
 */
bool mfd_tbar_driver_apply(float t_norm, bool *out_completed_throw, char *out_err, size_t err_size);

/*
 * Resets the per-transition T-bar release latch when a T-bar slot unparks or reinitializes.
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
