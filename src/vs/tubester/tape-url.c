/*
 * Tubester - a tape path becomes a file:/// address for the deck page
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tubester/tape-url.h"

#include <string.h>

static bool is_separator(char c)
{
	return c == '\\' || c == '/';
}

static bool is_drive_letter(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_unc(const char *path)
{
	/* \\server\share: a server name, a separator, a share name. The device forms \\.\ and \\?\ are not shares. */
	if (!is_separator(path[0]) || !is_separator(path[1]) || path[2] == '\0' || is_separator(path[2]) ||
	    path[2] == '.' || path[2] == '?')
		return false;
	const char *sep = path + 2;
	while (*sep && !is_separator(*sep))
		sep++;
	return *sep != '\0' && sep[1] != '\0' && !is_separator(sep[1]);
}

bool tub_path_is_usable(const char *path)
{
	if (!path)
		return false;
	size_t len = strlen(path);
	if (len < 3 || len > TUB_PATH_MAX)
		return false;
	bool drive = is_drive_letter(path[0]) && path[1] == ':' && is_separator(path[2]);
	if (!drive && !is_unc(path))
		return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)path[i];
		if (c < 0x20 || c == 0x7f || c == '?' || c == '*' || c == '<' || c == '>' || c == '|' || c == '"')
			return false;
	}
	return true;
}

static bool is_unreserved(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
	       c == '_' || c == '~';
}

static bool put(char *out, size_t out_size, size_t *n, char c)
{
	if (*n + 1 >= out_size)
		return false;
	out[(*n)++] = c;
	return true;
}

static bool put_str(char *out, size_t out_size, size_t *n, const char *s)
{
	while (*s) {
		if (!put(out, out_size, n, *s++))
			return false;
	}
	return true;
}

size_t tub_path_to_file_url(const char *path, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	if (!out || out_size == 0)
		return 0;
	out[0] = '\0';
	if (!tub_path_is_usable(path))
		return 0;

	size_t n = 0;
	const char *p = path;
	if (is_unc(path)) {
		/* \\server\share\file -> file://server/share/file */
		if (!put_str(out, out_size, &n, "file://")) {
			out[0] = '\0';
			return 0;
		}
		p += 2;
	} else {
		/* C:\a -> file:///C:/a */
		if (!put_str(out, out_size, &n, "file:///")) {
			out[0] = '\0';
			return 0;
		}
	}
	for (; *p; p++) {
		unsigned char c = (unsigned char)*p;
		bool ok;
		if (is_separator((char)c)) {
			ok = put(out, out_size, &n, '/');
		} else if (is_unreserved(c) || (c == ':' && p == path + 1)) {
			ok = put(out, out_size, &n, (char)c);
		} else {
			ok = put(out, out_size, &n, '%') && put(out, out_size, &n, hex[c >> 4]) &&
			     put(out, out_size, &n, hex[c & 15]);
		}
		if (!ok) {
			out[0] = '\0';
			return 0;
		}
	}
	out[n] = '\0';
	return n;
}
