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

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when both strings hold the same sequence of printf conversions, each written exactly alike ("%%" is no
 * conversion). Only %s, %d, %u, %f, %g and %.Nf, %.Ng are known; any other '%' makes a string invalid, and an invalid
 * string equals nothing, so a translation can only pass on conversions the code's English has (R-I18N-2). False when
 * either is NULL. */
bool vs_format_signature_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
