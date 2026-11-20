/*
 * Midi Fadester — test-mfd-text
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/mfd-text.h"
#include "core/slot-messages.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(cond)                                                                   \
	do {                                                                            \
		if (!(cond)) {                                                          \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			return 1;                                                       \
		}                                                                       \
	} while (0)

static const char *german(const char *key)
{
	if (strcmp(key, "Greeting") == 0) {
		return "Hallo %1, %2 Dateien";
	}
	if (strcmp(key, "Empty") == 0) {
		return "";
	}
	return NULL;
}

static int expect_fill(const char *text, const char *const *values, size_t count, const char *expected)
{
	char out[128];
	memset(out, 'x', sizeof(out));
	size_t len = mfd_text_fill(out, sizeof(out), text, values, count);
	if (strcmp(out, expected) != 0 || len != strlen(expected)) {
		fprintf(stderr, "fill '%s' gave '%s', expected '%s'\n", text ? text : "(null)", out, expected);
		return 1;
	}
	return 0;
}

int main(void)
{
	/* lookup and English fallback */
	REQUIRE(strcmp(mfd_text("Greeting", "Hello %1"), "Hello %1") == 0);
	mfd_text_set_lookup(german);
	REQUIRE(strcmp(mfd_text("Greeting", "Hello %1"), "Hallo %1, %2 Dateien") == 0);
	REQUIRE(strcmp(mfd_text("Missing", "fallback"), "fallback") == 0);
	REQUIRE(strcmp(mfd_text("Empty", "fallback"), "fallback") == 0);
	REQUIRE(strcmp(mfd_text(NULL, "no key"), "no key") == 0);
	REQUIRE(strcmp(mfd_text_or_english(true, "Greeting", "Hello %1"), "Hallo %1, %2 Dateien") == 0);
	REQUIRE(strcmp(mfd_text_or_english(false, "Greeting", "Hello %1"), "Hello %1") == 0);
	mfd_text_set_lookup(NULL);
	REQUIRE(strcmp(mfd_text("Greeting", "Hello %1"), "Hello %1") == 0);

	/* placeholders: in order, out of order, repeated, missing, %10, other % kept */
	const char *two[] = {"Ada", "3"};
	REQUIRE(expect_fill("Hallo %1, %2 Dateien", two, 2U, "Hallo Ada, 3 Dateien") == 0);
	REQUIRE(expect_fill("%2 then %1 and %1", two, 2U, "3 then Ada and Ada") == 0);
	REQUIRE(expect_fill("only %1 and %3", two, 2U, "only Ada and ") == 0);
	REQUIRE(expect_fill("%9", two, 2U, "") == 0);
	const char *none[] = {NULL};
	REQUIRE(expect_fill("a %1 b", none, 1U, "a  b") == 0);
	REQUIRE(expect_fill("a %1 b", NULL, 0U, "a  b") == 0);
	REQUIRE(expect_fill("%10", two, 2U, "Ada0") == 0);
	REQUIRE(expect_fill("100% sure, %s %d %%", two, 2U, "100% sure, %s %d %%") == 0);
	REQUIRE(expect_fill("ends with %", two, 2U, "ends with %") == 0);
	REQUIRE(expect_fill("", two, 2U, "") == 0);
	REQUIRE(expect_fill(NULL, two, 2U, "") == 0);

	/* a value holding a placeholder is never filled again (single pass) */
	const char *tricky[] = {"Scene %2", "Cam %1"};
	REQUIRE(expect_fill("%1 / %2", tricky, 2U, "Scene %2 / Cam %1") == 0);

	/* UTF-8 passes through */
	const char *word[] = {"\xE5\x90\x8D"};
	REQUIRE(expect_fill("\xE6\x96\x87%1\xE5\xAD\x97", word, 1U, "\xE6\x96\x87\xE5\x90\x8D\xE5\xAD\x97") == 0);

	/* buffer too small: cut at a character boundary, always NUL-terminated */
	char small[6];
	REQUIRE(mfd_text_fill(small, sizeof(small), "Hello %1", two, 2U) == 5U);
	REQUIRE(strcmp(small, "Hello") == 0);
	char cjk[6];
	/* three-byte characters: 5 bytes of room hold one whole character, not one and two thirds */
	REQUIRE(mfd_text_fill(cjk, sizeof(cjk), "%1%1", word, 1U) == 3U);
	REQUIRE(strcmp(cjk, "\xE5\x90\x8D") == 0);
	/* exact fit, and cuts inside two- and four-byte characters */
	char exact[6];
	REQUIRE(mfd_text_fill(exact, sizeof(exact), "Hello", NULL, 0U) == 5U);
	REQUIRE(strcmp(exact, "Hello") == 0);
	char two_bytes[3];
	REQUIRE(mfd_text_fill(two_bytes, 2U, "\xC3\xA9", NULL, 0U) == 0U);
	REQUIRE(two_bytes[0] == '\0');
	REQUIRE(mfd_text_fill(two_bytes, 3U, "\xC3\xA9\xC3\xA9", NULL, 0U) == 2U);
	REQUIRE(strcmp(two_bytes, "\xC3\xA9") == 0);
	char four_bytes[8];
	for (size_t room = 2U; room <= 4U; ++room) {
		REQUIRE(mfd_text_fill(four_bytes, room, "\xF0\x9F\x8E\x9A", NULL, 0U) == 0U);
		REQUIRE(four_bytes[0] == '\0');
	}
	REQUIRE(mfd_text_fill(four_bytes, 5U, "\xF0\x9F\x8E\x9A!", NULL, 0U) == 4U);
	REQUIRE(strcmp(four_bytes, "\xF0\x9F\x8E\x9A") == 0);
	REQUIRE(mfd_text_fill(four_bytes, 7U, "ab\xF0\x9F\x8E\x9A", NULL, 0U) == 6U);
	char one[1];
	REQUIRE(mfd_text_fill(one, sizeof(one), "abc", NULL, 0U) == 0U);
	REQUIRE(one[0] == '\0');
	REQUIRE(mfd_text_fill(NULL, 0U, "abc", NULL, 0U) == 0U);

	/* slot messages use the same filler */
	struct mfd_msg msg;
	mfd_msg_set(&msg, MFD_MSG_LAYER_NOT_IN_SCENE, "Cam %2", "Red", NULL, NULL, NULL);
	char text[64];
	mfd_msg_fill(&msg, "%2: %1 / %3", text, sizeof(text));
	REQUIRE(strcmp(text, "Red: Cam %2 / ") == 0);

	/* UTF-8 checks (C-01): what a fixed buffer leaves when it cuts a long non-ASCII name is repaired */
	REQUIRE(mfd_utf8_is_valid(NULL));
	REQUIRE(mfd_utf8_is_valid(""));
	REQUIRE(mfd_utf8_is_valid("Plain -> \xD0\x9F\xD1\x80\xD0\xB8 \xE6\x97\xA5 \xF0\x9F\x8E\x9A"));
	REQUIRE(!mfd_utf8_is_valid("cut \xD0"));         /* a two-byte character cut after its first byte */
	REQUIRE(!mfd_utf8_is_valid("cut \xE6\x97"));     /* a three-byte one cut after two */
	REQUIRE(!mfd_utf8_is_valid("\x80 stray"));       /* a continuation byte on its own */
	REQUIRE(!mfd_utf8_is_valid("\xC0\xAF"));         /* overlong '/' */
	REQUIRE(!mfd_utf8_is_valid("\xED\xA0\x80"));     /* a surrogate */
	REQUIRE(!mfd_utf8_is_valid("\xF4\x90\x80\x80")); /* past U+10FFFF */
	char fix1[] = "Scene \xD0\x9F\xD0";
	REQUIRE(mfd_utf8_make_valid(fix1));
	REQUIRE(strcmp(fix1, "Scene \xD0\x9F") == 0);
	char fix2[] = "a\x80"
		      "b\xE6\x97\xA5"
		      "c\xE6\x97";
	REQUIRE(mfd_utf8_make_valid(fix2));
	REQUIRE(strcmp(fix2, "ab\xE6\x97\xA5"
			     "c") == 0);
	char ok[] = "unchanged \xC3\xA9";
	REQUIRE(!mfd_utf8_make_valid(ok));
	REQUIRE(strcmp(ok, "unchanged \xC3\xA9") == 0);
	REQUIRE(!mfd_utf8_make_valid(NULL));
	/* a "%s -> %s" label of two 31-letter Cyrillic names, cut to 127 bytes as the engine's label buffer does */
	char cyr[63];
	for (size_t i = 0; i < 31U; ++i) {
		cyr[2U * i] = (char)0xD0;
		cyr[2U * i + 1U] = (char)0x96;
	}
	cyr[62] = '\0';
	char lbl[128];
	snprintf(lbl, sizeof(lbl), "%s -> %s", cyr, cyr);
	REQUIRE(strlen(lbl) == 127U && !mfd_utf8_is_valid(lbl));
	REQUIRE(mfd_utf8_make_valid(lbl));
	REQUIRE(strlen(lbl) == 126U && mfd_utf8_is_valid(lbl));

	printf("test-mfd-text: PASS\n");
	return 0;
}
