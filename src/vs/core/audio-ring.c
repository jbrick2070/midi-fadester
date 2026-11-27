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
	ring->use_rear = false;
	ring->front_silent_blocks = 0;
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

/* The block's peak on one plane, non-finite samples read as 0. */
static float plane_peak(const float *plane, size_t frames)
{
	float peak = 0.0f;
	for (size_t i = 0; i < frames; i++) {
		const float a = fabsf(sanitize_sample(plane[i]));
		if (a > peak)
			peak = a;
	}
	return peak;
}

/* The LFE plane of OBS's speaker layout for this channel count (2.1: 2; 4.1, 5.1, 7.1: 3), or -1 (4.0 has none). */
static int lfe_plane(size_t channels)
{
	if (channels == 3)
		return 2;
	if (channels == 5 || channels == 6 || channels == 8)
		return 3;
	return -1;
}

void vs_audio_ring_push_planar(struct vs_audio_ring *ring, const float *const *planes, size_t channels, size_t frames,
			       bool muted, bool react_when_muted, uint64_t timestamp_ns)
{
	if (!ring || !ring->mutex_initialized || frames == 0)
		return;

	pthread_mutex_lock(&ring->mutex);
	size_t active_ch = 0;
	const float *valid_planes[VS_AUDIO_MAX_CHANNELS];
	if (planes && (!muted || react_when_muted)) {
		/* With OBS set to 2.1, 4.0, 4.1, 5.1 or 7.1, the first two planes are front left and right. A stereo source
		 * fills only those (the rest are silent), and OBS spreads a mono source over all of them, so averaging every
		 * plane would put stereo music 9.5 dB (5.1) to 12 dB (7.1) under the fixed onset thresholds. Front left and
		 * right alone give every layout the stereo layout's level. While the front stays silent and the centre or
		 * rear carry sound (dialogue only in the centre, surround-only ambience), those planes are averaged instead,
		 * only the ones with sound and never the LFE, so the passage arrives near the level stereo would; the switch
		 * holds (VS_AUDIO_RING_FALLBACK_HOLD), so the choice does not flip from block to block. */
		const size_t limit = channels < VS_AUDIO_MAX_CHANNELS ? channels : VS_AUDIO_MAX_CHANNELS;
		const size_t front = limit > 2 ? 2 : limit;
		float front_peak = 0.0f;
		for (size_t ch = 0; ch < front; ch++) {
			if (planes[ch] != NULL) {
				valid_planes[active_ch++] = planes[ch];
				const float p = plane_peak(planes[ch], frames);
				if (p > front_peak)
					front_peak = p;
			}
		}
		if (limit > 2) {
			const int lfe = lfe_plane(limit);
			size_t rear_ch = 0;
			const float *rear[VS_AUDIO_MAX_CHANNELS];
			for (size_t ch = 2; ch < limit; ch++) {
				if (planes[ch] != NULL && (int)ch != lfe &&
				    plane_peak(planes[ch], frames) >= VS_AUDIO_RING_SILENT_PEAK) {
					rear[rear_ch++] = planes[ch];
				}
			}
			if (front_peak >= VS_AUDIO_RING_FRONT_BACK_PEAK) {
				ring->use_rear = false;
				ring->front_silent_blocks = 0;
			} else if (front_peak < VS_AUDIO_RING_SILENT_PEAK) {
				if (ring->front_silent_blocks < VS_AUDIO_RING_FALLBACK_HOLD)
					ring->front_silent_blocks++;
				if (ring->front_silent_blocks >= VS_AUDIO_RING_FALLBACK_HOLD && rear_ch > 0)
					ring->use_rear = true;
			} else {
				ring->front_silent_blocks = 0; /* quiet but not silent: the choice stays as it is */
			}
			if (ring->use_rear && rear_ch > 0) {
				for (size_t ch = 0; ch < rear_ch; ch++) {
					valid_planes[ch] = rear[ch];
				}
				active_ch = rear_ch;
			}
		} else {
			ring->use_rear = false;
			ring->front_silent_blocks = 0;
		}
	}

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
