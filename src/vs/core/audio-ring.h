/*
visual-sidechain
Copyright (C) 2026 Jeffrey A. Brick
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Power-of-two capacity: 32,768 mono samples (~682.7 ms at 48 kHz), covering
 * the 4096-sample low-band FFT window plus up to +500 ms (24,000 samples) of
 * positive sync_offset_ms delay with headroom (R-ANA-2, R-ANA-3). */
#define VS_AUDIO_RING_CAPACITY 32768u
#define VS_AUDIO_RING_MASK (VS_AUDIO_RING_CAPACITY - 1u)
#define VS_AUDIO_MAX_CHANNELS 8u

struct vs_audio_ring {
	pthread_mutex_t mutex;
	bool mutex_initialized;
	float data[VS_AUDIO_RING_CAPACITY];
	uint32_t write_pos;
	uint64_t total_written;
	uint64_t last_timestamp_ns;
};

bool vs_audio_ring_init(struct vs_audio_ring *ring);
void vs_audio_ring_free(struct vs_audio_ring *ring);
void vs_audio_ring_reset(struct vs_audio_ring *ring);

/* Pushes mono samples (sanitizing NaN/Inf to 0.0f). Safe on the audio thread:
 * no allocations, no logging, no OBS calls (R-THR-2). */
void vs_audio_ring_push_mono(struct vs_audio_ring *ring, const float *samples, size_t frames, uint64_t timestamp_ns);

/* Downmixes planar float channels (1..VS_AUDIO_MAX_CHANNELS) to mono, sanitizing
 * NaN/Inf to 0.0f. When muted is true and react_when_muted is false, pushes 0.0f
 * silence (R-TAP-5). */
void vs_audio_ring_push_planar(struct vs_audio_ring *ring, const float *const *planes, size_t channels, size_t frames,
			       bool muted, bool react_when_muted, uint64_t timestamp_ns);

/* Reads a chronological window of `window_size` samples ending `offset_samples`
 * behind the write head. If fewer than `window_size + offset_samples` samples
 * have been written since init/reset, leading positions are zero-padded
 * deterministically (R-ANA-2, R-ANA-3). Returns total_written and last_timestamp_ns
 * through optional out pointers. */
bool vs_audio_ring_read_window(struct vs_audio_ring *ring, size_t window_size, size_t offset_samples, float *out_window,
			       uint64_t *out_total_written, uint64_t *out_last_timestamp_ns);

#ifdef __cplusplus
}
#endif
