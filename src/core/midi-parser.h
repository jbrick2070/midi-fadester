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

struct mfd_midi_parser {
	uint8_t cc_msb[16][32];
	bool cc_has_msb[16][32];
	bool cc_14bit_active[16][32];
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
bool mfd_midi_addr_matches(const struct mfd_midi_addr *binding, const struct mfd_midi_addr *incoming);

#ifdef __cplusplus
}
#endif
