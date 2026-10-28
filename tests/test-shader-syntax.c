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

static bool check_production_source_file(const char *src_path)
{
	FILE *fp = fopen(src_path, "rb");
	if (!fp) {
		fprintf(stderr, "FAIL: cannot open %s\n", src_path);
		return false;
	}
	fseek(fp, 0, SEEK_END);
	long len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (len <= 32L || len >= 1048576L) {
		fprintf(stderr, "FAIL: invalid file length %ld for %s\n", len, src_path);
		fclose(fp);
		return false;
	}
	char *buf = (char *)malloc((size_t)len + 1U);
	if (!buf) {
		fclose(fp);
		return false;
	}
	size_t nread = fread(buf, 1U, (size_t)len, fp);
	fclose(fp);
	if (nread != (size_t)len) {
		free(buf);
		return false;
	}
	buf[len] = '\0';

	if ((unsigned char)buf[0] == 0xEFU && (unsigned char)buf[1] == 0xBBU && (unsigned char)buf[2] == 0xBFU) {
		fprintf(stderr, "FAIL: UTF-8 BOM detected in %s\n", src_path);
		free(buf);
		return false;
	}

	const char *forbidden_tokens[] = {
		"150 ms", "QTimer", "QBasicTimer", "startTimer", "SetTimer", "sleep_for", "Sleep(",
	};
	for (size_t i = 0U; i < sizeof(forbidden_tokens) / sizeof(forbidden_tokens[0]); ++i) {
		if (strstr(buf, forbidden_tokens[i]) != NULL) {
			fprintf(stderr, "FAIL: forbidden token '%s' found in %s\n", forbidden_tokens[i], src_path);
			free(buf);
			return false;
		}
	}

	free(buf);
	return true;
}

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

	/* Verify all supplied production source files have zero UTF-8 BOMs and zero forbidden timer/sleep tokens */
	if (argc > 2) {
		for (int i = 2; i < argc; ++i) {
			ASSERT_TRUE(check_production_source_file(argv[i]));
		}
	} else {
		ASSERT_TRUE(check_production_source_file("src/ui/mfd-settings-dialog.cpp"));
	}

	printf("test-shader-syntax: PASS (%d production files verified)\n", (argc > 2) ? (argc - 2) : 1);
	return 0;
}
