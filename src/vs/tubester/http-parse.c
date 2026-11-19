/*
 * Tubester - the few parts of an HTTP request the report listener reads
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tubester/http-parse.h"

#include <string.h>

size_t tub_http_headers_end(const char *buf, size_t len)
{
	if (!buf || len < 4)
		return 0;
	for (size_t i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
			return i + 4;
	}
	return 0;
}

bool tub_http_is_report_post(const char *buf, size_t headers_len)
{
	static const char *const k_lines[] = {"POST /report HTTP/1.1\r\n", "POST /report HTTP/1.0\r\n"};
	if (!buf)
		return false;
	for (size_t i = 0; i < sizeof(k_lines) / sizeof(k_lines[0]); i++) {
		size_t n = strlen(k_lines[i]);
		if (headers_len >= n && memcmp(buf, k_lines[i], n) == 0)
			return true;
	}
	return false;
}

static bool lower_equals(const char *a, size_t len, const char *lower)
{
	if (strlen(lower) != len)
		return false;
	for (size_t i = 0; i < len; i++) {
		char c = a[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c != lower[i])
			return false;
	}
	return true;
}

bool tub_http_content_length(const char *buf, size_t headers_len, size_t *out)
{
	if (!buf || !out || headers_len < 4)
		return false;
	bool found = false;
	size_t value = 0;
	/* walk the header lines after the request line; each ends in CRLF */
	size_t pos = 0;
	while (pos + 1 < headers_len && !(buf[pos] == '\r' && buf[pos + 1] == '\n'))
		pos++;
	pos += 2;
	while (pos < headers_len) {
		size_t end = pos;
		while (end + 1 < headers_len && !(buf[end] == '\r' && buf[end + 1] == '\n'))
			end++;
		if (end == pos)
			break; /* the blank line */
		const char *colon = memchr(buf + pos, ':', end - pos);
		if (colon && lower_equals(buf + pos, (size_t)(colon - (buf + pos)), "content-length")) {
			if (found)
				return false;
			const char *p = colon + 1;
			const char *stop = buf + end;
			while (p < stop && (*p == ' ' || *p == '\t'))
				p++;
			if (p == stop)
				return false;
			size_t n = 0;
			for (; p < stop && *p != ' ' && *p != '\t'; p++) {
				if (*p < '0' || *p > '9')
					return false;
				n = n * 10 + (size_t)(*p - '0');
				if (n > TUB_HTTP_BODY_MAX)
					return false;
			}
			for (; p < stop; p++) {
				if (*p != ' ' && *p != '\t')
					return false;
			}
			found = true;
			value = n;
		}
		pos = end + 2;
	}
	if (!found)
		return false;
	*out = value;
	return true;
}
