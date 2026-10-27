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
	REQUIRE(!mfd_is_apc40_mk2_name("Launchpad Mini"));
	REQUIRE(!mfd_is_apc40_mk2_name(NULL));

	struct mfd_apc40_default_slot slots[MFD_APC40_DEFAULT_SLOT_COUNT];
	size_t n = mfd_apc40_get_default_slots(slots, MFD_APC40_DEFAULT_SLOT_COUNT);
	REQUIRE(n == MFD_APC40_DEFAULT_SLOT_COUNT);

	/* Slot 0: Crossfader (Ch 1 / 0, CC 15) -> Studio Mode T-Bar (D-1) */
	REQUIRE(strcmp(slots[0].id, "crossfader") == 0);
	REQUIRE(strcmp(slots[0].kind, "crossfader") == 0);
	REQUIRE(slots[0].addr.type == MFD_MIDI_CC);
	REQUIRE(slots[0].addr.channel == 0U);
	REQUIRE(slots[0].addr.number == 15U);
	REQUIRE(slots[0].default_target == MFD_TARGET_TBAR);

	/* Slots 1..8: Track Faders 1..8 (Ch 1..8 / 0..7, CC 7) -> start Unassigned (Item 4) */
	for (uint8_t ch = 0; ch < 8U; ++ch) {
		char expected_id[16];
		snprintf(expected_id, sizeof(expected_id), "fader_%u", (unsigned)(ch + 1U));
		REQUIRE(strcmp(slots[1U + ch].id, expected_id) == 0);
		REQUIRE(strcmp(slots[1U + ch].kind, "fader") == 0);
		REQUIRE(slots[1U + ch].addr.type == MFD_MIDI_CC);
		REQUIRE(slots[1U + ch].addr.channel == ch);
		REQUIRE(slots[1U + ch].addr.number == 7U);
		REQUIRE(slots[1U + ch].default_target == MFD_TARGET_UNASSIGNED);
	}

	/* Slot 9: Master Fader (Ch 1 / 0, CC 14) -> starts Unassigned (Item 4) */
	REQUIRE(strcmp(slots[9].id, "master") == 0);
	REQUIRE(strcmp(slots[9].kind, "master") == 0);
	REQUIRE(slots[9].addr.type == MFD_MIDI_CC);
	REQUIRE(slots[9].addr.channel == 0U);
	REQUIRE(slots[9].addr.number == 14U);
	REQUIRE(slots[9].default_target == MFD_TARGET_UNASSIGNED);

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
