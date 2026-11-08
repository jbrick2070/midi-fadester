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
	string label = "Maximum Flash Rate (Hz)";
	string tooltip = "Limits this effect to 0.5-3 flashes per second. Faster triggers are skipped. Flashing may still harm photosensitive viewers.";
	string widget_type = "slider";
	float minimum = 0.5;
	float maximum = 3.0;
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

	/* Photosensitivity cap (R-SHD-3 (8)): the host admits at most max_flash_rate_hz flashes per second, whatever the
	 * audio, and hands this shader vs_flash_envelope: one rise and one fall back to exactly 0 per flash. Its limiter
	 * reads onset_threshold and max_flash_rate_hz and triggers on max(audio_kick, audio_snare * 0.85,
	 * audio_spectral_flux). */
	float flash_amt = saturate(vs_flash_envelope) * flash_intensity;

	float3 tinted = lerp(base.rgb, flash_tint.rgb, flash_amt);
	if (allow_invert && vs_flash_envelope > 0.65) {
		float3 inv = 1.0 - base.rgb;
		tinted = lerp(tinted, inv, flash_amt * 0.75);
	}

	return float4(saturate(tinted), base.a);
}
