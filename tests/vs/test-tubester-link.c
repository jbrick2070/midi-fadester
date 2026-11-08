/*
 * Tubester - link and command unit tests (REQUIREMENTS R-DCK-7, R-CTL-2, R-LNK-2, R-LNK-3)
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

#include "tubester/command.h"
#include "tubester/deck-link.h"

#include <util/bmem.h>
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define TOKEN "0123456789abcdef0123456789ABCDEF"
#define SEC 1000000000ULL

static obs_data_t *report(const char *token, const char *inc, int gen, int seq, const char *ev, const char *more)
{
	char body[1024];
	snprintf(body, sizeof(body), "{\"token\":\"%s\",\"inc\":\"%s\",\"gen\":%d,\"seq\":%d,\"ev\":\"%s\"%s%s}", token,
		 inc, gen, seq, ev, more && *more ? "," : "", more ? more : "");
	obs_data_t *d = NULL;
	bool ok = tub_link_parse_body(body, strlen(body), &d);
	assert(ok && d);
	return d;
}

static enum tub_report_verdict accept(struct tub_link *l, obs_data_t *d, uint64_t now)
{
	enum tub_report_verdict v = tub_link_accept(l, d, now);
	obs_data_release(d);
	return v;
}

static void test_init(void)
{
	struct tub_link l;
	assert(tub_link_init(&l, TOKEN));
	assert(!l.linked);
	assert(tub_link_state(&l, 0) == TUB_STATE_NO_CONNECTION);
	assert(!tub_link_init(&l, NULL));
	assert(!tub_link_init(&l, "short"));
	assert(!tub_link_init(&l, "0123456789abcdef0123456789abcdeg"));
	assert(!tub_link_init(&l, "0123456789abcdef0123456789abcdef0"));
	assert(tub_link_next_gen(&l) == 1);
	assert(tub_link_next_gen(&l) == 2);
	/* a link whose init failed accepts nothing, not even a report with no token at all */
	assert(!tub_link_init(&l, "short"));
	assert(accept(&l, report("", "p", 0, 1, "hello-ok", NULL), SEC) == TUB_REPORT_BAD_TOKEN);
	obs_data_t *d = obs_data_create_from_json("{\"inc\":\"p\",\"seq\":1,\"ev\":\"hello-ok\"}");
	assert(accept(&l, d, SEC) == TUB_REPORT_BAD_TOKEN);
	assert(!l.linked && l.bad_token_count == 2);
	printf("  test_init: PASS\n");
}

static void test_parse_body(void)
{
	obs_data_t *d = NULL;
	const char *good = "{\"token\":\"x\",\"inc\":\"abc\",\"seq\":1,\"ev\":\"tick\"}";
	assert(tub_link_parse_body(good, strlen(good), &d) && d);
	obs_data_release(d);
	/* not NUL-terminated at the length given: only the first object is read */
	const char *two = "{\"inc\":\"abc\",\"seq\":1,\"ev\":\"tick\"}garbage";
	assert(tub_link_parse_body(two, strlen(two) - 7, &d) && d);
	obs_data_release(d);
	assert(!tub_link_parse_body(two, strlen(two), &d) && !d);
	assert(!tub_link_parse_body("", 0, &d));
	assert(!tub_link_parse_body(NULL, 5, &d));
	assert(!tub_link_parse_body("[1,2]", 5, &d));
	assert(!tub_link_parse_body("42", 2, &d));
	assert(!tub_link_parse_body("{\"token\":\"x\"}", 13, &d));
	assert(!tub_link_parse_body("{\"inc\":", 7, &d));
	char *huge = bzalloc(70000);
	memset(huge, ' ', 69999);
	assert(!tub_link_parse_body(huge, 69999, &d));
	bfree(huge);
	/* anything that is not exactly one JSON object is refused before the libobs reader sees it, so nothing is logged */
	static const char *const bad[] = {
		"{xyz}",
		"{\"inc\":\"a\",}",
		"{\"inc\":01}",
		"{\"inc\":\"\\x\"}",
		"{\"inc\":\"a\"",
		"{\"inc\":\"a\"}}",
		"{\"inc\":tru}",
		"{\"inc\":\"a\" \"b\"}",
		"{\"inc\":1e400}",
		"{\"inc\":12345678901234567890}",
		"{\"inc\":\"\x01\"}",
		"{\"inc\":-}",
		"{\"inc\":1.}",
		"{\"inc\":\"\\u12g4\"}",
		"not json",
		"\"text\"",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		assert(!tub_link_parse_body(bad[i], strlen(bad[i]), &d) && !d);
	/* nor anything the libobs reader would refuse with a log line, or that no report has */
	static const char *const odd[] = {
		"{\"inc\":\"a\",\"inc\":\"b\"}",       /* a key twice (libobs reads with JSON_REJECT_DUPLICATES) */
		"{\"inc\":\"a\",\"\\u0069nc\":\"b\"}", /* an escaped key */
		"{\"inc\":\"a\",\"x\":{\"y\":1}}",     /* nested object */
		"{\"inc\":\"a\",\"x\":[1]}",           /* nested array */
		"{\"inc\":\"a\\u0000\"}",              /* \u0000 */
		"{\"inc\":\"a\\ud800\"}",              /* a lone surrogate escape */
		"{\"inc\":\"a\\udc00\\ud800\"}",
		"{\"inc\":\"\xC3\x28\"}",         /* invalid UTF-8 */
		"{\"inc\":\"\xC0\xAF\"}",         /* overlong */
		"{\"inc\":\"\xED\xA0\x80\"}",     /* an encoded surrogate */
		"{\"inc\":\"\xF4\x90\x80\x80\"}", /* past U+10FFFF */
		"{\"inc\":\"\xE2\x82\"}",         /* cut short */
	};
	for (size_t i = 0; i < sizeof(odd) / sizeof(odd[0]); i++)
		assert(!tub_link_parse_body(odd[i], strlen(odd[i]), &d) && !d);
	char nul_body[] = "{\"inc\":\"a\"}\0";
	assert(!tub_link_parse_body(nul_body, sizeof(nul_body) - 1, &d));
	char deep[128] = "{\"inc\":\"a\",\"x\":";
	for (int i = 0; i < 40; i++)
		strcat(deep, "[");
	assert(!tub_link_parse_body(deep, strlen(deep), &d));
	/* and every valid shape a page sends is still read */
	const char *ok =
		" {\"inc\":\"a\\u00e9\\n\xC3\xBC\xE6\x97\xA5\xF0\x9F\x8E\x9E\",\"seq\":1,\"ct\":-0.5e-3,\"rate\":1.25,"
		"\"paused\":true,\"x\":null,\"src\":\"it's 100% #1.mp4\"} ";
	assert(tub_link_parse_body(ok, strlen(ok), &d) && d);
	assert(obs_data_get_double(d, "rate") == 1.25);
	obs_data_release(d);
	printf("  test_parse_body: PASS\n");
}

static void test_report_read(void)
{
	struct tub_report r;
	obs_data_t *d = report(
		TOKEN, "page1", 3, 7, "playing",
		"\"ct\":12.5,\"dur\":122.56,\"rate\":1.5,\"rs\":4,\"vw\":1920,\"vh\":1080,\"paused\":false,\"src\":\"reel.mp4\"");
	assert(tub_report_read(d, &r));
	assert(r.duration == 122.56);
	assert(strcmp(r.incarnation, "page1") == 0 && r.gen == 3 && r.seq == 7);
	assert(strcmp(r.event, "playing") == 0 && strcmp(r.src, "reel.mp4") == 0);
	assert(r.current_time == 12.5 && r.rate == 1.5 && r.ready_state == 4 && r.video_width == 1920);
	assert(r.video_height == 1080 && !r.paused && !r.ended && !r.no_picture && r.error_code == 0);
	obs_data_release(d);
	/* missing fields and negative numbers are refused; long strings are cut, never overrun */
	/* a length the page could not know (a stream, a broken file) reads 0 */
	d = obs_data_create_from_json("{\"inc\":\"p\",\"seq\":1,\"ev\":\"tick\",\"dur\":-3}");
	assert(tub_report_read(d, &r) && r.duration == 0.0);
	obs_data_release(d);
	d = obs_data_create_from_json("{\"inc\":\"p\",\"seq\":1}");
	assert(!tub_report_read(d, &r));
	obs_data_release(d);
	d = obs_data_create_from_json("{\"inc\":\"\",\"seq\":1,\"ev\":\"tick\"}");
	assert(!tub_report_read(d, &r));
	obs_data_release(d);
	d = obs_data_create_from_json("{\"inc\":\"p\",\"seq\":-1,\"ev\":\"tick\"}");
	assert(!tub_report_read(d, &r));
	obs_data_release(d);
	d = obs_data_create_from_json("{\"inc\":\"p\",\"seq\":1,\"gen\":-4,\"ev\":\"tick\"}");
	assert(!tub_report_read(d, &r));
	obs_data_release(d);
	char body[2048];
	snprintf(body, sizeof(body), "{\"inc\":\"%0400d\",\"seq\":1,\"ev\":\"%0100d\",\"src\":\"%0600d\"}", 1, 2, 3);
	d = obs_data_create_from_json(body);
	assert(tub_report_read(d, &r));
	assert(strlen(r.incarnation) == TUB_INCARNATION_MAX - 1);
	assert(strlen(r.event) == 15);
	assert(strlen(r.src) == TUB_SRC_MAX - 1);
	obs_data_release(d);
	assert(!tub_report_read(NULL, &r));
	printf("  test_report_read: PASS\n");
}

static void test_accept_ordering(void)
{
	struct tub_link l;
	assert(tub_link_init(&l, TOKEN));
	uint64_t gen = tub_link_next_gen(&l);
	assert(gen == 1);

	/* the first report links the page */
	assert(accept(&l, report(TOKEN, "page1", 1, 1, "hello-ok", NULL), 10 * SEC) == TUB_REPORT_NEW_PAGE);
	assert(l.linked && l.seq == 1);
	assert(accept(&l, report(TOKEN, "page1", 1, 2, "playing", "\"rs\":4,\"vw\":1280"), 11 * SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, 11 * SEC) == TUB_STATE_PLAYING);

	/* out of order within the page: ignored, state unchanged */
	assert(accept(&l, report(TOKEN, "page1", 1, 2, "pause", "\"paused\":true"), 12 * SEC) == TUB_REPORT_STALE);
	assert(accept(&l, report(TOKEN, "page1", 1, 1, "pause", "\"paused\":true"), 12 * SEC) == TUB_REPORT_STALE);
	assert(tub_link_state(&l, 12 * SEC) == TUB_STATE_PLAYING);

	/* a wrong token is dropped and counted, whatever else it says */
	assert(accept(&l, report("0123456789abcdef0123456789abcdee", "page1", 1, 9, "error", "\"err\":4"), 12 * SEC) ==
	       TUB_REPORT_BAD_TOKEN);
	assert(accept(&l, report("", "page1", 1, 9, "error", "\"err\":4"), 12 * SEC) == TUB_REPORT_BAD_TOKEN);
	assert(l.bad_token_count == 2 && l.seq == 2);
	assert(tub_link_state(&l, 12 * SEC) == TUB_STATE_PLAYING);

	/* a newer tape was commanded: reports from the old tape keep the link alive but do not set state */
	gen = tub_link_next_gen(&l);
	assert(gen == 2);
	assert(accept(&l, report(TOKEN, "page1", 1, 3, "ended", "\"ended\":true"), 13 * SEC) ==
	       TUB_REPORT_OLD_GENERATION);
	assert(l.seq == 3 && l.last_report_ns == 13 * SEC);
	assert(tub_link_state(&l, 13 * SEC) == TUB_STATE_READY);
	assert(accept(&l, report(TOKEN, "page1", 2, 4, "playing", "\"rs\":4,\"vw\":1280"), 14 * SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, 14 * SEC) == TUB_STATE_PLAYING);
	/* a report from a generation ahead of ours on the same page is not stale and does not move ours */
	assert(accept(&l, report(TOKEN, "page1", 3, 5, "playing", "\"rs\":4,\"vw\":1280"), 14 * SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(l.gen == 2);

	/* the page reloaded: a new incarnation restarts the sequence */
	assert(accept(&l, report(TOKEN, "page2", 0, 1, "hello-ok", NULL), 15 * SEC) == TUB_REPORT_NEW_PAGE);
	assert(strcmp(l.incarnation, "page2") == 0 && l.seq == 1);
	assert(tub_link_state(&l, 15 * SEC) == TUB_STATE_READY);
	/* late posts from the page before the reload are stale, so the tape is not resent twice */
	assert(accept(&l, report(TOKEN, "page1", 2, 99, "playing", NULL), 15 * SEC) == TUB_REPORT_STALE);
	assert(accept(&l, report(TOKEN, "page1", 2, 98, "playing", NULL), 15 * SEC) == TUB_REPORT_STALE);
	assert(strcmp(l.incarnation, "page2") == 0 && l.seq == 1);
	/* a third page is new again, and then page2 is retired too */
	assert(accept(&l, report(TOKEN, "page3", 0, 1, "hello-ok", NULL), 16 * SEC) == TUB_REPORT_NEW_PAGE);
	assert(accept(&l, report(TOKEN, "page2", 0, 2, "tick", NULL), 16 * SEC) == TUB_REPORT_STALE);
	/* a late report from the first page, two reloads ago, cannot take the link back from the live page3 */
	assert(accept(&l, report(TOKEN, "page1", 2, 100, "tick", NULL), 16 * SEC) == TUB_REPORT_STALE);
	assert(strcmp(l.incarnation, "page3") == 0);
	assert(accept(&l, report(TOKEN, "page3", 2, 2, "tick", NULL), 16 * SEC) == TUB_REPORT_ACCEPTED);

	/* malformed reports change nothing */
	obs_data_t *d = obs_data_create_from_json("{\"token\":\"" TOKEN "\",\"inc\":\"page1\"}");
	assert(accept(&l, d, 17 * SEC) == TUB_REPORT_MALFORMED);
	assert(tub_link_accept(&l, NULL, 17 * SEC) == TUB_REPORT_MALFORMED);
	assert(l.seq == 2);
	printf("  test_accept_ordering: PASS\n");
}

static void test_page_outlives_filter(void)
{
	/* OBS restarted or the filter was recreated while the page kept running on tape generation 5 */
	struct tub_link l;
	assert(tub_link_init(&l, TOKEN));
	assert(accept(&l, report(TOKEN, "old-page", 5, 400, "playing", "\"rs\":4,\"vw\":1280"), SEC) ==
	       TUB_REPORT_NEW_PAGE);
	assert(l.gen == 5);
	/* the next tape command carries 6, which the page will not ignore as old */
	assert(tub_link_next_gen(&l) == 6);
	assert(accept(&l, report(TOKEN, "old-page", 5, 401, "playing", "\"rs\":4,\"vw\":1280"), 2 * SEC) ==
	       TUB_REPORT_OLD_GENERATION);
	assert(accept(&l, report(TOKEN, "old-page", 6, 402, "playing", "\"rs\":4,\"vw\":1280"), 3 * SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, 3 * SEC) == TUB_STATE_PLAYING);
	printf("  test_page_outlives_filter: PASS\n");
}

static void test_retired_pages_and_self_heal(void)
{
	struct tub_link l;
	assert(tub_link_init(&l, TOKEN));
	assert(accept(&l, report(TOKEN, "A", 0, 1, "hello", NULL), 1 * SEC) == TUB_REPORT_NEW_PAGE);
	assert(accept(&l, report(TOKEN, "B", 0, 1, "hello", NULL), 2 * SEC) == TUB_REPORT_NEW_PAGE);
	assert(accept(&l, report(TOKEN, "C", 0, 1, "hello", NULL), 3 * SEC) == TUB_REPORT_NEW_PAGE);
	/* reloads A, B, C, then a delayed report from A: it must not make A current and lock out the live C */
	assert(accept(&l, report(TOKEN, "A", 0, 50, "tick", NULL), 3 * SEC + 100) == TUB_REPORT_STALE);
	assert(accept(&l, report(TOKEN, "B", 0, 50, "tick", NULL), 3 * SEC + 200) == TUB_REPORT_STALE);
	assert(strcmp(l.incarnation, "C") == 0);
	assert(accept(&l, report(TOKEN, "C", 0, 2, "tick", NULL), 3 * SEC + 300) == TUB_REPORT_ACCEPTED);

	/* sixteen more reloads push A and B out of the ring of retired pages */
	char inc[8];
	uint64_t t = 4 * SEC;
	for (int i = 0; i < TUB_RETIRED_MAX; i++) {
		snprintf(inc, sizeof(inc), "R%d", i);
		assert(accept(&l, report(TOKEN, inc, 0, 1, "hello", NULL), t) == TUB_REPORT_NEW_PAGE);
		t += SEC / 10;
	}
	snprintf(inc, sizeof(inc), "R%d", TUB_RETIRED_MAX - 1);
	/* a report from an evicted page looks new and takes the link... */
	assert(accept(&l, report(TOKEN, "A", 0, 60, "tick", NULL), t) == TUB_REPORT_NEW_PAGE);
	/* ...the live page is refused while that report is fresh... */
	assert(accept(&l, report(TOKEN, inc, 0, 2, "tick", NULL), t + SEC) == TUB_REPORT_STALE);
	assert(strcmp(l.incarnation, "A") == 0);
	/* ...and wins the link back once nothing has been accepted for longer than the timeout: it heals by itself */
	assert(accept(&l, report(TOKEN, inc, 0, 3, "tick", NULL), t + TUB_LINK_TIMEOUT_NS + 1) == TUB_REPORT_NEW_PAGE);
	assert(strcmp(l.incarnation, inc) == 0);
	/* and A, retired again, is stale while that page lives */
	assert(accept(&l, report(TOKEN, "A", 0, 61, "tick", NULL), t + TUB_LINK_TIMEOUT_NS + 2) == TUB_REPORT_STALE);
	printf("  test_retired_pages_and_self_heal: PASS\n");
}

static void test_states(void)
{
	struct tub_link l;
	assert(tub_link_init(&l, TOKEN));
	tub_link_next_gen(&l);
	assert(accept(&l, report(TOKEN, "p", 1, 1, "hello-ok", NULL), SEC) == TUB_REPORT_NEW_PAGE);
	assert(tub_link_state(&l, SEC) == TUB_STATE_READY);
	assert(accept(&l, report(TOKEN, "p", 1, 2, "loadedmetadata", "\"rs\":1,\"vw\":640,\"paused\":true"), SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_PAUSED);
	assert(accept(&l, report(TOKEN, "p", 1, 3, "playing", "\"rs\":4,\"vw\":640"), SEC) == TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_PLAYING);
	assert(accept(&l, report(TOKEN, "p", 1, 4, "ended", "\"rs\":4,\"vw\":640,\"ended\":true,\"paused\":true"),
		      SEC) == TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_ENDED);
	/* sound with no picture: MPEG-4 Part 2, Theora and ProRes report playing with a zero width (F-4) */
	assert(accept(&l, report(TOKEN, "p", 1, 5, "playing", "\"rs\":4,\"vw\":0,\"vh\":0"), SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_NO_PICTURE);
	assert(accept(&l, report(TOKEN, "p", 1, 6, "nopicture", "\"nopic\":true,\"rs\":1"), SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_NO_PICTURE);
	/* an error wins over everything, by event or by code */
	assert(accept(&l, report(TOKEN, "p", 1, 7, "error", "\"err\":4"), SEC) == TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_CANNOT_PLAY);
	assert(accept(&l, report(TOKEN, "p", 1, 8, "tick", "\"err\":4,\"rs\":4,\"vw\":640"), SEC) ==
	       TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, SEC) == TUB_STATE_CANNOT_PLAY);
	/* a tick with nothing loaded yet is Ready */
	assert(accept(&l, report(TOKEN, "p", 1, 9, "tick", "\"rs\":0,\"vw\":0"), 2 * SEC) == TUB_REPORT_ACCEPTED);
	assert(tub_link_state(&l, 2 * SEC) == TUB_STATE_READY);
	/* silence for longer than the timeout is No connection, and a clock that went backwards is too */
	assert(tub_link_state(&l, 2 * SEC + TUB_LINK_TIMEOUT_NS) == TUB_STATE_READY);
	assert(tub_link_state(&l, 2 * SEC + TUB_LINK_TIMEOUT_NS + 1) == TUB_STATE_NO_CONNECTION);
	assert(tub_link_state(&l, SEC) == TUB_STATE_NO_CONNECTION);
	assert(strcmp(tub_deck_state_name(TUB_STATE_NO_PICTURE), "No picture in this file") == 0);
	assert(strcmp(tub_deck_state_name(TUB_STATE_NO_CONNECTION), "No connection") == 0);
	assert(strcmp(tub_deck_state_name((enum tub_deck_state)99), "Unknown") == 0);
	printf("  test_states: PASS\n");
}

static void check_json(char *json, const char *expected)
{
	assert(json);
	assert(tub_command_is_safe(json));
	assert(strcmp(json, expected) == 0);
	bfree(json);
}

static void test_commands(void)
{
	check_json(tub_command_hello("deck-a_1", 4477, TOKEN),
		   "{\"hello\":true,\"deck\":\"deck-a_1\",\"port\":4477,\"token\":\"" TOKEN "\"}");
	assert(!tub_command_hello("deck a", 4477, TOKEN));
	assert(!tub_command_hello("", 4477, TOKEN));
	assert(!tub_command_hello("deck\"a", 4477, TOKEN));
	assert(!tub_command_hello("deck", 0, TOKEN));
	assert(!tub_command_hello("deck", 65536, TOKEN));
	assert(!tub_command_hello("deck", 4477, "short"));
	assert(!tub_command_hello("deck", 4477, NULL));
	assert(!tub_command_hello("deck", 4477, "0123456789abcdef0123456789abcde\""));

	check_json(tub_command_tape(7, "file:///C:/tapes/reel.mp4", 0, true, true),
		   "{\"gen\":7,\"tape\":\"file:///C:/tapes/reel.mp4\",\"at\":0.0,\"loop\":true,\"playing\":true}");
	/* a paused deck's tape loads paused (R-CTL-4) */
	check_json(tub_command_tape(1, "file:///C:/it%27s.mp4", 12.25, false, false),
		   "{\"gen\":1,\"tape\":\"file:///C:/it%27s.mp4\",\"at\":12.25,\"loop\":false,\"playing\":false}");
	assert(!tub_command_tape(0, "file:///C:/a.mp4", 0, true, true));
	assert(!tub_command_tape(1, "C:\\a.mp4", 0, true, true));
	assert(!tub_command_tape(1, "http://evil/a.mp4", 0, true, true));
	assert(!tub_command_tape(1, "file://", 0, true, true));
	assert(!tub_command_tape(1, "file:///", 0, true, true));
	assert(!tub_command_tape(1, "file:///C:", 0, true, true));
	assert(!tub_command_tape(1, "file:///C:/", 0, true, true));
	assert(!tub_command_tape(1, "file:///tapes/a.mp4", 0, true, true));
	assert(!tub_command_tape(1, "file://nas", 0, true, true));
	assert(!tub_command_tape(1, "file://nas/", 0, true, true));
	check_json(tub_command_tape(2, "file://nas/share/a.mp4", 0, true, true),
		   "{\"gen\":2,\"tape\":\"file://nas/share/a.mp4\",\"at\":0.0,\"loop\":true,\"playing\":true}");
	char *odd = tub_command_rate(1.1, true);
	assert(odd && strncmp(odd, "{\"rate\":1.1", 11) == 0);
	bfree(odd);
	assert(!tub_command_tape(1, NULL, 0, true, true));
	assert(!tub_command_tape(1, "file:///C:/a.mp4", -1, true, true));
	assert(!tub_command_tape(1, "file:///C:/a.mp4", INFINITY, true, true));

	check_json(tub_command_rate(1.5, true), "{\"rate\":1.5,\"keepPitch\":false}");
	check_json(tub_command_rate(0.25, false), "{\"rate\":0.25,\"keepPitch\":true}");
	check_json(tub_command_rate(8.0, true), "{\"rate\":8.0,\"keepPitch\":false}");
	assert(!tub_command_rate(0.2, true));
	assert(!tub_command_rate(16.0, true));
	assert(!tub_command_rate(NAN, true));

	check_json(tub_command_seek(61.5), "{\"seek\":61.5}");
	check_json(tub_command_seek(0), "{\"seek\":0.0}");
	assert(!tub_command_seek(-0.5));
	/* play carries the start position, where an ended tape starts again (D-11) */
	check_json(tub_command_play(0), "{\"play\":true,\"start\":0.0}");
	check_json(tub_command_play(42.5), "{\"play\":true,\"start\":42.5}");
	assert(!tub_command_play(-1));
	assert(!tub_command_play(NAN));
	assert(!tub_command_play(INFINITY));
	check_json(tub_command_pause(), "{\"pause\":true}");
	check_json(tub_command_loop(true), "{\"loop\":true}");
	check_json(tub_command_loop(false), "{\"loop\":false}");
	check_json(tub_command_eject(7), "{\"gen\":7,\"eject\":true}");
	assert(!tub_command_eject(0));
	printf("  test_commands: PASS\n");
}

static void test_command_safety(void)
{
	assert(tub_command_is_safe("{\"a\":\"line\\nbreak\"}"));
	assert(!tub_command_is_safe("{\"a\":\"line\nbreak\"}"));
	assert(!tub_command_is_safe("{\"a\":\"line\rbreak\"}"));
	assert(!tub_command_is_safe("{\"a\":\"\xE2\x80\xA8\"}"));
	assert(!tub_command_is_safe("{\"a\":\"\xE2\x80\xA9\"}"));
	assert(tub_command_is_safe("{\"a\":\"\xE2\x80\xA7\"}"));
	assert(!tub_command_is_safe("[1]"));
	assert(!tub_command_is_safe("{"));
	assert(!tub_command_is_safe(""));
	assert(!tub_command_is_safe(NULL));
	/* a tape address carrying a raw line separator is refused as a command, even though it is valid JSON */
	assert(!tub_command_tape(1, "file:///C:/a\xE2\x80\xA8.mp4", 0, true, true));
	/* a quote in the address is escaped by the writer, so it cannot end the string early */
	char *json = tub_command_tape(1, "file:///C:/a\"b.mp4", 0, true, true);
	assert(json && strstr(json, "a\\\"b") != NULL);
	bfree(json);
	printf("  test_command_safety: PASS\n");
}

int main(void)
{
	printf("test-tubester-link\n");
	test_init();
	test_parse_body();
	test_report_read();
	test_accept_ordering();
	test_page_outlives_filter();
	test_retired_pages_and_self_heal();
	test_states();
	test_commands();
	test_command_safety();
	printf("test-tubester-link: ALL PASS\n");
	return 0;
}
