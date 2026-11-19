/*
 * Visual Sidechain - FFT Visualizer Overlays (fft-visualizer-overlay.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Renders 32/512-bin spectrum bars, a polar circular spectrum ring, and a live
 * time-domain oscilloscope waveform (from Row 1 of audio_fft) over the source.
 */

uniform int overlay_mode <
	string label = "Visualizer Mode";
	string widget_type = "select";
	string option_0 = "Bars + Oscilloscope";
	string option_1 = "Polar Spectrum Ring";
	string option_2 = "Spectrum Bars Only";
	int minimum = 0;
	int maximum = 2;
	int step = 1;
> = 0;

uniform float bar_height_scale <
	string label = "Spectrum Bar Height";
	string widget_type = "slider";
	float minimum = 0.05;
	float maximum = 1.0;
	float step = 0.01;
> = 0.38;

uniform float overlay_opacity <
	string label = "Overlay Opacity";
	string widget_type = "slider";
	float minimum = 0.1;
	float maximum = 1.0;
	float step = 0.02;
> = 0.88;

uniform float4 bar_low_color <
	string label = "Low Frequency Color";
> = float4(0.08, 0.92, 0.65, 1.0);

uniform float4 bar_high_color <
	string label = "High Frequency Color";
> = float4(0.98, 0.22, 0.68, 1.0);

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
	float4 base = image.Sample(texture_sampler, uv);

	float3 grad_rgb = lerp(bar_low_color.rgb, bar_high_color.rgb, uv.x);
	float overlay_mask = 0.0;
	float3 overlay_rgb = grad_rgb;

	if (overlay_mode == 0 || overlay_mode == 2) {
		/* 32-band segmented spectrum bars along the bottom edge */
		float band_idx = floor(uv.x * 32.0);
		float band_frac = frac(uv.x * 32.0);
		float band_u = (band_idx + 0.5) / 32.0;

		float spec = vs_sample_spectrum(band_u);
		float fft_fine = vs_sample_fft(uv.x);
		float energy = (0.7 * spec + 0.3 * fft_fine) * bar_height_scale;

		float y_from_bottom = 1.0 - uv.y;
		float in_bar_x = step(0.12, band_frac) * step(band_frac, 0.88);
		float in_bar_y = step(y_from_bottom, energy);
		float cap_line = smoothstep(0.012, 0.0, abs(y_from_bottom - energy)) * in_bar_x;

		overlay_mask = max(overlay_mask, (in_bar_x * in_bar_y * 0.78 + cap_line));
	}

	if (overlay_mode == 0) {
		/* Oscilloscope waveform across upper center using Row 1 of audio_fft */
		float wave_val = vs_sample_waveform(uv.x);
		float wave_y = 0.25 + (wave_val - 0.5) * 0.32;
		float wave_dist = abs(uv.y - wave_y);
		float thickness = max(uv_pixel_interval.y * 2.5, 0.003);
		float wave_line = smoothstep(thickness * 2.2, thickness * 0.4, wave_dist);
		overlay_rgb = lerp(overlay_rgb, float3(1.0, 0.95, 0.85), wave_line);
		overlay_mask = max(overlay_mask, wave_line);
	}

	if (overlay_mode == 1) {
		/* Polar circular FFT spectrum ring */
		float aspect = (uv_size.y > 0.0) ? (uv_size.x / uv_size.y) : 1.7778;
		float2 p = (uv - float2(0.5, 0.5)) * float2(aspect, 1.0);
		float r = length(p);
		float ang = abs(atan2(p.y, p.x + 1e-6)) / 3.14159265;

		float ring_base = 0.22 + 0.04 * audio_kick;
		float ring_fft = vs_sample_fft(ang) * bar_height_scale * 0.45;
		float ring_outer = ring_base + ring_fft;

		float in_ring = smoothstep(ring_base - 0.006, ring_base, r) *
				(1.0 - smoothstep(ring_outer, ring_outer + 0.008, r));
		overlay_rgb = lerp(bar_low_color.rgb, bar_high_color.rgb, ang);
		overlay_mask = max(overlay_mask, in_ring);
	}

	float blend = saturate(overlay_mask * overlay_opacity);
	float3 out_rgb = lerp(base.rgb, overlay_rgb, blend);
	return float4(out_rgb, max(base.a, blend));
}
