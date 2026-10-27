/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mfd_midi_msg_type {
	MFD_MIDI_NONE = 0,
	MFD_MIDI_CC = 1,
	MFD_MIDI_NOTE_ON = 2,
	MFD_MIDI_NOTE_OFF = 3,
	MFD_MIDI_PITCH_BEND = 4,
};

struct mfd_midi_addr {
	enum mfd_midi_msg_type type;
	uint8_t channel; /* 0..15 (MIDI Channels 1..16) */
	uint8_t number;  /* 0..127 (CC or Note number; 0 for Pitch Bend) */
	bool is_14bit;   /* true when 14-bit CC MSB+LSB pair or Pitch Bend */
};

struct mfd_midi_raw_event {
	uint32_t raw_msg;
	uint32_t timestamp_ms;
};

struct mfd_midi_decoded {
	struct mfd_midi_addr addr;
	uint16_t raw_value; /* 0..127 (7-bit) or 0..16383 (14-bit) */
	float normalized;   /* 0.0f .. 1.0f */
};

enum mfd_target_type {
	MFD_TARGET_UNASSIGNED = 0,
	MFD_TARGET_TBAR = 1,
	MFD_TARGET_TWO_SOURCE_MIX = 2,
	MFD_TARGET_SINGLE_LAYER = 3,
	MFD_TARGET_AUDIO_VOLUME = 4,
};

enum mfd_crossfade_curve {
	MFD_CURVE_DIP_FREE = 0,
	MFD_CURVE_FULL_CENTER = 1,
	MFD_CURVE_COSINE = 2,
	MFD_CURVE_LINEAR = 3,
};

#ifdef __cplusplus
}
#endif
