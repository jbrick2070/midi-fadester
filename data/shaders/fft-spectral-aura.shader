/*
 * Visual Sidechain - FFT Spectral Edge / Border Aura (fft-spectral-aura.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Draws an FFT-reactive spectral aura around alpha cutouts (such as OBS 33's
 * NVIDIA Background Removal silhouette) and/or the frame border, mapping polar
 * angle around the subject to the 512-bin dual-FFT texture (audio_fft).
 */

uniform float aura_radius_px <
	string label = "Aura Radius (px)";
	string widget_type = "slider";
	float minimum = 2.0;
	float maximum = 48.0;
	float step = 0.5;
> = 16.0;

uniform float aura_intensity <
	string label = "Aura Intensity";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 3.0;
	float step = 0.05;
> = 1.4;

uniform float fft_reactivity <
	string label = "FFT Bin Reactivity";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 2.0;
	float step = 0.05;
> = 1.0;

uniform bool border_aura <
	string label = "Include Frame Border Aura";
> = true;

uniform float4 low_freq_color <
	string label = "Bass / Low FFT Color";
> = float4(1.0, 0.18, 0.45, 1.0);

uniform float4 high_freq_color <
	string label = "Treble / High FFT Color";
> = float4(0.10, 0.85, 1.0, 1.0);

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;
	float4 base = image.Sample(texture_sampler, uv);

	/* Map polar angle around frame center to [0, 1] spectrum coordinate */
	float2 centered = uv - float2(0.5, 0.5);
	float angle = atan2(centered.y, centered.x + 1e-6);
	float norm_angle = abs(angle) / 3.14159265;

	float fft_val = vs_sample_fft(norm_angle);
	float spec_val = vs_sample_spectrum(norm_angle);
	float drive = saturate(lerp(0.35, 1.0, fft_reactivity) * (0.65 * fft_val + 0.35 * spec_val) +
			       0.45 * audio_kick + 0.25 * audio_bass + 0.20 * audio_custom_band);

	/* Sample 8 directions around current pixel to detect alpha silhouette edge */
	float radius = aura_radius_px * (0.5 + 0.9 * drive);
	float2 step_uv = uv_pixel_interval * radius;

	float neighbor_alpha = 0.0;
	[unroll]
	for (int i = 0; i < 8; i++) {
		float theta = (float)i * 0.78539816;
		float2 dir = float2(cos(theta), sin(theta));
		float2 sample_uv = clamp(uv + dir * step_uv, 0.0, 1.0);
		neighbor_alpha = max(neighbor_alpha, image.Sample(texture_sampler, sample_uv).a);
	}

	float silhouette_edge = saturate(neighbor_alpha - base.a);

	/* Optional frame-perimeter aura for opaque sources without alpha cutout */
	float2 edge_dist = min(uv, 1.0 - uv);
	float min_edge = min(edge_dist.x, edge_dist.y);
	float border_width = max(uv_pixel_interval.x, uv_pixel_interval.y) * radius * 1.5;
	float frame_edge = border_aura ? saturate(1.0 - min_edge / max(border_width, 1e-4)) : 0.0;
	frame_edge = frame_edge * frame_edge;

	float aura_mask = max(silhouette_edge, frame_edge) * aura_intensity * drive;
	float3 aura_rgb = lerp(low_freq_color.rgb, high_freq_color.rgb, saturate(norm_angle * 0.8 + 0.2 * audio_high));

	float3 out_rgb = base.rgb * base.a + aura_rgb * aura_mask * (1.0 - 0.5 * base.a);
	float out_a = saturate(base.a + aura_mask);
	return float4(out_rgb, out_a);
}
