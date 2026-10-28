/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_TRUE(cond)                                                               \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

int main(int argc, char **argv)
{
	const char *effect_path = (argc > 1) ? argv[1] : "data/effects/mfd_layer_mixer.effect";
	FILE *fp = fopen(effect_path, "rb");
	ASSERT_TRUE(fp != NULL);

	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	ASSERT_TRUE(len > 64L && len < 65536L);

	char *buf = (char *)malloc((size_t)len + 1U);
	ASSERT_TRUE(buf != NULL);

	size_t read_bytes = fread(buf, 1U, (size_t)len, fp);
	fclose(fp);
	ASSERT_TRUE(read_bytes == (size_t)len);
	buf[len] = '\0';

	/* Reject UTF-8 BOM */
	ASSERT_TRUE(
		!((unsigned char)buf[0] == 0xEFU && (unsigned char)buf[1] == 0xBBU && (unsigned char)buf[2] == 0xBFU));

	/* Verify required OBS effect uniforms, sampler, entry points, and DrawAlpha technique */
	ASSERT_TRUE(strstr(buf, "uniform float4x4 ViewProj;") != NULL);
	ASSERT_TRUE(strstr(buf, "uniform texture2d image;") != NULL);
	ASSERT_TRUE(strstr(buf, "uniform float opacity") != NULL);
	ASSERT_TRUE(strstr(buf, "sampler_state textureSampler") != NULL);
	ASSERT_TRUE(strstr(buf, "VertData VSDefault(VertData v_in)") != NULL);
	ASSERT_TRUE(strstr(buf, "float4 PSAlpha(VertData v_in) : TARGET") != NULL);
	ASSERT_TRUE(strstr(buf, "return col * clamp(opacity, 0.0, 1.0);") != NULL);
	ASSERT_TRUE(strstr(buf, "technique DrawAlpha") != NULL);
	ASSERT_TRUE(strstr(buf, "vertex_shader = VSDefault(v_in);") != NULL);
	ASSERT_TRUE(strstr(buf, "pixel_shader  = PSAlpha(v_in);") != NULL);

	free(buf);

	/* Verify UI dialog source has no UTF-8 BOM and no stale 150 ms timer-fallback tooltip copy */
	const char *ui_path = (argc > 2) ? argv[2] : "src/ui/mfd-settings-dialog.cpp";
	FILE *ui_fp = fopen(ui_path, "rb");
	ASSERT_TRUE(ui_fp != NULL);
	fseek(ui_fp, 0, SEEK_END);
	long ui_len = ftell(ui_fp);
	fseek(ui_fp, 0, SEEK_SET);
	ASSERT_TRUE(ui_len > 64L && ui_len < 524288L);
	char *ui_buf = (char *)malloc((size_t)ui_len + 1U);
	ASSERT_TRUE(ui_buf != NULL);
	size_t ui_read = fread(ui_buf, 1U, (size_t)ui_len, ui_fp);
	fclose(ui_fp);
	ASSERT_TRUE(ui_read == (size_t)ui_len);
	ui_buf[ui_len] = '\0';
	ASSERT_TRUE(!((unsigned char)ui_buf[0] == 0xEFU && (unsigned char)ui_buf[1] == 0xBBU &&
		      (unsigned char)ui_buf[2] == 0xBFU));
	ASSERT_TRUE(strstr(ui_buf, "150 ms") == NULL);
	free(ui_buf);

	printf("test-shader-syntax: PASS\n");
	return 0;
}
