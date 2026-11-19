/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The colour an APC40 mkII RGB pad shows for a velocity (0..127), as 0xRRGGBB. */
uint32_t mfd_apc40_palette_rgb(uint8_t velocity);

#ifdef __cplusplus
}
#endif
