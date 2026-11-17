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

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h> /* arc4random_buf: the kernel's CSPRNG, never fails */
#else
#include <unistd.h> /* getentropy */
#endif

#include <util/bmem.h>

/* Fills raw with bytes from the operating system's cryptographic generator. False when the system refuses. */
static bool system_random(unsigned char *raw, size_t bytes)
{
#if defined(_WIN32)
	NTSTATUS status = BCryptGenRandom(NULL, raw, (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	return BCRYPT_SUCCESS(status);
#elif defined(__APPLE__)
	arc4random_buf(raw, bytes);
	return true;
#else
	/* getentropy gives at most 256 bytes a call */
	for (size_t done = 0; done < bytes;) {
		size_t n = bytes - done > 256 ? 256 : bytes - done;
		if (getentropy(raw + done, n) != 0)
			return false;
		done += n;
	}
	return true;
#endif
}

bool tub_random_hex(char *out, size_t chars)
{
	if (!out)
		return false;
	out[0] = '\0';
	if (chars == 0)
		return true;
	size_t bytes = (chars + 1) / 2;
	unsigned char *raw = bmalloc(bytes);
	if (!system_random(raw, bytes)) {
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
