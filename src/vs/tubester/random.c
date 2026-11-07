/*
 * Tubester - random hex from the system's generator, for deck ids and tokens
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tubester/random.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <util/bmem.h>

bool tub_random_hex(char *out, size_t chars)
{
	if (!out)
		return false;
	out[0] = '\0';
	if (chars == 0)
		return true;
	size_t bytes = (chars + 1) / 2;
	unsigned char *raw = bmalloc(bytes);
	NTSTATUS status = BCryptGenRandom(NULL, raw, (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (!BCRYPT_SUCCESS(status)) {
		bfree(raw);
		return false;
	}
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < chars; i++) {
		unsigned char b = raw[i / 2];
		out[i] = digits[(i % 2 == 0) ? (b >> 4) : (b & 0x0F)];
	}
	out[chars] = '\0';
	bfree(raw);
	return true;
}
