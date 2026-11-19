/*
 * Visual Sidechain - Audio Modulator Filter (Layer 3b)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

extern struct obs_source_info vs_modulator_filter_info;

void vs_register_modulator_filter(void);
void vs_modulator_filter_apply_panic(obs_source_t *source);
void vs_modulator_filter_apply_restore(obs_source_t *source);

#ifdef __cplusplus
}
#endif
