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
	const char *id;
	const char *label;
	const char *kind;
	struct mfd_midi_addr addr;
	enum mfd_target_type default_target;
	enum mfd_crossfade_curve default_curve;
};

bool mfd_is_apc40_mk2_name(const char *device_name);
size_t mfd_apc40_get_default_slots(struct mfd_apc40_default_slot *out_slots, size_t max_slots);

int mfd_slot_index_for_id(const char *slot_id);
const char *mfd_slot_id_for_index(int index);
const char *mfd_slot_kind_for_index(int index);

const char *mfd_target_mode_to_string(enum mfd_target_type target);
bool mfd_target_mode_from_string(const char *str, enum mfd_target_type *out_target);

const char *mfd_curve_to_string(enum mfd_crossfade_curve curve);
bool mfd_curve_from_string(const char *str, enum mfd_crossfade_curve *out_curve);

void mfd_row_controls_for_target(enum mfd_target_type target, bool *out_show_source_a, bool *out_show_source_b,
				 bool *out_show_curve, bool *out_show_afv);

#ifdef __cplusplus
}
#endif
