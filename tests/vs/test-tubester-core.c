/*
 * Tubester - tape path and speed unit tests (REQUIREMENTS R-TAP-3, R-SPD-1, R-SPD-3, R-SPD-4)
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include "tubester/speed.h"
#include "tubester/tape-url.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_path_usable(void)
{
	assert(tub_path_is_usable("C:\\tapes\\reel.mp4"));
	assert(tub_path_is_usable("c:/tapes/reel.mp4"));
	assert(tub_path_is_usable("\\\\nas\\share\\reel.mp4"));
	assert(tub_path_is_usable("D:\\it's 100% #1 & \xC3\xBC\xC3\xAF \xE6\x97\xA5\xE6\x9C\xAC.mp4"));
	assert(!tub_path_is_usable(NULL));
	assert(!tub_path_is_usable(""));
	assert(!tub_path_is_usable("C:"));
	/* a drive root alone names no file */
	assert(!tub_path_is_usable("C:\\"));
	assert(!tub_path_is_usable("C:/"));
	assert(!tub_path_is_usable("C:\\\\/"));
	assert(tub_path_is_usable("C:\\a"));
	assert(!tub_path_is_usable("reel.mp4"));
	assert(!tub_path_is_usable("\\reel.mp4"));
	assert(!tub_path_is_usable("/tapes/reel.mp4"));
	assert(!tub_path_is_usable("C:reel.mp4"));
	assert(!tub_path_is_usable("\\\\\\reel.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re*el.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re?el.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re\nel.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re\"el.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re<el.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re>el.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re|el.mp4"));
	assert(!tub_path_is_usable("C:\\tapes\\re\x7f"
				   "el.mp4"));
	/* UNC needs a server, a share and a path; device forms are not shares */
	assert(tub_path_is_usable("//nas/share/reel.mp4"));
	assert(!tub_path_is_usable("\\\\nas"));
	assert(!tub_path_is_usable("\\\\nas\\"));
	assert(!tub_path_is_usable("\\\\nas\\\\reel.mp4"));
	assert(!tub_path_is_usable("\\\\.\\C:\\reel.mp4"));
	assert(!tub_path_is_usable("\\\\?\\C:\\reel.mp4"));
	char *too_long = malloc(TUB_PATH_MAX + 2);
	memset(too_long, 'a', TUB_PATH_MAX + 1);
	memcpy(too_long, "C:\\", 3);
	too_long[TUB_PATH_MAX + 1] = '\0';
	assert(!tub_path_is_usable(too_long));
	too_long[TUB_PATH_MAX] = '\0';
	assert(tub_path_is_usable(too_long));
	free(too_long);
	printf("  test_path_usable: PASS\n");
}

static void test_path_to_url(void)
{
	char out[512];
	assert(tub_path_to_file_url("C:\\tapes\\reel.mp4", out, sizeof(out)) == strlen("file:///C:/tapes/reel.mp4"));
	assert(strcmp(out, "file:///C:/tapes/reel.mp4") == 0);
	assert(tub_path_to_file_url("c:/tapes/reel.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///c:/tapes/reel.mp4") == 0);
	/* the characters the probes proved: space, apostrophe, percent, hash, ampersand, umlauts, kanji */
	assert(tub_path_to_file_url("D:\\it's 100% #1 & \xC3\xBC.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///D:/it%27s%20100%25%20%231%20%26%20%C3%BC.mp4") == 0);
	assert(tub_path_to_file_url("E:\\\xE6\x97\xA5\xE6\x9C\xAC\\a.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///E:/%E6%97%A5%E6%9C%AC/a.mp4") == 0);
	/* a second colon is not a drive separator */
	assert(tub_path_to_file_url("C:\\a:b.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///C:/a%3Ab.mp4") == 0);
	assert(tub_path_to_file_url("\\\\nas\\share\\reel.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file://nas/share/reel.mp4") == 0);
	/* unusable paths and small buffers give an empty string and 0 */
	assert(tub_path_to_file_url("reel.mp4", out, sizeof(out)) == 0 && out[0] == '\0');
	assert(tub_path_to_file_url("C:\\tapes\\reel.mp4", out, 10) == 0 && out[0] == '\0');
	for (size_t size = 1; size <= 9; size++) {
		memset(out, 'x', sizeof(out));
		assert(tub_path_to_file_url("C:\\tapes\\reel.mp4", out, size) == 0 && out[0] == '\0');
		memset(out, 'x', sizeof(out));
		assert(tub_path_to_file_url("\\\\nas\\share\\reel.mp4", out, size) == 0 && out[0] == '\0');
	}
	assert(tub_path_to_file_url("C:\\tapes\\reel.mp4", out, strlen("file:///C:/tapes/reel.mp4")) == 0);
	assert(tub_path_to_file_url("C:\\tapes\\reel.mp4", out, strlen("file:///C:/tapes/reel.mp4") + 1) > 0);
	assert(tub_path_to_file_url("C:\\tapes\\reel.mp4", NULL, 0) == 0);
	printf("  test_path_to_url: PASS\n");
}

static void test_speed_range(void)
{
	assert(tub_speed_clamp_pct(0) == 25);
	assert(tub_speed_clamp_pct(-5) == 25);
	assert(tub_speed_clamp_pct(25) == 25);
	assert(tub_speed_clamp_pct(100) == 100);
	assert(tub_speed_clamp_pct(800) == 800);
	assert(tub_speed_clamp_pct(801) == 800);
	assert(tub_speed_clamp_pct(1600) == 800);
	assert(tub_speed_pct_to_rate(100) == 1.0);
	assert(tub_speed_pct_to_rate(25) == 0.25);
	assert(tub_speed_pct_to_rate(1600) == 8.0);
	printf("  test_speed_range: PASS\n");
}

static void test_speed_ladder(void)
{
	assert(tub_speed_step_pct(100, 1) == 125);
	assert(tub_speed_step_pct(100, -1) == 75);
	assert(tub_speed_step_pct(110, 1) == 125);
	assert(tub_speed_step_pct(110, -1) == 100);
	assert(tub_speed_step_pct(800, 1) == 800);
	assert(tub_speed_step_pct(25, -1) == 25);
	assert(tub_speed_step_pct(600, 1) == 800);
	assert(tub_speed_step_pct(26, -1) == 25);
	assert(tub_speed_step_pct(1600, 1) == 800);
	assert(tub_speed_step_pct(0, 1) == 50);
	assert(tub_speed_step_pct(110, 0) == 110);
	assert(tub_speed_step_pct(5000, 0) == 800);
	/* up the whole ladder and back lands where it started */
	int pct = 25, steps = 0;
	while (pct < 800) {
		pct = tub_speed_step_pct(pct, 1);
		steps++;
	}
	assert(steps == 10);
	while (pct > 25)
		pct = tub_speed_step_pct(pct, -1);
	assert(pct == 25);
	printf("  test_speed_ladder: PASS\n");
}

static void test_coalesce(void)
{
	struct tub_coalesce c;
	int pct = 0;
	tub_coalesce_init(&c);
	/* the first change goes at once */
	assert(tub_coalesce_offer(&c, 110, 1000));
	assert(!tub_coalesce_flush(&c, 1000, &pct));
	/* a burst inside the interval is held, and only the last value survives */
	assert(!tub_coalesce_offer(&c, 120, 1000 + 10000000));
	assert(!tub_coalesce_offer(&c, 130, 1000 + 20000000));
	assert(!tub_coalesce_offer(&c, 140, 1000 + 30000000));
	assert(!tub_coalesce_flush(&c, 1000 + 40000000, &pct));
	assert(tub_coalesce_flush(&c, 1000 + TUB_COALESCE_INTERVAL_NS, &pct));
	assert(pct == 140);
	assert(!tub_coalesce_flush(&c, 1000 + 2 * TUB_COALESCE_INTERVAL_NS, &pct));
	/* a change after the interval goes at once and drops any pending value */
	assert(!tub_coalesce_offer(&c, 150, 1000 + TUB_COALESCE_INTERVAL_NS + 1));
	assert(tub_coalesce_offer(&c, 160, 1000 + 3 * TUB_COALESCE_INTERVAL_NS));
	assert(!tub_coalesce_flush(&c, 1000 + 9 * TUB_COALESCE_INTERVAL_NS, &pct));
	/* a clock that went backwards does not hold a change forever */
	assert(tub_coalesce_offer(&c, 170, 10));
	/* a Knob ramp: 160 changes 50 ms apart all go through, as the probes required */
	tub_coalesce_init(&c);
	uint64_t t = 0;
	int sent = 0;
	for (int i = 0; i < 160; i++, t += TUB_COALESCE_INTERVAL_NS) {
		if (tub_coalesce_offer(&c, 100 + i, t))
			sent++;
	}
	assert(sent == 160);
	printf("  test_coalesce: PASS\n");
}

int main(void)
{
	printf("test-tubester-core\n");
	test_path_usable();
	test_path_to_url();
	test_speed_range();
	test_speed_ladder();
	test_coalesce();
	printf("test-tubester-core: ALL PASS\n");
	return 0;
}
