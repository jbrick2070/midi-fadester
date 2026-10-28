/*
 * Visual Sidechain - Polygon Tunnel (fft-polygon-tunnel.shader)
 * Copyright (C) 2026 Jeffrey Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Original GPL-2.0-or-later HLSL shader for Visual Sidechain.
 * Logarithmically spaced polygon outlines travel toward the viewer, with
 * bounded audio offsets and a softened center. The source's alpha is preserved.
 */

uniform int tunnel_sides <
	string label = "Polygon Sides";
	string widget_type = "slider";
	int minimum = 3;
	int maximum = 8;
	int step = 1;
> = 6;

uniform float tunnel_depth <
	string label = "Tunnel Depth";
	string widget_type = "slider";
	string tooltip = "Ring density per doubling of radius. Bind a parameter fader here to open or deepen the tunnel.";
	float minimum = 2.0;
	float maximum = 8.0;
	float step = 0.1;
> = 3.0;

uniform float tunnel_speed <
	string label = "Travel Speed";
	string widget_type = "slider";
	string tooltip = "Rings per second; positive travels outward, negative inward, zero stops travel.";
	float minimum = -0.3;
	float maximum = 0.3;
	float step = 0.01;
> = 0.15;

uniform float tunnel_rotation_speed <
	string label = "Rotation Speed";
	string widget_type = "slider";
	string tooltip = "Radians per second; zero stops rotation. Audio can still add a small rotation offset.";
	float minimum = -0.3;
	float maximum = 0.3;
	float step = 0.01;
> = 0.08;

uniform float tunnel_line_width <
	string label = "Line Width";
	string widget_type = "slider";
	float minimum = 0.005;
	float maximum = 0.12;
	float step = 0.005;
> = 0.035;

uniform float tunnel_audio_amount <
	string label = "Audio Response";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 0.6;

uniform float tunnel_opacity <
	string label = "Tunnel Opacity";
	string widget_type = "slider";
	float minimum = 0.0;
	float maximum = 1.0;
	float step = 0.02;
> = 0.65;

uniform float4 tunnel_low_color <
	string label = "Inner / Bass Color";
> = float4(1.0, 0.15, 0.45, 1.0);

uniform float4 tunnel_high_color <
	string label = "Outer / Treble Color";
> = float4(0.1, 0.8, 1.0, 1.0);

float4 mainImage(VertData v_in) : TARGET
{
	float4 base = image.Sample(texture_sampler, v_in.uv);
	float aspect = uv_size.x / max(uv_size.y, 1.0);
	float2 p = (v_in.uv - float2(0.5, 0.5)) * float2(aspect, 1.0);
	float response = saturate(tunnel_audio_amount);
	float bass = saturate(audio_bass) * response;
	float kick = saturate(audio_kick) * response;
	float highs = saturate(audio_high) * response;

	/* Audio offsets never multiply elapsed time, so a transient cannot jump the clock. */
	float rotation = elapsed_time * clamp(tunnel_rotation_speed, -0.3, 0.3) + bass * 0.18 + kick * 0.12;
	float angle = atan2(p.y, p.x + 1e-6) + rotation;
	float sector = 6.28318530 / clamp((float)tunnel_sides, 3.0, 8.0);
	float folded = angle - sector * floor(angle / sector + 0.5);
	float radius = max(length(p) * cos(folded), 0.0001);
	float phase = log2(radius) * clamp(tunnel_depth, 2.0, 8.0) -
		      elapsed_time * clamp(tunnel_speed, -0.3, 0.3) + bass * 0.18 + kick * 0.12;

	/* Differentiate before wrapping the phase to avoid artificial seam derivatives. */
	float aa = max(abs(ddx(phase)) + abs(ddy(phase)), 0.0001);
	float ring_distance = abs(frac(phase + 0.5) - 0.5);
	float width = clamp(tunnel_line_width, 0.005, 0.12);
	float outline = 1.0 - smoothstep(width, width + aa, ring_distance);
	float density_fade = 1.0 - smoothstep(0.12, 0.4, aa);
	float center_fade = smoothstep(0.012, 0.045, radius);
	float4 tint = lerp(tunnel_low_color, tunnel_high_color, saturate(radius * 1.5 + highs * 0.2));
	float coverage = saturate(outline * density_fade * center_fade * saturate(tunnel_opacity) *
				 (0.7 + 0.3 * highs) * tint.a);

	return float4(lerp(base.rgb, saturate(tint.rgb) * base.a, coverage), base.a);
}
