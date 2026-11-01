/*
 * Visual Sidechain - translated printf formats
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The translation of key when its printf conversions are exactly those of english, else english (R-I18N-2). The
 * english literal is the trusted format the caller's arguments match, and must equal en-US.ini's text for key
 * (check-locale-keys). Every translated text passed to printf as a format goes through this. */
const char *vs_text_format(const char *key, const char *english);

#ifdef __cplusplus
}
#endif
