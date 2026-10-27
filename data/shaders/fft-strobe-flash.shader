/*
 * Visual Sidechain - FFT Strobe & Flash (fft-strobe-flash.shader)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Onset and spectral-flux gated brightness, color tint, or luma-invert flash
 * with a configurable maximum flash rate Hz cap for photosensitive safety.
 */

uniform float flash_intensity <
	string label = "Flash Intensity";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 0.55;

uniform float onset_threshold <
	string label = "Kick / Flux Trigger Threshold";
	string widget_type = "slider";
	float minimum = 0.05;
	float maximum = 0.90;
	float step = 0.02;
> = 0.30;

uniform float max_flash_rate_hz <
	string label = "Max Strobe Rate Cap (Hz)";
	string widget_type = "slider";
	float minimum = 0.5;
	float maximum = 8.0;
	float step = 0.25;
> = 3.0;

uniform bool allow_invert <
	string label = "Invert Colors on Peak Hits";
> = false;

uniform float4 flash_tint <
	string label = "Flash Tint Color";
> = float4(1.0, 0.92, 0.80, 1.0);

float4 mainImage(VertData v_in) : TARGET
{
	float4 base = image.Sample(texture_sampler, v_in.uv);

	float trigger_sig = max(audio_kick, max(audio_snare * 0.85, audio_spectral_flux));
	float over_thresh = saturate((trigger_sig - onset_threshold) / max(1.0 - onset_threshold, 1e-4));

	/* Rate-limiting envelope gated by max_flash_rate_hz */
	float rate_hz = clamp(max_flash_rate_hz, 0.5, 8.0);
	float phase_gate = 0.5 + 0.5 * cos(elapsed_time * rate_hz * 6.28318530);
	float flash_amt = over_thresh * phase_gate * flash_intensity;

	float3 tinted = lerp(base.rgb, flash_tint.rgb, flash_amt);
	if (allow_invert && over_thresh > 0.65) {
		float3 inv = 1.0 - base.rgb;
		tinted = lerp(tinted, inv, flash_amt * 0.75);
	}

	return float4(saturate(tinted), base.a);
}
