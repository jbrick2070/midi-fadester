/*
 * Visual Sidechain - FFT Chromatic Prism & Bloom (fft-chromatic-prism-bloom.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Bass, mid, and high FFT bins independently drive radial RGB prism dispersion,
 * spectral centroid hue rotation, and highlight bloom.
 */

uniform float prism_dispersion <
	string label = "Radial Prism Dispersion";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.01;
> = 0.40;

uniform float bloom_intensity <
	string label = "FFT Highlight Bloom";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 2.0;
	float step = 0.05;
> = 0.85;

uniform float bloom_threshold <
	string label = "Bloom Luma Threshold";
	string widget_type = "slider";
	float minimum = 0.1;
	float maximum = 0.95;
	float step = 0.01;
> = 0.55;

uniform float hue_shift_amount <
	string label = "Centroid Hue Shift";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.01;
> = 0.25;

float3 rotate_hue(float3 rgb, float angle_rad)
{
	float c = cos(angle_rad);
	float s = sin(angle_rad);
	float3 k = float3(0.57735, 0.57735, 0.57735);
	return rgb * c + cross(k, rgb) * s + k * dot(k, rgb) * (1.0 - c);
}

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
	float2 center_vec = uv - float2(0.5, 0.5);
	float dist = length(center_vec);

	float low_fft = vs_sample_spectrum(0.12);
	float mid_fft = vs_sample_spectrum(0.45);
	float high_fft = vs_sample_spectrum(0.82);

	float disp = prism_dispersion * 0.045 * (0.55 * audio_bass + 0.45 * audio_kick + 0.35 * low_fft) * (0.25 + dist);
	float2 uv_r = clamp(uv + center_vec * disp, 0.0, 1.0);
	float2 uv_g = clamp(uv + center_vec * (disp * 0.25 * (mid_fft - 0.5)), 0.0, 1.0);
	float2 uv_b = clamp(uv - center_vec * disp * (0.8 + 0.4 * high_fft), 0.0, 1.0);

	float4 base_g = image.Sample(texture_sampler, uv_g);
	float r = image.Sample(texture_sampler, uv_r).r;
	float b = image.Sample(texture_sampler, uv_b).b;
	float3 prism_rgb = float3(r, base_g.g, b);

	/* 8-tap highlight bloom scaled by audio_mid + audio_high + audio_kick */
	float bloom_drive = bloom_intensity * (0.45 * audio_kick + 0.35 * audio_mid + 0.40 * audio_high);
	float2 bloom_step = uv_pixel_interval * (3.0 + 10.0 * bloom_drive);
	float3 bloom_accum = float3(0.0, 0.0, 0.0);

	[unroll]
	for (int i = 0; i < 8; i++) {
		float ang = (float)i * 0.78539816;
		float2 tap_uv = clamp(uv + float2(cos(ang), sin(ang)) * bloom_step, 0.0, 1.0);
		float3 tap_c = image.Sample(texture_sampler, tap_uv).rgb;
		float luma = dot(tap_c, float3(0.2126, 0.7152, 0.0722));
		float weight = saturate((luma - bloom_threshold) / max(1.0 - bloom_threshold, 1e-4));
		bloom_accum += tap_c * weight;
	}
	bloom_accum *= (0.125 * bloom_drive);

	float hue_angle = hue_shift_amount * (audio_spectral_centroid * 3.14159265 + 0.5 * audio_beat);
	float3 graded = saturate(rotate_hue(prism_rgb + bloom_accum, hue_angle));

	return float4(graded, base_g.a);
}
