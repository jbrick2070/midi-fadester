/*
 * Visual Sidechain - Pure-C RIFF/WAVE Reader & Deterministic Fixture Generator
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VS_WAV_TWO_PI 6.28318530717958647692f

struct vs_wav_data {
	uint32_t sample_rate;
	uint16_t channels;
	size_t frames;
	float *mono_samples;
};

static inline void vs_wav_free(struct vs_wav_data *wav)
{
	if (!wav)
		return;
	free(wav->mono_samples);
	memset(wav, 0, sizeof(*wav));
}

static inline bool vs_wav_write_mono_pcm16(const char *path, uint32_t sample_rate, const float *samples, size_t frames)
{
	if (!path || !samples || frames == 0 || sample_rate == 0)
		return false;

	FILE *fp = fopen(path, "wb");
	if (!fp)
		return false;

	const uint32_t data_bytes = (uint32_t)(frames * sizeof(int16_t));
	const uint32_t riff_size = 36u + data_bytes;
	const uint32_t fmt_size = 16u;
	const uint16_t audio_format = 1u; /* PCM */
	const uint16_t num_channels = 1u;
	const uint32_t byte_rate = sample_rate * sizeof(int16_t);
	const uint16_t block_align = sizeof(int16_t);
	const uint16_t bits_per_sample = 16u;

	fwrite("RIFF", 1, 4, fp);
	fwrite(&riff_size, sizeof(uint32_t), 1, fp);
	fwrite("WAVE", 1, 4, fp);
	fwrite("fmt ", 1, 4, fp);
	fwrite(&fmt_size, sizeof(uint32_t), 1, fp);
	fwrite(&audio_format, sizeof(uint16_t), 1, fp);
	fwrite(&num_channels, sizeof(uint16_t), 1, fp);
	fwrite(&sample_rate, sizeof(uint32_t), 1, fp);
	fwrite(&byte_rate, sizeof(uint32_t), 1, fp);
	fwrite(&block_align, sizeof(uint16_t), 1, fp);
	fwrite(&bits_per_sample, sizeof(uint16_t), 1, fp);
	fwrite("data", 1, 4, fp);
	fwrite(&data_bytes, sizeof(uint32_t), 1, fp);

	for (size_t i = 0; i < frames; i++) {
		float s = samples[i];
		if (s > 1.0f)
			s = 1.0f;
		if (s < -1.0f)
			s = -1.0f;
		int32_t pcm = (int32_t)lrintf(s * 32767.0f);
		if (pcm > 32767)
			pcm = 32767;
		if (pcm < -32768)
			pcm = -32768;
		const int16_t out = (int16_t)pcm;
		fwrite(&out, sizeof(int16_t), 1, fp);
	}

	fclose(fp);
	return true;
}

static inline bool vs_wav_read(const char *path, struct vs_wav_data *out_wav)
{
	if (!path || !out_wav)
		return false;
	memset(out_wav, 0, sizeof(*out_wav));

	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;

	char magic[4];
	uint32_t riff_size = 0;
	char wave_id[4];
	if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, "RIFF", 4) != 0) {
		fclose(fp);
		return false;
	}
	if (fread(&riff_size, sizeof(uint32_t), 1, fp) != 1) {
		fclose(fp);
		return false;
	}
	if (fread(wave_id, 1, 4, fp) != 4 || memcmp(wave_id, "WAVE", 4) != 0) {
		fclose(fp);
		return false;
	}

	uint16_t audio_format = 0;
	uint16_t num_channels = 0;
	uint32_t sample_rate = 0;
	uint16_t bits_per_sample = 0;
	bool found_fmt = false;

	char chunk_id[4];
	uint32_t chunk_size = 0;
	while (fread(chunk_id, 1, 4, fp) == 4 && fread(&chunk_size, sizeof(uint32_t), 1, fp) == 1) {
		if (memcmp(chunk_id, "fmt ", 4) == 0) {
			uint32_t byte_rate = 0;
			uint16_t block_align = 0;
			if (chunk_size < 16u) {
				fclose(fp);
				return false;
			}
			if (fread(&audio_format, sizeof(uint16_t), 1, fp) != 1 ||
			    fread(&num_channels, sizeof(uint16_t), 1, fp) != 1 ||
			    fread(&sample_rate, sizeof(uint32_t), 1, fp) != 1 ||
			    fread(&byte_rate, sizeof(uint32_t), 1, fp) != 1 ||
			    fread(&block_align, sizeof(uint16_t), 1, fp) != 1 ||
			    fread(&bits_per_sample, sizeof(uint16_t), 1, fp) != 1) {
				fclose(fp);
				return false;
			}
			if (chunk_size > 16u)
				fseek(fp, (long)(chunk_size - 16u), SEEK_CUR);
			found_fmt = true;
		} else if (memcmp(chunk_id, "data", 4) == 0) {
			if (!found_fmt || num_channels == 0) {
				fclose(fp);
				return false;
			}
			if (audio_format == 1u && bits_per_sample == 16u) {
				const size_t frame_bytes = (size_t)num_channels * sizeof(int16_t);
				const size_t frames = chunk_size / frame_bytes;
				int16_t *raw = (int16_t *)malloc(chunk_size);
				float *mono = (float *)calloc(frames, sizeof(float));
				if (!raw || !mono) {
					free(raw);
					free(mono);
					fclose(fp);
					return false;
				}
				if (fread(raw, 1, frames * frame_bytes, fp) != frames * frame_bytes) {
					free(raw);
					free(mono);
					fclose(fp);
					return false;
				}
				for (size_t i = 0; i < frames; i++) {
					float sum = 0.0f;
					for (uint16_t ch = 0; ch < num_channels; ch++)
						sum += (float)raw[i * num_channels + ch] / 32768.0f;
					mono[i] = sum / (float)num_channels;
				}
				free(raw);
				fclose(fp);
				out_wav->sample_rate = sample_rate;
				out_wav->channels = num_channels;
				out_wav->frames = frames;
				out_wav->mono_samples = mono;
				return true;
			} else if (audio_format == 3u && bits_per_sample == 32u) {
				const size_t frame_bytes = (size_t)num_channels * sizeof(float);
				const size_t frames = chunk_size / frame_bytes;
				float *raw = (float *)malloc(chunk_size);
				float *mono = (float *)calloc(frames, sizeof(float));
				if (!raw || !mono) {
					free(raw);
					free(mono);
					fclose(fp);
					return false;
				}
				if (fread(raw, 1, frames * frame_bytes, fp) != frames * frame_bytes) {
					free(raw);
					free(mono);
					fclose(fp);
					return false;
				}
				for (size_t i = 0; i < frames; i++) {
					float sum = 0.0f;
					for (uint16_t ch = 0; ch < num_channels; ch++)
						sum += raw[i * num_channels + ch];
					mono[i] = sum / (float)num_channels;
				}
				free(raw);
				fclose(fp);
				out_wav->sample_rate = sample_rate;
				out_wav->channels = num_channels;
				out_wav->frames = frames;
				out_wav->mono_samples = mono;
				return true;
			}
			fclose(fp);
			return false;
		} else {
			fseek(fp, (long)chunk_size, SEEK_CUR);
		}
	}

	fclose(fp);
	return false;
}

/*
 * Ensures the 4 canonical 48 kHz WAV test fixtures exist in fixtures_dir.
 * Generated deterministically in pure C (zero Python).
 */
static inline bool vs_wav_ensure_fixtures(const char *fixtures_dir)
{
	if (!fixtures_dir)
		return false;

	const uint32_t sr = 48000u;
	char path[512];

	/* 1. kick-120bpm-4bars.wav (4.0 s, 8 kicks every 0.5 s, 4 snares at 0.5, 1.5, 2.5, 3.5 s). */
	snprintf(path, sizeof(path), "%s/kick-120bpm-4bars.wav", fixtures_dir);
	{
		const size_t frames = (size_t)sr * 4u;
		float *buf = (float *)calloc(frames, sizeof(float));
		if (!buf)
			return false;

		for (size_t i = 0; i < frames; i++) {
			const float t = (float)i / (float)sr;
			float sample = 0.0f;

			/* 8 kicks at t = 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5 s */
			for (int beat = 0; beat < 8; beat++) {
				const float t0 = (float)beat * 0.5f;
				const float dt = t - t0;
				if (dt >= 0.0f && dt < 0.160f) {
					const float env = (1.0f - expf(-dt / 0.002f)) * expf(-dt / 0.038f);
					sample += 0.65f * env * sinf(VS_WAV_TWO_PI * 90.0f * dt);
				}
			}

			/* 4 snares on beats 2 & 4 (t = 0.5, 1.5, 2.5, 3.5 s) */
			for (int beat = 1; beat < 8; beat += 2) {
				const float t0 = (float)beat * 0.5f;
				const float dt = t - t0;
				if (dt >= 0.0f && dt < 0.140f) {
					const float env = (1.0f - expf(-dt / 0.0015f)) * expf(-dt / 0.032f);
					const float body = 0.35f * sinf(VS_WAV_TWO_PI * 520.0f * dt) +
							   0.35f * sinf(VS_WAV_TWO_PI * 1180.0f * dt) +
							   0.30f * sinf(VS_WAV_TWO_PI * 2450.0f * dt);
					sample += 0.55f * env * body;
				}
			}

			buf[i] = sample;
		}

		const bool ok = vs_wav_write_mono_pcm16(path, sr, buf, frames);
		free(buf);
		if (!ok)
			return false;
	}

	/* 2. subbass-35hz-vs-kick-95hz.wav (2.5 s: continuous 35 Hz drone + 95 Hz kicks every 0.5 s). */
	snprintf(path, sizeof(path), "%s/subbass-35hz-vs-kick-95hz.wav", fixtures_dir);
	{
		const size_t frames = (size_t)(sr * 2.5f);
		float *buf = (float *)calloc(frames, sizeof(float));
		if (!buf)
			return false;

		for (size_t i = 0; i < frames; i++) {
			const float t = (float)i / (float)sr;
			float sample = 0.48f * sinf(VS_WAV_TWO_PI * 35.15625f * t);

			for (int k = 1; k <= 4; k++) {
				const float t0 = (float)k * 0.5f;
				const float dt = t - t0;
				if (dt >= 0.0f && dt < 0.140f) {
					const float env = (1.0f - expf(-dt / 0.002f)) * expf(-dt / 0.035f);
					sample += 0.48f * env * sinf(VS_WAV_TWO_PI * 95.0f * dt);
				}
			}

			buf[i] = sample;
		}

		const bool ok = vs_wav_write_mono_pcm16(path, sr, buf, frames);
		free(buf);
		if (!ok)
			return false;
	}

	/* 3. dynamic-range-40db.wav (2.0 s: 1.0 s at -36 dBFS followed by 1.0 s at -6 dBFS). */
	snprintf(path, sizeof(path), "%s/dynamic-range-40db.wav", fixtures_dir);
	{
		const size_t frames = (size_t)sr * 2u;
		float *buf = (float *)calloc(frames, sizeof(float));
		if (!buf)
			return false;

		const float amp_quiet = powf(10.0f, -36.0f / 20.0f);
		const float amp_loud = powf(10.0f, -6.0f / 20.0f);
		for (size_t i = 0; i < frames; i++) {
			const float t = (float)i / (float)sr;
			const float amp = (i < sr) ? amp_quiet : amp_loud;
			buf[i] = amp * sinf(VS_WAV_TWO_PI * 100.0f * t);
		}

		const bool ok = vs_wav_write_mono_pcm16(path, sr, buf, frames);
		free(buf);
		if (!ok)
			return false;
	}

	/* 4. silence-and-dc.wav (1.5 s: 0.5 s silence, 0.5 s -70 dBFS noise, 0.5 s +0.25 DC offset). */
	snprintf(path, sizeof(path), "%s/silence-and-dc.wav", fixtures_dir);
	{
		const size_t frames = (size_t)(sr * 1.5f);
		float *buf = (float *)calloc(frames, sizeof(float));
		if (!buf)
			return false;

		const float amp_m70 = powf(10.0f, -70.0f / 20.0f);
		for (size_t i = 0; i < frames; i++) {
			const float t = (float)i / (float)sr;
			if (i < sr / 2u) {
				buf[i] = 0.0f;
			} else if (i < sr) {
				buf[i] = amp_m70 * sinf(VS_WAV_TWO_PI * 60.0f * t);
			} else {
				buf[i] = 0.25f;
			}
		}

		const bool ok = vs_wav_write_mono_pcm16(path, sr, buf, frames);
		free(buf);
		if (!ok)
			return false;
	}

	return true;
}
