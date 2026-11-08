/*
 * Tubester - commands to the deck page, written as JSON for javascript_event
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tubester/command.h"
#include "tubester/deck-link.h"
#include "tubester/speed.h"

#include <obs-data.h>
#include <util/bmem.h>
#include <math.h>
#include <string.h>

bool tub_command_is_safe(const char *json)
{
	if (!json || json[0] != '{')
		return false;
	size_t len = strlen(json);
	if (len < 2 || json[len - 1] != '}')
		return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)json[i];
		if (c == '\n' || c == '\r')
			return false;
		/* U+2028 and U+2029 are E2 80 A8 and E2 80 A9 in UTF-8 */
		if (c == 0xE2 && i + 2 < len && (unsigned char)json[i + 1] == 0x80 &&
		    ((unsigned char)json[i + 2] == 0xA8 || (unsigned char)json[i + 2] == 0xA9))
			return false;
	}
	return true;
}

static char *finish(obs_data_t *data)
{
	const char *json = obs_data_get_json(data);
	char *out = NULL;
	if (json && tub_command_is_safe(json))
		out = bstrdup(json);
	obs_data_release(data);
	return out;
}

static bool is_plain_id(const char *s)
{
	if (!s || !*s || strlen(s) > 64)
		return false;
	for (; *s; s++) {
		char c = *s;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
			  c == '_';
		if (!ok)
			return false;
	}
	return true;
}

char *tub_command_hello(const char *deck_id, int port, const char *token)
{
	if (!is_plain_id(deck_id) || port < 1 || port > 65535 || !token || strlen(token) != TUB_TOKEN_LEN ||
	    !is_plain_id(token))
		return NULL;
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "hello", true);
	obs_data_set_string(d, "deck", deck_id);
	obs_data_set_int(d, "port", port);
	obs_data_set_string(d, "token", token);
	return finish(d);
}

char *tub_command_tape(uint64_t gen, const char *file_url, double at_seconds, double start_seconds, bool loop,
		       bool playing)
{
	/* Only what tub_path_to_file_url writes: file:///X:/... or file://server/share/... */
	if (gen == 0 || !file_url || strncmp(file_url, "file://", 7) != 0 || !isfinite(at_seconds) || at_seconds < 0 ||
	    !isfinite(start_seconds) || start_seconds < 0)
		return NULL;
	const char *rest = file_url + 7;
	bool drive = rest[0] == '/' && ((rest[1] >= 'A' && rest[1] <= 'Z') || (rest[1] >= 'a' && rest[1] <= 'z')) &&
		     rest[2] == ':' && rest[3] == '/' && rest[4] != '\0';
	bool share = rest[0] != '\0' && rest[0] != '/' && strchr(rest, '/') != NULL && strchr(rest, '/')[1] != '\0';
	if (!drive && !share)
		return NULL;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "gen", (long long)gen);
	obs_data_set_string(d, "tape", file_url);
	obs_data_set_double(d, "at", at_seconds);
	obs_data_set_double(d, "start", start_seconds);
	obs_data_set_bool(d, "loop", loop);
	obs_data_set_bool(d, "playing", playing);
	return finish(d);
}

char *tub_command_rate(double rate, bool tape_pitch)
{
	double lo = (double)TUB_SPEED_MIN_PCT / 100.0, hi = (double)TUB_SPEED_MAX_PCT / 100.0;
	if (!isfinite(rate) || rate < lo || rate > hi)
		return NULL;
	obs_data_t *d = obs_data_create();
	obs_data_set_double(d, "rate", rate);
	obs_data_set_bool(d, "keepPitch", !tape_pitch);
	return finish(d);
}

char *tub_command_eject(uint64_t gen)
{
	if (gen == 0)
		return NULL;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "gen", (long long)gen);
	obs_data_set_bool(d, "eject", true);
	return finish(d);
}

char *tub_command_loop(bool loop)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "loop", loop);
	return finish(d);
}

char *tub_command_seek(double seconds)
{
	if (!isfinite(seconds) || seconds < 0)
		return NULL;
	obs_data_t *d = obs_data_create();
	obs_data_set_double(d, "seek", seconds);
	return finish(d);
}

char *tub_command_play(double start_seconds)
{
	if (!isfinite(start_seconds) || start_seconds < 0)
		return NULL;
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "play", true);
	obs_data_set_double(d, "start", start_seconds);
	return finish(d);
}

char *tub_command_pause(void)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "pause", true);
	return finish(d);
}
