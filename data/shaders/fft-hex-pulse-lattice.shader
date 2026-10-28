/*
 * Visual Sidechain - Hex Pulse Lattice (fft-hex-pulse-lattice.shader)
 * Copyright (C) 2026 Jeffrey Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Inset honeycomb outlines breathe with bass, carry a smooth kick ripple,
 * and brighten with high frequencies. The source's alpha is preserved.
 */

uniform int hex_cells <
	string label = "Cell Density";
	string widget_type = "slider";
	int minimum = 4;
	int maximum = 32;
	int step = 1;
> = 12;

uniform float hex_line_width <
	string label = "Line Width";
	string widget_type = "slider";
	float minimum = 0.005;
	float maximum = 0.08;
	float step = 0.005;
> = 0.025;

uniform float hex_ripple_speed <
	string label = "Ripple Speed";
	string widget_type = "slider";
	string tooltip = "Outward ripple cycles per second; zero freezes the ripple's travel.";
	float minimum = 0.0;
	float maximum = 0.5;
	float step = 0.01;
> = 0.15;

uniform float hex_audio_amount <
	string label = "Audio Response";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 0.6;

uniform float hex_opacity <
	string label = "Lattice Opacity";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 0.6;

uniform float4 hex_low_color <
	string label = "Inner / Bass Color";
> = float4(0.08, 0.7, 1.0, 1.0);

uniform float4 hex_high_color <
	string label = "Outer / Treble Color";
> = float4(0.8, 0.12, 1.0, 1.0);

float4 mainImage(VertData v_in) : TARGET
{
	float4 base = image.Sample(texture_sampler, v_in.uv);
	float aspect = uv_size.x / max(uv_size.y, 1.0);
	float cells = clamp((float)hex_cells, 4.0, 32.0);
	float2 p = (v_in.uv - float2(0.5, 0.5)) * float2(aspect, 1.0) * cells;

	/* Two offset rectangular grids form triangularly spaced hex centers. */
	float2 spacing = float2(1.0, 1.73205081);
	float2 q1 = p - spacing * floor(p / spacing + 0.5);
	float2 q2 = p - spacing * (floor(p / spacing) + 0.5);
	float2 q = dot(q1, q1) < dot(q2, q2) ? q1 : q2;
	float2 center = p - q;
	float radial = length(center) / cells;
	float response = saturate(hex_audio_amount);
	float bass = saturate(audio_bass) * response;
	float kick = saturate(audio_kick) * response;
	float highs = saturate(audio_high) * response;
	float ripple_phase = radial * 10.0 - elapsed_time * clamp(hex_ripple_speed, 0.0, 0.5) * 6.28318530;
	float radius = 0.405 + 0.065 * bass + 0.025 * kick * sin(ripple_phase);

	float2 aq = abs(q);
	float hex_distance = max(aq.x, dot(aq, float2(0.5, 0.86602540)));
	float aa = max(abs(ddx(hex_distance)) + abs(ddy(hex_distance)), cells / max(uv_size.y, 1.0) * 0.5);
	float width = clamp(hex_line_width, 0.005, 0.08);
	float outline = 1.0 - smoothstep(width, width + aa, abs(hex_distance - radius));
	float4 tint = lerp(hex_low_color, hex_high_color, saturate(radial * 1.2 + highs * 0.25));
	float coverage = saturate(outline * saturate(hex_opacity) * (0.7 + 0.3 * highs) * tint.a);

	/* Input and output RGB are premultiplied; never paint outside source alpha. */
	return float4(lerp(base.rgb, saturate(tint.rgb) * base.a, coverage), base.a);
}
