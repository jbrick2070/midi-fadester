/*
 * Visual Sidechain - End-to-End Dual-FFT Audio Analyzer Pipeline
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include "audio-ring.h"
#include "envelope.h"
#include "fft.h"
#include "onset.h"
#include "spectrum.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vs_signal_id {
	VS_SIG_LEVEL = 0,
	VS_SIG_SUB_BASS = 1,
	VS_SIG_BASS = 2,
	VS_SIG_MID = 3,
	VS_SIG_HIGH = 4,
	VS_SIG_KICK = 5,
	VS_SIG_SNARE = 6,
	VS_SIG_BEAT = 7,
	VS_SIG_CENTROID = 8,
	VS_SIG_FLUX = 9,
	VS_SIG_CUSTOM_HZ = 10,
	VS_SIG_SPECTRUM_BAND_0 = 100,  /* 100..131 address spectrum[0..31] */
	VS_SIG_FADER_CROSSFADER = 200, /* 200..209 address Fadester slots crossfader, fader_1..8, master */
	VS_SIG_FADER_1 = 201,
	VS_SIG_FADER_2 = 202,
	VS_SIG_FADER_3 = 203,
	VS_SIG_FADER_4 = 204,
	VS_SIG_FADER_5 = 205,
	VS_SIG_FADER_6 = 206,
	VS_SIG_FADER_7 = 207,
	VS_SIG_FADER_8 = 208,
	VS_SIG_FADER_MASTER = 209
};

struct vs_audio_frame {
	float level;                       /* [0.0, 1.0] RMS envelope */
	float sub_bass;                    /* [0.0, 1.0] 20..60 Hz (from 4096-pt FFT) */
	float bass;                        /* [0.0, 1.0] 20..250 Hz (from 4096-pt FFT) */
	float mid;                         /* [0.0, 1.0] 250..4000 Hz (from 2048-pt FFT) */
	float high;                        /* [0.0, 1.0] 4000..20000 Hz (from 2048-pt FFT) */
	float kick;                        /* [0.0, 1.0] 40..130 Hz 4096-pt spectral-flux onset */
	float snare;                       /* [0.0, 1.0] 150..4000 Hz 2048-pt transient onset */
	float beat;                        /* [0.0, 1.0) phase-locked beat ramp/pulse */
	float spectral_centroid;           /* [0.0, 1.0] normalized spectral brightness */
	float spectral_flux;               /* [0.0, 1.0] wideband positive onset flux */
	float spectrum[VS_SPECTRUM_BANDS]; /* [0.0, 1.0] 32 log-spaced bands (dual-FFT merged) */
	float fft_bins[VS_FFT_TEX_BINS];   /* [0.0, 1.0] 512 log-freq bins for Row 0 of audio_fft */
	float waveform[VS_FFT_TEX_BINS];   /* [0.0, 1.0] 512 mono samples for Row 1 of audio_fft */
	float peak_raw;                    /* for obs-shaderfilter audio_peak compat */
	float magnitude_raw;               /* for obs-shaderfilter audio_magnitude compat */
	float estimated_bpm;               /* [60.0, 180.0] estimated tempo */
};

struct vs_analyzer_config {
	uint32_t sample_rate;    /* e.g. 48000 */
	uint32_t fft_size_low;   /* default 4096 (11.72 Hz spacing at 48 kHz for <= 250 Hz) */
	uint32_t fft_size_mid;   /* default 2048 (23.44 Hz spacing, 42.7 ms window for > 250 Hz) */
	float sync_offset_ms;    /* -200.0 .. +500.0, default 0.0 */
	bool auto_gain;          /* default true */
	float gate_db;           /* default -55.0 dBFS */
	float floor_db;          /* manual mode floor, default -45.0 dBFS */
	float ceiling_db;        /* manual mode ceiling, default -6.0 dBFS */
	float attack_ms;         /* 0.0 .. 500.0, default 10.0 */
	float release_ms;        /* 0.0 .. 2000.0, default 180.0 */
	float kick_sensitivity;  /* 0.0 .. 1.0, default 0.5 */
	float snare_sensitivity; /* 0.0 .. 1.0, default 0.5 */
};

struct vs_analyzer {
	struct vs_analyzer_config cfg;
	struct vs_audio_ring ring;
	struct vs_fft_plan fft_low;
	struct vs_fft_plan fft_mid;
	struct vs_spectrum spectrum;
	struct vs_onset onset;
	struct vs_envelope_config env_cfg;
	struct vs_envelope_channel env_level;
	struct vs_envelope_channel env_sub_bass;
	struct vs_envelope_channel env_bass;
	struct vs_envelope_channel env_mid;
	struct vs_envelope_channel env_high;
	struct vs_envelope_channel env_spectrum[VS_SPECTRUM_BANDS];
	float win_low[VS_FFT_LOW_SIZE];
	float win_mid[VS_FFT_MID_SIZE];
	float mag_low[VS_MAX_BINS_LOW];
	float mag_mid[VS_MAX_BINS_MID];
	uint64_t last_written_samples;
	float time_since_audio_sec;
	bool initialized;
};

void vs_analyzer_default_config(struct vs_analyzer_config *cfg);
bool vs_analyzer_init(struct vs_analyzer *ana, const struct vs_analyzer_config *cfg);
void vs_analyzer_free(struct vs_analyzer *ana);
void vs_analyzer_reset(struct vs_analyzer *ana);
void vs_analyzer_update_config(struct vs_analyzer *ana, const struct vs_analyzer_config *cfg);

void vs_analyzer_push_mono(struct vs_analyzer *ana, const float *samples, size_t frames, uint64_t timestamp_ns);
void vs_analyzer_push_planar(struct vs_analyzer *ana, const float *const *planes, size_t channels, size_t frames,
			     bool muted, bool react_when_muted, uint64_t timestamp_ns);

void vs_analyzer_step(struct vs_analyzer *ana, float dt_sec, struct vs_audio_frame *out_frame);

float vs_analyzer_get_signal(const struct vs_analyzer *ana, const struct vs_audio_frame *frame, enum vs_signal_id sig,
			     float custom_low_hz, float custom_high_hz, enum vs_hz_band_mode custom_mode);

#ifdef __cplusplus
}
#endif
