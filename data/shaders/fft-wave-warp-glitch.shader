/*
 * Visual Sidechain - FFT Wave Warp & Glitch (fft-wave-warp-glitch.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Low FFT bins drive large fluid sinusoidal warps, mid bins drive fine ripples,
 * and high-frequency spectral flux / snare transients trigger horizontal glitch slices.
 */

uniform float bass_warp_amount <
	string label = "Bass Wave Warp Amount";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.01;
> = 0.35;

uniform float mid_ripple_amount <
	string label = "Mid Frequency Ripple";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.01;
> = 0.25;

uniform float glitch_strength <
	string label = "Flux / Snare Glitch Strength";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.01;
> = 0.45;

uniform float glitch_slices <
	string label = "Glitch Scanline Density";
	string widget_type = "slider";
	float minimum = 4.0;
	float maximum = 64.0;
	float step = 1.0;
> = 24.0;

float hash11(float p)
{
	return frac(sin(p * 127.1 + 311.7) * 43758.5453);
}

float4 mainImage(VertData v_in) : TARGET
{
	float2 uv = v_in.uv;

	float low_bin = vs_sample_fft(0.08 * uv.y + 0.02);
	float mid_bin = vs_sample_fft(0.35 + 0.30 * uv.y);

	float bass_drive = (0.6 * audio_bass + 0.4 * audio_sub_bass + 0.5 * low_bin) * bass_warp_amount;
	float mid_drive = (0.6 * audio_mid + 0.4 * mid_bin) * mid_ripple_amount;

	float wave_x = sin(uv.y * 10.0 + elapsed_time * 3.5) * 0.035 * bass_drive +
		       sin(uv.y * 42.0 - elapsed_time * 9.0) * 0.012 * mid_drive;
	float wave_y = cos(uv.x * 8.0 + elapsed_time * 2.8) * 0.025 * bass_drive;

	/* High-frequency spectral flux + snare scanline slice displacement */
	float slice_id = floor(uv.y * glitch_slices);
	float slice_rng = hash11(slice_id + floor(elapsed_time * 18.0));
	float transient_gate = saturate(max(audio_snare, audio_spectral_flux * 1.2) - 0.22);
	float slice_active = step(0.68, slice_rng) * transient_gate;
	float glitch_shift = (slice_rng - 0.5) * 0.16 * glitch_strength * slice_active;

	float2 warped_uv = clamp(uv + float2(wave_x + glitch_shift, wave_y), 0.0, 1.0);
	float chroma_split = 0.012 * glitch_strength * slice_active;

	float4 col_c = image.Sample(texture_sampler, warped_uv);
	float col_r = image.Sample(texture_sampler, clamp(warped_uv + float2(chroma_split, 0.0), 0.0, 1.0)).r;
	float col_b = image.Sample(texture_sampler, clamp(warped_uv - float2(chroma_split, 0.0), 0.0, 1.0)).b;

	return float4(col_r, col_c.g, col_b, col_c.a);
}
