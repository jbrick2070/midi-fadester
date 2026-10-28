/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/midi-queue.h"
#include <util/threading.h>
#include <string.h>

void mfd_midi_queue_init(struct mfd_midi_queue *q)
{
	if (!q) {
		return;
	}
	memset(q->buffer, 0, sizeof(q->buffer));
	os_atomic_store_long(&q->write_idx, 0L);
	os_atomic_store_long(&q->read_idx, 0L);
	os_atomic_store_long(&q->dropped_count, 0L);
}

bool mfd_midi_queue_push(struct mfd_midi_queue *q, uint32_t raw_msg, uint32_t timestamp_ms)
{
	if (!q) {
		return false;
	}

	uint32_t w = (uint32_t)os_atomic_load_long(&q->write_idx);
	uint32_t r = (uint32_t)os_atomic_load_long(&q->read_idx);

	if ((w - r) >= MFD_MIDI_QUEUE_CAPACITY) {
		os_atomic_inc_long(&q->dropped_count);
		return false;
	}

	uint32_t slot = w & MFD_MIDI_QUEUE_MASK;
	q->buffer[slot].raw_msg = raw_msg;
	q->buffer[slot].timestamp_ms = timestamp_ms;
	os_atomic_store_long(&q->write_idx, (long)(w + 1U));
	return true;
}

bool mfd_midi_queue_pop(struct mfd_midi_queue *q, struct mfd_midi_raw_event *out_ev)
{
	if (!q || !out_ev) {
		return false;
	}

	uint32_t r = (uint32_t)os_atomic_load_long(&q->read_idx);
	uint32_t w = (uint32_t)os_atomic_load_long(&q->write_idx);

	if (r == w) {
		return false;
	}

	uint32_t slot = r & MFD_MIDI_QUEUE_MASK;
	*out_ev = q->buffer[slot];
	os_atomic_store_long(&q->read_idx, (long)(r + 1U));
	return true;
}

uint32_t mfd_midi_queue_dropped(const struct mfd_midi_queue *q)
{
	return q ? (uint32_t)os_atomic_load_long(&q->dropped_count) : 0U;
}

uint32_t mfd_midi_queue_count(const struct mfd_midi_queue *q)
{
	if (!q) {
		return 0U;
	}
	uint32_t w = (uint32_t)os_atomic_load_long(&q->write_idx);
	uint32_t r = (uint32_t)os_atomic_load_long(&q->read_idx);
	return w - r;
}
