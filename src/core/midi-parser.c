/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/midi-parser.h"
#include <stdio.h>
#include <string.h>

void mfd_midi_parser_init(struct mfd_midi_parser *p)
{
	if (!p) {
		return;
	}
	memset(p, 0, sizeof(*p));
}

void mfd_midi_parser_start_learn(struct mfd_midi_parser *p)
{
	if (!p) {
		return;
	}
	p->learning = true;
	p->has_learned = false;
	memset(&p->learned_addr, 0, sizeof(p->learned_addr));
}

void mfd_midi_parser_cancel_learn(struct mfd_midi_parser *p)
{
	if (!p) {
		return;
	}
	p->learning = false;
	p->has_learned = false;
}

bool mfd_midi_parser_is_learning(const struct mfd_midi_parser *p)
{
	return p ? p->learning : false;
}

bool mfd_midi_parser_pop_learned(struct mfd_midi_parser *p, struct mfd_midi_addr *out_addr)
{
	if (!p || !out_addr || !p->has_learned) {
		return false;
	}
	*out_addr = p->learned_addr;
	p->has_learned = false;
	p->learning = false;
	return true;
}

bool mfd_midi_addr_matches(const struct mfd_midi_addr *binding, const struct mfd_midi_addr *incoming)
{
	if (!binding || !incoming) {
		return false;
	}
	if (binding->type == MFD_MIDI_NONE || incoming->type == MFD_MIDI_NONE) {
		return false;
	}
	if (binding->channel != incoming->channel) {
		return false;
	}
	if (binding->type == MFD_MIDI_PITCH_BEND && incoming->type == MFD_MIDI_PITCH_BEND) {
		return true;
	}
	if (binding->type != incoming->type) {
		return false;
	}
	return binding->number == incoming->number;
}

bool mfd_midi_parser_feed(struct mfd_midi_parser *p, uint32_t raw_msg, struct mfd_midi_decoded *out_decoded)
{
	if (!p || !out_decoded) {
		return false;
	}

	uint8_t status = (uint8_t)(raw_msg & 0xFFU);
	if (status < 0x80U || status >= 0xF0U) {
		return false;
	}

	uint8_t kind = (uint8_t)(status & 0xF0U);
	uint8_t channel = (uint8_t)(status & 0x0FU);
	uint8_t data1 = (uint8_t)((raw_msg >> 8U) & 0x7FU);
	uint8_t data2 = (uint8_t)((raw_msg >> 16U) & 0x7FU);

	memset(out_decoded, 0, sizeof(*out_decoded));
	out_decoded->addr.channel = channel;

	if (kind == 0xB0U) {
		out_decoded->addr.type = MFD_MIDI_CC;
		if (data1 < 32U) {
			p->cc_msb[channel][data1] = data2;
			p->cc_has_msb[channel][data1] = true;
			out_decoded->addr.number = data1;
			if (p->cc_14bit_active[channel][data1]) {
				uint16_t val14 = (uint16_t)((uint16_t)data2 << 7U);
				out_decoded->addr.is_14bit = true;
				out_decoded->raw_value = val14;
				out_decoded->normalized = (float)val14 / 16383.0f;
			} else {
				out_decoded->addr.is_14bit = false;
				out_decoded->raw_value = data2;
				out_decoded->normalized = (float)data2 / 127.0f;
			}
		} else if (data1 < 64U) {
			uint8_t base_cc = (uint8_t)(data1 - 32U);
			if (p->cc_has_msb[channel][base_cc]) {
				uint16_t val14 = (uint16_t)(((uint16_t)p->cc_msb[channel][base_cc] << 7U) | data2);
				p->cc_14bit_active[channel][base_cc] = true;
				out_decoded->addr.number = base_cc;
				out_decoded->addr.is_14bit = true;
				out_decoded->raw_value = val14;
				out_decoded->normalized = (float)val14 / 16383.0f;
				if (p->has_learned && p->learned_addr.type == MFD_MIDI_CC &&
				    p->learned_addr.channel == channel && p->learned_addr.number == base_cc) {
					p->learned_addr.is_14bit = true;
				}
			} else {
				out_decoded->addr.number = data1;
				out_decoded->addr.is_14bit = false;
				out_decoded->raw_value = data2;
				out_decoded->normalized = (float)data2 / 127.0f;
			}
		} else {
			out_decoded->addr.number = data1;
			out_decoded->addr.is_14bit = false;
			out_decoded->raw_value = data2;
			out_decoded->normalized = (float)data2 / 127.0f;
		}
	} else if (kind == 0x90U) {
		out_decoded->addr.number = data1;
		out_decoded->addr.is_14bit = false;
		if (data2 == 0U) {
			out_decoded->addr.type = MFD_MIDI_NOTE_OFF;
			out_decoded->raw_value = 0U;
			out_decoded->normalized = 0.0f;
		} else {
			out_decoded->addr.type = MFD_MIDI_NOTE_ON;
			out_decoded->raw_value = data2;
			out_decoded->normalized = (float)data2 / 127.0f;
		}
	} else if (kind == 0x80U) {
		out_decoded->addr.type = MFD_MIDI_NOTE_OFF;
		out_decoded->addr.number = data1;
		out_decoded->addr.is_14bit = false;
		out_decoded->raw_value = data2;
		out_decoded->normalized = 0.0f;
	} else if (kind == 0xE0U) {
		uint16_t bend14 = (uint16_t)(((uint16_t)data2 << 7U) | data1);
		out_decoded->addr.type = MFD_MIDI_PITCH_BEND;
		out_decoded->addr.number = 0U;
		out_decoded->addr.is_14bit = true;
		out_decoded->raw_value = bend14;
		out_decoded->normalized = (float)bend14 / 16383.0f;
	} else {
		return false;
	}

	if (p->learning && out_decoded->addr.type != MFD_MIDI_NOTE_OFF) {
		p->learned_addr = out_decoded->addr;
		p->has_learned = true;
		p->learning = false;
	}

	return true;
}

void mfd_format_last_midi_readout(bool has_event, const struct mfd_midi_decoded *ev, char *out_buf, size_t out_size)
{
	if (!out_buf || out_size == 0U) {
		return;
	}
	if (!has_event || !ev || ev->addr.type == MFD_MIDI_NONE) {
		snprintf(out_buf, out_size, "No MIDI received yet");
		return;
	}

	unsigned ch = (unsigned)ev->addr.channel + 1U;
	unsigned num = (unsigned)ev->addr.number;
	unsigned val = (unsigned)ev->raw_value;

	switch (ev->addr.type) {
	case MFD_MIDI_CC:
		snprintf(out_buf, out_size, "Last MIDI: Ch %u CC %u = %u", ch, num, val);
		break;
	case MFD_MIDI_PITCH_BEND:
		snprintf(out_buf, out_size, "Last MIDI: Ch %u Pitch Bend = %u", ch, val);
		break;
	case MFD_MIDI_NOTE_ON:
		snprintf(out_buf, out_size, "Last MIDI: Ch %u Note %u = %u", ch, num, val);
		break;
	case MFD_MIDI_NOTE_OFF:
		snprintf(out_buf, out_size, "Last MIDI: Ch %u Note Off %u = %u", ch, num, val);
		break;
	default:
		snprintf(out_buf, out_size, "No MIDI received yet");
		break;
	}
}

const char *mfd_learn_button_text(bool is_learning_this_row)
{
	return is_learning_this_row ? "Move a control..." : "Learn";
}
