/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/mfd-text.h"

#include <stdint.h>
#include <string.h>

static mfd_text_lookup_fn lookup_text;

void mfd_text_set_lookup(mfd_text_lookup_fn lookup)
{
	lookup_text = lookup;
}

const char *mfd_text(const char *key, const char *english)
{
	const char *found = (lookup_text && key) ? lookup_text(key) : NULL;
	return (found && *found) ? found : english;
}

const char *mfd_text_or_english(bool translate, const char *key, const char *english)
{
	return translate ? mfd_text(key, english) : english;
}

/* Appends up to len bytes of src; false once out is full. */
static bool append_bytes(char *out, size_t out_size, size_t *used, const char *src, size_t len)
{
	const size_t room = out_size - 1U - *used;
	const size_t take = (len < room) ? len : room;
	memcpy(out + *used, src, take);
	*used += take;
	return take == len;
}

size_t mfd_text_fill(char *out, size_t out_size, const char *text, const char *const *values, size_t count)
{
	if (!out || out_size == 0U) {
		return 0U;
	}
	size_t used = 0U;
	bool fits = true;
	const char *c = text ? text : "";
	while (*c && fits) {
		const char *mark = strchr(c, '%');
		if (!mark) {
			fits = append_bytes(out, out_size, &used, c, strlen(c));
			break;
		}
		fits = append_bytes(out, out_size, &used, c, (size_t)(mark - c));
		if (!fits) {
			break;
		}
		if (mark[1] >= '1' && mark[1] <= '9') {
			const size_t index = (size_t)(mark[1] - '1');
			if (values && index < count && values[index]) {
				fits = append_bytes(out, out_size, &used, values[index], strlen(values[index]));
			}
			c = mark + 2;
		} else {
			fits = append_bytes(out, out_size, &used, "%", 1U);
			c = mark + 1;
		}
	}
	if (!fits) {
		/* never end inside a UTF-8 character: drop continuation bytes and the lead byte they belong to */
		size_t end = used;
		while (end > 0U && (((unsigned char)out[end - 1U]) & 0xC0U) == 0x80U) {
			--end;
		}
		if (end > 0U && (((unsigned char)out[end - 1U]) & 0x80U) != 0U) {
			const unsigned char lead = (unsigned char)out[end - 1U];
			const size_t need = (lead >= 0xF0U) ? 4U : (lead >= 0xE0U) ? 3U : (lead >= 0xC0U) ? 2U : 1U;
			if (used - (end - 1U) < need) {
				used = end - 1U;
			}
		}
	}
	out[used] = '\0';
	return used;
}

/* Length of the well-formed UTF-8 sequence at p, or 0 when p does not start one. A NUL inside a sequence fails the
 * continuation test, so this never reads past the string's end. */
static size_t utf8_sequence_length(const unsigned char *p)
{
	const unsigned char c = p[0];
	if (c < 0x80U) {
		return 1U;
	}
	size_t n;
	uint32_t cp;
	if (c >= 0xC2U && c <= 0xDFU) {
		n = 2U;
		cp = c & 0x1FU;
	} else if (c >= 0xE0U && c <= 0xEFU) {
		n = 3U;
		cp = c & 0x0FU;
	} else if (c >= 0xF0U && c <= 0xF4U) {
		n = 4U;
		cp = c & 0x07U;
	} else {
		return 0U; /* a continuation byte on its own, C0/C1 (overlong) or F5..FF */
	}
	for (size_t i = 1U; i < n; ++i) {
		if ((p[i] & 0xC0U) != 0x80U) {
			return 0U;
		}
		cp = (cp << 6U) | (uint32_t)(p[i] & 0x3FU);
	}
	if ((n == 3U && cp < 0x800U) || (n == 4U && (cp < 0x10000U || cp > 0x10FFFFU)) ||
	    (cp >= 0xD800U && cp <= 0xDFFFU)) {
		return 0U;
	}
	return n;
}

bool mfd_utf8_is_valid(const char *s)
{
	if (!s) {
		return true;
	}
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		const size_t n = utf8_sequence_length(p);
		if (n == 0U) {
			return false;
		}
		p += n;
	}
	return true;
}

bool mfd_utf8_make_valid(char *s)
{
	if (!s) {
		return false;
	}
	unsigned char *r = (unsigned char *)s;
	unsigned char *w = r;
	bool changed = false;
	while (*r) {
		const size_t n = utf8_sequence_length(r);
		if (n == 0U) {
			++r;
			changed = true;
			continue;
		}
		if (w != r) {
			memmove(w, r, n);
		}
		w += n;
		r += n;
	}
	*w = '\0';
	return changed;
}
