/*
visual-sidechain
Copyright (C) 2026 Jeffrey A. Brick
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/audio-ring.h"

#include <math.h>
#include <string.h>

static inline float sanitize_sample(float s)
{
	return isfinite(s) ? s : 0.0f;
}

bool vs_audio_ring_init(struct vs_audio_ring *ring)
{
	if (!ring)
		return false;

	memset(ring, 0, sizeof(*ring));
	if (pthread_mutex_init(&ring->mutex, NULL) != 0)
		return false;

	ring->mutex_initialized = true;
	return true;
}

void vs_audio_ring_free(struct vs_audio_ring *ring)
{
	if (!ring)
		return;

	if (ring->mutex_initialized) {
		pthread_mutex_destroy(&ring->mutex);
		ring->mutex_initialized = false;
	}
}

void vs_audio_ring_reset(struct vs_audio_ring *ring)
{
	if (!ring || !ring->mutex_initialized)
		return;

	pthread_mutex_lock(&ring->mutex);
	memset(ring->data, 0, sizeof(ring->data));
	ring->write_pos = 0;
	ring->total_written = 0;
	ring->last_timestamp_ns = 0;
	pthread_mutex_unlock(&ring->mutex);
}

void vs_audio_ring_push_mono(struct vs_audio_ring *ring, const float *samples, size_t frames, uint64_t timestamp_ns)
{
	if (!ring || !ring->mutex_initialized || frames == 0)
		return;

	pthread_mutex_lock(&ring->mutex);
	uint32_t wp = ring->write_pos;
	for (size_t i = 0; i < frames; i++) {
		float val = samples ? sanitize_sample(samples[i]) : 0.0f;
		ring->data[wp] = val;
		wp = (wp + 1u) & VS_AUDIO_RING_MASK;
	}
	ring->write_pos = wp;
	ring->total_written += (uint64_t)frames;
	ring->last_timestamp_ns = timestamp_ns;
	pthread_mutex_unlock(&ring->mutex);
}

/* Whether every sample of these planes over the block is (near) silent: below about -120 dBFS, or not finite. */
static bool planes_silent(const float *const *planes, size_t count, size_t frames)
{
	for (size_t ch = 0; ch < count; ch++) {
		for (size_t i = 0; i < frames; i++) {
			if (fabsf(sanitize_sample(planes[ch][i])) >= 1e-6f)
				return false;
		}
	}
	return true;
}

void vs_audio_ring_push_planar(struct vs_audio_ring *ring, const float *const *planes, size_t channels, size_t frames,
			       bool muted, bool react_when_muted, uint64_t timestamp_ns)
{
	if (!ring || !ring->mutex_initialized || frames == 0)
		return;

	size_t active_ch = 0;
	const float *valid_planes[VS_AUDIO_MAX_CHANNELS];
	if (planes && (!muted || react_when_muted)) {
		/* With OBS set to 2.1, 4.0, 4.1, 5.1 or 7.1, the first two planes are front left and right. A stereo source
		 * fills only those (the rest are silent), and OBS spreads a mono source over all of them, so averaging every
		 * plane would put stereo music 9.5 dB (5.1) to 12 dB (7.1) under the fixed onset thresholds. Front left and
		 * right alone give every layout the stereo layout's level. A block whose front planes are silent while
		 * another plane is not (dialogue only in the centre, surround-only ambience) averages those other planes
		 * instead, so it is not read as silence. */
		const size_t limit = channels < VS_AUDIO_MAX_CHANNELS ? channels : VS_AUDIO_MAX_CHANNELS;
		const size_t front = limit > 2 ? 2 : limit;
		for (size_t ch = 0; ch < front; ch++) {
			if (planes[ch] != NULL) {
				valid_planes[active_ch++] = planes[ch];
			}
		}
		if (limit > 2 && planes_silent(valid_planes, active_ch, frames)) {
			size_t rear_ch = 0;
			const float *rear[VS_AUDIO_MAX_CHANNELS];
			for (size_t ch = 2; ch < limit; ch++) {
				if (planes[ch] != NULL) {
					rear[rear_ch++] = planes[ch];
				}
			}
			if (rear_ch > 0 && !planes_silent(rear, rear_ch, frames)) {
				for (size_t ch = 0; ch < rear_ch; ch++) {
					valid_planes[ch] = rear[ch];
				}
				active_ch = rear_ch;
			}
		}
	}

	pthread_mutex_lock(&ring->mutex);
	uint32_t wp = ring->write_pos;
	if (active_ch == 0) {
		for (size_t i = 0; i < frames; i++) {
			ring->data[wp] = 0.0f;
			wp = (wp + 1u) & VS_AUDIO_RING_MASK;
		}
	} else {
		const float inv_ch = 1.0f / (float)active_ch;
		for (size_t i = 0; i < frames; i++) {
			float sum = 0.0f;
			for (size_t ch = 0; ch < active_ch; ch++) {
				sum += sanitize_sample(valid_planes[ch][i]);
			}
			ring->data[wp] = sum * inv_ch;
			wp = (wp + 1u) & VS_AUDIO_RING_MASK;
		}
	}
	ring->write_pos = wp;
	ring->total_written += (uint64_t)frames;
	ring->last_timestamp_ns = timestamp_ns;
	pthread_mutex_unlock(&ring->mutex);
}

bool vs_audio_ring_read_window(struct vs_audio_ring *ring, size_t window_size, size_t offset_samples, float *out_window,
			       uint64_t *out_total_written, uint64_t *out_last_timestamp_ns)
{
	if (!ring || !ring->mutex_initialized || !out_window || window_size == 0)
		return false;

	if (window_size > VS_AUDIO_RING_CAPACITY || offset_samples > VS_AUDIO_RING_CAPACITY - window_size)
		return false;

	pthread_mutex_lock(&ring->mutex);
	uint64_t total = ring->total_written;
	uint64_t ts = ring->last_timestamp_ns;
	uint32_t wp = ring->write_pos;

	size_t available_after_offset = 0;
	if (total > (uint64_t)offset_samples) {
		uint64_t avail = total - (uint64_t)offset_samples;
		size_t max_hist = VS_AUDIO_RING_CAPACITY - offset_samples;
		available_after_offset = (avail > (uint64_t)max_hist) ? max_hist : (size_t)avail;
	}

	size_t copy_count = (available_after_offset < window_size) ? available_after_offset : window_size;
	size_t zero_pad = window_size - copy_count;

	if (zero_pad > 0) {
		memset(out_window, 0, zero_pad * sizeof(float));
	}

	uint32_t end_pos = (wp - (uint32_t)offset_samples) & VS_AUDIO_RING_MASK;
	uint32_t start_pos = (end_pos - (uint32_t)copy_count) & VS_AUDIO_RING_MASK;

	for (size_t i = 0; i < copy_count; i++) {
		out_window[zero_pad + i] = ring->data[(start_pos + (uint32_t)i) & VS_AUDIO_RING_MASK];
	}
	pthread_mutex_unlock(&ring->mutex);

	if (out_total_written)
		*out_total_written = total;
	if (out_last_timestamp_ns)
		*out_last_timestamp_ns = ts;

	return true;
}
