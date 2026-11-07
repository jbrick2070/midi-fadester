/*
 * Tubester - the link between a deck filter and its page: token, incarnation, generation, sequence, and deck state
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tubester/deck-link.h"

#include <util/bmem.h>
#include <string.h>

static bool is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool tub_link_init(struct tub_link *l, const char *token)
{
	memset(l, 0, sizeof(*l));
	if (!token || strlen(token) != TUB_TOKEN_LEN)
		return false;
	for (size_t i = 0; i < TUB_TOKEN_LEN; i++) {
		if (!is_hex(token[i]))
			return false;
	}
	memcpy(l->token, token, TUB_TOKEN_LEN + 1);
	return true;
}

uint64_t tub_link_next_gen(struct tub_link *l)
{
	return ++l->gen;
}

/* A quiet check that a body is a report-shaped JSON object, so anything else is dropped without a word: the JSON reader
 * in libobs (jansson, with JSON_REJECT_DUPLICATES) logs an error for every text it refuses, and a local process could
 * fill the log that way. A report is one flat object: string keys without escapes, no key twice, and values that are
 * strings, numbers, true, false or null. Strings must be valid UTF-8 and carry no \u0000 and no surrogate escape (the
 * page sends text as UTF-8, never as escapes). Each function returns the position after what it read, or NULL. */
#define TUB_JSON_KEYS_MAX 32
#define TUB_JSON_KEY_LEN_MAX 64

static const char *json_ws(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
		p++;
	return p;
}

static bool json_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned json_hex_value(const char *p)
{
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
	}
	return v;
}

/* Length of the well-formed UTF-8 sequence at p (RFC 3629: no overlong forms, no surrogates, nothing past U+10FFFF),
 * or 0. */
static size_t utf8_sequence(const unsigned char *p, const unsigned char *end)
{
	unsigned char c = p[0];
	if (c < 0x80)
		return 1;
	size_t n;
	unsigned char lo = 0x80, hi = 0xBF;
	if (c >= 0xC2 && c <= 0xDF) {
		n = 2;
	} else if (c >= 0xE0 && c <= 0xEF) {
		n = 3;
		if (c == 0xE0)
			lo = 0xA0;
		else if (c == 0xED)
			hi = 0x9F;
	} else if (c >= 0xF0 && c <= 0xF4) {
		n = 4;
		if (c == 0xF0)
			lo = 0x90;
		else if (c == 0xF4)
			hi = 0x8F;
	} else {
		return 0;
	}
	if ((size_t)(end - p) < n || p[1] < lo || p[1] > hi)
		return 0;
	for (size_t i = 2; i < n; i++) {
		if (p[i] < 0x80 || p[i] > 0xBF)
			return 0;
	}
	return n;
}

/* A string. With allow_escapes false (keys), any backslash is refused, so equal keys are equal bytes. */
static const char *json_string(const char *p, const char *end, bool allow_escapes)
{
	if (p >= end || *p != '"')
		return NULL;
	p++;
	while (p < end) {
		unsigned char c = (unsigned char)*p;
		if (c == '"')
			return p + 1;
		if (c < 0x20)
			return NULL;
		if (c == '\\') {
			if (!allow_escapes || ++p >= end)
				return NULL;
			if (*p == 'u') {
				if (end - p < 5 || !json_hex(p[1]) || !json_hex(p[2]) || !json_hex(p[3]) ||
				    !json_hex(p[4]))
					return NULL;
				unsigned cp = json_hex_value(p + 1);
				if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF))
					return NULL;
				p += 5;
			} else if (*p != '\0' && strchr("\"\\/bfnrt", *p)) {
				p++;
			} else {
				return NULL;
			}
			continue;
		}
		size_t n = utf8_sequence((const unsigned char *)p, (const unsigned char *)end);
		if (n == 0)
			return NULL;
		p += n;
	}
	return NULL;
}

/* One to max digits. The limits keep every number inside what the libobs reader takes without an error: integers fit
 * 64 bits, and no real number overflows a double. Reports never come near them. */
static const char *json_digits(const char *p, const char *end, size_t max)
{
	const char *start = p;
	while (p < end && *p >= '0' && *p <= '9')
		p++;
	size_t n = (size_t)(p - start);
	return (n > 0 && n <= max) ? p : NULL;
}

static const char *json_number(const char *p, const char *end)
{
	if (p < end && *p == '-')
		p++;
	if (p < end && *p == '0')
		p++;
	else if (!(p = json_digits(p, end, 18)))
		return NULL;
	if (p < end && *p == '.' && !(p = json_digits(p + 1, end, 32)))
		return NULL;
	if (p < end && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < end && (*p == '+' || *p == '-'))
			p++;
		if (!(p = json_digits(p, end, 2)))
			return NULL;
	}
	return p;
}

static const char *json_literal(const char *p, const char *end, const char *word)
{
	size_t n = strlen(word);
	return ((size_t)(end - p) >= n && memcmp(p, word, n) == 0) ? p + n : NULL;
}

/* A flat value: no nested object or array, which no report has. */
static const char *json_scalar(const char *p, const char *end)
{
	if (p >= end)
		return NULL;
	switch (*p) {
	case '"':
		return json_string(p, end, true);
	case 't':
		return json_literal(p, end, "true");
	case 'f':
		return json_literal(p, end, "false");
	case 'n':
		return json_literal(p, end, "null");
	default:
		return json_number(p, end);
	}
}

/* True when the text is exactly one report-shaped object, with nothing but white space around it. */
static bool json_is_report_object(const char *body, size_t len)
{
	const char *end = body + len;
	const char *keys[TUB_JSON_KEYS_MAX];
	size_t key_lens[TUB_JSON_KEYS_MAX];
	size_t key_count = 0;
	const char *p = json_ws(body, end);
	if (p >= end || *p != '{')
		return false;
	p = json_ws(p + 1, end);
	if (p < end && *p == '}')
		return json_ws(p + 1, end) == end;
	for (;;) {
		const char *key = p;
		if (!(p = json_string(p, end, false)))
			return false;
		size_t key_len = (size_t)(p - key);
		if (key_len > TUB_JSON_KEY_LEN_MAX + 2 || key_count == TUB_JSON_KEYS_MAX)
			return false;
		for (size_t i = 0; i < key_count; i++) {
			if (key_lens[i] == key_len && memcmp(keys[i], key, key_len) == 0)
				return false;
		}
		keys[key_count] = key;
		key_lens[key_count++] = key_len;
		p = json_ws(p, end);
		if (p >= end || *p != ':')
			return false;
		if (!(p = json_scalar(json_ws(p + 1, end), end)))
			return false;
		p = json_ws(p, end);
		if (p < end && *p == ',') {
			p = json_ws(p + 1, end);
			continue;
		}
		return p < end && *p == '}' && json_ws(p + 1, end) == end;
	}
}

bool tub_link_parse_body(const char *body, size_t len, obs_data_t **out)
{
	*out = NULL;
	if (!body || len == 0 || len > 65536)
		return false;
	if (!json_is_report_object(body, len))
		return false;
	char *copy = bmalloc(len + 1);
	memcpy(copy, body, len);
	copy[len] = '\0';
	obs_data_t *data = obs_data_create_from_json(copy);
	bfree(copy);
	if (!data)
		return false;
	/* An array or a bare value parses to an empty object: only an object with the report fields is a report */
	if (!obs_data_has_user_value(data, "inc")) {
		obs_data_release(data);
		return false;
	}
	*out = data;
	return true;
}

static void copy_string(char *dst, size_t size, const char *src)
{
	if (!src)
		src = "";
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

bool tub_report_read(obs_data_t *data, struct tub_report *r)
{
	memset(r, 0, sizeof(*r));
	if (!data || !obs_data_has_user_value(data, "inc") || !obs_data_has_user_value(data, "seq") ||
	    !obs_data_has_user_value(data, "ev"))
		return false;
	copy_string(r->incarnation, sizeof(r->incarnation), obs_data_get_string(data, "inc"));
	copy_string(r->src, sizeof(r->src), obs_data_get_string(data, "src"));
	copy_string(r->event, sizeof(r->event), obs_data_get_string(data, "ev"));
	if (r->incarnation[0] == '\0' || r->event[0] == '\0')
		return false;
	long long gen = obs_data_get_int(data, "gen");
	long long seq = obs_data_get_int(data, "seq");
	if (gen < 0 || seq < 0)
		return false;
	r->gen = (uint64_t)gen;
	r->seq = (uint64_t)seq;
	r->current_time = obs_data_get_double(data, "ct");
	r->rate = obs_data_get_double(data, "rate");
	r->ready_state = (int)obs_data_get_int(data, "rs");
	r->video_width = (int)obs_data_get_int(data, "vw");
	r->video_height = (int)obs_data_get_int(data, "vh");
	r->error_code = (int)obs_data_get_int(data, "err");
	r->paused = obs_data_get_bool(data, "paused");
	r->ended = obs_data_get_bool(data, "ended");
	r->no_picture = obs_data_get_bool(data, "nopic");
	return true;
}

static bool is_retired(const struct tub_link *l, const char *incarnation)
{
	for (size_t i = 0; i < TUB_RETIRED_MAX; i++) {
		if (l->retired[i][0] != '\0' && strcmp(l->retired[i], incarnation) == 0)
			return true;
	}
	return false;
}

enum tub_report_verdict tub_link_accept(struct tub_link *l, obs_data_t *data, uint64_t now_ns)
{
	if (!data)
		return TUB_REPORT_MALFORMED;
	const char *token = obs_data_get_string(data, "token");
	/* A link whose init failed has an empty token and accepts nothing */
	if (l->token[0] == '\0' || !token || strcmp(token, l->token) != 0) {
		l->bad_token_count++;
		return TUB_REPORT_BAD_TOKEN;
	}
	struct tub_report r;
	if (!tub_report_read(data, &r))
		return TUB_REPORT_MALFORMED;

	bool new_page = !l->linked || strcmp(r.incarnation, l->incarnation) != 0;
	if (!new_page && r.seq <= l->seq)
		return TUB_REPORT_STALE;
	/* A late post from a page a reload replaced must not take the link back and reload the tape again. While the
	 * current page keeps reporting it cannot be gone; once it has been silent past the timeout, the page that reports
	 * wins, so a wrong turn (a retired page evicted from the ring, or one reporting after a long stall) heals. */
	if (new_page && l->linked && is_retired(l, r.incarnation) && now_ns >= l->last_report_ns &&
	    now_ns - l->last_report_ns <= TUB_LINK_TIMEOUT_NS)
		return TUB_REPORT_STALE;
	l->seq = r.seq;
	l->last_report_ns = now_ns;
	if (new_page) {
		/* A fresh page knows no tape yet, so its generation is behind ours by design: the caller resends. A
		 * page that outlived the filter (OBS restart, filter recreated) is ahead, and ours catches up so the
		 * next tape command is not ignored as old. */
		if (l->linked) {
			memcpy(l->retired[l->retired_next], l->incarnation, TUB_INCARNATION_MAX);
			l->retired_next = (l->retired_next + 1) % TUB_RETIRED_MAX;
		}
		memcpy(l->incarnation, r.incarnation, sizeof(l->incarnation));
		l->linked = true;
		if (r.gen > l->gen)
			l->gen = r.gen;
		l->last = r;
		return TUB_REPORT_NEW_PAGE;
	}
	if (r.gen < l->gen) {
		/* The page is still on an earlier tape. It is alive, but its state is not the deck's. */
		return TUB_REPORT_OLD_GENERATION;
	}
	l->last = r;
	return TUB_REPORT_ACCEPTED;
}

enum tub_deck_state tub_link_state(const struct tub_link *l, uint64_t now_ns)
{
	if (!l->linked || now_ns < l->last_report_ns || now_ns - l->last_report_ns > TUB_LINK_TIMEOUT_NS)
		return TUB_STATE_NO_CONNECTION;
	const struct tub_report *r = &l->last;
	if (r->event[0] == '\0' || r->gen < l->gen)
		return TUB_STATE_READY;
	if (r->error_code != 0 || strcmp(r->event, "error") == 0)
		return TUB_STATE_CANNOT_PLAY;
	if (r->no_picture || (r->ready_state >= 1 && r->video_width == 0))
		return TUB_STATE_NO_PICTURE;
	if (r->ended)
		return TUB_STATE_ENDED;
	if (r->ready_state < 1)
		return TUB_STATE_READY;
	return r->paused ? TUB_STATE_PAUSED : TUB_STATE_PLAYING;
}

const char *tub_deck_state_name(enum tub_deck_state s)
{
	switch (s) {
	case TUB_STATE_NO_CONNECTION:
		return "No connection";
	case TUB_STATE_READY:
		return "Ready";
	case TUB_STATE_PLAYING:
		return "Playing";
	case TUB_STATE_PAUSED:
		return "Paused";
	case TUB_STATE_ENDED:
		return "Ended";
	case TUB_STATE_CANNOT_PLAY:
		return "Cannot play this file";
	case TUB_STATE_NO_PICTURE:
		return "No picture in this file";
	}
	return "Unknown";
}
