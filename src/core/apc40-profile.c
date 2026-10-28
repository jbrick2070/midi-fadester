/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/apc40-profile.h"
#include <ctype.h>
#include <string.h>

static const struct mfd_apc40_default_slot k_apc40_defaults[MFD_APC40_DEFAULT_SLOT_COUNT] = {
	{"crossfader",
	 "Crossfader (A/B)",
	 "crossfader",
	 {MFD_MIDI_CC, 0U, 15U, false},
	 MFD_TARGET_TBAR,
	 MFD_CURVE_DIP_FREE},
	{"fader_1", "Track Fader 1", "fader", {MFD_MIDI_CC, 0U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_2", "Track Fader 2", "fader", {MFD_MIDI_CC, 1U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_3", "Track Fader 3", "fader", {MFD_MIDI_CC, 2U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_4", "Track Fader 4", "fader", {MFD_MIDI_CC, 3U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_5", "Track Fader 5", "fader", {MFD_MIDI_CC, 4U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_6", "Track Fader 6", "fader", {MFD_MIDI_CC, 5U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_7", "Track Fader 7", "fader", {MFD_MIDI_CC, 6U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"fader_8", "Track Fader 8", "fader", {MFD_MIDI_CC, 7U, 7U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
	{"master", "Master Fader", "master", {MFD_MIDI_CC, 0U, 14U, false}, MFD_TARGET_UNASSIGNED, MFD_CURVE_DIP_FREE},
};

static bool contains_case_insensitive(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle) {
		return false;
	}
	size_t nlen = strlen(needle);
	for (const char *p = haystack; *p; ++p) {
		size_t i = 0;
		while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
			++i;
		}
		if (i == nlen) {
			return true;
		}
	}
	return false;
}

bool mfd_is_apc40_mk2_name(const char *device_name)
{
	if (!device_name) {
		return false;
	}
	return contains_case_insensitive(device_name, "APC40 mkII") ||
	       contains_case_insensitive(device_name, "APC40 mk2") || contains_case_insensitive(device_name, "APC40");
}

enum mfd_controller_profile mfd_profile_for_device(const char *configured_name, bool is_open, const char *opened_name)
{
	if (configured_name && configured_name[0] != '\0') {
		return mfd_is_apc40_mk2_name(configured_name) ? MFD_PROFILE_APC40_MK2 : MFD_PROFILE_GENERIC;
	}
	if (is_open && opened_name && opened_name[0] != '\0' && mfd_is_apc40_mk2_name(opened_name)) {
		return MFD_PROFILE_APC40_MK2;
	}
	return MFD_PROFILE_GENERIC;
}

size_t mfd_apc40_get_default_slots(struct mfd_apc40_default_slot *out_slots, size_t max_slots)
{
	if (!out_slots || max_slots == 0U) {
		return MFD_APC40_DEFAULT_SLOT_COUNT;
	}
	size_t count = (max_slots < MFD_APC40_DEFAULT_SLOT_COUNT) ? max_slots : MFD_APC40_DEFAULT_SLOT_COUNT;
	memcpy(out_slots, k_apc40_defaults, count * sizeof(struct mfd_apc40_default_slot));
	return count;
}

int mfd_slot_index_for_id(const char *slot_id)
{
	if (!slot_id || !*slot_id) {
		return -1;
	}
	for (size_t i = 0U; i < MFD_APC40_DEFAULT_SLOT_COUNT; ++i) {
		if (strcmp(k_apc40_defaults[i].id, slot_id) == 0) {
			return (int)i;
		}
	}
	return -1;
}

const char *mfd_slot_id_for_index(int index)
{
	if (index < 0 || (size_t)index >= MFD_APC40_DEFAULT_SLOT_COUNT) {
		return "";
	}
	return k_apc40_defaults[index].id;
}

const char *mfd_slot_kind_for_index(int index)
{
	if (index < 0 || (size_t)index >= MFD_APC40_DEFAULT_SLOT_COUNT) {
		return "fader";
	}
	return k_apc40_defaults[index].kind;
}

const char *mfd_target_mode_to_string(enum mfd_target_type target)
{
	switch (target) {
	case MFD_TARGET_TBAR:
		return "tbar";
	case MFD_TARGET_TWO_SOURCE_MIX:
		return "crossfade_ab";
	case MFD_TARGET_SINGLE_LAYER:
		return "layer_opacity";
	case MFD_TARGET_AUDIO_VOLUME:
		return "audio_volume";
	case MFD_TARGET_UNASSIGNED:
	default:
		return "unassigned";
	}
}

bool mfd_target_mode_from_string(const char *str, enum mfd_target_type *out_target)
{
	if (!str || !out_target) {
		return false;
	}
	if (strcmp(str, "unassigned") == 0) {
		*out_target = MFD_TARGET_UNASSIGNED;
		return true;
	}
	if (strcmp(str, "tbar") == 0) {
		*out_target = MFD_TARGET_TBAR;
		return true;
	}
	if (strcmp(str, "crossfade_ab") == 0) {
		*out_target = MFD_TARGET_TWO_SOURCE_MIX;
		return true;
	}
	if (strcmp(str, "layer_opacity") == 0) {
		*out_target = MFD_TARGET_SINGLE_LAYER;
		return true;
	}
	if (strcmp(str, "audio_volume") == 0) {
		*out_target = MFD_TARGET_AUDIO_VOLUME;
		return true;
	}
	return false;
}

const char *mfd_curve_to_string(enum mfd_crossfade_curve curve)
{
	switch (curve) {
	case MFD_CURVE_DIP_FREE:
		return "dip_free";
	case MFD_CURVE_FULL_CENTER:
		return "full_center";
	case MFD_CURVE_COSINE:
		return "equal_power";
	case MFD_CURVE_LINEAR:
		return "linear";
	case MFD_CURVE_CUT:
		return "cut";
	default:
		return "dip_free";
	}
}

bool mfd_curve_from_string(const char *str, enum mfd_crossfade_curve *out_curve)
{
	if (!str || !out_curve) {
		return false;
	}
	if (strcmp(str, "dip_free") == 0) {
		*out_curve = MFD_CURVE_DIP_FREE;
		return true;
	}
	if (strcmp(str, "full_center") == 0) {
		*out_curve = MFD_CURVE_FULL_CENTER;
		return true;
	}
	if (strcmp(str, "equal_power") == 0) {
		*out_curve = MFD_CURVE_COSINE;
		return true;
	}
	if (strcmp(str, "linear") == 0) {
		*out_curve = MFD_CURVE_LINEAR;
		return true;
	}
	if (strcmp(str, "cut") == 0) {
		*out_curve = MFD_CURVE_CUT;
		return true;
	}
	return false;
}

void mfd_row_controls_for_target(enum mfd_target_type target, bool *out_show_source_a, bool *out_show_source_b,
				 bool *out_show_curve, bool *out_show_afv)
{
	bool show_a = false;
	bool show_b = false;
	bool show_curve = false;
	bool show_afv = false;

	switch (target) {
	case MFD_TARGET_TWO_SOURCE_MIX:
		show_a = true;
		show_b = true;
		show_curve = true;
		show_afv = true;
		break;
	case MFD_TARGET_SINGLE_LAYER:
		show_a = true;
		show_b = false;
		show_curve = false;
		show_afv = true;
		break;
	case MFD_TARGET_AUDIO_VOLUME:
		show_a = true;
		show_b = false;
		show_curve = false;
		show_afv = false;
		break;
	case MFD_TARGET_TBAR:
	case MFD_TARGET_UNASSIGNED:
	default:
		break;
	}

	if (out_show_source_a) {
		*out_show_source_a = show_a;
	}
	if (out_show_source_b) {
		*out_show_source_b = show_b;
	}
	if (out_show_curve) {
		*out_show_curve = show_curve;
	}
	if (out_show_afv) {
		*out_show_afv = show_afv;
	}
}
