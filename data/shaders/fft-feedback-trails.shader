/*
 * Visual Sidechain - FFT Feedback Trails (fft-feedback-trails.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Uses the previous_output ping-pong render target so FFT energy drives
 * motion-trail persistence, radial zoom drift, and spectral hue shift.
 */

uniform float base_persistence <
	string label = "Base Trail Persistence";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 0.96;
	float step = 0.01;
> = 0.72;

uniform float fft_persistence_boost <
	string label = "FFT Persistence Boost";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 0.35;
	float step = 0.01;
> = 0.20;

uniform float trail_zoom_drift <
	string label = "Trail Radial Zoom Drift";
	string widget_type = "slider";
	float minimum = -0.05;
	float maximum = 0.05;
	float step = 0.002;
> = 0.014;

uniform float trail_hue_drift <
	string label = "Trail Spectral Hue Shift";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 0.5;
	float step = 0.01;
> = 0.12;

float3 shift_trail_hue(float3 rgb, float a)
{
	float c = cos(a);
	float s = sin(a);
	float3 k = float3(0.57735, 0.57735, 0.57735);
	return rgb * c + cross(k, rgb) * s + k * dot(k, rgb) * (1.0 - c);
}

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
	float4 current_col = image.Sample(texture_sampler, uv);

	float energy = saturate(0.45 * audio_bass + 0.35 * audio_kick + 0.20 * vs_sample_spectrum(0.25));
	float decay = clamp(base_persistence + fft_persistence_boost * energy, 0.0, 0.965);

	float2 centered = uv - float2(0.5, 0.5);
	float zoom = 1.0 - trail_zoom_drift * (0.4 + 0.6 * energy);
	float2 prev_uv = clamp(float2(0.5, 0.5) + centered * zoom, 0.0, 1.0);

	float4 prev_col = previous_output.Sample(texture_sampler, prev_uv);
	float3 shifted_prev = saturate(shift_trail_hue(prev_col.rgb, trail_hue_drift * (0.3 + 0.7 * audio_spectral_centroid)));

	float3 blended = max(current_col.rgb, shifted_prev * decay);
	float out_a = max(current_col.a, prev_col.a * decay);
	return float4(blended, out_a);
}
