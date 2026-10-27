/*
 * Midi Fadester — test-midi-queue
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/midi-queue.h"
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(void)
{
	struct mfd_midi_queue q;
	struct mfd_midi_raw_event ev;

	mfd_midi_queue_init(&q);
	REQUIRE(mfd_midi_queue_count(&q) == 0U);
	REQUIRE(mfd_midi_queue_dropped(&q) == 0U);
	REQUIRE(!mfd_midi_queue_pop(&q, &ev));

	/* Fill to exact capacity (1024 events) */
	for (uint32_t i = 0; i < MFD_MIDI_QUEUE_CAPACITY; ++i) {
		uint32_t msg = 0x004007B0U | (i & 0x7FU);
		REQUIRE(mfd_midi_queue_push(&q, msg, 1000U + i));
	}
	REQUIRE(mfd_midi_queue_count(&q) == MFD_MIDI_QUEUE_CAPACITY);
	REQUIRE(mfd_midi_queue_dropped(&q) == 0U);

	/* Overflow by 5 events */
	for (uint32_t i = 0; i < 5U; ++i) {
		REQUIRE(!mfd_midi_queue_push(&q, 0x007F0FB0U, 5000U + i));
	}
	REQUIRE(mfd_midi_queue_dropped(&q) == 5U);

	/* Drain half, then push another half to verify wrap-around across 1024 boundary */
	for (uint32_t i = 0; i < 512U; ++i) {
		REQUIRE(mfd_midi_queue_pop(&q, &ev));
		REQUIRE(ev.raw_msg == (0x004007B0U | (i & 0x7FU)));
		REQUIRE(ev.timestamp_ms == 1000U + i);
	}
	REQUIRE(mfd_midi_queue_count(&q) == 512U);

	for (uint32_t i = 0; i < 512U; ++i) {
		REQUIRE(mfd_midi_queue_push(&q, 0x00000EB0U | ((i & 0x7FU) << 16U), 9000U + i));
	}
	REQUIRE(mfd_midi_queue_count(&q) == MFD_MIDI_QUEUE_CAPACITY);

	for (uint32_t i = 512U; i < MFD_MIDI_QUEUE_CAPACITY; ++i) {
		REQUIRE(mfd_midi_queue_pop(&q, &ev));
		REQUIRE(ev.raw_msg == (0x004007B0U | (i & 0x7FU)));
		REQUIRE(ev.timestamp_ms == 1000U + i);
	}
	for (uint32_t i = 0; i < 512U; ++i) {
		REQUIRE(mfd_midi_queue_pop(&q, &ev));
		REQUIRE(ev.raw_msg == (0x00000EB0U | ((i & 0x7FU) << 16U)));
		REQUIRE(ev.timestamp_ms == 9000U + i);
	}
	REQUIRE(mfd_midi_queue_count(&q) == 0U);
	REQUIRE(!mfd_midi_queue_pop(&q, &ev));

	printf("test-midi-queue: PASS\n");
	return 0;
}
