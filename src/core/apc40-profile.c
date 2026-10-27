/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/apc40-profile.h"
#include <ctype.h>
#include <string.h>

static const struct mfd_apc40_default_slot k_apc40_defaults[MFD_APC40_DEFAULT_SLOT_COUNT] = {
	{"Crossfader (A/B)", {MFD_MIDI_CC, 0U, 15U, false}, MFD_TARGET_TBAR, MFD_CURVE_DIP_FREE},
	{"Track Fader 1", {MFD_MIDI_CC, 0U, 7U, false}, MFD_TARGET_TWO_SOURCE_MIX, MFD_CURVE_DIP_FREE},
	{"Track Fader 2", {MFD_MIDI_CC, 1U, 7U, false}, MFD_TARGET_SINGLE_LAYER, MFD_CURVE_DIP_FREE},
	{"Track Fader 3", {MFD_MIDI_CC, 2U, 7U, false}, MFD_TARGET_SINGLE_LAYER, MFD_CURVE_DIP_FREE},
	{"Track Fader 4", {MFD_MIDI_CC, 3U, 7U, false}, MFD_TARGET_SINGLE_LAYER, MFD_CURVE_DIP_FREE},
	{"Track Fader 5", {MFD_MIDI_CC, 4U, 7U, false}, MFD_TARGET_AUDIO_VOLUME, MFD_CURVE_DIP_FREE},
	{"Track Fader 6", {MFD_MIDI_CC, 5U, 7U, false}, MFD_TARGET_AUDIO_VOLUME, MFD_CURVE_DIP_FREE},
	{"Track Fader 7", {MFD_MIDI_CC, 6U, 7U, false}, MFD_TARGET_AUDIO_VOLUME, MFD_CURVE_DIP_FREE},
	{"Track Fader 8", {MFD_MIDI_CC, 7U, 7U, false}, MFD_TARGET_AUDIO_VOLUME, MFD_CURVE_DIP_FREE},
	{"Master Fader", {MFD_MIDI_CC, 0U, 14U, false}, MFD_TARGET_AUDIO_VOLUME, MFD_CURVE_DIP_FREE},
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

size_t mfd_apc40_get_default_slots(struct mfd_apc40_default_slot *out_slots, size_t max_slots)
{
	if (!out_slots || max_slots == 0U) {
		return MFD_APC40_DEFAULT_SLOT_COUNT;
	}
	size_t count = (max_slots < MFD_APC40_DEFAULT_SLOT_COUNT) ? max_slots : MFD_APC40_DEFAULT_SLOT_COUNT;
	memcpy(out_slots, k_apc40_defaults, count * sizeof(struct mfd_apc40_default_slot));
	return count;
}
