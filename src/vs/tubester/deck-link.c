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

bool tub_link_parse_body(const char *body, size_t len, obs_data_t **out)
{
	*out = NULL;
	if (!body || len == 0 || len > 65536)
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
	/* A late post from the page before the reload must not flip the link back and reload the tape again */
	if (new_page && l->linked && strcmp(r.incarnation, l->previous_incarnation) == 0)
		return TUB_REPORT_STALE;
	l->seq = r.seq;
	l->last_report_ns = now_ns;
	if (new_page) {
		/* A fresh page knows no tape yet, so its generation is behind ours by design: the caller resends. A
		 * page that outlived the filter (OBS restart, filter recreated) is ahead, and ours catches up so the
		 * next tape command is not ignored as old. */
		memcpy(l->previous_incarnation, l->incarnation, sizeof(l->previous_incarnation));
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
