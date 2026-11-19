/*
 * Visual Sidechain - printf signature check for translated format strings
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "text-format.h"

#include <string.h>

/* The next conversion at or after *pos: its start and length (from '%' to the conversion letter, inclusive). Only the
 * forms the English texts use are known: %s, %d, %u, %f and %g, the last two with an optional ".N" precision; no
 * flags, widths, "*", length modifiers or %n, so nothing a translation could pass on can make printf misbehave.
 * Returns 1 when one is found, 0 at the end of the string, -1 for anything else after a '%'. */
static int next_conversion(const char **pos, const char **start, size_t *len)
{
	const char *p = *pos;
	for (;;) {
		p = strchr(p, '%');
		if (!p)
			return 0;
		if (p[1] == '%') {
			p += 2;
			continue;
		}
		const char *q = p + 1;
		if (*q == '.') {
			q++;
			if (*q < '0' || *q > '9')
				return -1;
			while (*q >= '0' && *q <= '9')
				q++;
			if (*q != 'f' && *q != 'g')
				return -1;
		} else if (!*q || !strchr("sdufg", *q)) {
			return -1;
		}
		*start = p;
		*len = (size_t)(q - p) + 1u;
		*pos = q + 1;
		return 1;
	}
}

bool vs_format_signature_equal(const char *a, const char *b)
{
	if (!a || !b)
		return false;
	for (;;) {
		const char *sa = NULL, *sb = NULL;
		size_t la = 0, lb = 0;
		int ra = next_conversion(&a, &sa, &la);
		int rb = next_conversion(&b, &sb, &lb);
		if (ra < 0 || rb < 0 || ra != rb)
			return false;
		if (ra == 0)
			return true;
		if (la != lb || memcmp(sa, sb, la) != 0)
			return false;
	}
}
