/*
 * Midi Fadester — test-midi-queue
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/midi-queue.h"
#include <util/threading.h>
#include <stdio.h>
#include <stdlib.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

#define SPSC_STRESS_TOTAL 100000U

struct spsc_ctx {
	struct mfd_midi_queue q;
	volatile bool producer_done;
	uint32_t pushed_ok;
	uint32_t consumed_ok;
	bool order_error;
};

static void *spsc_producer_thread(void *arg)
{
	struct spsc_ctx *ctx = (struct spsc_ctx *)arg;
	for (uint32_t seq = 1U; seq <= SPSC_STRESS_TOTAL; ++seq) {
		uint32_t msg = 0x00000FB0U | ((seq & 0x7FU) << 16U);
		if (mfd_midi_queue_push(&ctx->q, msg, seq)) {
			++ctx->pushed_ok;
		}
	}
	os_atomic_store_bool(&ctx->producer_done, true);
	return NULL;
}

static void *spsc_consumer_thread(void *arg)
{
	struct spsc_ctx *ctx = (struct spsc_ctx *)arg;
	uint32_t last_ts = 0U;
	struct mfd_midi_raw_event ev;
	for (;;) {
		bool done = os_atomic_load_bool(&ctx->producer_done);
		bool got_any = false;
		while (mfd_midi_queue_pop(&ctx->q, &ev)) {
			got_any = true;
			uint32_t expected_msg = 0x00000FB0U | ((ev.timestamp_ms & 0x7FU) << 16U);
			if (ev.timestamp_ms <= last_ts || ev.raw_msg != expected_msg) {
				ctx->order_error = true;
			}
			last_ts = ev.timestamp_ms;
			++ctx->consumed_ok;
		}
		if (done && !got_any) {
			break;
		}
	}
	return NULL;
}

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

	/* Multi-threaded SPSC acquire/release stress test across 100,000 events */
	struct spsc_ctx ctx;
	mfd_midi_queue_init(&ctx.q);
	ctx.producer_done = false;
	ctx.pushed_ok = 0U;
	ctx.consumed_ok = 0U;
	ctx.order_error = false;

	pthread_t prod;
	pthread_t cons;
	REQUIRE(pthread_create(&cons, NULL, spsc_consumer_thread, &ctx) == 0);
	REQUIRE(pthread_create(&prod, NULL, spsc_producer_thread, &ctx) == 0);
	REQUIRE(pthread_join(prod, NULL) == 0);
	REQUIRE(pthread_join(cons, NULL) == 0);

	REQUIRE(!ctx.order_error);
	REQUIRE(ctx.consumed_ok == ctx.pushed_ok);
	REQUIRE(ctx.pushed_ok + mfd_midi_queue_dropped(&ctx.q) == SPSC_STRESS_TOTAL);
	REQUIRE(mfd_midi_queue_count(&ctx.q) == 0U);

	printf("test-midi-queue: PASS\n");
	return 0;
}
