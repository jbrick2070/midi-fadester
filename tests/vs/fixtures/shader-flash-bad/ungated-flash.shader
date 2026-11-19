/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Negative test fixture for check-shader-flash-safety.cmake: multiplies final colour by raw audio_kick.
 */

float4 mainImage(VertData v_in) : TARGET
{
	float4 base = image.Sample(texture_sampler, v_in.uv);
	return float4(base.rgb * (0.5 + 0.5 * audio_kick), base.a);
}
