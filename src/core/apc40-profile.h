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

enum mfd_controller_profile {
	MFD_PROFILE_GENERIC = 0,
	MFD_PROFILE_APC40_MK2 = 1,
	MFD_PROFILE_APC_MINI_MK2 = 2,
	MFD_PROFILE_AKAI_MIDIMIX = 3,
	MFD_PROFILE_KORG_NANOKONTROL2 = 4,
	MFD_PROFILE_LAUNCH_CONTROL_XL = 5,
};

enum mfd_profile_support_tier {
	MFD_PROFILE_TIER_CUSTOM = 0,
	MFD_PROFILE_TIER_VERIFIED = 1,
	MFD_PROFILE_TIER_BETA_UNTESTED = 2,
};

enum mfd_desk_upper_style {
	MFD_DESK_UPPER_NONE = 0,
	MFD_DESK_UPPER_CLIP_PADS = 1,
	MFD_DESK_UPPER_KNOB_ROWS_3 = 2,
	MFD_DESK_UPPER_TRANSPORT_AND_KNOBS_1 = 3,
};

struct mfd_controller_profile_def {
	enum mfd_controller_profile id;
	const char *slug;
	const char *display_name;
	const char *short_name;
	enum mfd_profile_support_tier tier;
	const char *spec_note;
	bool has_crossfader;
	int track_fader_count;
	bool has_master_fader;
	enum mfd_desk_upper_style upper_style;
	const char *const *usb_match_tokens;
	struct mfd_apc40_default_slot slots[MFD_APC40_DEFAULT_SLOT_COUNT];
};

size_t mfd_profile_count(void);
const struct mfd_controller_profile_def *mfd_profile_get_by_index(size_t index);
const struct mfd_controller_profile_def *mfd_profile_get_def(enum mfd_controller_profile profile);
const struct mfd_controller_profile_def *mfd_profile_find_by_slug(const char *slug);
const char *mfd_profile_tier_to_string(enum mfd_profile_support_tier tier);
bool mfd_profile_has_physical_slot(enum mfd_controller_profile profile, int slot_index);
enum mfd_controller_profile mfd_profile_match_device_name(const char *device_name);

bool mfd_is_apc40_mk2_name(const char *device_name);
enum mfd_controller_profile mfd_profile_for_device(const char *configured_name, bool is_open, const char *opened_name);
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
