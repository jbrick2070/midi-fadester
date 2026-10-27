/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "core/midi-types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_APC40_DEFAULT_SLOT_COUNT 10U

struct mfd_apc40_default_slot {
	const char *label;
	struct mfd_midi_addr addr;
	enum mfd_target_type default_target;
	enum mfd_crossfade_curve default_curve;
};

bool mfd_is_apc40_mk2_name(const char *device_name);
size_t mfd_apc40_get_default_slots(struct mfd_apc40_default_slot *out_slots, size_t max_slots);

#ifdef __cplusplus
}
#endif
