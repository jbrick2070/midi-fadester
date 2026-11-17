/*
 * visual-sidechain - test fixture: HLSL forms the Mac's GLSL and Metal compilers refuse
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Comments may say (float)i and [unroll]; only code counts.
 */

uniform int folds = 6;

float4 mainImage(VertData v_in) : TARGET
{
	float acc = 0.0;
	[unroll]
	for (int i = 0; i < 4; i++) {
		acc += (float)i * 0.25;
	}
	float f = max((float) folds, 2.0);
	return image.Sample(texture_sampler, v_in.uv) * saturate(acc / f) * audio_level;
}
