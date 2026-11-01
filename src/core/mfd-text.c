/*
 * Midi Fadester
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "core/mfd-text.h"

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
