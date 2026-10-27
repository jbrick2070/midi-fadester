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
	REQUIRE(slots[0].addr.type == MFD_MIDI_CC);
	REQUIRE(slots[0].addr.channel == 0U);
	REQUIRE(slots[0].addr.number == 15U);
	REQUIRE(slots[0].default_target == MFD_TARGET_TBAR);

	/* Slots 1..8: Track Faders 1..8 (Ch 1..8 / 0..7, CC 7) */
	for (uint8_t ch = 0; ch < 8U; ++ch) {
		REQUIRE(slots[1U + ch].addr.type == MFD_MIDI_CC);
		REQUIRE(slots[1U + ch].addr.channel == ch);
		REQUIRE(slots[1U + ch].addr.number == 7U);
	}

	/* Slot 9: Master Fader (Ch 1 / 0, CC 14) */
	REQUIRE(slots[9].addr.type == MFD_MIDI_CC);
	REQUIRE(slots[9].addr.channel == 0U);
	REQUIRE(slots[9].addr.number == 14U);

	printf("test-apc40-profile: PASS\n");
	return 0;
}
