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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes chars lowercase hex digits and a terminator into out, which holds at least chars + 1 bytes. False, with out
 * set to "", when the system generator fails: the caller then has no token, and the deck accepts no report. */
bool tub_random_hex(char *out, size_t chars);

#ifdef __cplusplus
}
#endif
