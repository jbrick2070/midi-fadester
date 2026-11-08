/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stddef.h>
#include "core/midi-types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MFD_MIDI_14BIT_PAIR_WINDOW_MS 3U

struct mfd_midi_parser {
	uint8_t cc_msb[16][32];
	bool cc_has_msb[16][32];
	bool cc_14bit_active[16][32];
	uint32_t cc_msb_time_ms[16][32]; /* timestamp of the last CC 0..31 message */
	bool cc_lsb_independent[16][32]; /* CC (32 + i) is bound as its own 7-bit control */
	bool learning;
	bool has_learned;
	struct mfd_midi_addr learned_addr;
};

void mfd_midi_parser_init(struct mfd_midi_parser *p);
void mfd_midi_parser_start_learn(struct mfd_midi_parser *p);
void mfd_midi_parser_cancel_learn(struct mfd_midi_parser *p);
bool mfd_midi_parser_is_learning(const struct mfd_midi_parser *p);
bool mfd_midi_parser_pop_learned(struct mfd_midi_parser *p, struct mfd_midi_addr *out_addr);
bool mfd_midi_parser_feed(struct mfd_midi_parser *p, uint32_t raw_msg, struct mfd_midi_decoded *out_decoded);
bool mfd_midi_parser_feed_at(struct mfd_midi_parser *p, uint32_t raw_msg, uint32_t timestamp_ms,
			     struct mfd_midi_decoded *out_decoded);
void mfd_midi_parser_set_lsb_independent(struct mfd_midi_parser *p, uint8_t channel, uint8_t cc, bool independent);
void mfd_midi_parser_clear_lsb_independent(struct mfd_midi_parser *p);
void mfd_midi_parser_reset_cc_state(struct mfd_midi_parser *p);
bool mfd_midi_addr_matches(const struct mfd_midi_addr *binding, const struct mfd_midi_addr *incoming);

void mfd_format_last_midi_readout(bool has_event, const struct mfd_midi_decoded *ev, char *out_buf, size_t out_size);
const char *mfd_learn_button_text(bool is_learning_this_row);

#ifdef __cplusplus
}
#endif
