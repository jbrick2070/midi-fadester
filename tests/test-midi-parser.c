/*
 * Midi Fadester — test-midi-parser
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/midi-parser.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static uint32_t pack_midi(uint8_t status, uint8_t data1, uint8_t data2)
{
	return (uint32_t)status | ((uint32_t)data1 << 8U) | ((uint32_t)data2 << 16U);
}

int main(void)
{
	struct mfd_midi_parser p;
	struct mfd_midi_decoded dec;
	mfd_midi_parser_init(&p);

	/* 1. Channel-aware APC40 mkII CC 7 across Channels 1..8 (0..7) */
	struct mfd_midi_addr strip1 = {MFD_MIDI_CC, 0U, 7U, false};
	struct mfd_midi_addr strip2 = {MFD_MIDI_CC, 1U, 7U, false};

	for (uint8_t ch = 0; ch < 8U; ++ch) {
		uint8_t val = (uint8_t)(ch * 15U + 10U);
		REQUIRE(mfd_midi_parser_feed(&p, pack_midi((uint8_t)(0xB0U | ch), 7U, val), &dec));
		REQUIRE(dec.addr.type == MFD_MIDI_CC);
		REQUIRE(dec.addr.channel == ch);
		REQUIRE(dec.addr.number == 7U);
		REQUIRE(!dec.addr.is_14bit);
		REQUIRE(dec.raw_value == val);
		REQUIRE(fabsf(dec.normalized - ((float)val / 127.0f)) < 1e-6f);

		if (ch == 0U) {
			REQUIRE(mfd_midi_addr_matches(&strip1, &dec.addr));
			REQUIRE(!mfd_midi_addr_matches(&strip2, &dec.addr));
		} else if (ch == 1U) {
			REQUIRE(!mfd_midi_addr_matches(&strip1, &dec.addr));
			REQUIRE(mfd_midi_addr_matches(&strip2, &dec.addr));
		}
	}

	/* 2. 14-bit CC MSB (CC 15) + LSB (CC 47) on Channel 1 (0) */
	mfd_midi_parser_init(&p);
	mfd_midi_parser_start_learn(&p);
	REQUIRE(mfd_midi_parser_is_learning(&p));

	REQUIRE(mfd_midi_parser_feed(&p, pack_midi(0xB0U, 15U, 0x40U), &dec));
	REQUIRE(!mfd_midi_parser_is_learning(&p));
	REQUIRE(mfd_midi_parser_feed(&p, pack_midi(0xB0U, 47U, 0x2AU), &dec));
	REQUIRE(dec.addr.type == MFD_MIDI_CC);
	REQUIRE(dec.addr.channel == 0U);
	REQUIRE(dec.addr.number == 15U);
	REQUIRE(dec.addr.is_14bit);
	REQUIRE(dec.raw_value == ((0x40U << 7U) | 0x2AU));
	REQUIRE(fabsf(dec.normalized - ((float)((0x40U << 7U) | 0x2AU) / 16383.0f)) < 1e-6f);

	struct mfd_midi_addr learned;
	REQUIRE(mfd_midi_parser_pop_learned(&p, &learned));
	REQUIRE(learned.type == MFD_MIDI_CC);
	REQUIRE(learned.channel == 0U);
	REQUIRE(learned.number == 15U);
	REQUIRE(learned.is_14bit);

	/* 3. Note On with velocity 0 is treated as Note Off and ignored by Learn */
	mfd_midi_parser_start_learn(&p);
	REQUIRE(mfd_midi_parser_feed(&p, pack_midi(0x93U, 50U, 0U), &dec));
	REQUIRE(dec.addr.type == MFD_MIDI_NOTE_OFF);
	REQUIRE(mfd_midi_parser_is_learning(&p));

	REQUIRE(mfd_midi_parser_feed(&p, pack_midi(0x93U, 50U, 127U), &dec));
	REQUIRE(dec.addr.type == MFD_MIDI_NOTE_ON);
	REQUIRE(mfd_midi_parser_pop_learned(&p, &learned));
	REQUIRE(learned.type == MFD_MIDI_NOTE_ON);
	REQUIRE(learned.channel == 3U);
	REQUIRE(learned.number == 50U);

	/* 4. Live MIDI header readout & Learn button text (Item 1) */
	char readout[128] = {0};
	mfd_format_last_midi_readout(false, NULL, readout, sizeof(readout));
	REQUIRE(strcmp(readout, "No MIDI received yet") == 0);

	REQUIRE(mfd_midi_parser_feed(&p, pack_midi(0xB0U, 7U, 64U), &dec));
	mfd_format_last_midi_readout(true, &dec, readout, sizeof(readout));
	REQUIRE(strcmp(readout, "Last MIDI: Ch 1 CC 7 = 64") == 0);

	REQUIRE(strcmp(mfd_learn_button_text(false), "Learn") == 0);
	REQUIRE(strcmp(mfd_learn_button_text(true), "Move a control...") == 0);

	/* 5. A CC 32..63 bound as its own 7-bit control never pairs with CC n-32 (MIDImix Master CC 62 vs FX 4 CC 30) */
	mfd_midi_parser_init(&p);
	mfd_midi_parser_set_lsb_independent(&p, 0U, 62U, true);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 30U, 64U), 100U, &dec));
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 62U, 100U), 100U, &dec));
	REQUIRE(dec.addr.type == MFD_MIDI_CC);
	REQUIRE(dec.addr.number == 62U);
	REQUIRE(!dec.addr.is_14bit);
	REQUIRE(dec.raw_value == 100U);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 30U, 65U), 101U, &dec));
	REQUIRE(dec.addr.number == 30U);
	REQUIRE(!dec.addr.is_14bit);
	REQUIRE(dec.raw_value == 65U);

	/* 6. A first pairing needs the LSB right behind its MSB; once paired, a late LSB still pairs */
	mfd_midi_parser_init(&p);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 16U, 10U), 1000U, &dec));
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 48U, 20U), 1500U, &dec));
	REQUIRE(dec.addr.number == 48U);
	REQUIRE(!dec.addr.is_14bit);
	REQUIRE(dec.raw_value == 20U);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 15U, 0x40U), 2000U, &dec));
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 47U, 0x2AU), 2001U, &dec));
	REQUIRE(dec.addr.number == 15U);
	REQUIRE(dec.addr.is_14bit);
	REQUIRE(dec.raw_value == ((0x40U << 7U) | 0x2AU));
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 47U, 0x10U), 9000U, &dec));
	REQUIRE(dec.addr.number == 15U);
	REQUIRE(dec.addr.is_14bit);
	REQUIRE(dec.raw_value == ((0x40U << 7U) | 0x10U));

	/* 7. A new device or layout forgets the pairing history: the same LSB alone is its own 7-bit control */
	mfd_midi_parser_reset_cc_state(&p);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 47U, 0x2AU), 9100U, &dec));
	REQUIRE(dec.addr.number == 47U);
	REQUIRE(!dec.addr.is_14bit);
	REQUIRE(dec.raw_value == 0x2AU);

	/* 8. Learn captures an LSB-numbered control as itself, even right behind its MSB partner */
	mfd_midi_parser_init(&p);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 16U, 10U), 100U, &dec));
	mfd_midi_parser_start_learn(&p);
	REQUIRE(mfd_midi_parser_feed_at(&p, pack_midi(0xB0U, 48U, 20U), 101U, &dec));
	REQUIRE(mfd_midi_parser_pop_learned(&p, &learned));
	REQUIRE(learned.type == MFD_MIDI_CC);
	REQUIRE(learned.number == 48U);
	REQUIRE(!learned.is_14bit);

	printf("test-midi-parser: PASS\n");
	return 0;
}
