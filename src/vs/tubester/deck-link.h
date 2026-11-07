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

#pragma once

#include <obs-data.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TUB_TOKEN_LEN 32
#define TUB_INCARNATION_MAX 32
#define TUB_SRC_MAX 260

/* A deck's report has not arrived for this long: No connection (R-DCK-7). */
#define TUB_LINK_TIMEOUT_NS 3000000000ULL

enum tub_deck_state {
	TUB_STATE_NO_CONNECTION,
	TUB_STATE_READY,
	TUB_STATE_PLAYING,
	TUB_STATE_PAUSED,
	TUB_STATE_ENDED,
	TUB_STATE_CANNOT_PLAY,
	TUB_STATE_NO_PICTURE,
};

/* What the page said, parsed from one report. Only fields the page sent are set; the rest stay zero. */
struct tub_report {
	char incarnation[TUB_INCARNATION_MAX];
	char src[TUB_SRC_MAX];
	char event[16];
	uint64_t gen;
	uint64_t seq;
	double current_time;
	double rate;
	int ready_state;
	int video_width;
	int video_height;
	int error_code;
	bool paused;
	bool ended;
	bool no_picture;
};

enum tub_report_verdict {
	TUB_REPORT_ACCEPTED,
	TUB_REPORT_NEW_PAGE,       /* accepted, and the page has reloaded: resend the tape and settings */
	TUB_REPORT_OLD_GENERATION, /* from a tape command already superseded: accepted for liveness, not for state */
	TUB_REPORT_STALE,          /* out of order within the same page: ignored */
	TUB_REPORT_BAD_TOKEN,      /* ignored, counted */
	TUB_REPORT_MALFORMED,
};

struct tub_link {
	char token[TUB_TOKEN_LEN + 1];
	char incarnation[TUB_INCARNATION_MAX];
	char previous_incarnation[TUB_INCARNATION_MAX];
	struct tub_report last;
	uint64_t gen;
	uint64_t seq;
	uint64_t last_report_ns;
	uint64_t bad_token_count;
	bool linked;
};

/* token: exactly TUB_TOKEN_LEN hex characters. Returns false when it is not, and the link then accepts no report. */
bool tub_link_init(struct tub_link *l, const char *token);

/* A new generation for a tape command (R-LNK-3). */
uint64_t tub_link_next_gen(struct tub_link *l);

/* Parses the body the page posted: JSON with token, inc, gen, seq, ev and the player fields. body need not be
 * NUL-terminated. The caller releases *out with obs_data_release. False on anything that is not a JSON object. */
bool tub_link_parse_body(const char *body, size_t len, obs_data_t **out);

/* Fills r from a parsed report. Returns false when a required field (inc, seq, ev) is missing. */
bool tub_report_read(obs_data_t *data, struct tub_report *r);

/* Checks the token and the ordering, then records the report as the deck's latest when accepted. A report from the
 * page before the last reload is stale. On a new page whose generation is ahead, the link's generation catches up. */
enum tub_report_verdict tub_link_accept(struct tub_link *l, obs_data_t *data, uint64_t now_ns);

/* The deck state to show, from the latest accepted report and the time since it arrived (R-DCK-7). */
enum tub_deck_state tub_link_state(const struct tub_link *l, uint64_t now_ns);

const char *tub_deck_state_name(enum tub_deck_state s);

#ifdef __cplusplus
}
#endif
