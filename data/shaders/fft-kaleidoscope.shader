/*
 * Visual Sidechain - FFT Kaleidoscope (fft-kaleidoscope.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * N-fold polar mirror symmetry where rotation speed, zoom, and per-sector
 * radial offset sample audio_fft, audio_kick, and audio_beat.
 */

uniform int symmetry_folds <
	string label = "Symmetry Folds (Segments)";
	string widget_type = "slider";
	int minimum = 2;
	int maximum = 16;
	int step = 1;
> = 6;

uniform float rotation_speed <
	string label = "Beat Rotation Speed";
	string widget_type = "slider";
	float minimum = -2.0;
	float maximum = 2.0;
	float step = 0.05;
> = 0.45;

uniform float radial_fft_shift <
	string label = "FFT Radial Displacement";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 0.5;
	float step = 0.01;
> = 0.18;

uniform float dry_wet_mix <
	string label = "Dry / Wet Mix";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 1.0;

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
	float4 dry = image.Sample(texture_sampler, uv);

	float aspect = (uv_size.y > 0.0) ? (uv_size.x / uv_size.y) : 1.7778;
	float2 p = (uv - float2(0.5, 0.5)) * float2(aspect, 1.0);
	float r = length(p);
	float a = atan2(p.y, p.x + 1e-6);

	float folds = max((float)symmetry_folds, 2.0);
	float sector = 6.28318530 / folds;

	a += elapsed_time * rotation_speed + audio_beat * 0.65 + audio_kick * 0.25;
	float mod_a = a - sector * floor(a / sector);
	mod_a = min(mod_a, sector - mod_a);

	float norm_sector_u = saturate(mod_a / max(0.5 * sector, 1e-4));
	float fft_val = vs_sample_fft(norm_sector_u);

	float shifted_r = r * (1.0 - 0.18 * audio_kick) + radial_fft_shift * fft_val;
	float2 k_uv = float2(0.5, 0.5) + float2(cos(mod_a), sin(mod_a)) * shifted_r / float2(aspect, 1.0);
	k_uv = abs(frac(k_uv * 0.5 + 0.5) * 2.0 - 1.0);

	float4 wet = image.Sample(texture_sampler, clamp(k_uv, 0.0, 1.0));
	return lerp(dry, wet, saturate(dry_wet_mix));
}
