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

#define MFD_MIDI_QUEUE_CAPACITY 1024U
#define MFD_MIDI_QUEUE_MASK (MFD_MIDI_QUEUE_CAPACITY - 1U)

struct mfd_midi_queue {
	struct mfd_midi_raw_event buffer[MFD_MIDI_QUEUE_CAPACITY];
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t dropped_count;
};

void mfd_midi_queue_init(struct mfd_midi_queue *q);
bool mfd_midi_queue_push(struct mfd_midi_queue *q, uint32_t raw_msg, uint32_t timestamp_ms);
bool mfd_midi_queue_pop(struct mfd_midi_queue *q, struct mfd_midi_raw_event *out_ev);
uint32_t mfd_midi_queue_dropped(const struct mfd_midi_queue *q);
uint32_t mfd_midi_queue_count(const struct mfd_midi_queue *q);

#ifdef __cplusplus
}
#endif
