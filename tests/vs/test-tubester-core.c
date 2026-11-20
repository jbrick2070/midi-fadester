/*
 * Tubester - tape path and speed unit tests (REQUIREMENTS R-TAP-3, R-SPD-1, R-SPD-3, R-SPD-4, R-SPD-5)
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Windows rules, checked on every platform */
#define win_usable(path) tub_path_is_usable_as((path), TUB_PATH_STYLE_WINDOWS)
#define win_url(path, out, size) tub_path_to_file_url_as((path), TUB_PATH_STYLE_WINDOWS, (out), (size))
/* The POSIX (macOS) rules, checked on every platform */
#define posix_usable(path) tub_path_is_usable_as((path), TUB_PATH_STYLE_POSIX)
#define posix_url(path, out, size) tub_path_to_file_url_as((path), TUB_PATH_STYLE_POSIX, (out), (size))

static void test_posix_path_usable(void)
{
	assert(posix_usable("/Users/me/Movies/reel.mp4"));
	assert(posix_usable("/Volumes/NAS share/reel.mp4"));
	assert(posix_usable("/Users/me/it's 100% #1 & \xC3\xBC\xC3\xAF \xE6\x97\xA5\xE6\x9C\xAC.mp4"));
	assert(posix_usable("/a"));
	assert(posix_usable("/tapes/back\\slash.mp4")); /* a backslash is part of a name */
	assert(!posix_usable(NULL));
	assert(!posix_usable(""));
	assert(!posix_usable("/"));
	assert(!posix_usable("///"));
	assert(!posix_usable("reel.mp4"));
	assert(!posix_usable("tapes/reel.mp4"));
	assert(!posix_usable("~/reel.mp4"));
	assert(!posix_usable("C:\\tapes\\reel.mp4"));
	assert(!posix_usable("\\\\nas\\share\\reel.mp4"));
	assert(!posix_usable("/tapes/re\nel.mp4"));
	assert(!posix_usable("/tapes/re\x7f"
			     "el.mp4"));
	/* ? * < > | " cannot be in a Windows name but are legal in a Mac one (E-05) */
	assert(posix_usable("/tapes/re*el.mp4"));
	assert(posix_usable("/tapes/re\"el.mp4"));
	assert(posix_usable("/Users/me/Movies/Why?.mp4"));
	assert(posix_usable("/Users/me/<a|b>.mp4"));
	assert(!win_usable("C:\\tapes\\re*el.mp4"));
	assert(!win_usable("C:\\tapes\\re\"el.mp4"));
	assert(!win_usable("C:\\tapes\\Why?.mp4"));
	{
		char url[128];
		assert(posix_url("/tapes/Why?.mp4", url, sizeof(url)) > 0);
		assert(strcmp(url, "file:///tapes/Why%3F.mp4") == 0);
	}
	char *too_long = malloc(TUB_PATH_MAX + 2);
	memset(too_long, 'a', TUB_PATH_MAX + 1);
	too_long[0] = '/';
	too_long[TUB_PATH_MAX + 1] = '\0';
	assert(!posix_usable(too_long));
	too_long[TUB_PATH_MAX] = '\0';
	assert(posix_usable(too_long));
	free(too_long);
	printf("  test_posix_path_usable: PASS\n");
}

static void test_posix_path_to_url(void)
{
	char out[512];
	assert(posix_url("/Users/me/reel.mp4", out, sizeof(out)) == strlen("file:///Users/me/reel.mp4"));
	assert(strcmp(out, "file:///Users/me/reel.mp4") == 0);
	assert(posix_url("/Users/me/it's 100% #1 & \xC3\xBC.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///Users/me/it%27s%20100%25%20%231%20%26%20%C3%BC.mp4") == 0);
	/* the plugin's own page inside the bundle: spaces in Application Support */
	assert(posix_url("/Library/Application Support/obs-studio/plugins/visual-sidechain.plugin/Contents/Resources/"
			 "deck.html",
			 out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///Library/Application%20Support/obs-studio/plugins/visual-sidechain.plugin/Contents/"
			   "Resources/deck.html") == 0);
	/* a colon and a backslash are characters of a name, not separators */
	assert(posix_url("/a:b\\c.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///a%3Ab%5Cc.mp4") == 0);
	/* doubled slashes are written once */
	assert(posix_url("//Users//me///reel.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///Users/me/reel.mp4") == 0);
	assert(posix_url("reel.mp4", out, sizeof(out)) == 0 && out[0] == '\0');
	for (size_t size = 1; size <= strlen("file:///a"); size++) {
		memset(out, 'x', sizeof(out));
		assert(posix_url("/a", out, size) == 0 && out[0] == '\0');
	}
	assert(posix_url("/a", out, strlen("file:///a") + 1) == strlen("file:///a"));
	printf("  test_posix_path_to_url: PASS\n");
}

/* The plain functions follow the system the plugin runs on */
static void test_host_path_style(void)
{
	char out[64];
#ifdef _WIN32
	assert(TUB_PATH_STYLE_HOST == TUB_PATH_STYLE_WINDOWS);
	assert(tub_path_is_usable("C:\\tapes\\reel.mp4") && !tub_path_is_usable("/tapes/reel.mp4"));
	assert(tub_path_to_file_url("C:\\a.mp4", out, sizeof(out)) > 0 && strcmp(out, "file:///C:/a.mp4") == 0);
#else
	assert(TUB_PATH_STYLE_HOST == TUB_PATH_STYLE_POSIX);
	assert(tub_path_is_usable("/tapes/reel.mp4") && !tub_path_is_usable("C:\\tapes\\reel.mp4"));
	assert(tub_path_to_file_url("/a.mp4", out, sizeof(out)) > 0 && strcmp(out, "file:///a.mp4") == 0);
#endif
	printf("  test_host_path_style: PASS\n");
}

static void test_path_usable(void)
{
	assert(win_usable("C:\\tapes\\reel.mp4"));
	assert(win_usable("c:/tapes/reel.mp4"));
	assert(win_usable("\\\\nas\\share\\reel.mp4"));
	assert(win_usable("D:\\it's 100% #1 & \xC3\xBC\xC3\xAF \xE6\x97\xA5\xE6\x9C\xAC.mp4"));
	assert(!win_usable(NULL));
	assert(!win_usable(""));
	assert(!win_usable("C:"));
	/* a drive root alone names no file */
	assert(!win_usable("C:\\"));
	assert(!win_usable("C:/"));
	assert(!win_usable("C:\\\\/"));
	assert(win_usable("C:\\a"));
	assert(!win_usable("reel.mp4"));
	assert(!win_usable("\\reel.mp4"));
	assert(!win_usable("/tapes/reel.mp4"));
	assert(!win_usable("C:reel.mp4"));
	assert(!win_usable("\\\\\\reel.mp4"));
	assert(!win_usable("C:\\tapes\\re*el.mp4"));
	assert(!win_usable("C:\\tapes\\re?el.mp4"));
	assert(!win_usable("C:\\tapes\\re\nel.mp4"));
	assert(!win_usable("C:\\tapes\\re\"el.mp4"));
	assert(!win_usable("C:\\tapes\\re<el.mp4"));
	assert(!win_usable("C:\\tapes\\re>el.mp4"));
	assert(!win_usable("C:\\tapes\\re|el.mp4"));
	assert(!win_usable("C:\\tapes\\re\x7f"
			   "el.mp4"));
	/* UNC needs a server, a share and a path; device forms are not shares */
	assert(win_usable("//nas/share/reel.mp4"));
	assert(!win_usable("\\\\nas"));
	assert(!win_usable("\\\\nas\\"));
	assert(!win_usable("\\\\nas\\\\reel.mp4"));
	assert(!win_usable("\\\\.\\C:\\reel.mp4"));
	assert(!win_usable("\\\\?\\C:\\reel.mp4"));
	char *too_long = malloc(TUB_PATH_MAX + 2);
	memset(too_long, 'a', TUB_PATH_MAX + 1);
	memcpy(too_long, "C:\\", 3);
	too_long[TUB_PATH_MAX + 1] = '\0';
	assert(!win_usable(too_long));
	too_long[TUB_PATH_MAX] = '\0';
	assert(win_usable(too_long));
	free(too_long);
	printf("  test_path_usable: PASS\n");
}

static void test_path_to_url(void)
{
	char out[512];
	assert(win_url("C:\\tapes\\reel.mp4", out, sizeof(out)) == strlen("file:///C:/tapes/reel.mp4"));
	assert(strcmp(out, "file:///C:/tapes/reel.mp4") == 0);
	assert(win_url("c:/tapes/reel.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///c:/tapes/reel.mp4") == 0);
	/* the characters the probes proved: space, apostrophe, percent, hash, ampersand, umlauts, kanji */
	assert(win_url("D:\\it's 100% #1 & \xC3\xBC.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///D:/it%27s%20100%25%20%231%20%26%20%C3%BC.mp4") == 0);
	assert(win_url("E:\\\xE6\x97\xA5\xE6\x9C\xAC\\a.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///E:/%E6%97%A5%E6%9C%AC/a.mp4") == 0);
	/* a second colon is not a drive separator */
	assert(win_url("C:\\a:b.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file:///C:/a%3Ab.mp4") == 0);
	assert(win_url("\\\\nas\\share\\reel.mp4", out, sizeof(out)) > 0);
	assert(strcmp(out, "file://nas/share/reel.mp4") == 0);
	/* unusable paths and small buffers give an empty string and 0 */
	assert(win_url("reel.mp4", out, sizeof(out)) == 0 && out[0] == '\0');
	assert(win_url("C:\\tapes\\reel.mp4", out, 10) == 0 && out[0] == '\0');
	for (size_t size = 1; size <= 9; size++) {
		memset(out, 'x', sizeof(out));
		assert(win_url("C:\\tapes\\reel.mp4", out, size) == 0 && out[0] == '\0');
		memset(out, 'x', sizeof(out));
		assert(win_url("\\\\nas\\share\\reel.mp4", out, size) == 0 && out[0] == '\0');
	}
	assert(win_url("C:\\tapes\\reel.mp4", out, strlen("file:///C:/tapes/reel.mp4")) == 0);
	assert(win_url("C:\\tapes\\reel.mp4", out, strlen("file:///C:/tapes/reel.mp4") + 1) > 0);
	assert(win_url("C:\\tapes\\reel.mp4", NULL, 0) == 0);
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
	/* a stored setting is clamped before it is narrowed: 4294967396 would wrap to 100 as an int */
	assert(tub_speed_pct_from_setting(150) == 150);
	assert(tub_speed_pct_from_setting(4294967396LL) == 800);
	assert(tub_speed_pct_from_setting(-4294967196LL) == 25);
	assert(tub_speed_pct_from_setting(9223372036854775807LL) == 800);
	assert(tub_speed_pct_from_setting(-9223372036854775807LL - 1) == 25);
	assert(tub_speed_pct_from_setting(0) == 25);
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

	/* signed multi-step ladder (P1-6) */
	assert(tub_speed_step_n_pct(100, 2) == 150);
	assert(tub_speed_step_n_pct(150, -3) == 75);
	assert(tub_speed_step_n_pct(100, 50) == 800);
	assert(tub_speed_step_n_pct(100, -50) == 25);
	assert(tub_speed_step_n_pct(137, 0) == 137);

	/* integer snapping and bounds check for speed_set (P1-6) */
	int snapped = 0;
	assert(tub_speed_snap_pct(25.0, &snapped) && snapped == 25);
	assert(tub_speed_snap_pct(800.0, &snapped) && snapped == 800);
	assert(tub_speed_snap_pct(133.6, &snapped) && snapped == 134);
	assert(tub_speed_snap_pct(133.4, &snapped) && snapped == 133);
	assert(!tub_speed_snap_pct(24.9, &snapped));
	assert(!tub_speed_snap_pct(800.1, &snapped));
	assert(!tub_speed_snap_pct(NAN, &snapped));
	assert(!tub_speed_snap_pct(INFINITY, &snapped));
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
	/* a forced send counts as the interval's send and replaces a pending value */
	tub_coalesce_init(&c);
	assert(tub_coalesce_offer(&c, 120, 1000));
	assert(!tub_coalesce_offer(&c, 130, 1000 + 10000000));
	tub_coalesce_note_sent(&c, 1000 + 20000000);
	assert(!tub_coalesce_flush(&c, 1000 + 20000000 + TUB_COALESCE_INTERVAL_NS, &pct));
	assert(!tub_coalesce_offer(&c, 140, 1000 + 30000000));
	assert(!tub_coalesce_flush(&c, 1000 + 20000000 + TUB_COALESCE_INTERVAL_NS - 1, &pct));
	assert(tub_coalesce_flush(&c, 1000 + 20000000 + TUB_COALESCE_INTERVAL_NS, &pct));
	assert(pct == 140);
	/* before any offer, a forced send still starts the interval */
	tub_coalesce_init(&c);
	tub_coalesce_note_sent(&c, 5000);
	assert(!tub_coalesce_offer(&c, 100, 5000 + 1));
	printf("  test_coalesce: PASS\n");
}

static void test_speed_from_fader(void)
{
	/* the ends, and the middle of a ratio scale (R-SPD-5) */
	assert(tub_speed_from_fader(0.0f, 50, 200) == 50);
	assert(tub_speed_from_fader(1.0f, 50, 200) == 200);
	assert(tub_speed_from_fader(0.5f, 50, 200) == 100);
	assert(tub_speed_from_fader(0.25f, 50, 200) == 71);
	assert(tub_speed_from_fader(0.5f, 25, 800) == 141);
	assert(tub_speed_from_fader(0.75f, 25, 800) == 336);
	/* min above max turns the fader round */
	assert(tub_speed_from_fader(0.0f, 200, 50) == 200);
	assert(tub_speed_from_fader(1.0f, 200, 50) == 50);
	assert(tub_speed_from_fader(0.5f, 200, 50) == 100);
	/* equal ends give one speed */
	assert(tub_speed_from_fader(0.3f, 100, 100) == 100);
	/* ends outside the speed range are clamped first */
	assert(tub_speed_from_fader(0.0f, 10, 2000) == 25);
	assert(tub_speed_from_fader(1.0f, 10, 2000) == 800);
	assert(tub_speed_from_fader(0.5f, 10, 2000) == 141);
	assert(tub_speed_from_fader(0.5f, -40, 0) == 25);
	/* a value outside 0 to 1 counts as the nearer end; not a number counts as 0 */
	assert(tub_speed_from_fader(-1.0f, 50, 200) == 50);
	assert(tub_speed_from_fader(2.0f, 50, 200) == 200);
	assert(tub_speed_from_fader(NAN, 50, 200) == 50);
	assert(tub_speed_from_fader(INFINITY, 50, 200) == 200);
	assert(tub_speed_from_fader(-INFINITY, 50, 200) == 50);
	/* the whole travel never steps backwards and stays in range */
	int last = 0;
	for (int i = 0; i <= 1000; i++) {
		int pct = tub_speed_from_fader((float)i / 1000.0f, 25, 800);
		assert(pct >= last && pct >= 25 && pct <= 800);
		last = pct;
	}
	assert(last == 800);
	printf("  test_speed_from_fader: PASS\n");
}

int main(void)
{
	printf("test-tubester-core\n");
	test_path_usable();
	test_path_to_url();
	test_posix_path_usable();
	test_posix_path_to_url();
	test_host_path_style();
	test_speed_range();
	test_speed_ladder();
	test_coalesce();
	test_speed_from_fader();
	printf("test-tubester-core: ALL PASS\n");
	return 0;
}
