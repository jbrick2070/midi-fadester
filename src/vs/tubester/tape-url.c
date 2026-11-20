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

/* Windows takes both slashes; on POSIX a backslash is part of a name */
static bool is_separator(char c, enum tub_path_style style)
{
	return c == '/' || (style == TUB_PATH_STYLE_WINDOWS && c == '\\');
}

static bool is_win_separator(char c)
{
	return is_separator(c, TUB_PATH_STYLE_WINDOWS);
}

static bool is_drive_letter(char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_unc(const char *path)
{
	/* \\server\share: a server name, a separator, a share name. The device forms \\.\ and \\?\ are not shares. */
	if (!is_win_separator(path[0]) || !is_win_separator(path[1]) || path[2] == '\0' || is_win_separator(path[2]) ||
	    path[2] == '.' || path[2] == '?')
		return false;
	const char *sep = path + 2;
	while (*sep && !is_win_separator(*sep))
		sep++;
	return *sep != '\0' && sep[1] != '\0' && !is_win_separator(sep[1]);
}

/* True when something other than separators follows path[from]: a root alone (C:\ or /) names no file */
static bool names_something(const char *path, size_t from, size_t len, enum tub_path_style style)
{
	for (size_t i = from; i < len; i++) {
		if (!is_separator(path[i], style))
			return true;
	}
	return false;
}

bool tub_path_is_usable_as(const char *path, enum tub_path_style style)
{
	if (!path)
		return false;
	size_t len = strlen(path);
	if (style == TUB_PATH_STYLE_POSIX) {
		if (len < 2 || len > TUB_PATH_MAX || path[0] != '/' || !names_something(path, 1, len, style))
			return false;
	} else {
		if (len < 3 || len > TUB_PATH_MAX)
			return false;
		bool drive = is_drive_letter(path[0]) && path[1] == ':' && is_win_separator(path[2]);
		if (!drive && !is_unc(path))
			return false;
		if (drive && !names_something(path, 3, len, style))
			return false;
	}
	/* Control characters are refused everywhere. ? * < > | " cannot be in a Windows name, but are legal in a Mac
	 * one, and the address writer encodes them, so a Mac tape may hold them (E-05). */
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)path[i];
		if (c < 0x20 || c == 0x7f)
			return false;
		if (style == TUB_PATH_STYLE_WINDOWS &&
		    (c == '?' || c == '*' || c == '<' || c == '>' || c == '|' || c == '"'))
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

size_t tub_path_to_file_url_as(const char *path, enum tub_path_style style, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	if (!out || out_size == 0)
		return 0;
	out[0] = '\0';
	if (!tub_path_is_usable_as(path, style))
		return 0;

	size_t n = 0;
	const char *p = path;
	if (style == TUB_PATH_STYLE_POSIX) {
		/* /a -> file:///a; the path's own leading slash is the third one */
		if (!put_str(out, out_size, &n, "file://")) {
			out[0] = '\0';
			return 0;
		}
	} else if (is_unc(path)) {
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
		if (is_separator((char)c, style)) {
			/* POSIX reads //a as /a; the URL says it once */
			if (style == TUB_PATH_STYLE_POSIX && p > path && p[-1] == '/')
				continue;
			ok = put(out, out_size, &n, '/');
		} else if (is_unreserved(c) || (style == TUB_PATH_STYLE_WINDOWS && c == ':' && p == path + 1)) {
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

bool tub_path_is_usable(const char *path)
{
	return tub_path_is_usable_as(path, TUB_PATH_STYLE_HOST);
}

size_t tub_path_to_file_url(const char *path, char *out, size_t out_size)
{
	return tub_path_to_file_url_as(path, TUB_PATH_STYLE_HOST, out, out_size);
}
