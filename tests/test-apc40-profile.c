/*
 * Midi Fadester — test-apc40-profile
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/apc40-profile.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	REQUIRE(mfd_is_apc40_mk2_name("APC40 mkII"));
	REQUIRE(mfd_is_apc40_mk2_name("Akai APC40 mk2 (MIDI 1)"));
	REQUIRE(!mfd_is_apc40_mk2_name("APC mini mk2"));
	REQUIRE(!mfd_is_apc40_mk2_name("MIDI Mix"));
	REQUIRE(!mfd_is_apc40_mk2_name("Launchpad Mini"));
	REQUIRE(!mfd_is_apc40_mk2_name(NULL));

	/* Auto-selected profile resolution across all registered controllers */
	REQUIRE(mfd_profile_for_device("APC40 mkII", false, "") == MFD_PROFILE_APC40_MK2);
	REQUIRE(mfd_profile_for_device("APC40 mkII", true, "APC40 mkII") == MFD_PROFILE_APC40_MK2);
	REQUIRE(mfd_profile_for_device("APC mini mk2", true, "APC mini mk2") == MFD_PROFILE_APC_MINI_MK2);
	REQUIRE(mfd_profile_for_device("MIDI Mix", true, "MIDI Mix") == MFD_PROFILE_AKAI_MIDIMIX);
	REQUIRE(mfd_profile_for_device("Akai MIDImix", false, "") == MFD_PROFILE_AKAI_MIDIMIX);
	REQUIRE(mfd_profile_for_device("nanoKONTROL2 1 SLIDER/KNOB", true, "nanoKONTROL2 1 SLIDER/KNOB") ==
		MFD_PROFILE_KORG_NANOKONTROL2);
	REQUIRE(mfd_profile_for_device("Launch Control XL", false, "") == MFD_PROFILE_LAUNCH_CONTROL_XL);
	REQUIRE(mfd_profile_for_device("loopMIDI Port", true, "loopMIDI Port") == MFD_PROFILE_GENERIC);
	REQUIRE(mfd_profile_for_device("", true, "APC40 mkII") == MFD_PROFILE_APC40_MK2);
	REQUIRE(mfd_profile_for_device("", true, "MIDI Mix") == MFD_PROFILE_AKAI_MIDIMIX);
	REQUIRE(mfd_profile_for_device("", true, "loopMIDI Port") == MFD_PROFILE_GENERIC);
	REQUIRE(mfd_profile_for_device("", false, "") == MFD_PROFILE_GENERIC);

	/* Registry enumeration, slug lookup, support tiers, and uniqueness */
	REQUIRE(mfd_profile_count() == 6U);
	REQUIRE(mfd_profile_get_by_index(6U) == NULL);
	REQUIRE(mfd_profile_find_by_slug(NULL) == NULL);
	REQUIRE(mfd_profile_find_by_slug("") == NULL);
	REQUIRE(mfd_profile_find_by_slug("nonexistent_desk") == NULL);

	REQUIRE(strcmp(mfd_profile_tier_to_string(MFD_PROFILE_TIER_CUSTOM), "custom") == 0);
	REQUIRE(strcmp(mfd_profile_tier_to_string(MFD_PROFILE_TIER_VERIFIED), "verified") == 0);
	REQUIRE(strcmp(mfd_profile_tier_to_string(MFD_PROFILE_TIER_BETA_UNTESTED), "beta") == 0);

	size_t verified_count = 0U;
	size_t beta_count = 0U;
	for (size_t pi = 0U; pi < mfd_profile_count(); ++pi) {
		const struct mfd_controller_profile_def *def = mfd_profile_get_by_index(pi);
		REQUIRE(def != NULL);
		REQUIRE(def->slug != NULL && def->slug[0] != '\0');
		REQUIRE(def->display_name != NULL && def->display_name[0] != '\0');
		REQUIRE(def->short_name != NULL && def->short_name[0] != '\0');
		REQUIRE(def->spec_note != NULL && def->spec_note[0] != '\0');
		REQUIRE(mfd_profile_find_by_slug(def->slug) == def);
		REQUIRE(mfd_profile_get_def(def->id) == def);

		if (def->tier == MFD_PROFILE_TIER_VERIFIED) {
			++verified_count;
		} else if (def->tier == MFD_PROFILE_TIER_BETA_UNTESTED) {
			++beta_count;
			REQUIRE(strstr(def->spec_note, "Beta (untested on hardware)") != NULL);
		}

		/* Verify canonical 10 slot IDs & zero duplicate MIDI CC bindings within any profile */
		for (size_t s = 0U; s < MFD_APC40_DEFAULT_SLOT_COUNT; ++s) {
			REQUIRE(strcmp(def->slots[s].id, mfd_slot_id_for_index((int)s)) == 0);
			REQUIRE(strcmp(def->slots[s].kind, mfd_slot_kind_for_index((int)s)) == 0);
			const struct mfd_midi_addr *a = &def->slots[s].addr;
			if (def->id != MFD_PROFILE_GENERIC) {
				bool phys = mfd_profile_has_physical_slot(def->id, (int)s);
				REQUIRE(phys == (a->type != MFD_MIDI_NONE));
			}
			if (a->type == MFD_MIDI_NONE) {
				continue;
			}
			for (size_t s2 = s + 1U; s2 < MFD_APC40_DEFAULT_SLOT_COUNT; ++s2) {
				const struct mfd_midi_addr *b = &def->slots[s2].addr;
				if (b->type == MFD_MIDI_NONE) {
					continue;
				}
				bool same = (a->type == b->type && a->channel == b->channel && a->number == b->number);
				REQUIRE(!same);
			}
		}
	}
	REQUIRE(verified_count == 1U);
	REQUIRE(beta_count == 4U);

	/* Case-insensitive slug lookup */
	REQUIRE(mfd_profile_find_by_slug("APC40MK2") == mfd_profile_get_def(MFD_PROFILE_APC40_MK2));
	REQUIRE(mfd_profile_find_by_slug("MidiMix") == mfd_profile_get_def(MFD_PROFILE_AKAI_MIDIMIX));

	/* Verify APC40 mkII exact CCs (backward-compatible mfd_apc40_get_default_slots) */
	struct mfd_apc40_default_slot slots[MFD_APC40_DEFAULT_SLOT_COUNT];
	size_t n = mfd_apc40_get_default_slots(slots, MFD_APC40_DEFAULT_SLOT_COUNT);
	REQUIRE(n == MFD_APC40_DEFAULT_SLOT_COUNT);
	REQUIRE(strcmp(slots[0].id, "crossfader") == 0);
	REQUIRE(slots[0].addr.type == MFD_MIDI_CC && slots[0].addr.channel == 0U && slots[0].addr.number == 15U);
	REQUIRE(slots[0].default_target == MFD_TARGET_TBAR);
	for (uint8_t ch = 0; ch < 8U; ++ch) {
		REQUIRE(slots[1U + ch].addr.type == MFD_MIDI_CC);
		REQUIRE(slots[1U + ch].addr.channel == ch);
		REQUIRE(slots[1U + ch].addr.number == 7U);
		REQUIRE(slots[1U + ch].default_target == MFD_TARGET_UNASSIGNED);
	}
	REQUIRE(slots[9].addr.type == MFD_MIDI_CC && slots[9].addr.channel == 0U && slots[9].addr.number == 14U);

	/* Verify Beta Profile 1: Akai APC mini mk2 (Ch 1 CC 48..55 + Master CC 56, no crossfader) */
	const struct mfd_controller_profile_def *apc_mini = mfd_profile_get_def(MFD_PROFILE_APC_MINI_MK2);
	REQUIRE(!apc_mini->has_crossfader && apc_mini->has_master_fader && apc_mini->track_fader_count == 8);
	REQUIRE(apc_mini->slots[0].addr.type == MFD_MIDI_NONE);
	for (uint8_t i = 0; i < 8U; ++i) {
		REQUIRE(apc_mini->slots[1U + i].addr.type == MFD_MIDI_CC);
		REQUIRE(apc_mini->slots[1U + i].addr.channel == 0U);
		REQUIRE(apc_mini->slots[1U + i].addr.number == (uint8_t)(48U + i));
	}
	REQUIRE(apc_mini->slots[9].addr.type == MFD_MIDI_CC && apc_mini->slots[9].addr.number == 56U);

	/* Verify Beta Profile 2: Akai MIDImix (Ch 1 CC 19,23,27,31,49,53,57,61 + Master CC 62, no crossfader) */
	const struct mfd_controller_profile_def *midimix = mfd_profile_get_def(MFD_PROFILE_AKAI_MIDIMIX);
	const uint8_t expected_midimix_ccs[8] = {19U, 23U, 27U, 31U, 49U, 53U, 57U, 61U};
	REQUIRE(!midimix->has_crossfader && midimix->has_master_fader);
	REQUIRE(midimix->slots[0].addr.type == MFD_MIDI_NONE);
	for (uint8_t i = 0; i < 8U; ++i) {
		REQUIRE(midimix->slots[1U + i].addr.type == MFD_MIDI_CC);
		REQUIRE(midimix->slots[1U + i].addr.channel == 0U);
		REQUIRE(midimix->slots[1U + i].addr.number == expected_midimix_ccs[i]);
	}
	REQUIRE(midimix->slots[9].addr.type == MFD_MIDI_CC && midimix->slots[9].addr.number == 62U);

	/* Verify Beta Profile 3: Korg nanoKONTROL2 (Ch 1 CC 0..7, no crossfader, no master) */
	const struct mfd_controller_profile_def *nano = mfd_profile_get_def(MFD_PROFILE_KORG_NANOKONTROL2);
	REQUIRE(!nano->has_crossfader && !nano->has_master_fader);
	REQUIRE(nano->slots[0].addr.type == MFD_MIDI_NONE);
	for (uint8_t i = 0; i < 8U; ++i) {
		REQUIRE(nano->slots[1U + i].addr.type == MFD_MIDI_CC);
		REQUIRE(nano->slots[1U + i].addr.channel == 0U);
		REQUIRE(nano->slots[1U + i].addr.number == i);
	}
	REQUIRE(nano->slots[9].addr.type == MFD_MIDI_NONE);

	/* Verify Beta Profile 4: Novation Launch Control XL (Ch 1 CC 77..84, no crossfader, no master) */
	const struct mfd_controller_profile_def *lcxl = mfd_profile_get_def(MFD_PROFILE_LAUNCH_CONTROL_XL);
	REQUIRE(!lcxl->has_crossfader && !lcxl->has_master_fader);
	REQUIRE(lcxl->slots[0].addr.type == MFD_MIDI_NONE);
	for (uint8_t i = 0; i < 8U; ++i) {
		REQUIRE(lcxl->slots[1U + i].addr.type == MFD_MIDI_CC);
		REQUIRE(lcxl->slots[1U + i].addr.channel == 0U);
		REQUIRE(lcxl->slots[1U + i].addr.number == (uint8_t)(77U + i));
	}
	REQUIRE(lcxl->slots[9].addr.type == MFD_MIDI_NONE);

	/* Slot ID lookup */
	REQUIRE(mfd_slot_index_for_id("crossfader") == 0);
	REQUIRE(mfd_slot_index_for_id("fader_1") == 1);
	REQUIRE(mfd_slot_index_for_id("fader_8") == 8);
	REQUIRE(mfd_slot_index_for_id("master") == 9);
	REQUIRE(mfd_slot_index_for_id("fader_9") == -1);

	/* Row control visibility per Target Mode (Item 5) */
	bool show_a = true;
	bool show_b = true;
	bool show_curve = true;
	bool show_afv = true;

	mfd_row_controls_for_target(MFD_TARGET_UNASSIGNED, &show_a, &show_b, &show_curve, &show_afv);
	REQUIRE(!show_a && !show_b && !show_curve && !show_afv);

	mfd_row_controls_for_target(MFD_TARGET_TBAR, &show_a, &show_b, &show_curve, &show_afv);
	REQUIRE(!show_a && !show_b && !show_curve && !show_afv);

	mfd_row_controls_for_target(MFD_TARGET_SINGLE_LAYER, &show_a, &show_b, &show_curve, &show_afv);
	REQUIRE(show_a && !show_b && !show_curve && show_afv);

	mfd_row_controls_for_target(MFD_TARGET_AUDIO_VOLUME, &show_a, &show_b, &show_curve, &show_afv);
	REQUIRE(show_a && !show_b && !show_curve && !show_afv);

	mfd_row_controls_for_target(MFD_TARGET_TWO_SOURCE_MIX, &show_a, &show_b, &show_curve, &show_afv);
	REQUIRE(show_a && show_b && show_curve && show_afv);

	printf("test-apc40-profile: PASS\n");
	return 0;
}
