/*
 * Visual Sidechain - FFT Mirror & Split (fft-mirror-split.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Configurable 2-way, 4-way, diagonal, or multi-slice mirror where FFT bins
 * displace slices and beat phase modulates mirror axis offsets.
 */

uniform int mirror_mode <
	string label = "Mirror Mode";
	string widget_type = "select";
	string option_0 = "Horizontal 2-Way";
	string option_1 = "4-Way Quad";
	string option_2 = "Beat-Flipped Diagonal";
	string option_3 = "FFT Displaced Slices";
	int minimum = 0;
	int maximum = 3;
	int step = 1;
> = 1;

uniform float fft_slice_shift <
	string label = "FFT Slice Shift Amount";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 0.5;
	float step = 0.01;
> = 0.15;

uniform int slice_count <
	string label = "Slice Count (Mode 3)";
	string widget_type = "slider";
	int minimum = 2;
	int maximum = 24;
	int step = 1;
> = 8;

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
	float2 m_uv = uv;

	float kick_offset = (audio_kick * 0.08 + audio_bass * 0.04) * fft_slice_shift;

	if (mirror_mode == 0) {
		/* Horizontal 2-way mirror with bass-driven center seam breathe */
		float dx = abs(uv.x - 0.5);
		m_uv.x = clamp(0.5 - dx * (1.0 - kick_offset), 0.0, 1.0);
	} else if (mirror_mode == 1) {
		/* 4-way Quad mirror */
		float2 d = abs(uv - float2(0.5, 0.5));
		m_uv = clamp(float2(0.5, 0.5) - d * (1.0 - kick_offset), 0.0, 1.0);
	} else if (mirror_mode == 2) {
		/* Diagonal mirror flip modulated by beat phase */
		bool flip_diag = (audio_beat > 0.5);
		if ((!flip_diag && (uv.x > uv.y)) || (flip_diag && ((1.0 - uv.x) > uv.y))) {
			m_uv = float2(uv.y, uv.x);
		}
		m_uv = clamp(m_uv + float2(kick_offset, -kick_offset) * 0.35, 0.0, 1.0);
	} else {
		/* FFT Slice displacement + alternating mirror */
		float slices = max((float)slice_count, 2.0);
		float s_idx = floor(uv.y * slices);
		float s_u = (s_idx + 0.5) / slices;
		float bin_val = vs_sample_fft(s_u);
		float dir = (fmod(s_idx, 2.0) < 0.5) ? 1.0 : -1.0;
		m_uv.x = frac(uv.x + dir * bin_val * fft_slice_shift);
	}

	return image.Sample(texture_sampler, clamp(m_uv, 0.0, 1.0));
}
