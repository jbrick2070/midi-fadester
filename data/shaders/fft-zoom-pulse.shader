/*
 * Visual Sidechain - FFT Zoom Pulse & Barrel Lens (fft-zoom-pulse.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Radial scale punch, barrel lens distortion, and reactive edge vignette
 * driven by audio_kick, audio_bass, and audio_custom_band.
 */

uniform float kick_zoom_amount <
	string label = "Kick Zoom Punch";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 0.6;
	float step = 0.01;
> = 0.18;

uniform float bass_barrel_warp <
	string label = "Bass Barrel Lens Distortion";
	string widget_type = "slider";
	float minimum = -0.5;
	float maximum = 0.5;
	float step = 0.01;
> = 0.16;

uniform float vignette_strength <
	string label = "Reactive Edge Vignette";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 0.35;

float4 mainImage(VertData v_in) : TARGET
{
	float2 centered = v_in.uv - float2(0.5, 0.5);
	float r2 = dot(centered, centered);

	float pulse = 0.65 * audio_kick + 0.35 * audio_bass + 0.25 * audio_custom_band;
	float zoom_scale = 1.0 / (1.0 + kick_zoom_amount * pulse);

	float barrel = 1.0 + bass_barrel_warp * pulse * r2 * 4.0;
	float2 sample_uv = clamp(float2(0.5, 0.5) + centered * zoom_scale * barrel, 0.0, 1.0);

	float4 col = image.Sample(texture_sampler, sample_uv);

	float vig = 1.0 - saturate(r2 * 2.2 * vignette_strength * (0.4 + 0.6 * pulse));
	return float4(col.rgb * vig, col.a);
}
